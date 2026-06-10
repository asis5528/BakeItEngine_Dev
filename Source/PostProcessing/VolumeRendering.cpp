#include "VolumeRendering.h"
#include "Core/Vulkan/VulkanUtils.h"
#include "../Scene/Scene.h"
struct alignas(16) voulUBO {
    float val;

};
struct  CameraUBO {
    alignas(16)glm::mat4 invView;
    alignas(16)glm::mat4 invProj;
    alignas(16)glm::mat4 invProjNoJitter;
    alignas(16)glm::mat4 currViewProj;
    alignas(16)glm::mat4 prevViewProj;
    alignas(16)glm::mat4 currViewProjNoJitter;
    alignas(16)glm::mat4 prevViewProjNoJitter;
    alignas(16) glm::vec3 camPos;
    alignas(16)glm::vec2 resolution;
   float nearPlane;
   float farPlane;
   alignas(16) glm::vec4 brushData;
   alignas(16) glm::vec4 brushMeta; // x: action, y: shape mode (0 cube,1 sphere,2 plane)
   alignas(16) glm::vec4 taaData;   // x,y current jitter, z,w previous jitter
   alignas(16) glm::vec4 taaState;  // x: history valid (0/1)
   alignas(16) glm::vec4 floorBlobShadows[Scene::MaxFloorBlobShadows];
   alignas(16) glm::vec4 floorBlobBounceColors[Scene::MaxFloorBlobShadows];
   alignas(16) glm::vec4 floorBlobMeta; // x: count
    
};

