#include "Framebuffer.h"
#include "VulkanUtils.h"
#include <stdexcept>
#include <vector>
//REMEMBER TO NO APPLY MULTISAMPLED COLOR FOR NO MSAA SAMPLES FRAMEBUFFERS FOR OPTIMIZATION
Framebuffer::Framebuffer(VulkanDevice *vulkandevice , uint32_t width, uint32_t height, VkSampleCountFlagBits msaaSamples, VkFormat colorFormat, VkFormat depthFormat,int MultiLayerView, bool multiviewEnable) {
	this->width = width;
	this->height = height;
    this->vulkandevice = vulkandevice;
    this->multiLayerView = MultiLayerView;
    this->multiviewEnable = multiviewEnable && MultiLayerView > 1;
   // this->device = device;
    //this->physicalDevice = physicalDevice;
    this->msaaSamples = msaaSamples;
    this->colorFormat = colorFormat;
    this->depthFormat = depthFormat;
    recreate();
}
Framebuffer::~Framebuffer()
{
    clean();
}
void Framebuffer::clean() {
    if (displayImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(vulkandevice->device, displayImageView, nullptr);
        displayImageView = VK_NULL_HANDLE;
    }
    ColorImage->destroy();
    AuxColorImage->destroy();
    MotionVectorImage->destroy();
    DepthImage->destroy();
    // In 1-sample mode the Multisampled* pointers alias the base images
    // (no resolve targets exist) - avoid destroying them twice.
    if (MultisampledColorImage != ColorImage) {
        MultisampledColorImage->destroy();
        MultisampledAuxColorImage->destroy();
        MultisampledMotionVectorImage->destroy();
        MultisampledDepthImage->destroy();
    }
    vkDestroyFramebuffer(vulkandevice->device, framebuffer, nullptr);
    vkDestroyRenderPass(vulkandevice->device, renderPass, nullptr);
}
void Framebuffer::recreate() {
    createRenderPass();
    createFrameBuffer();
}
void Framebuffer::createRenderPass() {
      
    /*
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = colorFormat;
    colorAttachment.samples = msaaSamples;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = depthFormat;
    depthAttachment.samples = msaaSamples;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    
    VkAttachmentDescription colorAttachmentResolve{};
    colorAttachmentResolve.format = colorFormat;
    colorAttachmentResolve.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachmentResolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachmentResolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachmentResolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachmentResolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachmentResolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachmentResolve.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription depthAttachmentResolve{};
    depthAttachmentResolve.format = depthFormat;
    depthAttachmentResolve.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachmentResolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachmentResolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachmentResolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachmentResolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachmentResolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachmentResolve.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;


    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = 1;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorAttachmentResolveRef{};
    colorAttachmentResolveRef.attachment = 2;
    colorAttachmentResolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference2 depthAttachmentResolveRef{};
    depthAttachmentResolveRef.attachment = 3;
    depthAttachmentResolveRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescriptionDepthStencilResolve st;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;
    subpass.pDepthStencilAttachment = &depthAttachmentRef;
    subpass.pResolveAttachments = &colorAttachmentResolveRef;

    VkSubpassDescriptionDepthStencilResolve DSR;
    DSR.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
    DSR.pNext = nullptr;
    DSR.depthResolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    DSR.stencilResolveMode = VK_RESOLVE_MODE_NONE;
    DSR.pDepthStencilResolveAttachment = &depthAttachmentResolveRef;

    
    std::array<VkSubpassDependency, 2> dependencies;

    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;


    std::array<VkAttachmentDescription, 3> attachments = { colorAttachment, depthAttachment, colorAttachmentResolve };
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = dependencies.size();
    renderPassInfo.pDependencies = dependencies.data();
    if(multiLayerView>1){
        const uint32_t viewMask = 0b00000011;

       
        const uint32_t correlationMask = 0b00000011;

        VkRenderPassMultiviewCreateInfo renderPassMultiviewCI{};
        renderPassMultiviewCI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
        renderPassMultiviewCI.subpassCount = 1;
        renderPassMultiviewCI.pViewMasks = &viewMask;
        renderPassMultiviewCI.correlationMaskCount = 1;
        renderPassMultiviewCI.pCorrelationMasks = &correlationMask;

     //   renderPassInfo.pNext = &renderPassMultiviewCI;
    }
    if (vkCreateRenderPass(vulkandevice->device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
        throw std::runtime_error("failed to create render pass!");
    }
    */
    VkAttachmentDescription2 colorAttachment{}; 
    colorAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    colorAttachment.format = colorFormat;
    colorAttachment.samples = msaaSamples;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    colorAttachment.pNext = nullptr;
    

    VkAttachmentDescription2 auxColorAttachment{};
    auxColorAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    auxColorAttachment.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    auxColorAttachment.samples = msaaSamples;
    auxColorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    auxColorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    auxColorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    auxColorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    auxColorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    auxColorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    auxColorAttachment.pNext = nullptr;

    VkAttachmentDescription2 motionAttachment{};
    motionAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    motionAttachment.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    motionAttachment.samples = msaaSamples;
    motionAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    motionAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    motionAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    motionAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    motionAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    motionAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    motionAttachment.pNext = nullptr;

    VkAttachmentDescription2 depthAttachment{}; 
    depthAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    depthAttachment.format = depthFormat;
    depthAttachment.samples = msaaSamples;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depthAttachment.pNext = nullptr;
    
    VkAttachmentDescription2 colorAttachmentResolve{};
    colorAttachmentResolve.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    colorAttachmentResolve.format = colorFormat;
    colorAttachmentResolve.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachmentResolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachmentResolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachmentResolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachmentResolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachmentResolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachmentResolve.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    colorAttachmentResolve.pNext = nullptr;

    VkAttachmentDescription2 auxColorAttachmentResolve{};
    auxColorAttachmentResolve.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    auxColorAttachmentResolve.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    auxColorAttachmentResolve.samples = VK_SAMPLE_COUNT_1_BIT;
    auxColorAttachmentResolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    auxColorAttachmentResolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    auxColorAttachmentResolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    auxColorAttachmentResolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    auxColorAttachmentResolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    auxColorAttachmentResolve.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    auxColorAttachmentResolve.pNext = nullptr;

    VkAttachmentDescription2 motionAttachmentResolve{};
    motionAttachmentResolve.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    motionAttachmentResolve.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    motionAttachmentResolve.samples = VK_SAMPLE_COUNT_1_BIT;
    motionAttachmentResolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    motionAttachmentResolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    motionAttachmentResolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    motionAttachmentResolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    motionAttachmentResolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    motionAttachmentResolve.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    motionAttachmentResolve.pNext = nullptr;

    VkAttachmentDescription2 depthAttachmentResolve{};
    depthAttachmentResolve.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
    depthAttachmentResolve.format = depthFormat;
    depthAttachmentResolve.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachmentResolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachmentResolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachmentResolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachmentResolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachmentResolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachmentResolve.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depthAttachmentResolve.pNext = nullptr;

    VkAttachmentReference2 colorAttachmentRef{};
    colorAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachmentRef.pNext = nullptr;

    VkAttachmentReference2 auxColorAttachmentRef{};
    auxColorAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
    auxColorAttachmentRef.attachment = 1;
    auxColorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    auxColorAttachmentRef.pNext = nullptr;

    VkAttachmentReference2 motionAttachmentRef{};
    motionAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
    motionAttachmentRef.attachment = 2;
    motionAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    motionAttachmentRef.pNext = nullptr;

    VkAttachmentReference2 depthAttachmentRef{};
    depthAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
    depthAttachmentRef.attachment = 3;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachmentRef.pNext = nullptr;

    VkAttachmentReference2 colorAttachmentResolveRef{};
    colorAttachmentResolveRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
    colorAttachmentResolveRef.attachment = 4;
    colorAttachmentResolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachmentResolveRef.pNext = nullptr;

    VkAttachmentReference2 auxColorAttachmentResolveRef{};
    auxColorAttachmentResolveRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
    auxColorAttachmentResolveRef.attachment = 5;
    auxColorAttachmentResolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    auxColorAttachmentResolveRef.pNext = nullptr;

    VkAttachmentReference2 motionAttachmentResolveRef{};
    motionAttachmentResolveRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
    motionAttachmentResolveRef.attachment = 6;
    motionAttachmentResolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    motionAttachmentResolveRef.pNext = nullptr;

    VkAttachmentReference2 depthAttachmentResolveRef{};
    depthAttachmentResolveRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
    depthAttachmentResolveRef.attachment = 7;
    depthAttachmentResolveRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachmentResolveRef.pNext = nullptr;

    // Resolve attachments are only legal when the color/depth attachments are
    // actually multisampled; in 1-sample mode we render straight into the
    // base images (createFrameBuffer aliases the Multisampled* pointers).
    const bool msaaEnabled = (msaaSamples != VK_SAMPLE_COUNT_1_BIT);

    VkSubpassDescription2 subpass{};
    subpass.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    std::array<VkAttachmentReference2, 3> colorAttachments = { colorAttachmentRef, auxColorAttachmentRef, motionAttachmentRef };
    std::array<VkAttachmentReference2, 3> colorResolveAttachments = { colorAttachmentResolveRef, auxColorAttachmentResolveRef, motionAttachmentResolveRef };
    subpass.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
    subpass.pColorAttachments = colorAttachments.data();
    subpass.pDepthStencilAttachment = &depthAttachmentRef;
    subpass.pResolveAttachments = msaaEnabled ? colorResolveAttachments.data() : nullptr;
    subpass.pInputAttachments = nullptr;
    subpass.preserveAttachmentCount = 0;
    subpass.inputAttachmentCount = 0;
    subpass.pPreserveAttachments = nullptr;
    subpass.pNext = nullptr;

    VkSubpassDescriptionDepthStencilResolve DSR;
    DSR.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
    DSR.pNext = nullptr;
    DSR.depthResolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
    DSR.stencilResolveMode = VK_RESOLVE_MODE_NONE;
    DSR.pDepthStencilResolveAttachment = &depthAttachmentResolveRef;
    if (msaaEnabled) {
        subpass.pNext = &DSR;
    }

    
    std::array<VkSubpassDependency2, 2> dependencies;
    dependencies[0].sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
    dependencies[0].pNext = nullptr;
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    dependencies[0].viewOffset = 0;

    dependencies[1].srcSubpass = 0;
    dependencies[1].pNext = nullptr;
    dependencies[1].sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    // Depth stages/access included so sampling the depth attachment after the
    // pass (brush indicator depth test) is ordered even in 1-sample mode,
    // where there is no resolve op (resolves count as color-output writes).
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    dependencies[1].viewOffset = 0;

    std::vector<VkAttachmentDescription2> attachments = {
        colorAttachment,
        auxColorAttachment,
        motionAttachment,
        depthAttachment
    };
    if (msaaEnabled) {
        attachments.push_back(colorAttachmentResolve);
        attachments.push_back(auxColorAttachmentResolve);
        attachments.push_back(motionAttachmentResolve);
        attachments.push_back(depthAttachmentResolve);
    }

    // True multiview (renderpass2 style): subpass.viewMask 0b11 broadcasts
    // each draw to both array layers (gl_ViewIndex = eye), the framebuffer is
    // then created with layers = 1. The old VkRenderPassMultiviewCreateInfo
    // was renderpass1-only and silently ignored here - desktop keeps that
    // legacy single-view behavior (multiviewEnable false, only layer 0 drawn).
    const uint32_t viewMask = 0b11;
    if (multiviewEnable) {
        subpass.viewMask = viewMask;
    }

    VkRenderPassCreateInfo2 renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = dependencies.size();
    renderPassInfo.pDependencies = dependencies.data();
    renderPassInfo.pNext = nullptr;
    if (multiviewEnable) {
        renderPassInfo.correlatedViewMaskCount = 1;
        renderPassInfo.pCorrelatedViewMasks = &viewMask;
    }

#ifdef BAKEIT_PLATFORM_ANDROID
    // vkCreateRenderPass2 is core 1.2; Quest's driver/loader is Vulkan 1.1,
    // so resolve the identical-signature KHR extension entry point instead.
    static PFN_vkCreateRenderPass2KHR pfnCreateRenderPass2 = nullptr;
    if (!pfnCreateRenderPass2) {
        pfnCreateRenderPass2 = reinterpret_cast<PFN_vkCreateRenderPass2KHR>(
            vkGetDeviceProcAddr(vulkandevice->device, "vkCreateRenderPass2KHR"));
        if (!pfnCreateRenderPass2) {
            pfnCreateRenderPass2 = reinterpret_cast<PFN_vkCreateRenderPass2KHR>(
                vkGetDeviceProcAddr(vulkandevice->device, "vkCreateRenderPass2"));
        }
    }
    if (!pfnCreateRenderPass2 || pfnCreateRenderPass2(vulkandevice->device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
        throw std::runtime_error("failed to create render pass!");
    }
#else
    if (vkCreateRenderPass2(vulkandevice->device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
        throw std::runtime_error("failed to create render pass!");
    }
#endif
    
    
}

void Framebuffer::createFrameBuffer() {
    //Coz of multiview for vr,layer is assigned 2 and imageview type is a 2D array...change the code to make compatible for anytype of framebuffer textures in future
   
    //int multiviewLayer = 2;
    VkImageViewType viewType;
    if (multiLayerView >1) {
        viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    }
    else {
        viewType = VK_IMAGE_VIEW_TYPE_2D;
    }
    ColorImage = new VulkanImage(vulkandevice->device, vulkandevice->physicalDevice);
    // TRANSFER_SRC: the Quest path copies the rendered color layers into the
    // XrSwapchain image after the pass (Renderer::recordCommandBuffers).
    ColorImage->createImage(width, height,1, VK_IMAGE_TYPE_2D,1, this->msaaSamples, colorFormat, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT , VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, multiLayerView);
    ColorImage->createImageView(colorFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1, viewType, multiLayerView);

    AuxColorImage = new VulkanImage(vulkandevice->device, vulkandevice->physicalDevice);
    AuxColorImage->createImage(width, height, 1, VK_IMAGE_TYPE_2D, 1, this->msaaSamples, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, multiLayerView);
    AuxColorImage->createImageView(VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 1, viewType, multiLayerView);

    MotionVectorImage = new VulkanImage(vulkandevice->device, vulkandevice->physicalDevice);
    MotionVectorImage->createImage(width, height, 1, VK_IMAGE_TYPE_2D, 1, this->msaaSamples, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, multiLayerView);
    MotionVectorImage->createImageView(VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 1, viewType, multiLayerView);

    DepthImage = new VulkanImage(vulkandevice->device, vulkandevice->physicalDevice);
    DepthImage->createImage(width, height,1, VK_IMAGE_TYPE_2D, 1,this->msaaSamples, depthFormat, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, multiLayerView);
    DepthImage->createImageView(depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 1, viewType, multiLayerView);

    const bool msaaEnabled = (msaaSamples != VK_SAMPLE_COUNT_1_BIT);
    if (msaaEnabled) {
        // 1-sample resolve targets; consumers sample these. (Misnamed for
        // historical reasons: "Multisampled*" here means "the resolved image".)
        MultisampledColorImage = new VulkanImage(vulkandevice->device, vulkandevice->physicalDevice);
        MultisampledColorImage->createImage(width, height,1, VK_IMAGE_TYPE_2D, 1, VK_SAMPLE_COUNT_1_BIT, colorFormat, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT , VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, multiLayerView);
        MultisampledColorImage->createImageView(colorFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1, viewType, multiLayerView);

        MultisampledAuxColorImage = new VulkanImage(vulkandevice->device, vulkandevice->physicalDevice);
        MultisampledAuxColorImage->createImage(width, height, 1, VK_IMAGE_TYPE_2D, 1, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, multiLayerView);
        MultisampledAuxColorImage->createImageView(VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 1, viewType, multiLayerView);

        MultisampledMotionVectorImage = new VulkanImage(vulkandevice->device, vulkandevice->physicalDevice);
        MultisampledMotionVectorImage->createImage(width, height, 1, VK_IMAGE_TYPE_2D, 1, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, multiLayerView);
        MultisampledMotionVectorImage->createImageView(VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 1, viewType, multiLayerView);

        MultisampledDepthImage = new VulkanImage(vulkandevice->device, vulkandevice->physicalDevice);
        MultisampledDepthImage->createImage(width, height, 1, VK_IMAGE_TYPE_2D, 1, VK_SAMPLE_COUNT_1_BIT, depthFormat, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, multiLayerView);
        MultisampledDepthImage->createImageView(depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 1, viewType, multiLayerView);
    }
    else {
        // No resolve in 1-sample mode: consumers sample the base attachments
        // directly. Alias the pointers so the rest of the engine is unchanged.
        MultisampledColorImage = ColorImage;
        MultisampledAuxColorImage = AuxColorImage;
        MultisampledMotionVectorImage = MotionVectorImage;
        MultisampledDepthImage = DepthImage;
    }

    std::vector<VkImageView> attachments = {
        ColorImage->imageView,
        AuxColorImage->imageView,
        MotionVectorImage->imageView,
        DepthImage->imageView
    };
    if (msaaEnabled) {
        attachments.push_back(MultisampledColorImage->imageView);
        attachments.push_back(MultisampledAuxColorImage->imageView);
        attachments.push_back(MultisampledMotionVectorImage->imageView);
        attachments.push_back(MultisampledDepthImage->imageView);
    }
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderPass;
    framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    framebufferInfo.pAttachments = attachments.data();
    framebufferInfo.width = width;
    framebufferInfo.height = height;
    // With multiview the spec requires layers = 1 (the viewMask spreads the
    // draw across the attachment array layers instead).
    framebufferInfo.layers = multiviewEnable ? 1 : multiLayerView;

    if (vkCreateFramebuffer(vulkandevice->device, &framebufferInfo, nullptr, &framebuffer) != VK_SUCCESS) {
        throw std::runtime_error("failed to create framebuffer!");
    }

    // Single-layer 2D view over the display image for plain-sampler2D
    // consumers (the ImGui viewport texture).
    VkImageViewCreateInfo displayViewInfo{};
    displayViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    displayViewInfo.image = MultisampledColorImage->image;
    displayViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    displayViewInfo.format = colorFormat;
    displayViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    displayViewInfo.subresourceRange.baseMipLevel = 0;
    displayViewInfo.subresourceRange.levelCount = 1;
    displayViewInfo.subresourceRange.baseArrayLayer = 0;
    displayViewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(vulkandevice->device, &displayViewInfo, nullptr, &displayImageView) != VK_SUCCESS) {
        throw std::runtime_error("failed to create display image view!");
    }
}
void Framebuffer::beginRenderPass(VkCommandBuffer commandBuffer) {
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = this->renderPass;
    renderPassInfo.framebuffer = this->framebuffer;
    // renderPassInfo.framebuffer = swapChainFramebuffers[i];
    renderPassInfo.renderArea.offset = { 0,0 };
    VkExtent2D resolution;
    resolution.width = width;
    resolution.height = height;
    renderPassInfo.renderArea.extent = resolution;

    std::array<VkClearValue, 4> clearValues{};
    clearValues[0].color = { {1.f, 1.f, 1.f, 1.f} };
    clearValues[1].color = { {0.f, 0.f, 0.f, 0.f} };
    clearValues[2].color = { {0.f, 0.f, 0.f, 0.f} };
    clearValues[3].depthStencil = { 1.0f, 0 };


    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    
}
