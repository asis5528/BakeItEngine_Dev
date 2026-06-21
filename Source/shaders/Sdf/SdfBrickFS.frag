#version 450
#extension GL_EXT_multiview : enable

// SdfBrickFS - renders the sparse SDF brick data.
//
// Each instanced cube proxy covers one active block (8x8x8 cells). The
// fragment shader sphere-traces the SDF stored in the brick atlas (SSBO of
// floats, 9x9x9 samples per brick: 8 cells + 1 sample of positive padding
// shared with the +X/+Y/+Z neighbor) to find the surface, shades it, and
// writes real depth at the hit point so SDF surfaces compose correctly with
// rasterized geometry.
//
// Spaces used:
//   local  - the block's unit cube, [-0.5, 0.5] per axis (ray march space)
//   global - voxel units across the whole chunk, [0, 64*8) per axis
//            (cross-brick sampling for smooth normals)
//   world  - via blockCenterObject/blockSizeWS and ubo.model (lighting, depth)
//
// Stored SDF values are world-space distances; localDistanceScale() converts
// them into local cube units so they can drive the march step directly.

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view[2];
    mat4 proj[2];
} ubo;

// Binding 2 is part of the shared object pipeline layout; this shader does
// not sample it. BasicUbo.color.x doubles as the block-wireframe debug flag
// (Scene::update writes it; block COLORS come from the per-block SSBO, so
// the rest of the vector stays free).
layout(binding = 1) uniform BasicUbo {
    vec4 color; // x: block wireframe on/off
} ubo2;
layout(binding = 2) uniform sampler3D texSampler;       // fine field (tile pool / dense)
layout(binding = 3) uniform sampler3D coarseTexSampler; // sparse COARSE tile pool

// SBO bindings shifted +1 (binding 3 is now the coarse tile-pool sampler).
layout(std430, binding = 4) readonly buffer BrickBuffer {
    float bricks[]; // brick atlas: 9*9*9 floats per brick
};
layout(std430, binding = 5) readonly buffer BlockBuffer {
    int blocks[];   // 64^3 block table: brick index | flags, negative = empty
};
// Coarse block table (cbpa^3): a coarse TILE INDEX for surface coarse blocks,
// else a negative sentinel. Walked by the coarse tile path for cross-tile taps.
layout(std430, binding = 9) readonly buffer CoarseBlockBuffer {
    int coarseBlocks[];
};
// Legacy coarse brick atlas (dummy in the tile build; kept for descriptor parity).
layout(std430, binding = 10) readonly buffer CoarseBrickBuffer {
    float coarseBricks[];
};

layout(location = 2) in vec3 localCoord;          // entry point on the cube, local space
layout(location = 3) in vec3 cameraPos;           // camera, world space
layout(location = 4) in vec3 worldPosition;       // entry point, world space
layout(location = 5) flat in int brickIndex2;     // this block's brick base offset
layout(location = 6) in vec3 camLocal;            // camera, local space
layout(location = 7) flat in float blockEmissive;
layout(location = 8) flat in vec3 blockColor;
layout(location = 9) flat in float blockSizeWSIn; // block edge length, world units
layout(location = 10) flat in vec3 blockCenterObject;
layout(location = 11) flat in ivec3 blockCoordIn; // this block's coord in the 64^3 grid
layout(location = 12) flat in float coarseLevel;  // 0 fine, else N (coarse-LOD cube)
layout(location = 13) flat in int coarseBrickBase; // downsampled brick base, or -1 (full-res)

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outWorldPos;        // G-buffer slot (future deferred)
layout(location = 2) out vec4 outMotion;          // G-buffer slot (future deferred)

// The raymarch hit always lies inside the cube, i.e. at or behind the
// rasterized front-face depth - promising depth_greater lets the GPU keep
// early-Z enabled despite the gl_FragDepth write: fragments of bricks whose
// front face is already occluded are killed before this shader runs.
// (Draw order is roughly front-to-back via SdfDrawList's counting sort, so
// occluded bricks die early; pixels where a front brick missed/discarded
// still march the bricks behind, keeping hollow regions correct.)
//out float gl_FragDepth;
layout(depth_greater) out float gl_FragDepth;

const int BLOCK_EMISSIVE_FLAG = (1 << 30);
const int BLOCK_INDEX_MASK = BLOCK_EMISSIVE_FLAG - 1;
const int BLOCK_INTERIOR = -999998; // brick-less storage class: solid inside

const int BLOCKS_PER_AXIS = 64;
const int BRICK_DIM = 9;          // 8 cells + 1 padding sample per axis
const float CELLS_PER_BLOCK = 8.0;

// March tuning.
// - One cell is 1/8 of the cube; stored distances are reliable at that scale,
//   so never step less than half a cell: bounds the loop without overshooting.
// - The marched span is the cube diagonal (sqrt(3) ~ 1.73 local units) plus
//   the cross-brick MARGIN (1.5 voxels); at the minimum step that is ~31
//   iterations worst case, so 48 guarantees every ray terminates by hit or
//   span end - never by running out of steps. (Budgets that can be exhausted
//   by grazing rays advancing at MIN_STEP discard mid-block and let the
//   background streak through.) Typical rays still exit in 3-6 steps.
const int   MAX_STEPS = 48;
const float MIN_STEP = 0.5 / CELLS_PER_BLOCK;
const float HIT_EPSILON = 0.0005;

float localDistanceScale()
{
    return 1.0 / max(blockSizeWSIn, 1e-6);
}

float fetchBrick(int brickBase, ivec3 p)
{
    return bricks[brickBase + p.x + p.y * BRICK_DIM + p.z * BRICK_DIM * BRICK_DIM];
}

// Trilinear SDF sample inside this block's brick. pos is local cube space;
// returns the distance in local cube units.
float sampleBrick(vec3 pos)
{
    vec3 grid = clamp((pos + 0.5) * CELLS_PER_BLOCK, vec3(0.0), vec3(7.9999));
    ivec3 cell = ivec3(floor(grid));
    vec3 f = fract(grid);

    float v000 = fetchBrick(brickIndex2, cell + ivec3(0,0,0));
    float v100 = fetchBrick(brickIndex2, cell + ivec3(1,0,0));
    float v010 = fetchBrick(brickIndex2, cell + ivec3(0,1,0));
    float v110 = fetchBrick(brickIndex2, cell + ivec3(1,1,0));
    float v001 = fetchBrick(brickIndex2, cell + ivec3(0,0,1));
    float v101 = fetchBrick(brickIndex2, cell + ivec3(1,0,1));
    float v011 = fetchBrick(brickIndex2, cell + ivec3(0,1,1));
    float v111 = fetchBrick(brickIndex2, cell + ivec3(1,1,1));

    float s0 = mix(mix(v000, v100, f.x), mix(v010, v110, f.x), f.y);
    float s1 = mix(mix(v001, v101, f.x), mix(v011, v111, f.x), f.y);
    return mix(s0, s1, f.z) * localDistanceScale();
}

