#include "ScenePipelines.h"
#include "Platform/Platform.h"


ScenePipelines::ScenePipelines(VulkanBase *base,VkRenderPass renderPass) {

	this->base = base;
	
    GraphicsPipeline* opipe = new GraphicsPipeline();
    pipelines.push_back(opipe);
    PipelineInput input;
    input.vertexShaderpath = "shaders/World/ObjectShaderVS.spv";
    input.fragmentShaderpath = "shaders/World/ObjectTexturedFS.spv";
    input.renderPass = renderPass;
    //   input.MultiviewLayer = frameBuffer[0].multiLayerView;
    input.device = base->device;
    input.msaaSamples = base->msaaSamples;
    input.viewportExtent = base->presentExtent();
#ifdef BAKEIT_PLATFORM_ANDROID
    // Quest framebuffers are color+depth only (no aux/motion G-buffer slots).
    input.colorAttachmentCount = 1;
#else
    input.colorAttachmentCount = 3;
#endif
    input.descriptorTypes = { descriptorTypes{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_SHADER_STAGE_VERTEX_BIT},
        descriptorTypes{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_SHADER_STAGE_VERTEX_BIT},
        descriptorTypes{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_SHADER_STAGE_FRAGMENT_BIT} };
    pipelines[0]->bufferSize = { sizeof(UniformBufferObject),sizeof(ubo1) };
    pipelines[0]->setupPipeline(input);
    pipelines[0]->createGraphicsPipeline();

    GraphicsPipeline* skinned = new GraphicsPipeline();
    pipelines.push_back(skinned);
    input.skinned = true;
    input.vertexShaderpath = "shaders/World/ObjectSkinnedVS.spv";
    pipelines[1]->bufferSize = { sizeof(SkinnedUniformBufferObject),sizeof(ubo1) };
    pipelines[1]->setupPipeline(input);
    pipelines[1]->createGraphicsPipeline();

    GraphicsPipeline* basic = new GraphicsPipeline();
    pipelines.push_back(basic);
    input.skinned = false;
    input.vertexShaderpath = "shaders/World/ObjectShaderVS.spv";
    input.fragmentShaderpath = "shaders/World/ObjectBasicFS.spv";

    input.descriptorTypes = { descriptorTypes{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_SHADER_STAGE_VERTEX_BIT},
       descriptorTypes{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_SHADER_STAGE_FRAGMENT_BIT} };
    pipelines[2]->bufferSize = { sizeof(UniformBufferObject) ,sizeof(BasicUbo) };
    pipelines[2]->setupPipeline(input);
    pipelines[2]->createGraphicsPipeline();
    float k = 1.;

    GraphicsPipeline* skinnedBasic = new GraphicsPipeline();
    pipelines.push_back(skinnedBasic);
    input.skinned = true;
    input.vertexShaderpath = "shaders/World/ObjectSkinnedVS.spv";
    pipelines[3]->bufferSize = { sizeof(SkinnedUniformBufferObject),sizeof(BasicUbo) };
    pipelines[3]->setupPipeline(input);
    pipelines[3]->createGraphicsPipeline();

    GraphicsPipeline* volumetric = new GraphicsPipeline();
    volumetric->frontFaceCull = 1;
    pipelines.push_back(volumetric);
    input.skinned = false;
    input.vertexShaderpath = "shaders/Sdf/SdfBrickVS.spv";
    input.fragmentShaderpath = "shaders/Sdf/SdfBrickFS.spv";
    input.useInstanceIdAttribute = true;

    // Brick pipeline bindings (order = [UBOs][textures][SBOs]):
    //  0 UBO, 1 BasicUbo (VS reads coarse-LOD toggle .z),
    //  2 fine field sampler, 3 COARSE tile-pool sampler (NEW),
    //  4 bricks, 5 blocks, 6 (counter), 7 blockColors, 8 blockSpatial,
    //  9 coarse block table (VS+FS), 10 coarse dummy bricks (FS placeholder).
    input.descriptorTypes = { descriptorTypes{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_SHADER_STAGE_ALL_GRAPHICS},
         descriptorTypes{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_SHADER_STAGE_ALL_GRAPHICS}, // BasicUbo: VS now reads it too (coarse-LOD toggle in .z)
         descriptorTypes{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_SHADER_STAGE_FRAGMENT_BIT,
},descriptorTypes{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_SHADER_STAGE_FRAGMENT_BIT, // binding 3: coarse tile pool
},descriptorTypes{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_SHADER_STAGE_FRAGMENT_BIT,
} ,descriptorTypes{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_SHADER_STAGE_ALL_GRAPHICS,
},descriptorTypes{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_SHADER_STAGE_FRAGMENT_BIT,
},descriptorTypes{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_SHADER_STAGE_ALL_GRAPHICS,
},descriptorTypes{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_SHADER_STAGE_VERTEX_BIT,
// Coarse-LOD: binding 9 = coarse block table (VS reads the coarse tile index,
// FS walks it for cross-tile coarse normals), binding 10 = coarse dummy bricks.
},descriptorTypes{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_SHADER_STAGE_ALL_GRAPHICS,
},descriptorTypes{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_SHADER_STAGE_FRAGMENT_BIT,
} };
    pipelines[4]->bufferSize = { sizeof(UniformBufferObject),sizeof(BasicUbo) };

    pipelines[4]->setupPipeline(input);
    //pipelines[4]->infoLocal.depthStencil.depthWriteEnable = false;
    pipelines[4]->createGraphicsPipeline();
}
void ScenePipelines::recreate(VkRenderPass renderPass, VkExtent2D extent)
{
    for (GraphicsPipeline*& pipeline : pipelines) {
        pipeline->clean();
        pipeline->recreatePipeline(extent, renderPass);
    }
}


ScenePipelines::~ScenePipelines()
{
    for (GraphicsPipeline*& pipeline : pipelines) {
        delete pipeline;
    }
}
