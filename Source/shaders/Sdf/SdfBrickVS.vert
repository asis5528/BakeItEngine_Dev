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

layout(std430, binding = 4) readonly buffer BlockBuffer {
    int blocks[];   // 64^3 block table: brick index | flags, negative = empty
};
layout(std430, binding = 6) readonly buffer BlockColorBuffer {
    vec4 blockColors[];
};
layout(std430, binding = 7) readonly buffer BlockSpatialBuffer {
    vec4 blockSpatial[]; // xyz: block center (object space), w: block size
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
    blockSizeWSOut = blockSizeWS;
    vec3 centerObject = spatial.xyz;
    blockCenterObject = centerObject;

    vec3 centerWorld = (ubo.model * vec4(centerObject, 1.0)).xyz;
    vec3 vertWS = (ubo.model * vec4(centerObject + v * blockSizeWS, 1.0)).xyz;
    worldPosition = vertWS;

    // Camera world position from the view matrix. View is rigid (R | t),
    // so camera = -transpose(R) * t - no full matrix inverse needed.
    mat4 view = ubo.view[gl_ViewIndex];
    cameraPos = -(vec3(view[3]) * mat3(view));
    camLocal = (cameraPos - centerWorld) / blockSizeWS;

    localCoord = v;
    gl_Position = ubo.proj[gl_ViewIndex] * view * vec4(vertWS, 1.0);
}