// Sample the SDF at an arbitrary point of the global voxel grid by walking
// blocks[] - this is what lets normals stay smooth across brick boundaries:
// the offset taps land in whichever neighbor brick owns that position.
// hasData reports whether the point's block owns a brick. If it does not,
// the tap is clamped into the current brick and extrapolated by the
// clamped-off distance (sign from the sentinel: empty keeps growing
// positive, interior keeps falling negative). The estimate's *value* is
// close, but its *gradient* is not - callers should prefer real data
// (one-sided differences) and treat this as a last resort: normals built
// from it kink visibly along block faces, especially in specular regions.
float sampleFieldGlobal(vec3 globalGrid, out bool hasData)
{
    ivec3 blockCoord = ivec3(floor(globalGrid * (1.0 / CELLS_PER_BLOCK)));
    blockCoord = clamp(blockCoord, ivec3(0), ivec3(BLOCKS_PER_AXIS - 1));

    int blockLinear = blockCoord.x + blockCoord.y * BLOCKS_PER_AXIS + blockCoord.z * BLOCKS_PER_AXIS * BLOCKS_PER_AXIS;
    int blockRaw = blocks[blockLinear];
    hasData = (blockRaw >= 0);

    int brickIdx;
    vec3 localGrid;
    float bias = 0.0;
    if (blockRaw < 0) {
        brickIdx = brickIndex2;
        vec3 unclamped = globalGrid - vec3(blockCoordIn) * CELLS_PER_BLOCK;
        localGrid = clamp(unclamped, vec3(0.0), vec3(7.9999));
        float clampedOff = length(unclamped - localGrid) * (blockSizeWSIn / CELLS_PER_BLOCK);
        bias = (blockRaw == BLOCK_INTERIOR) ? -clampedOff : clampedOff;
    } else {
        brickIdx = blockRaw & BLOCK_INDEX_MASK;
        localGrid = clamp(globalGrid - vec3(blockCoord) * CELLS_PER_BLOCK, vec3(0.0), vec3(7.9999));
    }

    ivec3 cell = ivec3(floor(localGrid));
    vec3 f = fract(localGrid);

    float v000 = fetchBrick(brickIdx, cell + ivec3(0,0,0));
    float v100 = fetchBrick(brickIdx, cell + ivec3(1,0,0));
    float v010 = fetchBrick(brickIdx, cell + ivec3(0,1,0));
    float v110 = fetchBrick(brickIdx, cell + ivec3(1,1,0));
    float v001 = fetchBrick(brickIdx, cell + ivec3(0,0,1));
    float v101 = fetchBrick(brickIdx, cell + ivec3(1,0,1));
    float v011 = fetchBrick(brickIdx, cell + ivec3(0,1,1));
    float v111 = fetchBrick(brickIdx, cell + ivec3(1,1,1));

    float s0 = mix(mix(v000, v100, f.x), mix(v010, v110, f.x), f.y);
    float s1 = mix(mix(v001, v101, f.x), mix(v011, v111, f.x), f.y);
    return mix(s0, s1, f.z) + bias;
}

// Difference component for one axis, preferring real brick data: central
// difference when both taps have data, one-sided (vs. the center sample,
// scaled x2 to match the central baseline) when only one side does, and the
// extrapolated central difference as a last resort. This keeps normals from
// kinking along block faces next to brick-less (empty/interior sentinel)
// neighbors - the extrapolation estimates the field's value well but not
// its gradient, and the kink shows as view-dependent light/dark dashes in
// specular regions.
float axisDiff(float fp, bool okP, float fm, bool okM, float f0)
{
    if (okP && okM) return fp - fm;
    if (okP) return 2.0 * (fp - f0);
    if (okM) return 2.0 * (f0 - fm);
    return fp - fm;
}

// Normal from differences that cross brick boundaries via sampleFieldGlobal.
// hitLocalPos is in [-0.5, 0.5] of this block; the offset e = 0.5 voxel
// units. fallback is returned for degenerate gradients (constant field
// around the hit).
vec3 calcNormalAtGlobal(vec3 baseGlobal, vec3 fallback)
{
    const float e = 0.5;

    bool okPX, okMX, okPY, okMY, okPZ, okMZ;
    float px = sampleFieldGlobal(baseGlobal + vec3(e, 0, 0), okPX);
    float mx = sampleFieldGlobal(baseGlobal - vec3(e, 0, 0), okMX);
    float py = sampleFieldGlobal(baseGlobal + vec3(0, e, 0), okPY);
    float my = sampleFieldGlobal(baseGlobal - vec3(0, e, 0), okMY);
    float pz = sampleFieldGlobal(baseGlobal + vec3(0, 0, e), okPZ);
    float mz = sampleFieldGlobal(baseGlobal - vec3(0, 0, e), okMZ);

    // The center sample f0 is only read by axisDiff for a one-sided difference,
    // i.e. when exactly one neighbor on an axis has data. When every axis can
    // take a central (or fully-extrapolated) difference, f0 is unused - so skip
    // that 7th field tap. True for surface pixels whose neighborhood is fully
    // bricked (the common case); identical output either way.
    float f0 = 0.0;
    if ((okPX != okMX) || (okPY != okMY) || (okPZ != okMZ)) {
        bool okC;
        f0 = sampleFieldGlobal(baseGlobal, okC);
    }

    vec3 g = vec3(
        axisDiff(px, okPX, mx, okMX, f0),
        axisDiff(py, okPY, my, okMY, f0),
        axisDiff(pz, okPZ, mz, okMZ, f0));
    if (dot(g, g) <= 1e-12) {
        return fallback;
    }
    return normalize(g);
}

// Fine path: hitLocalPos is in [-0.5, 0.5] of this 1-block cube (8 cells).
vec3 calcNormalSmoothGlobal(vec3 hitLocalPos, vec3 fallback)
{
    vec3 baseGlobal = vec3(blockCoordIn) * CELLS_PER_BLOCK + (hitLocalPos + 0.5) * CELLS_PER_BLOCK;
    return calcNormalAtGlobal(baseGlobal, fallback);
}

// --- Fine dense 3D-texture path (SSBO-vs-texture perf test) -----------------
// ubo2.color.w flips the FINE march/normal between the SSBO brick atlas (the 8
// manual fetches in sampleBrick/sampleFieldGlobal) and a single hardware-
// trilinear tap into a dense 64*8+1 = 513^3 R16F texture (texSampler, binding
// 2; SdfFineToTexCS fills it from the same SSBO). Same field both ways, so the
// only difference is the fetch - watch the FPS as the toggle flips.
const float FINE_TEX_DIM = 513.0; // 64 blocks * 8 cells + 1 shared boundary

