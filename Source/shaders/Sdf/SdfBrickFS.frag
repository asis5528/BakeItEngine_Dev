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

// Bindings 1 and 2 are part of the shared object pipeline layout; this
// shader does not sample them.
layout(binding = 1) uniform BasicUbo {
    vec4 color;
} ubo2;
layout(binding = 2) uniform sampler3D texSampler;

layout(std430, binding = 3) readonly buffer BrickBuffer {
    float bricks[]; // brick atlas: 9*9*9 floats per brick
};
layout(std430, binding = 4) readonly buffer BlockBuffer {
    int blocks[];   // 64^3 block table: brick index | flags, negative = empty
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
vec3 calcNormalSmoothGlobal(vec3 hitLocalPos, vec3 fallback)
{
    vec3 baseGlobal = vec3(blockCoordIn) * CELLS_PER_BLOCK + (hitLocalPos + 0.5) * CELLS_PER_BLOCK;
    const float e = 0.5;

    bool okC, okPX, okMX, okPY, okMY, okPZ, okMZ;
    float f0 = sampleFieldGlobal(baseGlobal, okC);
    float px = sampleFieldGlobal(baseGlobal + vec3(e, 0, 0), okPX);
    float mx = sampleFieldGlobal(baseGlobal - vec3(e, 0, 0), okMX);
    float py = sampleFieldGlobal(baseGlobal + vec3(0, e, 0), okPY);
    float my = sampleFieldGlobal(baseGlobal - vec3(0, e, 0), okMY);
    float pz = sampleFieldGlobal(baseGlobal + vec3(0, 0, e), okPZ);
    float mz = sampleFieldGlobal(baseGlobal - vec3(0, 0, e), okMZ);

    vec3 g = vec3(
        axisDiff(px, okPX, mx, okMX, f0),
        axisDiff(py, okPY, my, okMY, f0),
        axisDiff(pz, okPZ, mz, okMZ, f0));
    if (dot(g, g) <= 1e-12) {
        return fallback;
    }
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

void main()
{
    outWorldPos = vec4(0.0);
    outMotion = vec4(0.0);

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
        if (t <= tExit) {
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
        discard;
    }

    vec3 hitLocalPos = ro + dirLocal * t;
    vec3 hitObjectPos = blockCenterObject + hitLocalPos * blockSizeWSIn;
    vec3 hitWorldPos = (ubo.model * vec4(hitObjectPos, 1.0)).xyz;

    vec3 dir = normalize(worldPosition - cameraPos);
    vec3 n = calcNormalSmoothGlobal(hitLocalPos, -dir);

    // Lighting: single directional light + wrap term + specular.
    vec3 light = normalize(vec3(10.0, 10.0, 10.0));
    float dif = clamp(dot(light, n), 0.0, 1.0);
    vec3 reflectDir = reflect(light, n);
    float spec = pow(max(dot(dir, reflectDir), 0.0), 32.0);

    vec3 baseColor = blockColor;
    float baseLum = dot(baseColor, vec3(0.2126, 0.7152, 0.0722));
    float baseChroma = max(baseColor.r, max(baseColor.g, baseColor.b)) - min(baseColor.r, min(baseColor.g, baseColor.b));
    bool matteFloorLike = (baseLum > 0.82 && baseChroma < 0.08);
    if (matteFloorLike) {
        outColor.rgb = baseColor * (0.18 + 0.82 * dif);
    } else {
        outColor.rgb = baseColor * dif * 0.7 + 0.1 + clamp(dot(dir, n) + 1.0, 0.0, 1.0) * 0.5 * vec3(1.0) + spec * 0.5;
    }
    if (blockEmissive > 0.5) {
        outColor.rgb = baseColor * (1.8 + 0.2 * dif);
    }
    //outColor.rgb = n;
    outColor.a = 1.0;

  //  outWorldPos = vec4(hitWorldPos, 1.0);

    // Real depth at the SDF hit so volumetric surfaces intersect correctly
    // with rasterized meshes and each other.
    vec4 clip = ubo.proj[gl_ViewIndex] * ubo.view[gl_ViewIndex] * vec4(hitWorldPos, 1.0);
    gl_FragDepth = clip.z / clip.w;
}
