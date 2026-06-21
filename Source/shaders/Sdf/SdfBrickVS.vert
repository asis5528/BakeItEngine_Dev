#version 450
#extension GL_EXT_multiview : enable

// SdfBrickVS - one instanced cube proxy per SDF block.
//
// The instance ID buffer normally holds only active block IDs (strict
// renderable set on release / snapshots). During a brush drag the list is
// rebuilt from a GPU readback that is 1-2 frames stale and uses the broad
// "allocated" set, so it can contain blocks the compute has since freed -
// the blockRaw < 0 guard below is the shader-side validity check that makes
// that approximate coverage safe (see BAKEIT_ENGINE_DIRECTION.md: drag =
// broad coverage + shader-side block validity). It also covers meshes drawn
// without an instance buffer (all 64^3 instances).

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view[2];
    mat4 proj[2];
} ubo;

// BasicUbo: .x block wireframe, .y deferred shading, .z coarse-LOD test toggle.
layout(binding = 1) uniform BasicUbo {
    vec4 color;
} ubo2;

// SBO bindings shifted +1 (binding 3 is now the coarse tile-pool sampler).
layout(std430, binding = 5) readonly buffer BlockBuffer {
    int blocks[];   // 64^3 block table: brick index | flags, negative = empty
};
layout(std430, binding = 7) readonly buffer BlockColorBuffer {
    vec4 blockColors[];
};
layout(std430, binding = 8) readonly buffer BlockSpatialBuffer {
    vec4 blockSpatial[]; // xyz: block center (object space), w: block size
};
// Coarse block table (cbpa^3, cbpa = 64/N): a coarse TILE INDEX into the sparse
// coarse tile pool for surface coarse blocks, else a negative sentinel.
layout(std430, binding = 9) readonly buffer CoarseBlockBuffer {
    int coarseBlocks[];
};

layout(location = 0) in vec3 inPosition;   // cube mesh: -2..+2
layout(location = 3) in int inBlockID;     // per-instance block ID

layout(location = 2) out vec3 localCoord;          // [-0.5, 0.5] cube position
layout(location = 3) out vec3 cameraPos;           // camera, world space
layout(location = 4) out vec3 worldPosition;       // vertex, world space
layout(location = 5) flat out int brickIndex;
layout(location = 6) out vec3 camLocal;            // camera, local cube space
layout(location = 7) flat out float blockEmissive;
layout(location = 8) flat out vec3 blockColor;
layout(location = 9) flat out float blockSizeWSOut;
layout(location = 10) flat out vec3 blockCenterObject;
layout(location = 11) flat out ivec3 blockCoordOut;
layout(location = 12) flat out float coarseLevelOut; // 0 fine, else N (coarse-LOD)
layout(location = 13) flat out int coarseBrickBaseOut; // downsampled brick base, or -1

const int BLOCK_EMISSIVE_FLAG = (1 << 30);
const int BLOCK_INDEX_MASK = BLOCK_EMISSIVE_FLAG - 1;
const int BLOCKS_PER_AXIS = 64;
// Distance-LOD tags on the per-instance block ID (set by SdfDrawListScatterCS).
// Block IDs are < 64^3 (2^18), so bits 29/30 are free. Must match the flags in
// SdfDrawListScatterCS.comp. (These ride on inBlockID, distinct from the brick-raw
// emissive flag above which rides on blocks[].)
//  - COARSE: this far block renders as a coarse-LOD cube.
//  - ANCHOR: this is the one child the scatter picked to emit its coarse block's
//    cube (the others degenerate) - replaces an N^3 per-vertex sibling scan.
const int COARSE_INSTANCE_FLAG = (1 << 30);
const int ANCHOR_INSTANCE_FLAG = (1 << 29);
const int LEVEL_INSTANCE_BIT   = (1 << 28); // coarse level: clear -> N=2, set -> N=4

// Shared coarse table layout: level-2 region [0, (64/2)^3) then level-4. Must match
// the tableBase math in SdfEditor (coarseTableTotal) and SdfBrickFS.
int coarseTableBaseFor(int N) { return (N <= 2) ? 0 : (32 * 32 * 32); }