// globalGrid is in fine cells across the whole chunk [0, 512]; returns the raw
// stored WORLD-space distance (callers scale to local cube units, exactly like
// sampleBrick / sampleFieldGlobal do).
float sampleFineTexGlobal(vec3 globalGrid)
{
    vec3 uvw = (globalGrid + 0.5) / FINE_TEX_DIM; // texel-center addressing
    return texture(texSampler, uvw).r;
}

// Normal from the dense texture's gradient, taken in GLOBAL cell space so the
// taps cross block boundaries through the one continuous texture (smooth across
// seams, like calcNormalAtGlobal). e = half a fine cell.
vec3 fineTexNormal(vec3 globalGrid, vec3 fallback)
{
    const float e = 0.5;
    vec3 g = vec3(
        sampleFineTexGlobal(globalGrid + vec3(e,0,0)) - sampleFineTexGlobal(globalGrid - vec3(e,0,0)),
        sampleFineTexGlobal(globalGrid + vec3(0,e,0)) - sampleFineTexGlobal(globalGrid - vec3(0,e,0)),
        sampleFineTexGlobal(globalGrid + vec3(0,0,e)) - sampleFineTexGlobal(globalGrid - vec3(0,0,e)));
    if (dot(g, g) <= 1e-12) return fallback;
    return normalize(g);
}

// --- Sparse tile-pool path (BAKEIT_SDF_TEXTURE_ATLAS, texMode 2) -------------
// The field lives in a sparse 3D-texture tile pool: each surface block owns one
// 9^3 tile (8 cells + 1 +pad), located by its block-table entry (reinterpreted
// as a TILE INDEX). texSampler (binding 2) is the pool. Each tile is self-
// contained, so in-block sampling clamps to [0, 7.999] and hardware trilinear
// never bleeds into the physically-adjacent (but spatially-unrelated) tile.
// TEX_TILES_PER_AXIS must match BAKEIT_SDF_TILE_TILES_PER_AXIS (SdfBackend.h).
const int   TEX_TILES_PER_AXIS = 32;
const float TEX_TILE_POOL_DIM  = float(TEX_TILES_PER_AXIS * 9);

// Field-storage precision: must match BAKEIT_SDF_TILE_R8 in SdfBackend.h and the
// SDF_TILE_R8 block in SdfBrushTexCS.comp. The brush scales the normalized-chunk
// distance into the texel's range on store; unscale it on every tile sample.
#define SDF_TILE_R8 1
#if SDF_TILE_R8
const float TILE_FIELD_INV_SCALE = 1.0 / 32.0;
#else
const float TILE_FIELD_INV_SCALE = 1.0;
#endif

// Coarse samples unscale with N/32 (not the fine 1/32): the coarse stamp widened
// the R8 range to 32/N to fit its N-block cube (SdfBrushTexCS / SdfEditor), so the
// coarse march can skip an empty coarse cube in ~1 step. coarseLevel = N here.
float coarseFieldInvScale()
{
#if SDF_TILE_R8
    return coarseLevel / 32.0;
#else
    return 1.0;
#endif
}

// Shared coarse table layout: level-2 region [0, (64/2)^3) then level-4. Must match
// coarseTableBaseFor in SdfBrickVS and the tableBase math in SdfEditor.
int coarseTableBaseFor(int N) { return (N <= 2) ? 0 : (32 * 32 * 32); }

ivec3 tileOriginTexel(int tileIndex)
{
    ivec3 tc = ivec3(
        tileIndex % TEX_TILES_PER_AXIS,
        (tileIndex / TEX_TILES_PER_AXIS) % TEX_TILES_PER_AXIS,
        tileIndex / (TEX_TILES_PER_AXIS * TEX_TILES_PER_AXIS));
    return tc * 9;
}

// Smooth (non-linear) tile-cell coordinate for hardware sampling. Hardware
// trilinear is piecewise-LINEAR in the per-texel fraction, so the field is only
// C0 - its gradient jumps at every cell boundary, which faceted both the surface
// and the finite-difference normals. Warping ONLY the fraction through a quintic
// smootherstep (6f^5-15f^4+10f^3; slope AND curvature zero at f=0 and f=1) makes
// the effective interpolation C2 across cell joints. Integer cells map to
// themselves (smootherstep(0)=0), so the stored samples are still hit exactly
// (and tile seams / the shared +pad stay continuous). A few ALU ops, no extra
// taps. (Inigo Quilez's "smooth texture interpolation", applied in cell space.)
// NOTE: warping the SAMPLE COORDINATE is iq's trick for COLOR textures, where
// only the look matters. For a distance FIELD it is wrong: texture(warp(x))
// resamples the field at shifted positions, so the smootherstep S-curve pulls
// the isosurface toward the cell grid -> grid-aligned lumps + "spooky" wavy
// silhouettes, and it adds zero-slope plateaus instead of removing bands. So
// these are passthrough (plain trilinear); coarse smoothness is handled in the
// NORMAL instead (coarseTexNormal), since the trilinear surface itself is fine.
vec3 smoothCell(vec3 cell)
{
    return cell;
}
vec3 smoothCell2(vec3 cell)
{
    return cell;
}

// In-block sample of THIS block's tile (brickIndex2 = tile index). pos is local
// cube space; returns the distance in LOCAL cube units (like sampleBrick).
float sampleTile(vec3 pos)
{
   // vec3 cell = clamp((pos + 0.5) * CELLS_PER_BLOCK, vec3(0.0), vec3(7.9999));
    vec3 cell = ((pos + 0.5) * CELLS_PER_BLOCK); // C2 hardware interp
    vec3 texel = vec3(tileOriginTexel(brickIndex2)) + cell;
    return texture(texSampler, (texel + 0.5) / TEX_TILE_POOL_DIM).r* TILE_FIELD_INV_SCALE * localDistanceScale();
}

// Cross-tile sample at a global voxel-grid point (the tile-pool analog of
// sampleFieldGlobal): walk blocks[] for the owning tile, or extrapolate from
// this block's tile when the neighbour is brick-less. Returns WORLD distance.
float sampleTileGlobal(vec3 globalGrid, out bool hasData)
{
    ivec3 blockCoord = clamp(ivec3(floor(globalGrid * (1.0 / CELLS_PER_BLOCK))),
                             ivec3(0), ivec3(BLOCKS_PER_AXIS - 1));
    int blockLinear = blockCoord.x + blockCoord.y * BLOCKS_PER_AXIS + blockCoord.z * BLOCKS_PER_AXIS * BLOCKS_PER_AXIS;
    int raw = blocks[blockLinear];
    hasData = (raw >= 0);

    int tileIdx;
    vec3 localCell;
    float bias = 0.0;
    if (raw < 0) {
        tileIdx = brickIndex2; // fall back to this fragment's tile
        vec3 unclamped = globalGrid - vec3(blockCoordIn) * CELLS_PER_BLOCK;
        localCell = clamp(unclamped, vec3(0.0), vec3(7.9999));
        float clampedOff = length(unclamped - localCell) * (blockSizeWSIn / CELLS_PER_BLOCK);
        bias = (raw == BLOCK_INTERIOR) ? -clampedOff : clampedOff;
    } else {
        tileIdx = raw & BLOCK_INDEX_MASK;
        localCell = clamp(globalGrid - vec3(blockCoord) * CELLS_PER_BLOCK, vec3(0.0), vec3(7.9999));
    }
    vec3 texel = vec3(tileOriginTexel(tileIdx)) + smoothCell2(localCell); // C2 hardware interp
    return texture(texSampler, (texel + 0.5) / TEX_TILE_POOL_DIM).r * TILE_FIELD_INV_SCALE + bias;
}

