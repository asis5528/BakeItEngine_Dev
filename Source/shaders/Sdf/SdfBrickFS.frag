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

const int BLOCK_EMISSIVE_FLAG = (1 << 30);
const int BLOCK_INDEX_MASK = BLOCK_EMISSIVE_FLAG - 1;

const int BLOCKS_PER_AXIS = 64;
const int BRICK_DIM = 9;          // 8 cells + 1 padding sample per axis
const float CELLS_PER_BLOCK = 8.0;

// March tuning.
// - One cell is 1/8 of the cube; stored distances are reliable at that scale,
//   so never step less than half a cell: bounds the loop without overshooting.
// - The cube diagonal is sqrt(3) ~ 1.73 local units; at the minimum step that
//   is ~28 iterations worst case, but sphere tracing leaves empty space in
//   1-3 steps and reaches a surface in 3-6, so 24 covers everything except
//   pathological grazing rays (which exit via tExit anyway).
const int   MAX_STEPS = 24;
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
// If the target block has no brick allocated, fall back to a clamped sample
// of the current brick so silhouettes don't get blown out by an artificially
// large empty-space value.
float sampleFieldGlobal(vec3 globalGrid)
{
    ivec3 blockCoord = ivec3(floor(globalGrid * (1.0 / CELLS_PER_BLOCK)));
    blockCoord = clamp(blockCoord, ivec3(0), ivec3(BLOCKS_PER_AXIS - 1));

    int blockLinear = blockCoord.x + blockCoord.y * BLOCKS_PER_AXIS + blockCoord.z * BLOCKS_PER_AXIS * BLOCKS_PER_AXIS;
    int blockRaw = blocks[blockLinear];

    int brickIdx;
    vec3 localGrid;
    if (blockRaw < 0) {
        brickIdx = brickIndex2;
        localGrid = clamp(globalGrid - vec3(blockCoordIn) * CELLS_PER_BLOCK, vec3(0.0), vec3(7.9999));
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
    return mix(s0, s1, f.z);
}

// Central-difference normal that crosses brick boundaries via
// sampleFieldGlobal. hitLocalPos is in [-0.5, 0.5] of this block; the offset
// e = 0.5 voxel units. fallback is returned for degenerate gradients
// (constant field around the hit).
vec3 calcNormalSmoothGlobal(vec3 hitLocalPos, vec3 fallback)
{
    vec3 baseGlobal = vec3(blockCoordIn) * CELLS_PER_BLOCK + (hitLocalPos + 0.5) * CELLS_PER_BLOCK;
    const float e = 0.5;

    float dx = sampleFieldGlobal(baseGlobal + vec3(e, 0, 0)) - sampleFieldGlobal(baseGlobal - vec3(e, 0, 0));
    float dy = sampleFieldGlobal(baseGlobal + vec3(0, e, 0)) - sampleFieldGlobal(baseGlobal - vec3(0, e, 0));
    float dz = sampleFieldGlobal(baseGlobal + vec3(0, 0, e)) - sampleFieldGlobal(baseGlobal - vec3(0, 0, e));

    vec3 g = vec3(dx, dy, dz);
    if (dot(g, g) <= 1e-12) {
        return fallback;
    }
    return normalize(g);
}

// Distance along the ray to where it leaves this block's unit cube.
float rayCubeExit(vec3 ro, vec3 rd)
{
    vec3 invDir = 1.0 / rd; // GLSL inf-safe: axis-parallel components yield +-inf
    vec3 tFar = (sign(rd) * 0.5 - ro) * invDir;
    return min(tFar.x, min(tFar.y, tFar.z));
}

void main()
{
    outWorldPos = vec4(0.0);
    outMotion = vec4(0.0);

    // March in local cube space; the entry point is this fragment's position
    // on the rasterized cube face, so t starts at 0 with no entry search.
    vec3 dirLocal = normalize(localCoord - camLocal);
    float tExit = rayCubeExit(localCoord, dirLocal) + 1e-4;

    float t = 0.0;
    float tPrev = 0.0;
    float dPrev = 1e9;
    bool hit = false;

    for (int i = 0; i < MAX_STEPS; i++)
    {
        float d = sampleBrick(localCoord + dirLocal * t);

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

        // The nearest surface is farther than the rest of this ray's path
        // through the cube: it cannot hit inside this block.
        if (d >= tExit - t)
            break;

        tPrev = t;
        dPrev = d;
        t += max(d, MIN_STEP);

        if (t >= tExit)
            break;
    }

    if (!hit) {
        discard;
    }

    vec3 hitLocalPos = localCoord + dirLocal * t;
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
    outColor.a = 1.0;

    outWorldPos = vec4(hitWorldPos, 1.0);

    // Real depth at the SDF hit so volumetric surfaces intersect correctly
    // with rasterized meshes and each other.
    vec4 clip = ubo.proj[gl_ViewIndex] * ubo.view[gl_ViewIndex] * vec4(hitWorldPos, 1.0);
    gl_FragDepth = clip.z / clip.w;
}
