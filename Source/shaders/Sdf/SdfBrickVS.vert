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

layout(std430, binding = 4) readonly buffer BlockBuffer {
    int blocks[];   // 64^3 block table: brick index | flags, negative = empty
};
layout(std430, binding = 6) readonly buffer BlockColorBuffer {
    vec4 blockColors[];
};
layout(std430, binding = 7) readonly buffer BlockSpatialBuffer {
    vec4 blockSpatial[]; // xyz: block center (object space), w: block size
};
// Coarse-LOD downsampled block table (16^3 for the N=4 level): brick base in the
// coarse atlas, or -1 if that coarse block has no downsampled brick yet.
layout(std430, binding = 8) readonly buffer CoarseBlockBuffer {
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

void main()
{
    // Convert the -2..+2 cube into the centered unit cube [-0.5, 0.5].
    // Proxies tile exactly; boundary-pixel handoff between bricks is handled
    // in SdfBrickFS by marching a margin past the cube with global sampling
    // (proxy inflation was tried and reverted - it created paired discards
    // for exactly grid-parallel rays, a worse artifact).
    vec3 v = inPosition * 0.25;

    int blockId = max(inBlockID, 0);
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
    // ubo2.color.z carries the coarse-LOD level: 0 = off (fine), else N, where a
    // coarse block = N x N x N fine blocks (2 / 4 / 8). N divides 64.
    int coarseN = int(ubo2.color.z + 0.5);
    bool coarse = coarseN >= 2;
    coarseBrickBaseOut = -1;
    if (coarse) {
        // Exactly one coarse cube per active N^3 coarse block: this instance
        // emits only if it is the lowest-linear-id active child (the "anchor");
        // the others degenerate. Anchor to the block's geometric CORNER (not the
        // anchor child) so the cube sits right even when the corner child is
        // empty - that corner-only assumption was leaving holes.
        ivec3 childBase = (blockCoordOut / coarseN) * coarseN;
        int cornerId = childBase.x + childBase.y * BLOCKS_PER_AXIS + childBase.z * BLOCKS_PER_AXIS * BLOCKS_PER_AXIS;
        int minActive = 0x7fffffff;
        for (int dz = 0; dz < coarseN; ++dz)
            for (int dy = 0; dy < coarseN; ++dy)
                for (int dx = 0; dx < coarseN; ++dx) {
                    ivec3 cc = childBase + ivec3(dx, dy, dz);
                    int cid = cc.x + cc.y * BLOCKS_PER_AXIS + cc.z * BLOCKS_PER_AXIS * BLOCKS_PER_AXIS;
                    if (blocks[cid] >= 0) { minActive = min(minActive, cid); }
                }
        if (blockId != minActive) {
            gl_Position = vec4(2e9, 2e9, 2e9, 1.0);
            return;
        }
        vec4 cornerSpatial = blockSpatial[cornerId];
        centerObject = cornerSpatial.xyz + vec3(float(coarseN - 1) * 0.5) * cornerSpatial.w; // corner -> coarse center
        blockSizeWS = cornerSpatial.w * float(coarseN);                                      // N blocks across
        blockCoordOut = childBase; // FS derives the coarse corner (x8) from this

        // Dense-coarse 3D-texture path is SHELVED: binding 2 (texSampler) is now
        // wired to the FINE dense atlas for the SSBO-vs-texture fine test, so the
        // coarse cube must NOT sample it. Leaving coarseBrickBaseOut = -1 makes
        // the FS fall back to its SSBO full-res coarse march (block-skip DDA over
        // the fine atlas). To re-enable the coarse-tex test, point binding 2 back
        // at coarseAtlasTextureIndex and restore the `coarseN == 2` gate here.
        coarseBrickBaseOut = -1;
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