// Cross-tile normal (mirrors calcNormalAtGlobal, but sampling the tile pool).
vec3 calcNormalAtGlobalTile(vec3 baseGlobal, vec3 fallback)
{
    const float e = 2.;
    bool okPX, okMX, okPY, okMY, okPZ, okMZ;
    float px = sampleTileGlobal(baseGlobal + vec3(e, 0, 0), okPX);
    float mx = sampleTileGlobal(baseGlobal - vec3(e, 0, 0), okMX);
    float py = sampleTileGlobal(baseGlobal + vec3(0, e, 0), okPY);
    float my = sampleTileGlobal(baseGlobal - vec3(0, e, 0), okMY);
    float pz = sampleTileGlobal(baseGlobal + vec3(0, 0, e), okPZ);
    float mz = sampleTileGlobal(baseGlobal - vec3(0, 0, e), okMZ);
    float f0 = 0.;
    if ((okPX != okMX) || (okPY != okMY) || (okPZ != okMZ)) {
        bool okC;
        f0 = sampleTileGlobal(baseGlobal, okC);
    }
    vec3 g = vec3(
        axisDiff(px, okPX, mx, okMX, f0),
        axisDiff(py, okPY, my, okMY, f0),
        axisDiff(pz, okPZ, mz, okMZ, f0));
    if (dot(g, g) <= 1e-12) return fallback;
    return normalize(g);
}

// Ray span through this block's unit cube (slab test). ro may sit slightly
// outside the cube (inflated proxy, see SdfBrickVS): tEnter is where the ray
// actually reaches the cube. Robust to grid-axis-parallel rays: plain 1.0/rd
// gives +-inf as intended, but sign(+-0.0) == 0.0 would collapse that axis -
// step() never returns a zero sign, and the clamped reciprocal keeps the
// parallel axis at huge finite distances (never the limiting slab).
void rayCubeSpan(vec3 ro, vec3 rd, out float tEnter, out float tExit)
{
    vec3 s = step(0.0, rd) * 2.0 - 1.0;
    vec3 inv = s / max(abs(rd), vec3(1e-12));
    vec3 tNear = (-s * 0.5 - ro) * inv;
    vec3 tFar  = ( s * 0.5 - ro) * inv;
    tEnter = max(max(tNear.x, tNear.y), tNear.z);
    tExit  = min(min(tFar.x,  tFar.y),  tFar.z);
}

// Coarse-LOD cube (covers N^3 fine blocks; N = coarseLevel, set by the VS). One
// of these replaces up to N^3 fine proxies when the region is far, killing the
// sub-pixel-triangle overshade. It sphere-traces the SAME full-res atlas, so the
// surface and normal match the fine cubes exactly. block attributes are the
// coarse cube's (blockSizeWSIn = N*block; blockCoordIn = the coarse corner's
// fine coord; blockCenterObject = coarse center). MAX_STEPS sized for N up to 8
// (a 64-cell span, ~1-cell empty steps along the diagonal).
const int   COARSE_MAX_STEPS = 42;

// Trilinear SDF of a KNOWN surface block's brick (local cube units). The coarse
// march does the block lookup itself, so it can DDA-skip empty blocks and stop
// in interior blocks - only calling this for the thin surface shell the ray
// actually crosses. This is what makes a deep coarse cube cheap per pixel.
float fetchSurfaceLocal(vec3 globalGrid, ivec3 bc, int brick, float invSize)
{
    vec3 lg = clamp(globalGrid - vec3(bc) * CELLS_PER_BLOCK, vec3(0.0), vec3(7.9999));
    ivec3 cell = ivec3(floor(lg));
    vec3 f = fract(lg);
    float v000 = fetchBrick(brick, cell + ivec3(0,0,0));
    float v100 = fetchBrick(brick, cell + ivec3(1,0,0));
    float v010 = fetchBrick(brick, cell + ivec3(0,1,0));
    float v110 = fetchBrick(brick, cell + ivec3(1,1,0));
    float v001 = fetchBrick(brick, cell + ivec3(0,0,1));
    float v101 = fetchBrick(brick, cell + ivec3(1,0,1));
    float v011 = fetchBrick(brick, cell + ivec3(0,1,1));
    float v111 = fetchBrick(brick, cell + ivec3(1,1,1));
    float s0 = mix(mix(v000, v100, f.x), mix(v010, v110, f.x), f.y);
    float s1 = mix(mix(v001, v101, f.x), mix(v011, v111, f.x), f.y);
    return mix(s0, s1, f.z) * invSize;
}

// --- Downsampled coarse brick path -----------------------------------------
// The coarse block has its OWN 9^3 brick covering the whole coarse cube (1/N
// res). Marching it is an 8-cell in-brick sphere-trace - as cheap as a fine
// cube - so a coarse pixel finally costs like a fine pixel.
float fetchCoarse(int base, ivec3 p)
{
    return coarseBricks[base + p.x + p.y * BRICK_DIM + p.z * BRICK_DIM * BRICK_DIM];
}

float sampleCoarseBrick(int base, vec3 pos) // pos in [-0.5,0.5]; returns world dist
{
    vec3 grid = clamp((pos + 0.5) * CELLS_PER_BLOCK, vec3(0.0), vec3(7.9999));
    ivec3 cell = ivec3(floor(grid));
    vec3 f = fract(grid);
    float v000 = fetchCoarse(base, cell + ivec3(0,0,0));
    float v100 = fetchCoarse(base, cell + ivec3(1,0,0));
    float v010 = fetchCoarse(base, cell + ivec3(0,1,0));
    float v110 = fetchCoarse(base, cell + ivec3(1,1,0));
    float v001 = fetchCoarse(base, cell + ivec3(0,0,1));
    float v101 = fetchCoarse(base, cell + ivec3(1,0,1));
    float v011 = fetchCoarse(base, cell + ivec3(0,1,1));
    float v111 = fetchCoarse(base, cell + ivec3(1,1,1));
    float s0 = mix(mix(v000, v100, f.x), mix(v010, v110, f.x), f.y);
    float s1 = mix(mix(v001, v101, f.x), mix(v011, v111, f.x), f.y);
    return mix(s0, s1, f.z);
}

