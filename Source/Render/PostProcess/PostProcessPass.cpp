#include "PostProcessPass.h"
#include "Core/Vulkan/VulkanUtils.h"
#include "Scene/Scene.h"

PostProcessPass::PostProcessPass(VulkanBase* base, Scene *scene, Framebuffer* previousFB)
{
	this->base = base;
    this->scene = scene;
    this->previousFB = previousFB;
    Framebuffer* outputFB = new Framebuffer(base->vulkandevice, base->swapChain->swapChainExtent.width, base->swapChain->swapChainExtent.height, base->vulkandevice->getMaxUsableSampleCount(), base->swapChain->swapChainImageFormat, utils::findDepthFormat(base->vulkandevice->physicalDevice), previousFB->multiLayerView);
    framebuffers.push_back(outputFB);

    Quad* quad = new Quad(base, framebuffers[0]);
    std::vector<descriptorTypes> passDescTypes = {
        descriptorTypes{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT }
    };
    quad->createGraphicsPipeline("shaders/Render/PostPassVS.spv", "shaders/Render/PostPassFS.spv", passDescTypes);
    VulkanTexture sceneColorTex;
    sceneColorTex.imageView = previousFB->MultisampledColorImage->imageView;
    sceneColorTex.imageSampler = base->vbuffer->createTextureSampler(1);
    sceneColorTex.imageViewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    quad->textures.push_back(sceneColorTex);
    quads.push_back(quad);
}

void PostProcessPass::createDescriptors() {
    for (auto q : quads) {
		q->createdescriptors();
    }
}

void PostProcessPass::update() {
    // no per-frame state for the passthrough
}

void PostProcessPass::renderPass(VkCommandBuffer commandBuffer)
{
    framebuffers[0]->beginRenderPass(commandBuffer);
    quads[0]->draw(commandBuffer);
    vkCmdEndRenderPass(commandBuffer);
}

void PostProcessPass::recreate(VkExtent2D ex)
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
    }
    if (!quads.empty()) {
        quads[0]->textures[0].imageView = previousFB->MultisampledColorImage->imageView;
    }
}

PostProcessPass::~PostProcessPass()
{
    for (Quad*& quad : quads) {
        delete quad->graphicsPipeline;
        delete quad;
    }
    for (Framebuffer*& fb : framebuffers) {
        delete fb;
    }
}
