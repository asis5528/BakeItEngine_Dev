// BakeIt Quest - Phase 1 skeleton.
//
// Boots OpenXR on the headset, creates the Vulkan instance/device THROUGH
// OpenXR (XR_KHR_vulkan_enable2 - the same contract Source/Core/Vulkan/
// VulkanPlatformHooks.h injects into VulkanBase), runs the XR frame loop and
// clears both eyes to an animated color. If you see the color cycling in the
// headset, every layer below the engine is proven: loader, session, Vulkan
// device, swapchain, frame timing, composition.
//
// Phase 2 replaces the clear with the brick renderer (multiview, one
// arraySize=2 swapchain - matching the engine's view[2]/proj[2] layout).

#include <android_native_app_glue.h>
#include <android/log.h>

// XR_USE_PLATFORM_ANDROID / XR_USE_GRAPHICS_API_VULKAN come from CMakeLists.
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cmath>
#include <cstring>
#include <vector>

#define LOG_TAG "BakeItQuest"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define XR_CHECK(call)                                                  \
    do {                                                                \
        XrResult res_ = (call);                                         \
        if (XR_FAILED(res_)) {                                          \
            LOGE("OpenXR error %d at %s:%d: %s", res_, __FILE__, __LINE__, #call); \
            std::abort();                                               \
        }                                                               \
    } while (0)

#define VK_CHECK(call)                                                  \
    do {                                                                \
        VkResult res_ = (call);                                         \
        if (res_ != VK_SUCCESS) {                                       \
            LOGE("Vulkan error %d at %s:%d: %s", res_, __FILE__, __LINE__, #call); \
            std::abort();                                               \
        }                                                               \
    } while (0)

namespace {

struct App
{
    android_app* androidApp = nullptr;
    bool resumed = false;

    XrInstance xrInstance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    bool sessionRunning = false;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;

    VkInstance vkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    // One swapchain, arraySize = 2 (left/right eye) - same layout the
    // engine's multiview renderer wants in Phase 2.
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int32_t swapchainWidth = 0;
    int32_t swapchainHeight = 0;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    std::vector<XrSwapchainImageVulkan2KHR> swapchainImages;

    float timeSeconds = 0.0f;
};

// ---------------------------------------------------------------- OpenXR ---

void createXrInstance(App& app)
{
    // The Android loader must be initialized with the app context before any
    // other OpenXR call.
    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader)));
    XrLoaderInitInfoAndroidKHR loaderInit{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInit.applicationVM = app.androidApp->activity->vm;
    loaderInit.applicationContext = app.androidApp->activity->clazz;
    XR_CHECK(initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInit)));

    const char* extensions[] = {
        XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
    };

    XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = app.androidApp->activity->vm;
    androidInfo.applicationActivity = app.androidApp->activity->clazz;

    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.next = &androidInfo;
    std::strcpy(createInfo.applicationInfo.applicationName, "BakeItQuest");
    std::strcpy(createInfo.applicationInfo.engineName, "BakeItEngine");
    createInfo.applicationInfo.applicationVersion = 1;
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    createInfo.enabledExtensionCount = 2;
    createInfo.enabledExtensionNames = extensions;
    XR_CHECK(xrCreateInstance(&createInfo, &app.xrInstance));

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(app.xrInstance, &systemInfo, &app.systemId));

    XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
    XR_CHECK(xrGetSystemProperties(app.xrInstance, app.systemId, &props));
    LOGI("OpenXR system: %s", props.systemName);
}

// ---------------------------------------------------------------- Vulkan ---
// Created THROUGH OpenXR: the runtime wraps instance/device creation and
// dictates the physical device. This mirrors VulkanPlatformHooks on desktop.