vec3 coarseBrickNormal(int base, vec3 pos, vec3 fallback)
{
    float e = 1.0 / CELLS_PER_BLOCK; // 1 coarse cell
    vec3 g = vec3(
        sampleCoarseBrick(base, pos + vec3(e,0,0)) - sampleCoarseBrick(base, pos - vec3(e,0,0)),
        sampleCoarseBrick(base, pos + vec3(0,e,0)) - sampleCoarseBrick(base, pos - vec3(0,e,0)),
        sampleCoarseBrick(base, pos + vec3(0,0,e)) - sampleCoarseBrick(base, pos - vec3(0,0,e)));
    if (dot(g, g) <= 1e-12) return fallback;
    return normalize(g);
}

void coarseShadeBrick(int base, float wire)
{
    float invSize = 1.0 / max(blockSizeWSIn, 1e-6);
    float minStep = 0.5 / CELLS_PER_BLOCK;

    vec3 dirLocal = normalize(localCoord - camLocal);
    vec3 ro = clamp(localCoord, vec3(-0.5), vec3(0.5));
    float tEnter, tExit;
    rayCubeSpan(ro, dirLocal, tEnter, tExit);
    tExit += 1e-4;
    if (tEnter > tExit || tExit < 0.0) {
        discard;
    }

    float t = max(tEnter, 0.0);
    bool hit = false;
    for (int i = 0; i < 32; i++) {
        vec3 p = ro + dirLocal * t;
        float d = sampleCoarseBrick(base, p);
        if (d < HIT_EPSILON) {
            hit = true;
            break;
        }
        t += max(d, minStep*0.5);
        if (t > tExit) {
            break;
        }
    }
    if (!hit) {
        if (wire > 0.05) {
            outColor = vec4(vec3(0.08, 0.08, 0.1), 1.0);
            gl_FragDepth = gl_FragCoord.z;
            return;
        }
        discard;
    }

    vec3 hitLocalPos = ro + dirLocal * t;
    vec3 hitObjectPos = blockCenterObject + hitLocalPos * blockSizeWSIn;
    vec3 hitWorldPos = (ubo.model * vec4(hitObjectPos, 1.0)).xyz;
    vec3 viewDir = normalize(worldPosition - cameraPos);
    vec3 n = coarseBrickNormal(base, hitLocalPos, -viewDir);
    n = normalize(mat3(ubo.model) * n);

    vec3 baseColor = vec3(0.0, 1.0, 0.0);
    vec3 light = normalize(vec3(10.0, 10.0, 10.0));
    float dif = clamp(dot(light, n), 0.0, 1.0);
    vec3 reflectDir = reflect(light, n);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0);
    outColor = vec4(baseColor * dif * 0.7 + 0.1 + clamp(dot(viewDir, n) + 1.0, 0.0, 1.0) * 0.5 * vec3(1.0) + spec * 0.5, 1.0);
    outColor.rgb = mix(outColor.rgb, vec3(0.08, 0.08, 0.1), wire * 0.85);

    vec4 clip = ubo.proj[gl_ViewIndex] * ubo.view[gl_ViewIndex] * vec4(hitWorldPos, 1.0);
    gl_FragDepth = clip.z / clip.w;
}

// --- Sparse coarse tile-pool path ------------------------------------------
// The coarse field is a SPARSE 3D-texture tile pool (coarseTexSampler, binding
// 3), built exactly like the fine pool but one LOD down: each surface coarse
// block owns one 9^3 tile, located by the coarse block table (coarseBlocks[],
// reinterpreted as a TILE INDEX). Same 32 tiles/axis as fine, so tileOriginTexel
// / TEX_TILE_POOL_DIM / TILE_FIELD_INV_SCALE all carry over. This mirrors the
// fine sampleTile / sampleTileGlobal path (the known-correct one).

// In-cube coarse sample: THIS cube's own tile (coarseBrickBase). pos is coarse
// cube local [-0.5,0.5]; returns the distance in local cube units like sampleTile.
float sampleCoarseTile(vec3 pos)
{
    vec3 cell = smoothCell(clamp((pos + 0.5) * CELLS_PER_BLOCK, vec3(0.0), vec3(7.9999))); // C2 hardware interp
    vec3 p = vec3(tileOriginTexel(coarseBrickBase)) + cell;
    return texture(coarseTexSampler, (p + 0.5) / TEX_TILE_POOL_DIM).r * coarseFieldInvScale() * localDistanceScale();
}

// Cross-tile coarse sample at a global coarse-cell coordinate ([0, cbpa*8]):
// walk coarseBlocks[] for the owning coarse tile, or extrapolate from this
// cube's tile when the neighbour coarse block is tile-less. Returns the stored
// (chunk-normalized == world) distance, like fine sampleTileGlobal.
float sampleCoarseTileGlobal(vec3 g)
{
    int cbpa = BLOCKS_PER_AXIS / int(coarseLevel + 0.5);   // coarse blocks/axis (64/N)
    ivec3 cbc = clamp(ivec3(floor(g * (1.0 / CELLS_PER_BLOCK))), ivec3(0), ivec3(cbpa - 1));
    int raw = coarseBlocks[coarseTableBaseFor(int(coarseLevel + 0.5)) + cbc.x + cbc.y * cbpa + cbc.z * cbpa * cbpa];

    int tileIdx;
    vec3 localCell;
    float bias = 0.0;
    if (raw < 0) {
        tileIdx = coarseBrickBase; // fall back to this cube's tile
        ivec3 cin = blockCoordIn / int(coarseLevel + 0.5);
        vec3 unclamped = g - vec3(cin) * CELLS_PER_BLOCK;
        localCell = clamp(unclamped, vec3(0.0), vec3(7.9999));
        float clampedOff = length(unclamped - localCell) * (blockSizeWSIn / CELLS_PER_BLOCK);
        bias = (raw == BLOCK_INTERIOR) ? -clampedOff : clampedOff;
    } else {
        tileIdx = raw & BLOCK_INDEX_MASK;
        localCell = clamp(g - vec3(cbc) * CELLS_PER_BLOCK, vec3(0.0), vec3(7.9999));
    }
    vec3 texel = vec3(tileOriginTexel(tileIdx)) + smoothCell(localCell); // C2 hardware interp
    return texture(coarseTexSampler, (texel + 0.5) / TEX_TILE_POOL_DIM).r * coarseFieldInvScale() + bias;
}