VolumeRendering::VolumeRendering(VulkanBase* base,Scene *scene, Framebuffer* previousFB)
{
	this->base = base;
    this->scene = scene;
    this->previousFB = previousFB;
    // [0] final postprocess output, [1] TAA history buffer.
    Framebuffer* frameBuffer_quad = new Framebuffer(base->vulkandevice, base->swapChain->swapChainExtent.width, base->swapChain->swapChainExtent.height, base->vulkandevice->getMaxUsableSampleCount(), base->swapChain->swapChainImageFormat, utils::findDepthFormat(base->vulkandevice->physicalDevice), previousFB->multiLayerView);
    Framebuffer* frameBuffer_history = new Framebuffer(base->vulkandevice, base->swapChain->swapChainExtent.width, base->swapChain->swapChainExtent.height, base->vulkandevice->getMaxUsableSampleCount(), base->swapChain->swapChainImageFormat, utils::findDepthFormat(base->vulkandevice->physicalDevice), previousFB->multiLayerView);
    framebuffers.push_back(frameBuffer_quad);
    framebuffers.push_back(frameBuffer_history);
    Quad* quad = new Quad(base, framebuffers[0]);
    std::vector<descriptorTypes> VolumeDescriptorTypes = { 
        descriptorTypes{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,VK_SHADER_STAGE_FRAGMENT_BIT},
        descriptorTypes{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_SHADER_STAGE_FRAGMENT_BIT },
        descriptorTypes{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_SHADER_STAGE_FRAGMENT_BIT } ,
        descriptorTypes{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_SHADER_STAGE_FRAGMENT_BIT } ,
        descriptorTypes{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_SHADER_STAGE_FRAGMENT_BIT } ,
        descriptorTypes{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,VK_SHADER_STAGE_FRAGMENT_BIT } ,
    
        descriptorTypes{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_SHADER_STAGE_FRAGMENT_BIT},
        descriptorTypes{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_SHADER_STAGE_FRAGMENT_BIT},
        descriptorTypes{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_SHADER_STAGE_FRAGMENT_BIT},
        descriptorTypes{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_SHADER_STAGE_FRAGMENT_BIT}
    };
    
	volumeUBO = base->vbuffer->createUniformBuffers({ sizeof(CameraUBO)  });

    quad->createGraphicsPipeline("shaders/PostProcessing/VolumePassVS.spv", "shaders/PostProcessing/VolumePassFS.spv",VolumeDescriptorTypes);
    VulkanTexture quadtex;
    quadtex.imageView = previousFB->MultisampledColorImage->imageView;
    quadtex.imageSampler = base->vbuffer->createTextureSampler(1);
    quadtex.imageViewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    quad->textures.push_back(quadtex);
    VulkanTexture quadtex2;
    quadtex2.imageView = previousFB->MultisampledDepthImage->imageView;
    quadtex2.imageSampler = base->vbuffer->createTextureSampler(1);
    quadtex2.imageViewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    quad->textures.push_back(quadtex2);
    VulkanTexture historyTex;
    historyTex.imageView = framebuffers[1]->MultisampledColorImage->imageView;
    historyTex.imageSampler = base->vbuffer->createTextureSampler(1);
    historyTex.imageViewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    quad->textures.push_back(historyTex);
    VulkanTexture auxWorldPosTex;
    auxWorldPosTex.imageView = previousFB->MultisampledAuxColorImage->imageView;
    auxWorldPosTex.imageSampler = base->vbuffer->createTextureSampler(1);
    auxWorldPosTex.imageViewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    quad->textures.push_back(auxWorldPosTex);
    VulkanTexture motionTex;
    motionTex.imageView = previousFB->MultisampledMotionVectorImage->imageView;
    motionTex.imageSampler = base->vbuffer->createTextureSampler(1);
    motionTex.imageViewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    quad->textures.push_back(motionTex);

    quad->shader.ubos = volumeUBO;
   // quad->createdescriptors();
    quads.push_back(quad);

    // Copy current postprocess result into history each frame.
    Quad* copyQuad = new Quad(base, framebuffers[1]);
    std::vector<descriptorTypes> copyDescTypes = {
        descriptorTypes{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT }
    };
    copyQuad->createGraphicsPipeline("shaders/PostProcessing/VolumePassVS.spv", "shaders/PostProcessing/TaaHistoryCopyFS.spv", copyDescTypes);
    VulkanTexture copySrcTex;
    copySrcTex.imageView = framebuffers[0]->MultisampledColorImage->imageView;
    copySrcTex.imageSampler = base->vbuffer->createTextureSampler(1);
    copySrcTex.imageViewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    copyQuad->textures.push_back(copySrcTex);
    quads.push_back(copyQuad);

    glm::mat4 renderProj = scene->proj[0];
    renderProj[1][1] *= -1.0f;
    glm::mat4 renderProjNoJitter = scene->projNoJitter[0];
    renderProjNoJitter[1][1] *= -1.0f;
    prevViewProj = renderProj * scene->view[0];
    prevViewProjNoJitter = renderProjNoJitter * scene->view[0];
    prevJitter = scene->taaJitter;
    base->vbuffer->transitionImageLayout(
        framebuffers[1]->MultisampledColorImage->image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        1,
        previousFB->multiLayerView);

}
void VolumeRendering::createDescriptors() {
    for (auto q : quads) {
		q->createdescriptors();
    }
}
void VolumeRendering::update() {
    static float val = 0.0f;
	CameraUBO camubo;
    glm::mat4 renderProj = scene->proj[0];
    renderProj[1][1] *= -1.0f;
    glm::mat4 renderProjNoJitter = scene->projNoJitter[0];
    renderProjNoJitter[1][1] *= -1.0f;
    
    camubo.invView = glm::inverse(scene->view[0]);
    camubo.invProj = glm::inverse(renderProj);
    camubo.invProjNoJitter = glm::inverse(scene->projNoJitter[0]);
    camubo.currViewProj = renderProj * scene->view[0];
    camubo.prevViewProj = prevViewProj;
    camubo.currViewProjNoJitter = renderProjNoJitter * scene->view[0];
    camubo.prevViewProjNoJitter = prevViewProjNoJitter;
    camubo.camPos = scene->camPos;
    camubo.resolution = glm::vec2(scene->viewport_resolution.x, scene->viewport_resolution.y);
    camubo.nearPlane = scene->camNearPlane;
	camubo.farPlane = scene->camFarPlane;
    camubo.brushData = scene->brushData;
    camubo.brushMeta = glm::vec4(scene->brushAction, float(scene->brushShapeMode), 0.0f, 0.0f);
    camubo.taaData = glm::vec4(scene->taaJitter, prevJitter);
    camubo.taaState = glm::vec4(
        hasTaaHistory ? 1.0f : 0.0f,
        scene->taaEnabled ? 1.0f : 0.0f,
        0.0f,
        0.0f);
    for (int i = 0; i < Scene::MaxFloorBlobShadows; ++i) {
        camubo.floorBlobShadows[i] = scene->floorBlobShadows[i];
        camubo.floorBlobBounceColors[i] = scene->floorBlobBounceColors[i];
    }
    camubo.floorBlobMeta = glm::vec4(float(scene->floorBlobShadowCount), 0.0f, 0.0f, 0.0f);
    prevJitter = scene->taaJitter;
	volumeUBO[0].map(base->device, sizeof(CameraUBO), &camubo);
    prevViewProj = camubo.currViewProj;
    prevViewProjNoJitter = camubo.currViewProjNoJitter;
	//volumeUBO[0].map(base->device, sizeof(CameraUBO), &scene->camera->cameraUBOData);
    //val += 0.0001f;
   // voulUBO vubo;
  //  vubo.val = val;
   // volumeUBO[0].map(base->device, sizeof(voulUBO), &vubo);
}
void VolumeRendering::renderPass(VkCommandBuffer commandBuffer)
{
    framebuffers[0]->beginRenderPass(commandBuffer);
    quads[0]->draw(commandBuffer);
    vkCmdEndRenderPass(commandBuffer);

    framebuffers[1]->beginRenderPass(commandBuffer);
    quads[1]->draw(commandBuffer);
    vkCmdEndRenderPass(commandBuffer);
    if (!hasTaaHistory) {
        hasTaaHistory = true;
    }
}