void main()
{
    // Convert the -2..+2 cube into the centered unit cube [-0.5, 0.5].
    // Proxies tile exactly; boundary-pixel handoff between bricks is handled
    // in SdfBrickFS by marching a margin past the cube with global sampling
    // (proxy inflation was tried and reverted - it created paired discards
    // for exactly grid-parallel rays, a worse artifact).
    vec3 v = inPosition * 0.25;

    // The draw-list scatter tags far blocks COARSE (distance LOD) and one child per
    // coarse block ANCHOR: strip both for the block-table lookup, keep them below.
    bool coarseFlag = (inBlockID & COARSE_INSTANCE_FLAG) != 0;
    bool anchorFlag = (inBlockID & ANCHOR_INSTANCE_FLAG) != 0;
    bool levelIs4   = (inBlockID & LEVEL_INSTANCE_BIT) != 0;
    int blockId = max(inBlockID & ~(COARSE_INSTANCE_FLAG | ANCHOR_INSTANCE_FLAG | LEVEL_INSTANCE_BIT), 0);
    int blockRaw = blocks[blockId];
    if (blockRaw < 0) {
        // Stale/empty block: emit a degenerate position so the triangle is
        // clipped before rasterization. No other outputs are needed - the
        // fragment shader never runs for this instance.
        gl_Position = vec4(2e9, 2e9, 2e9, 1.0);
        return;
    }

    blockCoordOut = ivec3(
        blockId % BLOCKS_PER_AXIS,
        (blockId / BLOCKS_PER_AXIS) % BLOCKS_PER_AXIS,
        blockId / (BLOCKS_PER_AXIS * BLOCKS_PER_AXIS));
    brickIndex = blockRaw & BLOCK_INDEX_MASK;
    blockEmissive = ((blockRaw & BLOCK_EMISSIVE_FLAG) != 0) ? 1.0 : 0.0;
    blockColor = blockColors[blockId].rgb;

    vec4 spatial = blockSpatial[blockId];
    float blockSizeWS = spatial.w;
    vec3 centerObject = spatial.xyz;

    // Step-1 coarse-LOD test (ubo2.color.z): render ONE 4x cube per 4^3 coarse
    // block, using the corner child (coord % 4 == 0) as its representative and
    // degenerating the other children. The 4x cube marches the full-res field
    // globally in the FS, so it shows the same surface as the 64 fine cubes it
    // replaces - just far fewer triangles. Coarse blocks whose corner child is
    // empty leave holes; that's expected for this reuse-the-fine-list test (the
    // coarse draw list in the next step emits one cube per active coarse block).
    // Distance LOD: the draw-list scatter put each block in a band and tagged the
    // instance (fine = no COARSE flag; coarse-2 = COARSE; coarse-4 = COARSE+LEVEL).
    // So the level is just a per-instance read - no global uniform needed.
    int coarseN = coarseFlag ? (levelIs4 ? 4 : 2) : 0;
    bool coarse = coarseN >= 2;
    coarseBrickBaseOut = -1;
    if (coarse) {
        // Exactly one coarse cube per active N^3 coarse block: the draw-list scatter
        // already claimed one active child as the ANCHOR via an atomic, so this is an
        // O(1) flag read instead of an N^3 per-vertex sibling scan (that scan was the
        // 8x8x8 bottleneck: 512 SSBO reads x 8 verts x every instance). Non-anchors
        // degenerate. The cube anchors to the block's geometric CORNER regardless of
        // which child won, so it sits right even if the corner child itself is empty.
        ivec3 childBase = (blockCoordOut / coarseN) * coarseN;
        int cornerId = childBase.x + childBase.y * BLOCKS_PER_AXIS + childBase.z * BLOCKS_PER_AXIS * BLOCKS_PER_AXIS;
        if (!anchorFlag) {
            gl_Position = vec4(2e9, 2e9, 2e9, 1.0);
            return;
        }
        vec4 cornerSpatial = blockSpatial[cornerId];
        centerObject = cornerSpatial.xyz + vec3(float(coarseN - 1) * 0.5) * cornerSpatial.w; // corner -> coarse center
        blockSizeWS = cornerSpatial.w * float(coarseN);                                      // N blocks across
        blockCoordOut = childBase; // FS derives the coarse corner (x8) from this

        // Sparse coarse TILE pool: the coarse field is analytically stamped at the
        // selected level N (coarseBlocks is the cbpa = 64/N table, refilled by the
        // CPU when the level changes), so the tile path now applies at ANY coarse N
        // (2/4/8). Pass the coarse TILE INDEX so the FS samples binding 3 (the
        // coarse tile pool); a negative entry means that coarse block has no tile.
        coarseBrickBaseOut = -1;
        if (coarseN >= 2) {
            ivec3 cbc = childBase / coarseN;            // coarse-block coord
            int cbpa = BLOCKS_PER_AXIS / coarseN;       // coarse blocks per axis (64/N)
            int cidx = coarseTableBaseFor(coarseN) + cbc.x + cbc.y * cbpa + cbc.z * cbpa * cbpa;
            int craw = coarseBlocks[cidx];
            coarseBrickBaseOut = (craw >= 0) ? (craw & BLOCK_INDEX_MASK) : -1;
        }
    }
    coarseLevelOut = coarse ? float(coarseN) : 0.0; // carries N to the FS

    blockSizeWSOut = blockSizeWS;
    blockCenterObject = centerObject;

    vec3 centerWorld = (ubo.model * vec4(centerObject, 1.0)).xyz;
    vec3 vertWS = (ubo.model * vec4(centerObject + v * blockSizeWS, 1.0)).xyz;
    worldPosition = vertWS;

    // Camera world position from the view matrix. View is rigid (R | t),
    // so camera = -transpose(R) * t - no full matrix inverse needed.
    mat4 view = ubo.view[gl_ViewIndex];
    cameraPos = -(vec3(view[3]) * mat3(view));
    // camLocal must live in OBJECT space (the cube/grid axes): bring the
    // camera into object space with the model's inverse instead of assuming
    // object == world. The model is rotation + uniform scale + translation,
    // so inverse(R*s) = transpose(R*s) / s^2 (the dot of a column with
    // itself is s^2).
    vec3 modelCol0 = vec3(ubo.model[0]);
    float invScale2 = 1.0 / max(dot(modelCol0, modelCol0), 1e-12);
    vec3 cameraObject = (transpose(mat3(ubo.model)) * (cameraPos - vec3(ubo.model[3]))) * invScale2;
    camLocal = (cameraObject - centerObject) / blockSizeWS;

    localCoord = v;
    gl_Position = ubo.proj[gl_ViewIndex] * view * vec4(vertWS, 1.0);
}