// Map this coarse cube's local position [-0.5,0.5] to its global coarse-cell
// coordinate. blockCoordIn is the coarse corner's FINE coord, so /N gives the
// coarse-block index and *8 its cell origin.
vec3 coarseGlobalFromLocal(vec3 pos)
{
    ivec3 cb = blockCoordIn / int(coarseLevel + 0.5);
    return vec3(cb) * CELLS_PER_BLOCK + (pos + 0.5) * CELLS_PER_BLOCK;
}

// March sampler: pos is coarse cube local [-0.5,0.5]; returns the local-cube
// distance (in-cube tile sample). The clamp keeps march taps inside this cube.
float sampleCoarseTex(vec3 pos)
{
    return sampleCoarseTile(clamp(pos, vec3(-0.5), vec3(0.5)));
}

// Coarse normal in GLOBAL coarse-grid space: the finite-difference taps cross
// coarse-cube boundaries by walking coarseBlocks[] to the neighbour tile, so the
// gradient stays continuous across coarse-cube seams (mirrors the fine tile
// normal). e = half a coarse cell. Scale cancels under normalize().
vec3 coarseTexNormal(vec3 pos, vec3 fallback)
{
    vec3 g = coarseGlobalFromLocal(pos); // no clamp - let taps cross seams
    // Wide central difference: trilinear's gradient is piecewise-constant per
    // cell, so a half-cell diff steps cell-to-cell and shows as concentric
    // normal-terracing on curved coarse surfaces. Spanning ~3 cells low-passes
    // the gradient (averages the per-cell steps) so the normal varies smoothly.
    // Bigger e = smoother but rounds genuine edges/corners - this is the knob.
    const float e = 2.;                 // coarse cells, global texel units
    vec3 grad = vec3(
        sampleCoarseTileGlobal(g + vec3(e,0,0)) - sampleCoarseTileGlobal(g - vec3(e,0,0)),
        sampleCoarseTileGlobal(g + vec3(0,e,0)) - sampleCoarseTileGlobal(g - vec3(0,e,0)),
        sampleCoarseTileGlobal(g + vec3(0,0,e)) - sampleCoarseTileGlobal(g - vec3(0,0,e)));
    if (dot(grad, grad) <= 1e-12) return fallback;
    return normalize(grad);
}

void coarseShadeTex(float wire)
{
    float minStep = 0.5 / CELLS_PER_BLOCK;

    vec3 dirLocal = normalize(localCoord - camLocal);
    vec3 ro = clamp(localCoord, vec3(-0.5), vec3(0.5));
    float tEnter, tExit;
    rayCubeSpan(ro, dirLocal, tEnter, tExit);
    tExit += 1e-4;
    if (tEnter > tExit || tExit < 0.0) {
        discard;
    }

    float t = max(tEnter, 0.0);
    bool hit = false;
    for (int i = 0; i < 32; i++) {
        vec3 p = ro + dirLocal * t;
        float d = sampleCoarseTex(p);
        if (d < (HIT_EPSILON)) {
            hit = true;
            break;
        }
        t += max(d, minStep);
        if (t > tExit) {
            break;
        }
    }
    if (!hit) {
        if (wire > 0.05) {
            outColor = vec4(vec3(0.08, 0.08, 0.1), 1.0);
            gl_FragDepth = gl_FragCoord.z;
            return;
        }
        discard;
    }

    vec3 hitLocalPos = ro + dirLocal * t;
    vec3 hitObjectPos = blockCenterObject + hitLocalPos * blockSizeWSIn;
    vec3 hitWorldPos = (ubo.model * vec4(hitObjectPos, 1.0)).xyz;
    vec3 viewDir = normalize(worldPosition - cameraPos);
    vec3 n = coarseTexNormal(hitLocalPos, -viewDir);
  // n = vec3(0.,1.,0.);
    n = normalize(mat3(ubo.model) * n);

    vec3 baseColor = vec3(0.76, 0.47, 0.34)*1.5;
    vec3 light = normalize(vec3(10.0, 10.0, 10.0));
    float dif = clamp(dot(light, n), 0.0, 1.0);
    vec3 reflectDir = reflect(light, n);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0);
    outColor = vec4(baseColor * dif * 0.7 + 0.1 + clamp(dot(viewDir, n) + 1.0, 0.0, 1.0) * 0.5 * vec3(1.0) + spec * 0.5, 1.0);
    outColor.rgb = mix(outColor.rgb, vec3(0.08, 0.08, 0.1), wire * 0.85);

    vec4 clip = ubo.proj[gl_ViewIndex] * ubo.view[gl_ViewIndex] * vec4(hitWorldPos, 1.0);
    gl_FragDepth = clip.z / clip.w;
}

