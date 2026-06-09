#version 450
#extension GL_EXT_multiview : enable

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 prevModel;
    mat4 view[2];
    mat4 proj[2];
    mat4 prevProj[2];
    mat4 projNoJitter[2];
    mat4 prevView[2];
    mat4 prevProjNoJitter[2];
} ubo;
layout( std430,binding = 4) readonly buffer BlockBuffer {
    int blocks[];   // single int
};
layout( std430,binding = 6) readonly buffer BlockColorBuffer {
    vec4 blockColors[];
};
layout( std430,binding = 7) readonly buffer BlockSpatialBuffer {
    vec4 blockSpatial[];
};
// Bind your compute-written blocks[] here (same underlying VkBuffer, different pipeline)


layout(location = 0) in vec3 inPosition;   // OBJ cube: -2..+2
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in int inBlockID;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 2) out vec3 localCoord;
layout(location = 3) out vec3 cameraPos;
layout(location = 4) out vec3 worldPosition;
layout(location = 5) flat out int brickIndex;
layout(location = 6) out vec3 camLocal;
layout(location = 7) flat out float blockEmissive;
layout(location = 8) flat out vec3 blockColor;
layout(location = 9) flat out float blockSizeWSOut;
layout(location = 10) flat out vec3 blockCenterObject;
layout(location = 11) flat out ivec3 blockCoordOut;

const int BLOCK_EMISSIVE_FLAG = (1 << 30);
const int BLOCK_INDEX_MASK = BLOCK_EMISSIVE_FLAG - 1;

void main()
{
    // Convert -2..+2 cube into centered unit cube -0.5..+0.5
    vec3 v = inPosition * (1.0 / 4.0);

    uint blockIdx = uint(max(inBlockID, 0));
    int linId = int(blockIdx);
    blockCoordOut = ivec3(linId % 64, (linId / 64) % 64, linId / (64 * 64));
    int blockRaw = blocks[blockIdx];
    if (blockRaw < 0) {
        gl_Position = vec4(2e9, 2e9, 2e9, 1.0);
        brickIndex = 0;
        blockEmissive = 0.0;
        blockColor = vec3(0.0);
        fragColor = vec3(0.0);
        fragTexCoord = inTexCoord;
        localCoord = v;
        cameraPos = vec3(0.0);
        worldPosition = vec3(0.0);
        camLocal = vec3(0.0);
        blockSizeWSOut = 1.0 / 64.0;
        return;
    }
    brickIndex = (blockRaw & BLOCK_INDEX_MASK);
    blockEmissive = ((blockRaw & BLOCK_EMISSIVE_FLAG) != 0) ? 1.0 : 0.0;
    blockColor = blockColors[blockIdx].rgb;
if(blockIdx>0) {
  //gl_Position = vec4(2e9, 2e9, 2e9, 1.0); // push off-screen
    //   return;
}
// Hide inactive blocks (your compute uses negatives for "not allocated/empty/packed")
    // If you ALSO want to draw packed-band blocks, change condition to:
    // if (bb.blocks[blockIdx] == -999999) ...
    
    vec4 spatial = blockSpatial[blockIdx];
    float blockSizeWS = spatial.w;
    blockSizeWSOut = blockSizeWS;
    vec3 centerWS = spatial.xyz;
    blockCenterObject = centerWS;
    vec3 centerWorld = (ubo.model * vec4(centerWS, 1.0)).xyz;
    // Scale unit cube to block size and translate to center
    vec3 vertWS = centerWS + v * blockSizeWS;
    vertWS = (ubo.model * vec4(vertWS, 1.0)).xyz;

    worldPosition = vertWS;
   // worldPosition += 0.001;
   // vertWS.xy+=0.0078;
    //vertWs-=0.0078;
    //worldPosition = vertWS;
    mat4 invView = inverse(ubo.view[gl_ViewIndex]);
       cameraPos = vec3(invView[3]);
   camLocal = (cameraPos - centerWorld) / blockSizeWS;

    vec3 cpos =  worldPosition+vec3(0.5+0.00);
vec3 ppos = cpos;


int blockPerAxis = 64;
ivec3 ires = ivec3(blockPerAxis,blockPerAxis,blockPerAxis);
    vec3  fres = vec3(ires);

    // texel space
    vec3 st = clamp(cpos * fres - 0.5,0.,fres.x-1);
    ivec3 i = ivec3(floor(st));
    
   // bricks[i.x+i.y*8+i.z*64] = h1.r;
  // blocks[i.x+i.y*8+i.z*64] = int(h1.r*255.);

//h = textureTrilinear( texSampler, cpos).r;
 //brickIndex = blocks[i.x+i.y*blockPerAxis+i.z*blockPerAxis*blockPerAxis];



    gl_Position = ubo.proj[gl_ViewIndex] *
                  ubo.view[gl_ViewIndex] *
                  vec4(vertWS, 1.0);
//worldPosition = inPosition.xyz;
    // If you actually want camera world position, compute it like this:
    // (assuming ubo.view is a standard view matrix)
    
   

    fragColor = inColor;
    fragTexCoord = inTexCoord;
    localCoord = v; // centered local coords [-0.5..+0.5]
}
