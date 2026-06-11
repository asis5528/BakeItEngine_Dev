#include "PostProcessPass.h"
#include "Core/Vulkan/VulkanUtils.h"
#include "World/Scene.h"
#include "Features/Sdf/Edit/BrushState.h"

namespace
{
// Mirrors IndicatorUbo in shaders/Render/PostPassFS.frag.
struct alignas(16) BrushIndicatorUbo
{
	glm::mat4 invProj;
	glm::mat4 invView;
	glm::vec4 camPos;            // xyz: camera position WS
	glm::vec4 brushCenterRadius; // xyz: brush center WS, w: effective radius
	glm::vec4 brushParams;       // x: shape, y: visible, z: action
	glm::vec4 brushColor;
};
}

PostProcessPass::PostProcessPass(VulkanBase* base, Scene *scene, Framebuffer* previousFB)
{
	this->base = base;
    this->scene = scene;
    this->previousFB = previousFB;
    Framebuffer* outputFB = new Framebuffer(base->vulkandevice, base->swapChain->swapChainExtent.width, base->swapChain->swapChainExtent.height, base->vulkandevice->getMaxUsableSampleCount(), base->swapChain->swapChainImageFormat, utils::findDepthFormat(base->vulkandevice->physicalDevice), previousFB->multiLayerView);
    framebuffers.push_back(outputFB);

    Quad* quad = new Quad(base, framebuffers[0]);
    std::vector<descriptorTypes> passDescTypes = {
        descriptorTypes{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT },
        descriptorTypes{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT },
        descriptorTypes{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT }
    };
    quad->createGraphicsPipeline("shaders/Render/PostPassVS.spv", "shaders/Render/PostPassFS.spv", passDescTypes);

    indicatorUbo = base->vbuffer->createUniformBuffers({ sizeof(BrushIndicatorUbo) });
    BrushIndicatorUbo initial{}; // visible = 0 until the brush is wired
    indicatorUbo[0].map(base->device, sizeof(initial), &initial);
    quad->shader.ubos = indicatorUbo;
    VulkanTexture sceneColorTex;
    sceneColorTex.imageView = previousFB->MultisampledColorImage->imageView;
    sceneColorTex.imageSampler = base->vbuffer->createTextureSampler(1);
    sceneColorTex.imageViewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    quad->textures.push_back(sceneColorTex);

    // Resolved scene depth ("Multisampled*" = the 1-sample resolve target;
    // aliases the base depth image when MSAA is off). The brush indicator is
    // depth-tested against it; read with texelFetch, so the filter mode of
    // the sampler is irrelevant.
    VulkanTexture sceneDepthTex;
    sceneDepthTex.imageView = previousFB->MultisampledDepthImage->imageView;
    sceneDepthTex.imageSampler = base->vbuffer->createTextureSampler(1);
    sceneDepthTex.imageViewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    quad->textures.push_back(sceneDepthTex);
    quads.push_back(quad);
}

void PostProcessPass::createDescriptors() {
    for (auto q : quads) {
		q->createdescriptors();
    }
}

void PostProcessPass::update() {
    BrushIndicatorUbo u{};
    u.invProj = glm::inverse(scene->camera->proj[0]);
    u.invView = glm::inverse(scene->camera->view[0]);
    u.camPos = glm::vec4(scene->camera->position, 1.0f);

    const BrushState* brush = scene->brush;
    const bool visible = brush != nullptr && scene->input != nullptr && !scene->input->cameraMoveMode;
    if (brush != nullptr) {
        // brush->data.xyz is the negated world-space center (compute convention);
        // remove stamps are 15% larger than add stamps, mirror that here.
        const float effectiveRadius = brush->radius * (brush->action < 0.5f ? 1.15f : 1.0f);
        u.brushCenterRadius = glm::vec4(-glm::vec3(brush->data), effectiveRadius);
        u.brushParams = glm::vec4(float(brush->shapeMode), visible ? 1.0f : 0.0f, brush->action, 0.0f);
        u.brushColor = brush->color;
    }
    indicatorUbo[0].map(base->device, sizeof(u), &u);
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
        quads[0]->textures[1].imageView = previousFB->MultisampledDepthImage->imageView;
    }
}

PostProcessPass::~PostProcessPass()
{
    for (UBO& ubo : indicatorUbo) {
        vkDestroyBuffer(base->device, ubo.buffer, nullptr);
        vkFreeMemory(base->device, ubo.bufferMemory, nullptr);
    }
    for (Quad*& quad : quads) {
        delete quad->graphicsPipeline;
        delete quad;
    }
    for (Framebuffer*& fb : framebuffers) {
        delete fb;
    }
}