void coarseShade()
{
    // Block-wireframe cage of the coarse cube (uniform branch on ubo2.color.x;
    // computed before any discard so fwidth stays well-defined). localCoord is
    // the coarse cube's [-0.5, 0.5], so this draws its 4-block cell edges.
    float wire = 0.0;
    if (ubo2.color.x > 0.5) {
        vec3 wq = abs(localCoord) * 2.0;
        float wm = min(max(wq.x, wq.y), min(max(wq.y, wq.z), max(wq.z, wq.x)));
        float wireWidth = max(fwidth(wm), 1e-4) * 1.5;
        wire = 1.0 - smoothstep(0.0, wireWidth, 1.0 - wm);
    }

    // Cheap path: this coarse block owns a coarse TILE - march that 8-cell tile.
    if (coarseBrickBase >= 0) {
        coarseShadeTex(wire); // sparse coarse tile pool (hardware trilinear, binding 3)
        return;
    }
    // No coarse tile for this block. In the texture-atlas build bricks[] is a DUMMY
    // (the field lives in the tile pool), so the full-res fallback below would read
    // garbage - that was the solid grey cubes at 4x/8x. A coarse block with active
    // fine children but no coarse tile is a feature smaller than one coarse cell that
    // legitimately vanishes at this LOD, so drop it (keep the cage if wireframe is on).
    if (wire > 0.05) {
        outColor = vec4(vec3(0.08, 0.08, 0.1), 1.0);
        gl_FragDepth = gl_FragCoord.z;
        return;
    }
    discard;
    return; // the full-res fallback below is dead in the texture-atlas build (dummy bricks[])

    int   coarseN = int(coarseLevel + 0.5);                  // N: coarse block = N^3 blocks
    float invSize = 1.0 / max(blockSizeWSIn, 1e-6);          // world dist -> coarse-local
    vec3  corner = vec3(blockCoordIn) * CELLS_PER_BLOCK;     // coarse corner, global voxels
    float cells = float(coarseN) * CELLS_PER_BLOCK;          // N blocks * 8 cells
    float minStep = 0.5 / cells;

    vec3 dirLocal = normalize(localCoord - camLocal);
    vec3 ro = clamp(localCoord, vec3(-0.5), vec3(0.5));
    float tEnter, tExit;
    rayCubeSpan(ro, dirLocal, tEnter, tExit);
    tExit += 1e-4;
    if (tEnter > tExit || tExit < 0.0) {
        discard;
    }

    float t = max(tEnter, 0.0);
    float tEnd = tExit;
    vec3  dg = dirLocal * cells; // ray direction in global voxels per unit t
    bool hit = false;
    for (int i = 0; i < COARSE_MAX_STEPS; i++) {
        vec3 g = corner + (ro + dirLocal * t + 0.5) * cells;
        ivec3 bc = clamp(ivec3(floor(g * (1.0 / CELLS_PER_BLOCK))), ivec3(0), ivec3(BLOCKS_PER_AXIS - 1));
        int raw = blocks[bc.x + bc.y * BLOCKS_PER_AXIS + bc.z * BLOCKS_PER_AXIS * BLOCKS_PER_AXIS];
        if (raw == BLOCK_INTERIOR) {       // inside the shape: the surface is here
            hit = true;
            break;
        }
        if (raw < 0) {                     // EMPTY: jump straight to the next block boundary
            vec3 bound = (vec3(bc) + step(0.0, dg)) * CELLS_PER_BLOCK;
            vec3 dtv = (bound - g) / dg;
            dtv = mix(dtv, vec3(1e9), lessThan(abs(dg), vec3(1e-6)));
            float dtStep = min(min(dtv.x, dtv.y), dtv.z);
            t += dtStep + 0.25 / cells;    // small epsilon to land inside the next block
            if (t > tEnd) break;
            continue;
        }
        // SURFACE block: real trilinear, sphere-trace within the thin shell.
        int brick = raw & BLOCK_INDEX_MASK;
        float d = fetchSurfaceLocal(g, bc, brick, invSize);
        if (d < HIT_EPSILON) {
            hit = true;
            break;
        }
        t += max(d, minStep);
        if (t > tEnd) {
            break;
        }
    }
    if (!hit) {
        // Show the cage even where the coarse ray misses, at the rasterized face
        // depth (gl_FragCoord.z trivially satisfies the depth_greater promise).
        if (wire > 0.05) {
            outColor = vec4(vec3(0.08, 0.08, 0.1), 1.0);
            gl_FragDepth = gl_FragCoord.z;
            return;
        }
        discard;
    }

    vec3 hitLocalPos = ro + dirLocal * t;
    vec3 hitObjectPos = blockCenterObject + hitLocalPos * blockSizeWSIn;
    vec3 hitWorldPos = (ubo.model * vec4(hitObjectPos, 1.0)).xyz;
    vec3 viewDir = normalize(worldPosition - cameraPos);

    vec3 baseGlobal = corner + (hitLocalPos + 0.5) * cells;
    vec3 n = calcNormalAtGlobal(baseGlobal, -viewDir);
    n = normalize(mat3(ubo.model) * n);

    vec3 baseColor = vec3(0.8, 0.8, 0.8);
    vec3 light = normalize(vec3(10.0, 10.0, 10.0));
    float dif = clamp(dot(light, n), 0.0, 1.0);
    vec3 reflectDir = reflect(light, n);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0);
    outColor = vec4(baseColor * dif * 0.7 + 0.1 + clamp(dot(viewDir, n) + 1.0, 0.0, 1.0) * 0.5 * vec3(1.0) + spec * 0.5, 1.0);
    // Wireframe cage overlay on the lit coarse surface.
    outColor.rgb = mix(outColor.rgb, vec3(0.08, 0.08, 0.1), wire * 0.85);

    vec4 clip = ubo.proj[gl_ViewIndex] * ubo.view[gl_ViewIndex] * vec4(hitWorldPos, 1.0);
    gl_FragDepth = clip.z / clip.w;
}

