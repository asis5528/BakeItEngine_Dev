#include "PostProcessPass.h"
#include "Core/Vulkan/VulkanUtils.h"
#include "World/Scene.h"
#include "Features/Sdf/Edit/BrushState.h"
#include "Features/Sdf/SdfVolume.h"
#ifdef BAKEIT_PLATFORM_ANDROID
#include <android/log.h>
#endif

namespace
{
// Mirrors IndicatorUbo in shaders/Render/PostPassFS.frag. Per-eye matrices:
// desktop fills eye 0 into both slots, the Quest multiview pass indexes them
// with gl_ViewIndex.
struct alignas(16) BrushIndicatorUbo
{
	glm::mat4 invProj[2];        // GL-convention NDC on every platform
	glm::mat4 invView[2];
	glm::vec4 camPos[2];         // xyz: eye position WS
	glm::vec4 brushCenterRadius; // xyz: brush center WS, w: effective radius
	glm::vec4 brushParams;       // x: shape, y: visible, z: action
	glm::vec4 brushColor;
	// On-device HUD (7-segment digits in the shader; desktop uses ImGui and
	// leaves hudA.x = 0). Rows top to bottom: fps, used bricks, atlas usage %,
	// brush radius (x1000), brush smoothness (x10000).
	glm::vec4 hudA; // x: show, y: fps, z: used bricks, w: atlas usage %
	glm::vec4 hudB; // x: radius*1000, y: smoothness*10000
};
}

PostProcessPass::PostProcessPass(VulkanBase* base, Scene *scene, Framebuffer* previousFB)
{
	this->base = base;
    this->scene = scene;
    this->previousFB = previousFB;
    // Inherit the scene FB's layer/multiview setup so the Quest path renders
    // both eyes of the post pass in one go.
    Framebuffer* outputFB = new Framebuffer(base->vulkandevice, base->presentExtent().width, base->presentExtent().height, base->vulkandevice->getMaxUsableSampleCount(), base->presentFormat(), utils::findDepthFormat(base->vulkandevice->physicalDevice), previousFB->multiLayerView, previousFB->multiviewEnable);
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
    for (int e = 0; e < 2; ++e) {
#ifdef BAKEIT_PLATFORM_ANDROID
        // Quest camera matrices are per-eye and the proj is Vulkan-convention
        // (y down); the shader's NDC math is GL-style on every platform, so
        // convert: proj_gl = diag(1,-1,1,1) * proj_vk.
        glm::mat4 flip(1.0f);
        flip[1][1] = -1.0f;
        u.invProj[e] = glm::inverse(flip * scene->camera->proj[e]);
        u.invView[e] = glm::inverse(scene->camera->view[e]);
        u.camPos[e] = glm::vec4(glm::vec3(u.invView[e][3]), 1.0f); // eye position WS
#else
        u.invProj[e] = glm::inverse(scene->camera->proj[0]);
        u.invView[e] = glm::inverse(scene->camera->view[0]);
        u.camPos[e] = glm::vec4(scene->camera->position, 1.0f);
#endif
    }

    const BrushState* brush = scene->brush;
    // No routed Input on Quest (null) - the brush is always visible there;
    // desktop hides it while the camera is being flown.
    const bool visible = brush != nullptr && (scene->input == nullptr || !scene->input->cameraMoveMode);
    if (brush != nullptr) {
        // brush->data.xyz is the negated world-space center (compute convention);
        // remove stamps are 15% larger than add stamps, mirror that here.
        const float effectiveRadius = brush->radius * (brush->action < 0.5f ? 1.15f : 1.0f);
        u.brushCenterRadius = glm::vec4(-glm::vec3(brush->data), effectiveRadius);
        u.brushParams = glm::vec4(float(brush->shapeMode), visible ? 1.0f : 0.0f, brush->action, 0.0f);
        u.brushColor = brush->color;
    }

#ifdef BAKEIT_PLATFORM_ANDROID
    // On-device stats HUD; desktop shows these in the ImGui panels instead.
    u.hudA.x = 1.0f;
    u.hudA.y = scene->fps;
    if (scene->sdfStats != nullptr) {
        u.hudA.z = float(scene->sdfStats->usedBricks);
        u.hudA.w = scene->sdfStats->usage * 100.0f;
    }
    if (brush != nullptr) {
        u.hudB.x = brush->radius * 1000.0f;
        u.hudB.y = brush->smoothness * 10000.0f;
    }
    {
        static int hudLog = 0;
        if (++hudLog >= 144) {
            hudLog = 0;
            __android_log_print(ANDROID_LOG_INFO, "BakeItQuest",
                                "hud: show=%.0f fps=%.0f bricks=%.0f uboSize=%zu",
                                double(u.hudA.x), double(u.hudA.y), double(u.hudA.z), sizeof(u));
        }
    }
#endif
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