void createVulkanViaXr(App& app)
{
    // Mandatory before session creation.
    PFN_xrGetVulkanGraphicsRequirements2KHR getRequirements = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(app.xrInstance, "xrGetVulkanGraphicsRequirements2KHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)));
    XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    XR_CHECK(getRequirements(app.xrInstance, app.systemId, &requirements));

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "BakeItQuest";
    appInfo.pEngineName = "BakeItEngine";
    appInfo.apiVersion = VK_API_VERSION_1_1; // multiview is core in 1.1

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &appInfo;

    PFN_xrCreateVulkanInstanceKHR createVkInstance = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(app.xrInstance, "xrCreateVulkanInstanceKHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&createVkInstance)));
    XrVulkanInstanceCreateInfoKHR xrInstanceInfo{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xrInstanceInfo.systemId = app.systemId;
    xrInstanceInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xrInstanceInfo.vulkanCreateInfo = &instanceInfo;
    VkResult vkResult = VK_SUCCESS;
    XR_CHECK(createVkInstance(app.xrInstance, &xrInstanceInfo, &app.vkInstance, &vkResult));
    VK_CHECK(vkResult);

    PFN_xrGetVulkanGraphicsDevice2KHR getDevice = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(app.xrInstance, "xrGetVulkanGraphicsDevice2KHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&getDevice)));
    XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    deviceGetInfo.systemId = app.systemId;
    deviceGetInfo.vulkanInstance = app.vkInstance;
    XR_CHECK(getDevice(app.xrInstance, &deviceGetInfo, &app.physicalDevice));

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(app.physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(app.physicalDevice, &familyCount, families.data());
    for (uint32_t i = 0; i < familyCount; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            app.queueFamily = i;
            break;
        }
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = app.queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceMultiviewFeatures multiview{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    multiview.multiview = VK_TRUE;

    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = &multiview;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;

    PFN_xrCreateVulkanDeviceKHR createVkDevice = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(app.xrInstance, "xrCreateVulkanDeviceKHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&createVkDevice)));
    XrVulkanDeviceCreateInfoKHR xrDeviceInfo{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xrDeviceInfo.systemId = app.systemId;
    xrDeviceInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xrDeviceInfo.vulkanPhysicalDevice = app.physicalDevice;
    xrDeviceInfo.vulkanCreateInfo = &deviceInfo;
    XR_CHECK(createVkDevice(app.xrInstance, &xrDeviceInfo, &app.device, &vkResult));
    VK_CHECK(vkResult);

    vkGetDeviceQueue(app.device, app.queueFamily, 0, &app.queue);

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = app.queueFamily;
    VK_CHECK(vkCreateCommandPool(app.device, &poolInfo, nullptr, &app.commandPool));

    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = app.commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(app.device, &allocInfo, &app.commandBuffer));

    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(app.device, &fenceInfo, nullptr, &app.fence));
}

// --------------------------------------------------- session + swapchain ---

void createSessionAndSwapchain(App& app)
{
    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = app.vkInstance;
    binding.physicalDevice = app.physicalDevice;
    binding.device = app.device;
    binding.queueFamilyIndex = app.queueFamily;
    binding.queueIndex = 0;

    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = app.systemId;
    XR_CHECK(xrCreateSession(app.xrInstance, &sessionInfo, &app.session));

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    XR_CHECK(xrCreateReferenceSpace(app.session, &spaceInfo, &app.localSpace));

    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(
        app.xrInstance, app.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr));
    std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(
        app.xrInstance, app.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, views.data()));
    app.swapchainWidth = static_cast<int32_t>(views[0].recommendedImageRectWidth);
    app.swapchainHeight = static_cast<int32_t>(views[0].recommendedImageRectHeight);
    LOGI("Eye buffer: %dx%d x%u views", app.swapchainWidth, app.swapchainHeight, viewCount);

    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(app.session, 0, &formatCount, nullptr));
    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(app.session, formatCount, &formatCount, formats.data()));
    app.swapchainFormat = static_cast<VkFormat>(formats[0]);
    for (int64_t f : formats) {
        if (f == VK_FORMAT_R8G8B8A8_SRGB) {
            app.swapchainFormat = VK_FORMAT_R8G8B8A8_SRGB;
            break;
        }
    }

    XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swapchainInfo.format = app.swapchainFormat;
    swapchainInfo.sampleCount = 1;
    swapchainInfo.width = static_cast<uint32_t>(app.swapchainWidth);
    swapchainInfo.height = static_cast<uint32_t>(app.swapchainHeight);
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 2; // both eyes in one array image (multiview-ready)
    swapchainInfo.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(app.session, &swapchainInfo, &app.swapchain));

    uint32_t imageCount = 0;
    XR_CHECK(xrEnumerateSwapchainImages(app.swapchain, 0, &imageCount, nullptr));
    app.swapchainImages.resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    XR_CHECK(xrEnumerateSwapchainImages(
        app.swapchain, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(app.swapchainImages.data())));
    LOGI("Swapchain: %u images, format %d", imageCount, app.swapchainFormat);
}

// ------------------------------------------------------------ frame loop ---

void renderClear(App& app, VkImage image)
{
    VK_CHECK(vkResetCommandBuffer(app.commandBuffer, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(app.commandBuffer, &begin));

    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 2;

    VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange = range;
    vkCmdPipelineBarrier(app.commandBuffer,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    // Animated clear so a frozen frame is distinguishable from success.
    VkClearColorValue color{};
    color.float32[0] = 0.5f + 0.5f * std::sin(app.timeSeconds * 0.9f);
    color.float32[1] = 0.5f + 0.5f * std::sin(app.timeSeconds * 0.7f + 2.0f);
    color.float32[2] = 0.5f + 0.5f * std::sin(app.timeSeconds * 1.1f + 4.0f);
    color.float32[3] = 1.0f;
    vkCmdClearColorImage(app.commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);

    VkImageMemoryBarrier toAttachment = toTransfer;
    toAttachment.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toAttachment.dstAccessMask = 0;
    toAttachment.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(app.commandBuffer,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toAttachment);

    VK_CHECK(vkEndCommandBuffer(app.commandBuffer));

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &app.commandBuffer;
    VK_CHECK(vkQueueSubmit(app.queue, 1, &submit, app.fence));
    VK_CHECK(vkWaitForFences(app.device, 1, &app.fence, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(app.device, 1, &app.fence));
}

void renderFrame(App& app)
{
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    XR_CHECK(xrWaitFrame(app.session, &waitInfo, &frameState));

    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    XR_CHECK(xrBeginFrame(app.session, &beginInfo));

    app.timeSeconds = static_cast<float>(frameState.predictedDisplayTime) * 1e-9f;

    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView projViews[2] = {
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    if (frameState.shouldRender) {
        XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = app.localSpace;
        XrViewState viewState{XR_TYPE_VIEW_STATE};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t viewCount = 0;
        XR_CHECK(xrLocateViews(app.session, &locateInfo, &viewState, 2, &viewCount, views));

        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XR_CHECK(xrAcquireSwapchainImage(app.swapchain, &acquireInfo, &imageIndex));
        XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        imageWait.timeout = XR_INFINITE_DURATION;
        XR_CHECK(xrWaitSwapchainImage(app.swapchain, &imageWait));

        renderClear(app, app.swapchainImages[imageIndex].image);

        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        XR_CHECK(xrReleaseSwapchainImage(app.swapchain, &releaseInfo));

        for (uint32_t i = 0; i < 2; ++i) {
            projViews[i].pose = views[i].pose;
            projViews[i].fov = views[i].fov;
            projViews[i].subImage.swapchain = app.swapchain;
            projViews[i].subImage.imageRect.offset = {0, 0};
            projViews[i].subImage.imageRect.extent = {app.swapchainWidth, app.swapchainHeight};
            projViews[i].subImage.imageArrayIndex = i;
        }
        layer.space = app.localSpace;
        layer.viewCount = 2;
        layer.views = projViews;
        endInfo.layerCount = 1;
        endInfo.layers = layers;
    }

    XR_CHECK(xrEndFrame(app.session, &endInfo));
}

// ---------------------------------------------------------------- events ---

void handleSessionStateChange(App& app, const XrEventDataSessionStateChanged& change)
{
    app.sessionState = change.state;
    switch (change.state) {
    case XR_SESSION_STATE_READY: {
        XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
        beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        XR_CHECK(xrBeginSession(app.session, &beginInfo));
        app.sessionRunning = true;
        LOGI("Session running");
        break;
    }
    case XR_SESSION_STATE_STOPPING:
        XR_CHECK(xrEndSession(app.session));
        app.sessionRunning = false;
        LOGI("Session stopped");
        break;
    case XR_SESSION_STATE_EXITING:
    case XR_SESSION_STATE_LOSS_PENDING:
        ANativeActivity_finish(app.androidApp->activity);
        break;
    default:
        break;
    }
}

void pollXrEvents(App& app)
{
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(app.xrInstance, &event) == XR_SUCCESS) {
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
            handleSessionStateChange(app, *reinterpret_cast<XrEventDataSessionStateChanged*>(&event));
            break;
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            ANativeActivity_finish(app.androidApp->activity);
            break;
        default:
            break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void onAppCmd(android_app* androidApp, int32_t cmd)
{
    App& app = *static_cast<App*>(androidApp->userData);
    switch (cmd) {
    case APP_CMD_RESUME: app.resumed = true; break;
    case APP_CMD_PAUSE: app.resumed = false; break;
    default: break;
    }
}

} // namespace

// ----------------------------------------------------------------- entry ---

void android_main(android_app* androidApp)
{
    App app;
    app.androidApp = androidApp;
    androidApp->userData = &app;
    androidApp->onAppCmd = onAppCmd;

    LOGI("BakeItQuest starting");
    createXrInstance(app);
    createVulkanViaXr(app);
    createSessionAndSwapchain(app);

    while (!androidApp->destroyRequested) {
        // Drain Android events; block while paused with no session running.
        for (;;) {
            int events = 0;
            android_poll_source* source = nullptr;
            const int timeoutMs = (!app.resumed && !app.sessionRunning) ? 250 : 0;
            int id = ALooper_pollOnce(timeoutMs, nullptr, &events, reinterpret_cast<void**>(&source));
            if (id < 0) {
                break;
            }
            if (source) {
                source->process(androidApp, source);
            }
        }

        pollXrEvents(app);

        if (app.sessionRunning) {
            renderFrame(app);
        }
    }

    if (app.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(app.device);
    }
    if (app.swapchain != XR_NULL_HANDLE) xrDestroySwapchain(app.swapchain);
    if (app.localSpace != XR_NULL_HANDLE) xrDestroySpace(app.localSpace);
    if (app.session != XR_NULL_HANDLE) xrDestroySession(app.session);
    if (app.fence != VK_NULL_HANDLE) vkDestroyFence(app.device, app.fence, nullptr);
    if (app.commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(app.device, app.commandPool, nullptr);
    if (app.device != VK_NULL_HANDLE) vkDestroyDevice(app.device, nullptr);
    if (app.vkInstance != VK_NULL_HANDLE) vkDestroyInstance(app.vkInstance, nullptr);
    if (app.xrInstance != XR_NULL_HANDLE) xrDestroyInstance(app.xrInstance);
    LOGI("BakeItQuest exiting");
}