void main()
{
    outWorldPos = vec4(0.0);
    outMotion = vec4(0.0);

    // Coarse-LOD proxy (uniform per draw in the Step-1 toggle, so no fwidth
    // divergence with the fine path below).
    if (coarseLevel > 0.5) {
        coarseShade();
        return;
    }

    // March in local cube space along the true pixel ray. Rasterization
    // assigns each boundary pixel to exactly one brick, but the surface
    // crossing can sit just past a shared face in the neighbor - and the
    // neighbor's fragment does not reliably exist there (its face is an
    // edge-on sliver triangle for grazing/grid-parallel rays). So the
    // handoff must not depend on coverage: march this brick's span with the
    // fast in-brick sampler, then continue a short MARGIN past the cube
    // with the global cross-brick sampler. Whichever brick the pixel landed
    // on finds crossings near its boundary itself; deeper crossings belong
    // to fragments of the bricks that contain them.
    // Direction from the *unclamped* attributes: the true pixel ray. The
    // origin is snapped onto the cube separately - edge-on sliver triangles
    // evaluate attributes at pixel centers that can sit slightly outside the
    // face (sub-pixel), and an off-cube origin makes the slab test reject
    // the fragment. Snapping only the origin keeps the ray unbent.
    // Block-wireframe debug factor (BasicUbo.color.x toggles it): the median
    // of |localCoord| reaches 0.5 exactly on the rasterized face's edges.
    // ubo2.color.x is a uniform, so this branch is uniform control flow and the
    // fwidth inside stays well-defined - skip the derivative + ALU entirely when
    // the wireframe is off (the default), including for fragments that discard.
    float wire = 0.0;
    if (ubo2.color.x > 0.5) {
        vec3 wq = abs(localCoord) * 2.0;
        float wm = min(max(wq.x, wq.y), min(max(wq.y, wq.z), max(wq.z, wq.x)));
        float wireWidth = max(fwidth(wm), 1e-4) * 1.5;
        wire = 1.0 - smoothstep(0.0, wireWidth, 1.0 - wm);
    }

    // FINE field fetch source (ubo2.color.w, uniform per draw):
    //   0 = SSBO brick atlas (default)
    //   1 = dense 513^3 3D texture (the "Fine: 3D texture fetch" A/B test)
    //   2 = sparse tile pool (BAKEIT_SDF_TEXTURE_ATLAS - texture-native field)
    int texMode = int(ubo2.color.w + 0.5);

    vec3 dirLocal = normalize(localCoord - camLocal);
    vec3 ro = clamp(localCoord, vec3(-0.5), vec3(0.5));
    float tEnter;
    float tExit;
    rayCubeSpan(ro, dirLocal, tEnter, tExit);
    tExit += 1e-4;
    if (tEnter > tExit || tExit < 0.0) {
        discard;
    }

    const float MARGIN = 1.5 / CELLS_PER_BLOCK;
    float t = max(tEnter, 0.0);

    // Normal fragments: march the in-cube span plus a short cross-brick
    // margin (a crossing just past a shared face must not depend on the
    // neighbor's fragment existing). Edge-on sliver fragments (in-cube span
    // ~ 0: the ray runs along a block face) exclusively own their boundary-
    // row pixels, so they march a full block diagonal globally - they are
    // confined to 1-px rows, so the extra cost is negligible.
    float tEnd = (tExit - t < MIN_STEP)
        ? t + 1.7321 + MARGIN
        : tExit + MARGIN;
    float tPrev = t;
    float dPrev = 1e9;
    bool hit = false;

    for (int i = 0; i < MAX_STEPS; i++)
    {
        vec3 p = ro + dirLocal * t;
        float d;
        if (texMode == 1) {
            // Dense texture: one hardware-trilinear tap (in-cube AND margin from
            // the same continuous volume).
            vec3 globalGrid = (vec3(blockCoordIn) + (p + 0.5)) * CELLS_PER_BLOCK;
            d = sampleFineTexGlobal(globalGrid) * localDistanceScale();
        } else if (texMode == 2) {
            // Sparse tiles: in-cube samples THIS block's tile; the margin walks
            // blocks[] to the neighbour's tile (each tile is self-contained).
            if (t <= tExit) {
                d = sampleTile(p);
            } else {
                bool okTap;
                vec3 globalGrid = (vec3(blockCoordIn) + (p + 0.5)) * CELLS_PER_BLOCK;
                d = sampleTileGlobal(globalGrid, okTap) * localDistanceScale();
            }
        } else if (t <= tExit) {
            d = sampleBrick(p);
        } else {
            // Past the cube: cross-brick sample (world distance -> local).
            bool okTap;
            vec3 globalGrid = (vec3(blockCoordIn) + (p + 0.5)) * CELLS_PER_BLOCK;
            d = sampleFieldGlobal(globalGrid, okTap) * localDistanceScale();
        }

        if (d < HIT_EPSILON)
        {
            // Secant refinement: place the hit where the field crosses zero
            // between the previous and current sample.
            float denom = dPrev - d;
            float alpha = (abs(denom) > 1e-8) ? clamp(dPrev / denom, 0.0, 1.0) : 0.0;
            t = mix(tPrev, t, alpha);
            hit = true;
            break;
        }

        // The nearest surface is farther than the rest of this ray's marched
        // span: it cannot hit before tEnd.
        if (d >= tEnd - t)
            break;

        tPrev = t;
        dPrev = d;
        t += max(d, MIN_STEP);

        // Clamp the last step onto the end of the span instead of jumping
        // past it, so a zero crossing in the final half-voxel is still
        // sampled (the next iteration either hits via secant or exits
        // through the d >= tEnd - t test).
        if (t >= tEnd)
            t = tEnd;
    }

    if (!hit) {
        // Wireframe debug: show the block cage even where the ray misses,
        // at the rasterized face's own depth (== gl_FragCoord.z, which
        // trivially satisfies the depth_greater promise).
        if (wire > 0.05) {
            outColor = vec4(vec3(0.08, 0.08, 0.1), 1.0);
            gl_FragDepth = gl_FragCoord.z;
            return;
        }
        discard;
    }
  
    vec3 hitLocalPos = ro + dirLocal * t;
    vec3 hitObjectPos = blockCenterObject + hitLocalPos * blockSizeWSIn;
    vec3 hitWorldPos = (ubo.model * vec4(hitObjectPos, 1.0)).xyz;
      
    vec3 dir = normalize(worldPosition - cameraPos);

    // Normal from the cross-brick SDF gradient (the field gradient lives in
    // object/grid space; rotate it into world space - the model is rigid).
    // Computed here for BOTH paths: this is the cache-coherent place to sample
    // the field (the march just warmed those taps), so the normal stays cheap.
    vec3 n;
    if (texMode == 1) {
        vec3 hitGlobal = (vec3(blockCoordIn) + (hitLocalPos + 0.5)) * CELLS_PER_BLOCK;
        n = fineTexNormal(hitGlobal, -dir);
    } else if (texMode == 2) {
        vec3 baseGlobal = vec3(blockCoordIn) * CELLS_PER_BLOCK + (hitLocalPos + 0.5) * CELLS_PER_BLOCK;
        n = calcNormalAtGlobalTile(baseGlobal, -dir);
       // n= vec3(0.,1.,0.);
    } else {
        n = calcNormalSmoothGlobal(hitLocalPos, -dir);
    }
    n = normalize(mat3(ubo.model) * n);

    // Surface albedo (user's debug experiment: flat green).
    vec3 baseColor = vec3(0.76, 0.47, 0.34)*1.5;

    if (ubo2.color.y > 0.5) {
        // Deferred shading (desktop): compute the normal HERE (cache-coherent)
        // and write albedo + the world normal to the G-buffer; the post pass
        // does only the lighting. ubo2.color.y is forced 0 on Quest.
        vec3 albedo = mix(baseColor, vec3(0.08, 0.08, 0.1), wire * 0.85);
        outColor = vec4(albedo, 1.0);
        // aux slot: xyz = world normal, w = coverage (1 surface, 0 cleared
        // background). MSAA averages samples; post normalize() recovers the
        // direction and w > 0.5 masks surface pixels.
        outWorldPos = vec4(n, 1.0);
    } else {
        // Forward lighting: directional light + wrap term + specular.
        vec3 light = normalize(vec3(10.0, 10.0, 10.0));
        float dif = clamp(dot(light, n), 0.0, 1.0);
        vec3 reflectDir = reflect(light, n);
        float spec = pow(max(dot(dir, reflectDir), 0.0), 32.0);

        float baseLum = dot(baseColor, vec3(0.2126, 0.7152, 0.0722));
        float baseChroma = max(baseColor.r, max(baseColor.g, baseColor.b)) - min(baseColor.r, min(baseColor.g, baseColor.b));
        bool matteFloorLike = (baseLum > 0.82 && baseChroma < 0.08);
        if (matteFloorLike) {
            outColor.rgb = baseColor * (0.18 + 0.82 * dif);
        } else {
         //   outColor.rgb = baseColor * dif  +baseColor* 0.0*(1.-dif) + clamp(dot(dir, n) + 1.0, 0.0, 1.0) * 0.5 * vec3(1.0) + spec * 0.5;
        outColor = vec4(baseColor * dif * 0.7 + 0.1 + clamp(dot(dir, n) + 1.0, 0.0, 1.0) * 0.5 * vec3(1.0) + spec * 0.5, 1.0);
        }
        if (blockEmissive > 0.5) {
            outColor.rgb = baseColor * (1.8 + 0.2 * dif);
        }

        // Wireframe debug overlay on the lit surface.
        outColor.rgb = mix(outColor.rgb, vec3(0.08, 0.08, 0.1), wire * 0.85);
        // outColor.rgb = n;
        outColor.a = 1.0;
    }
  vec4 clip = ubo.proj[gl_ViewIndex] * ubo.view[gl_ViewIndex] * vec4(hitWorldPos, 1.0);
    gl_FragDepth = clip.z / clip.w;
}