void VolumeRendering::recreate(VkExtent2D ex)
{
    for (Framebuffer*& framebuffer : framebuffers) {

        framebuffer->clean();
        framebuffer->width = ex.width;
        framebuffer->height = ex.height;
        framebuffer->recreate();
    }
    for (Quad*& quad : quads) {
        quad->graphicsPipeline->clean();
        quad->recreateGraphicsPipeline(ex);
        quad->cleandescriptors();
        //quad->createdescriptors();
    }
    if (!quads.empty()) {
        // Main TAA pass textures
        quads[0]->textures[0].imageView = previousFB->MultisampledColorImage->imageView;
        quads[0]->textures[1].imageView = previousFB->MultisampledDepthImage->imageView;
        quads[0]->textures[2].imageView = framebuffers[1]->MultisampledColorImage->imageView;
        quads[0]->textures[3].imageView = previousFB->MultisampledAuxColorImage->imageView;
        quads[0]->textures[4].imageView = previousFB->MultisampledMotionVectorImage->imageView;
    }
    if (quads.size() > 1) {
        // Copy pass source texture
        quads[1]->textures[0].imageView = framebuffers[0]->MultisampledColorImage->imageView;
    }
    hasTaaHistory = false;
    prevJitter = glm::vec2(0.0f);
    glm::mat4 renderProj = scene->proj[0];
    renderProj[1][1] *= -1.0f;
    glm::mat4 renderProjNoJitter = scene->projNoJitter[0];
    renderProjNoJitter[1][1] *= -1.0f;
    prevViewProj = renderProj * scene->view[0];
    prevViewProjNoJitter = renderProjNoJitter * scene->view[0];
    base->vbuffer->transitionImageLayout(
        framebuffers[1]->MultisampledColorImage->image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        1,
        previousFB->multiLayerView);
}

VolumeRendering::~VolumeRendering()
{
    for (Quad*& quad : quads) {
        delete quad->graphicsPipeline;
        delete quad;
    }
    for (Framebuffer*& fb : framebuffers) {
        delete fb;
    }
}
