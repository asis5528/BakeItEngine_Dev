// BakeIt Quest - Phase 3: SDF sculpting with the Touch controllers.
//
// OpenXR owns the frame loop and the Vulkan instance/device (created through
// XR_KHR_vulkan_enable2 via VulkanPlatformHooks - the seam VulkanBase
// exposes). The engine core (Core/World/Render/Features) is compiled in
// unchanged: Renderer records the offscreen scene pass with true multiview
// (both eyes in one pass) and ends each command buffer with a copy into the
// matching XrSwapchain image.
//
// Brushing: QuestBrushInput implements the engine's BrushInputSource from
// OpenXR actions - the same contract DesktopBrushInput fulfills with the
// mouse, feeding the identical SdfEditor stroke path.
//   Right controller: aim ray = brush wand, trigger = draw, grip = erase,
//                     thumbstick X/Y = radius / brush distance,
//                     A = shape toggle, B = emissive toggle.
//   Left controller:  thumbstick X/Y = smoothness / world zoom,
//                     trigger = recolor modifier, grip (hold) = block
//                     wireframe view, X = clear scene.
//
// Shader .spv files ship as APK assets, are extracted to internal storage at
// startup, and chdir() makes the engine's relative "shaders/..." paths work
// untouched.

#include <android_native_app_glue.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <jni.h> // runtime permission request (USE_SCENE) via the NativeActivity
#include <sys/stat.h>
#include <unistd.h>
#include "Core/Resources/VulkanImage.h"   // depth-viz target image
#include "Core/Pipelines/ComputePipeline.h" // depth linearize compute

// Engine headers first: Platform/Platform.h keys off __ANDROID__, VulkanBase
// pulls <vulkan/vulkan.h> (no GLFW on this platform).
#include "Platform/Platform.h"
#include "Core/Vulkan/VulkanBase.h"
#include "Core/Resources/ResourceManager.h"
#include "World/Scene.h"
#include "World/Camera/Camera.h"
#include "Render/Renderer.h"
#include "Features/Sdf/Edit/SdfEditor.h"
#include "Features/Sdf/Edit/BrushInputSource.h"
#include "EngineContext.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

// XR_USE_PLATFORM_ANDROID / XR_USE_GRAPHICS_API_VULKAN come from CMakeLists.
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
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

// Engine world -> XR LOCAL space. LOCAL's origin sits at the headset's pose
// when the session starts (eye level, not the floor!), so the ~1m SDF chunk
// around the world origin goes slightly below eye height and ~0.75m forward.
const glm::vec3 kWorldToStage(0.0f, -0.15f, -0.75f);

struct Engine; // fwd; set once the engine is up, for the diagnostics log
Engine* gEngineForDiag = nullptr;

// OpenXR action handles + the per-frame input snapshot they produce.
// syncXrInput() refreshes the snapshot once per frame (after xrWaitFrame, at
// the predicted display time); QuestBrushInput::poll() consumes it.
struct XrInput
{
    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction aimPose = XR_NULL_HANDLE;       // right aim pose -> brush ray
    XrAction drawTrigger = XR_NULL_HANDLE;   // right trigger (float)
    XrAction eraseSqueeze = XR_NULL_HANDLE;  // right grip (float)
    XrAction stickRight = XR_NULL_HANDLE;    // right thumbstick (vec2)
    XrAction stickLeft = XR_NULL_HANDLE;     // left thumbstick (vec2)
    XrAction shapeButton = XR_NULL_HANDLE;   // A (bool)
    XrAction emissiveButton = XR_NULL_HANDLE;// B (bool)
    XrAction recolorTrigger = XR_NULL_HANDLE;// left trigger (float)
    XrAction wireframeSqueeze = XR_NULL_HANDLE; // left grip (float) - hold for block wireframe
    XrAction clearButton = XR_NULL_HANDLE;   // X (bool)
    XrAction depthModeButton = XR_NULL_HANDLE; // Y (bool) - cycle depth viz mode
    XrSpace aimSpace = XR_NULL_HANDLE;

    // Per-frame snapshot (stage/LOCAL space pose).
    bool aimValid = false;
    glm::vec3 aimPos = glm::vec3(0.0f);
    glm::quat aimRot = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    bool drawDown = false;
    bool eraseDown = false;
    bool recolorDown = false;
    bool wireframeDown = false; // left grip held
    bool shapePressed = false;    // this-frame edges
    bool emissivePressed = false;
    bool clearPressed = false;
    bool depthModePressed = false; // Y edge - cycle depth viz
    glm::vec2 stickR = glm::vec2(0.0f);
    glm::vec2 stickL = glm::vec2(0.0f);
};

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

    // Vulkan handles mirror VulkanBase's (engine owns them; these are for the
    // XR binding and frame submission).
    VkInstance vkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;

    // One swapchain, arraySize = 2 (left/right eye) - the engine renders both
    // layers in one multiview pass and copies into these images.
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int32_t swapchainWidth = 0;
    int32_t swapchainHeight = 0;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    std::vector<XrSwapchainImageVulkan2KHR> swapchainImages;
    std::vector<VkFence> frameFences; // per swapchain image, created signaled
    VkFence lastSubmittedFence = VK_NULL_HANDLE;

    bool haveBaseTime = false;
    XrTime baseTime = 0;

    XrInput input;
    float lastFrameTimeSec = -1.0f;
    float worldScale = 1.0f; // left stick Y zooms the sculpt volume

    // XR_FB_passthrough: the real world as the background (the projection
    // layer alpha-blends over it; scene clear alpha is 0 on Quest).
    bool passthroughSupported = false;
    XrPassthroughFB passthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB passthroughLayer = XR_NULL_HANDLE;

    // XR_FB_foveation: fixed foveated rendering (shade the lens periphery at a
    // lower rate). Applied to the swapchain after creation; no render changes.
    bool foveationSupported = false;

    // XR_META_environment_depth (Quest 3): real-world depth map. The provider
    // generates a 2-layer depth texture array (one per eye); we acquire the latest
    // each frame. Step 1 just acquires + logs to verify the pipeline; the GPU
    // image hand-off to the post-process is step 2.
    bool depthSupported = false;
    bool depthPendingPermission = false; // requested USE_SCENE, waiting for the grant
    XrEnvironmentDepthProviderMETA depthProvider = XR_NULL_HANDLE;
    XrEnvironmentDepthSwapchainMETA depthSwapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageVulkan2KHR> depthImages;
    uint32_t depthWidth = 0;
    uint32_t depthHeight = 0;
    // Extension entry points (loaded via xrGetInstanceProcAddr).
    PFN_xrCreateEnvironmentDepthProviderMETA pfnCreateDepthProvider = nullptr;
    PFN_xrStartEnvironmentDepthProviderMETA pfnStartDepthProvider = nullptr;
    PFN_xrCreateEnvironmentDepthSwapchainMETA pfnCreateDepthSwapchain = nullptr;
    PFN_xrGetEnvironmentDepthSwapchainStateMETA pfnGetDepthSwapchainState = nullptr;
    PFN_xrEnumerateEnvironmentDepthSwapchainImagesMETA pfnEnumDepthSwapchainImages = nullptr;
    PFN_xrAcquireEnvironmentDepthImageMETA pfnAcquireDepthImage = nullptr;

    // Depth visualization (step 2): a per-frame compute linearizes the acquired depth
    // into a stable R16F 2-layer array the post pass samples. One compute instance per
    // depth image (submit the one for that frame's swapchainIndex) -> no descriptor churn.
    bool depthVizReady = false;
    bool depthBoundToPost = false;
    int  depthDisplayMode = 0;                  // overlay: 0 off, 1 depth, 2 world pos, 3 normal (Y cycles)
    VulkanImage* depthDisplayImage = nullptr;   // RGBA16F 2-layer storage+sampled viz target
    VulkanTexture depthDisplayTex{};
    std::vector<VkImageView> depthSrcViews;     // depth-aspect array view per XR image
    std::vector<VkSampler> depthSrcSamplers;
    std::vector<ComputePipeline*> depthVizComp; // one per XR image
    std::vector<UBO> depthParamsUbo;            // depth-viz params (see DepthVizParams)
};

// Per-frame uniforms for QuestDepthVizCS: depth linearization + per-eye world-space
// reconstruction (for world-position and normal viz). std140-matched to the shader.
struct DepthVizParams
{
    float nearZ; float farZ; int width; int height;
    int vizMode; int _pad0; int _pad1; int _pad2;   // 0 depth, 1 world pos, 2 normal
    glm::vec4 fovTan[2];                             // tanL, tanR, tanD, tanU per eye/layer
    glm::mat4 camToWorld[2];                         // depth-cam -> world (XR LOCAL) per eye
};

// The engine, wired the same way main.cpp wires it on desktop (minus
// ImGui/Assimp/PostProcess - see Source/Platform/Platform.h).
struct Engine
{
    VulkanBase* base = nullptr;
    Scene* scene = nullptr;
    Renderer* renderer = nullptr;
    Camera camera;
    EngineContext ctx;
    SdfEditor* sdfEditor = nullptr;
};

// ------------------------------------------------------- controller brush ---
// Platform-free brush intents (the same contract DesktopBrushInput fulfills
// with the mouse), built from the XrInput per-frame snapshot. The brush sits
// on the right controller's aim ray; SdfEditor places it BrushDistance along
// the ray (a wand tip), adjustable with the right thumbstick.
float stickDeadzone(float v)
{
    const float dz = 0.18f;
    if (std::fabs(v) < dz) return 0.0f;
    return (v - (v > 0.0f ? dz : -dz)) / (1.0f - dz);
}

class QuestBrushInput : public BrushInputSource
{
public:
    QuestBrushInput(const XrInput* input, const glm::vec3* worldToStage)
        : in(input), worldToStage(worldToStage) {}

    BrushInput poll() override
    {
        BrushInput b{};
        // Aim pose -> engine-world ray (-Z is the OpenXR aim forward).
        b.rayOrigin = in->aimPos - *worldToStage;
        b.rayDir = in->aimRot * glm::vec3(0.0f, 0.0f, -1.0f);

        b.interactionAllowed = in->aimValid;
        b.drawDown = in->drawDown;
        b.eraseDown = in->eraseDown;
        b.recolorModifier = in->recolorDown;
        b.shapeTogglePressed = in->shapePressed;
        b.emissiveTogglePressed = in->emissivePressed;

        // Tuned for 72Hz: full deflection sweeps each parameter's useful
        // range in a couple of seconds (SdfEditor scales these deltas).
        b.radiusDelta = stickDeadzone(in->stickR.x) * 3.5f;
        b.distanceDelta = stickDeadzone(in->stickR.y) * 1.2f;
        b.smoothnessDelta = stickDeadzone(in->stickL.x) * 0.35f;

        // SdfEditor's stroke gate wants a 2D cursor that moves while the
        // stroke moves (stamp when it travels > ~0.5 units). Feed it the
        // accumulated path length of the wand tip: 400 units/m makes a stamp
        // land every ~1.3mm of controller travel.
        const glm::vec3 tip = b.rayOrigin + b.rayDir * 0.25f;
        if (hasLastTip) {
            pathLength += double(glm::length(tip - lastTip)) * 400.0;
        }
        lastTip = tip;
        hasLastTip = true;
        b.strokeCursor = glm::dvec2(pathLength, 0.0);
        return b;
    }

private:
    const XrInput* in;
    const glm::vec3* worldToStage;
    glm::vec3 lastTip = glm::vec3(0.0f);
    bool hasLastTip = false;
    double pathLength = 0.0;
};

// ------------------------------------------------------------------ assets ---

void extractShaderAssets(android_app* androidApp)
{
    AAssetManager* mgr = androidApp->activity->assetManager;
    const std::string root = androidApp->activity->internalDataPath;

    // AAssetDir lists files only (no recursion); the shader tree's subdirs
    // are fixed, so walk them explicitly.
    const char* dirs[] = { "shaders", "shaders/Sdf", "shaders/World", "shaders/Render", "shaders/Fluid" };

    int extracted = 0;
    for (const char* d : dirs) {
        const std::string outDir = root + "/" + d;
        mkdir(outDir.c_str(), 0755);
        AAssetDir* dir = AAssetManager_openDir(mgr, d);
        if (!dir) continue;
        const char* name = nullptr;
        while ((name = AAssetDir_getNextFileName(dir)) != nullptr) {
            const std::string rel = std::string(d) + "/" + name;
            AAsset* asset = AAssetManager_open(mgr, rel.c_str(), AASSET_MODE_BUFFER);
            if (!asset) continue;
            const void* data = AAsset_getBuffer(asset);
            const size_t len = static_cast<size_t>(AAsset_getLength(asset));
            const std::string outPath = root + "/" + rel;
            if (FILE* f = std::fopen(outPath.c_str(), "wb")) {
                std::fwrite(data, 1, len, f);
                std::fclose(f);
                ++extracted;
            }
            AAsset_close(asset);
        }
        AAssetDir_close(dir);
    }

    // From here on the engine's relative paths ("shaders/Sdf/SdfBrushCS.spv")
    // resolve exactly like they do with Source/ as the working dir on desktop.
    chdir(root.c_str());
    LOGI("Extracted %d shader assets to %s", extracted, root.c_str());
}

// ---------------------------------------------------------------- OpenXR ---

void createXrInstance(App& app)
{
    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader)));
    XrLoaderInitInfoAndroidKHR loaderInit{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInit.applicationVM = app.androidApp->activity->vm;
    loaderInit.applicationContext = app.androidApp->activity->clazz;
    XR_CHECK(initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInit)));

    // Passthrough is optional (graceful fallback to the void background).
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> extProps(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, extProps.data());
    bool fovBase = false, fovConfig = false, fovUpdate = false;
    for (const XrExtensionProperties& p : extProps) {
        if (std::strcmp(p.extensionName, XR_FB_PASSTHROUGH_EXTENSION_NAME) == 0) {
            app.passthroughSupported = true;
        }
        if (std::strcmp(p.extensionName, XR_FB_FOVEATION_EXTENSION_NAME) == 0) fovBase = true;
        if (std::strcmp(p.extensionName, XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME) == 0) fovConfig = true;
        if (std::strcmp(p.extensionName, XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME) == 0) fovUpdate = true;
        if (std::strcmp(p.extensionName, XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME) == 0) {
            app.depthSupported = true;
        }
    }
    // Foveation needs all three FB extensions (the profile + the swapchain-state update).
    app.foveationSupported = fovBase && fovConfig && fovUpdate;

    std::vector<const char*> extensions = {
        XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
    };
    if (app.passthroughSupported) {
        extensions.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    }
    if (app.foveationSupported) {
        extensions.push_back(XR_FB_FOVEATION_EXTENSION_NAME);
        extensions.push_back(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME);
        extensions.push_back(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME);
    }
    if (app.depthSupported) {
        extensions.push_back(XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME);
    }

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
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.enabledExtensionNames = extensions.data();
    XR_CHECK(xrCreateInstance(&createInfo, &app.xrInstance));

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(app.xrInstance, &systemInfo, &app.systemId));

    XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
    XrSystemEnvironmentDepthPropertiesMETA depthProps{XR_TYPE_SYSTEM_ENVIRONMENT_DEPTH_PROPERTIES_META};
    if (app.depthSupported) props.next = &depthProps;
    XR_CHECK(xrGetSystemProperties(app.xrInstance, app.systemId, &props));
    LOGI("OpenXR system: %s", props.systemName);
    if (app.depthSupported) {
        app.depthSupported = depthProps.supportsEnvironmentDepth == XR_TRUE;
        LOGI("Environment depth: supported=%d handRemoval=%d",
             (int)depthProps.supportsEnvironmentDepth, (int)depthProps.supportsHandRemoval);
    }
}

// -------------------------------------------------- Vulkan via VulkanBase ---
// The engine's VulkanBase drives creation; the hooks reroute the actual
// instance/device calls through the OpenXR runtime (XR_KHR_vulkan_enable2).

void initEngineVulkan(App& app, Engine& eng)
{
    // Spec: must be called once before xrCreateVulkanInstanceKHR.
    PFN_xrGetVulkanGraphicsRequirements2KHR getRequirements = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(app.xrInstance, "xrGetVulkanGraphicsRequirements2KHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)));
    XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    XR_CHECK(getRequirements(app.xrInstance, app.systemId, &requirements));

    eng.base = new VulkanBase();
    VulkanPlatformHooks& hooks = eng.base->platformHooks;
    App* appPtr = &app;

    hooks.createInstance = [appPtr](const VkInstanceCreateInfo& ci, VkInstance& out) -> VkResult {
        PFN_xrCreateVulkanInstanceKHR createVkInstance = nullptr;
        XR_CHECK(xrGetInstanceProcAddr(appPtr->xrInstance, "xrCreateVulkanInstanceKHR",
                                       reinterpret_cast<PFN_xrVoidFunction*>(&createVkInstance)));
        XrVulkanInstanceCreateInfoKHR xrInfo{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
        xrInfo.systemId = appPtr->systemId;
        xrInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        xrInfo.vulkanCreateInfo = &ci;
        VkResult vkResult = VK_SUCCESS;
        XR_CHECK(createVkInstance(appPtr->xrInstance, &xrInfo, &out, &vkResult));
        return vkResult;
    };

    hooks.pickPhysicalDevice = [appPtr](VkInstance instance) -> VkPhysicalDevice {
        PFN_xrGetVulkanGraphicsDevice2KHR getDevice = nullptr;
        XR_CHECK(xrGetInstanceProcAddr(appPtr->xrInstance, "xrGetVulkanGraphicsDevice2KHR",
                                       reinterpret_cast<PFN_xrVoidFunction*>(&getDevice)));
        XrVulkanGraphicsDeviceGetInfoKHR getInfo{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
        getInfo.systemId = appPtr->systemId;
        getInfo.vulkanInstance = instance;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        XR_CHECK(getDevice(appPtr->xrInstance, &getInfo, &physicalDevice));
        return physicalDevice;
    };

    hooks.createDevice = [appPtr](VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo& ci, VkDevice& out) -> VkResult {
        PFN_xrCreateVulkanDeviceKHR createVkDevice = nullptr;
        XR_CHECK(xrGetInstanceProcAddr(appPtr->xrInstance, "xrCreateVulkanDeviceKHR",
                                       reinterpret_cast<PFN_xrVoidFunction*>(&createVkDevice)));
        XrVulkanDeviceCreateInfoKHR xrInfo{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
        xrInfo.systemId = appPtr->systemId;
        xrInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
        xrInfo.vulkanPhysicalDevice = physicalDevice;
        xrInfo.vulkanCreateInfo = &ci;
        VkResult vkResult = VK_SUCCESS;
        XR_CHECK(createVkDevice(appPtr->xrInstance, &xrInfo, &out, &vkResult));
        return vkResult;
    };

    // Desktop list minus VK_KHR_swapchain (the XrSwapchain provides images).
    hooks.deviceExtensionOverride = {
        VK_KHR_MULTIVIEW_EXTENSION_NAME,
        VK_KHR_MAINTENANCE2_EXTENSION_NAME,
        VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
        VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,
    };

    eng.base->init(nullptr);

    app.vkInstance = eng.base->vkinstance;
    app.physicalDevice = eng.base->vkPhysicalDevice;
    app.device = eng.base->device;
    app.queueFamily = eng.base->vulkandevice->queueID;
    app.queue = eng.base->queue;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(app.physicalDevice, &props);
    LOGI("Engine Vulkan device created through OpenXR: %s, maxStorageBufferRange %u MB",
         props.deviceName, props.limits.maxStorageBufferRange / (1024u * 1024u));
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
    swapchainInfo.arraySize = 2; // both eyes in one array image (multiview)
    swapchainInfo.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(app.session, &swapchainInfo, &app.swapchain));

    uint32_t imageCount = 0;
    XR_CHECK(xrEnumerateSwapchainImages(app.swapchain, 0, &imageCount, nullptr));
    app.swapchainImages.resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    XR_CHECK(xrEnumerateSwapchainImages(
        app.swapchain, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(app.swapchainImages.data())));
    LOGI("Swapchain: %u images, format %d", imageCount, app.swapchainFormat);

    // Fixed Foveated Rendering (XR_FB_foveation): shade the lens periphery at a
    // lower rate - a big win for this fragment-bound per-pixel SDF march; the
    // center stays full-res and you don't perceive the periphery sharply in a
    // headset. MEDIUM is the usual sweet spot; LOW/HIGH trade sharpness vs perf
    // (could be driven live by a controller button later). No render changes -
    // the runtime builds the density map and the GPU shades accordingly.
    if (app.foveationSupported) {
        PFN_xrCreateFoveationProfileFB pfnCreateFoveation = nullptr;
        PFN_xrUpdateSwapchainFB pfnUpdateSwapchain = nullptr;
        PFN_xrDestroyFoveationProfileFB pfnDestroyFoveation = nullptr;
        xrGetInstanceProcAddr(app.xrInstance, "xrCreateFoveationProfileFB",
            reinterpret_cast<PFN_xrVoidFunction*>(&pfnCreateFoveation));
        xrGetInstanceProcAddr(app.xrInstance, "xrUpdateSwapchainFB",
            reinterpret_cast<PFN_xrVoidFunction*>(&pfnUpdateSwapchain));
        xrGetInstanceProcAddr(app.xrInstance, "xrDestroyFoveationProfileFB",
            reinterpret_cast<PFN_xrVoidFunction*>(&pfnDestroyFoveation));
        if (pfnCreateFoveation && pfnUpdateSwapchain && pfnDestroyFoveation) {
            // LOW keeps the image sharp (only the far edges drop); MEDIUM/HIGH
            // give more perf but visibly soften the periphery. dynamic = the
            // runtime only foveates under GPU load (sharp when the scene is
            // light, foveates while sculpting) - good blur/perf balance.
            XrFoveationLevelProfileCreateInfoFB levelProfile{XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
            levelProfile.level = XR_FOVEATION_LEVEL_LOW_FB; // LOW_FB / MEDIUM_FB / HIGH_FB
            levelProfile.verticalOffset = 0.0f;
            levelProfile.dynamic = XR_FOVEATION_DYNAMIC_LEVEL_ENABLED_FB; // _DISABLED_FB = fixed
            XrFoveationProfileCreateInfoFB profileInfo{XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB};
            profileInfo.next = &levelProfile;
            XrFoveationProfileFB profile = XR_NULL_HANDLE;
            if (pfnCreateFoveation(app.session, &profileInfo, &profile) == XR_SUCCESS) {
                XrSwapchainStateFoveationFB foveationState{XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB};
                foveationState.profile = profile;
                XrResult fr = pfnUpdateSwapchain(app.swapchain,
                    reinterpret_cast<XrSwapchainStateBaseHeaderFB*>(&foveationState));
                pfnDestroyFoveation(profile);
                LOGI("Foveation: MEDIUM %s", fr == XR_SUCCESS ? "applied" : "update FAILED");
            } else {
                LOGI("Foveation: xrCreateFoveationProfileFB failed");
            }
        } else {
            LOGI("Foveation: proc addresses missing");
        }
    } else {
        LOGI("Foveation: XR_FB_foveation not supported, rendering full-res");
    }

    app.frameFences.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(app.device, &fenceInfo, nullptr, &app.frameFences[i]));
    }
}

// --------------------------------------------------------- passthrough -----
// XR_FB_passthrough: feed the headset cameras as the background layer. The
// projection layer alpha-blends over it (scene clear alpha is 0 on Quest),
// so the sculpture floats in the real room - mixed-reality cake decorating.

void createPassthrough(App& app)
{
    if (!app.passthroughSupported) {
        LOGI("Passthrough: extension not available, keeping void background");
        return;
    }

    PFN_xrCreatePassthroughFB createPassthroughFB = nullptr;
    PFN_xrCreatePassthroughLayerFB createLayerFB = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(app.xrInstance, "xrCreatePassthroughFB",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&createPassthroughFB)));
    XR_CHECK(xrGetInstanceProcAddr(app.xrInstance, "xrCreatePassthroughLayerFB",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&createLayerFB)));

    XrPassthroughCreateInfoFB ptInfo{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    ptInfo.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    XrResult result = createPassthroughFB(app.session, &ptInfo, &app.passthrough);
    if (XR_FAILED(result)) {
        LOGE("Passthrough create failed (%d), keeping void background", result);
        app.passthroughSupported = false;
        return;
    }

    XrPassthroughLayerCreateInfoFB layerInfo{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
    layerInfo.passthrough = app.passthrough;
    layerInfo.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    layerInfo.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    result = createLayerFB(app.session, &layerInfo, &app.passthroughLayer);
    if (XR_FAILED(result)) {
        LOGE("Passthrough layer create failed (%d), keeping void background", result);
        app.passthroughSupported = false;
        return;
    }
    LOGI("Passthrough background active");
}

// ------------------------------------------ environment depth (XR_META) -----
// Stand up the depth provider: load the extension entry points, create the
// provider + its readable depth swapchain (a 2-layer depth texture array), query
// its size, enumerate the Vulkan images, and start generation. Provider creation
// fails without the USE_SCENE permission - logged, non-fatal (depth stays off).
void setupEnvironmentDepth(App& app, Engine& eng)
{
    if (!app.depthSupported) {
        LOGI("Environment depth not available on this device/runtime");
        return;
    }
    auto load = [&](const char* name, PFN_xrVoidFunction* fn) {
        XR_CHECK(xrGetInstanceProcAddr(app.xrInstance, name, fn));
    };
    load("xrCreateEnvironmentDepthProviderMETA", reinterpret_cast<PFN_xrVoidFunction*>(&app.pfnCreateDepthProvider));
    load("xrStartEnvironmentDepthProviderMETA", reinterpret_cast<PFN_xrVoidFunction*>(&app.pfnStartDepthProvider));
    load("xrCreateEnvironmentDepthSwapchainMETA", reinterpret_cast<PFN_xrVoidFunction*>(&app.pfnCreateDepthSwapchain));
    load("xrGetEnvironmentDepthSwapchainStateMETA", reinterpret_cast<PFN_xrVoidFunction*>(&app.pfnGetDepthSwapchainState));
    load("xrEnumerateEnvironmentDepthSwapchainImagesMETA", reinterpret_cast<PFN_xrVoidFunction*>(&app.pfnEnumDepthSwapchainImages));
    load("xrAcquireEnvironmentDepthImageMETA", reinterpret_cast<PFN_xrVoidFunction*>(&app.pfnAcquireDepthImage));

    XrEnvironmentDepthProviderCreateInfoMETA pci{XR_TYPE_ENVIRONMENT_DEPTH_PROVIDER_CREATE_INFO_META};
    XrResult r = app.pfnCreateDepthProvider(app.session, &pci, &app.depthProvider);
    if (r != XR_SUCCESS) {
        LOGE("xrCreateEnvironmentDepthProviderMETA failed: %d - is com.oculus.permission.USE_SCENE granted?", r);
        app.depthSupported = false;
        return;
    }

    XrEnvironmentDepthSwapchainCreateInfoMETA sci{XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_CREATE_INFO_META};
    XR_CHECK(app.pfnCreateDepthSwapchain(app.depthProvider, &sci, &app.depthSwapchain));

    XrEnvironmentDepthSwapchainStateMETA state{XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_STATE_META};
    XR_CHECK(app.pfnGetDepthSwapchainState(app.depthSwapchain, &state));
    app.depthWidth = state.width;
    app.depthHeight = state.height;

    uint32_t imageCount = 0;
    XR_CHECK(app.pfnEnumDepthSwapchainImages(app.depthSwapchain, 0, &imageCount, nullptr));
    app.depthImages.resize(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    XR_CHECK(app.pfnEnumDepthSwapchainImages(app.depthSwapchain, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(app.depthImages.data())));

    XR_CHECK(app.pfnStartDepthProvider(app.depthProvider));
    LOGI("Environment depth started: %ux%u, %u swapchain images", app.depthWidth, app.depthHeight, imageCount);

    // --- Depth visualization data path (step 2) ---------------------------------
    // Persistent RGBA16F 2-layer array (one eye per layer) the post pass samples;
    // the compute writes the selected viz (depth gray / world position / normal)
    // into it, so it stays in GENERAL the whole time.
    const uint32_t dw = app.depthWidth, dh = app.depthHeight;
    app.depthDisplayImage = new VulkanImage(app.device, app.physicalDevice);
    app.depthDisplayImage->createImage(dw, dh, 1, VK_IMAGE_TYPE_2D, 1, VK_SAMPLE_COUNT_1_BIT,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 2 /* arrayCount */);
    app.depthDisplayImage->createImageView(VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 1,
        VK_IMAGE_VIEW_TYPE_2D_ARRAY, 2);
    eng.base->vbuffer->transitionImageLayout(app.depthDisplayImage->image, VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 1, 2);
    app.depthDisplayTex.image = app.depthDisplayImage->image;
    app.depthDisplayTex.imageView = app.depthDisplayImage->imageView;
    app.depthDisplayTex.imageSampler = eng.base->vbuffer->createTextureSampler(1);
    app.depthDisplayTex.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    app.depthDisplayTex.imageViewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;

    app.depthParamsUbo = eng.base->vbuffer->createUniformBuffers({ sizeof(DepthVizParams) });

    // One compute instance per depth image: each reads that image (depth-aspect array
    // view) and writes the shared display target. Per frame we submit the instance for
    // the acquired swapchainIndex - so no descriptor is ever updated in place.
    for (XrSwapchainImageVulkan2KHR& img : app.depthImages) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vi.format = VK_FORMAT_D16_UNORM; // GUESS - if the view fails, try D32_SFLOAT
        vi.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 2 };
        VkImageView v = VK_NULL_HANDLE;
        VkResult vr = vkCreateImageView(app.device, &vi, nullptr, &v);
        if (vr != VK_SUCCESS) {
            LOGE("depth-src image view failed: %d (wrong depth format guess?)", vr);
            app.depthVizReady = false;
            return;
        }
        app.depthSrcViews.push_back(v);
        VkSampler s = eng.base->vbuffer->createTextureSampler(1);
        app.depthSrcSamplers.push_back(s);

        VulkanTexture srcTex{};
        srcTex.image = img.image;
        srcTex.imageView = v;
        srcTex.imageSampler = s;
        srcTex.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; // GUESS - try DEPTH_STENCIL_READ_ONLY / GENERAL
        srcTex.imageViewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;

        ComputeInput ci;
        ci.computeShaderPath = "shaders/Sdf/QuestDepthVizCS.spv";
        ci.descriptorTypes = {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
        };
        ComputePipeline* cp = new ComputePipeline(eng.base, ci);
        cp->setupDescriptors(app.depthParamsUbo, { srcTex, app.depthDisplayTex }, {});
        cp->dispatch(1, 1, 1); // create command buffer + signaled fence
        app.depthVizComp.push_back(cp);
    }
    app.depthVizReady = true;
    LOGI("Depth viz ready: %u compute instances, R16F %ux%ux2",
         (uint32_t)app.depthVizComp.size(), dw, dh);
}

// USE_SCENE is a runtime permission, so manifest declaration alone never prompts -
// the app must call Activity.requestPermissions itself. These JNI helpers drive the
// standard Android flow against the NativeActivity (checkSelfPermission /
// requestPermissions are inherited from Context/Activity, API 23+; minSdk is 29).
static const char* kScenePermission = "com.oculus.permission.USE_SCENE";

bool androidHasPermission(android_app* androidApp, const char* permission)
{
    JavaVM* vm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    vm->AttachCurrentThread(&env, nullptr); // idempotent if already attached
    jobject activity = androidApp->activity->clazz;
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID check = env->GetMethodID(activityClass, "checkSelfPermission", "(Ljava/lang/String;)I");
    jstring jperm = env->NewStringUTF(permission);
    jint result = env->CallIntMethod(activity, check, jperm); // PERMISSION_GRANTED == 0
    env->DeleteLocalRef(jperm);
    env->DeleteLocalRef(activityClass);
    return result == 0;
}

void androidRequestPermission(android_app* androidApp, const char* permission)
{
    JavaVM* vm = androidApp->activity->vm;
    JNIEnv* env = nullptr;
    vm->AttachCurrentThread(&env, nullptr);
    jobject activity = androidApp->activity->clazz;
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID request = env->GetMethodID(activityClass, "requestPermissions", "([Ljava/lang/String;I)V");
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray perms = env->NewObjectArray(1, stringClass, env->NewStringUTF(permission));
    env->CallVoidMethod(activity, request, perms, 0 /* requestCode */);
    env->DeleteLocalRef(perms);
    env->DeleteLocalRef(stringClass);
    env->DeleteLocalRef(activityClass);
}

// Depth entry point: if supported and permission already granted, set up now;
// otherwise prompt and defer (renderFrame polls for the grant and finishes setup).
void requestDepthPermissionOrSetup(App& app, Engine& eng)
{
    if (!app.depthSupported) {
        LOGI("Environment depth not available on this device/runtime");
        return;
    }
    if (androidHasPermission(app.androidApp, kScenePermission)) {
        setupEnvironmentDepth(app, eng);
    } else {
        LOGI("Requesting %s (depth setup deferred until granted)", kScenePermission);
        androidRequestPermission(app.androidApp, kScenePermission);
        app.depthPendingPermission = true;
    }
}

// ------------------------------------------------------------ XR input -----

XrAction makeAction(XrActionSet set, XrActionType type, const char* name, const char* localized)
{
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    info.actionType = type;
    std::strcpy(info.actionName, name);
    std::strcpy(info.localizedActionName, localized);
    XrAction action = XR_NULL_HANDLE;
    XR_CHECK(xrCreateAction(set, &info, &action));
    return action;
}

void createXrInput(App& app)
{
    XrInput& in = app.input;

    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(setInfo.actionSetName, "brush");
    std::strcpy(setInfo.localizedActionSetName, "Brush");
    XR_CHECK(xrCreateActionSet(app.xrInstance, &setInfo, &in.actionSet));

    in.aimPose = makeAction(in.actionSet, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Aim Pose");
    in.drawTrigger = makeAction(in.actionSet, XR_ACTION_TYPE_FLOAT_INPUT, "draw", "Draw");
    in.eraseSqueeze = makeAction(in.actionSet, XR_ACTION_TYPE_FLOAT_INPUT, "erase", "Erase");
    in.stickRight = makeAction(in.actionSet, XR_ACTION_TYPE_VECTOR2F_INPUT, "stick_right", "Radius / Distance");
    in.stickLeft = makeAction(in.actionSet, XR_ACTION_TYPE_VECTOR2F_INPUT, "stick_left", "Hue / Smoothness");
    in.shapeButton = makeAction(in.actionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "shape_toggle", "Shape Toggle");
    in.emissiveButton = makeAction(in.actionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "emissive_toggle", "Emissive Toggle");
    in.recolorTrigger = makeAction(in.actionSet, XR_ACTION_TYPE_FLOAT_INPUT, "recolor", "Recolor Modifier");
    in.wireframeSqueeze = makeAction(in.actionSet, XR_ACTION_TYPE_FLOAT_INPUT, "wireframe", "Block Wireframe");
    in.clearButton = makeAction(in.actionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "clear_scene", "Clear Scene");
    in.depthModeButton = makeAction(in.actionSet, XR_ACTION_TYPE_BOOLEAN_INPUT, "depth_mode", "Depth Viz Mode");

    auto path = [&](const char* p) {
        XrPath out = XR_NULL_PATH;
        XR_CHECK(xrStringToPath(app.xrInstance, p, &out));
        return out;
    };

    const XrActionSuggestedBinding bindings[] = {
        { in.aimPose, path("/user/hand/right/input/aim/pose") },
        { in.drawTrigger, path("/user/hand/right/input/trigger/value") },
        { in.eraseSqueeze, path("/user/hand/right/input/squeeze/value") },
        { in.stickRight, path("/user/hand/right/input/thumbstick") },
        { in.shapeButton, path("/user/hand/right/input/a/click") },
        { in.emissiveButton, path("/user/hand/right/input/b/click") },
        { in.stickLeft, path("/user/hand/left/input/thumbstick") },
        { in.recolorTrigger, path("/user/hand/left/input/trigger/value") },
        { in.wireframeSqueeze, path("/user/hand/left/input/squeeze/value") },
        { in.clearButton, path("/user/hand/left/input/x/click") },
        { in.depthModeButton, path("/user/hand/left/input/y/click") },
    };
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    suggested.suggestedBindings = bindings;
    suggested.countSuggestedBindings = sizeof(bindings) / sizeof(bindings[0]);
    XR_CHECK(xrSuggestInteractionProfileBindings(app.xrInstance, &suggested));

    XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    spaceInfo.action = in.aimPose;
    spaceInfo.poseInActionSpace.orientation.w = 1.0f;
    XR_CHECK(xrCreateActionSpace(app.session, &spaceInfo, &in.aimSpace));

    XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &in.actionSet;
    XR_CHECK(xrAttachSessionActionSets(app.session, &attachInfo));
    LOGI("XR input: brush action set attached");
}

void syncXrInput(App& app, XrTime displayTime)
{
    XrInput& in = app.input;

    XrActiveActionSet active{in.actionSet, XR_NULL_PATH};
    XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &active;
    // XR_SESSION_NOT_FOCUSED is normal (system menu up etc.) - inputs just
    // read inactive this frame.
    XrResult syncResult = xrSyncActions(app.session, &syncInfo);
    if (XR_FAILED(syncResult)) {
        LOGE("xrSyncActions failed: %d", syncResult);
    }

    auto getFloat = [&](XrAction action) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = action;
        XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
        XR_CHECK(xrGetActionStateFloat(app.session, &gi, &s));
        return s.isActive ? s.currentState : 0.0f;
    };
    auto getPressedEdge = [&](XrAction action) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = action;
        XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
        XR_CHECK(xrGetActionStateBoolean(app.session, &gi, &s));
        return s.isActive && s.currentState && s.changedSinceLastSync;
    };
    auto getVec2 = [&](XrAction action) {
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = action;
        XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
        XR_CHECK(xrGetActionStateVector2f(app.session, &gi, &s));
        return s.isActive ? glm::vec2(s.currentState.x, s.currentState.y) : glm::vec2(0.0f);
    };

    // Analog buttons with hysteresis so held strokes don't flicker.
    const float drawValue = getFloat(in.drawTrigger);
    in.drawDown = drawValue > (in.drawDown ? 0.35f : 0.6f);
    const float eraseValue = getFloat(in.eraseSqueeze);
    in.eraseDown = eraseValue > (in.eraseDown ? 0.35f : 0.6f);
    in.recolorDown = getFloat(in.recolorTrigger) > 0.5f;
    const float wireValue = getFloat(in.wireframeSqueeze);
    in.wireframeDown = wireValue > (in.wireframeDown ? 0.35f : 0.6f);

    in.shapePressed = getPressedEdge(in.shapeButton);
    in.emissivePressed = getPressedEdge(in.emissiveButton);
    in.clearPressed = getPressedEdge(in.clearButton);
    in.depthModePressed = getPressedEdge(in.depthModeButton);
    in.stickR = getVec2(in.stickRight);
    in.stickL = getVec2(in.stickLeft);

    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    XR_CHECK(xrLocateSpace(in.aimSpace, app.localSpace, displayTime, &location));
    const XrSpaceLocationFlags needed =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    in.aimValid = (location.locationFlags & needed) == needed;
    if (in.aimValid) {
        in.aimPos = glm::vec3(location.pose.position.x, location.pose.position.y, location.pose.position.z);
        in.aimRot = glm::quat(location.pose.orientation.w, location.pose.orientation.x,
                              location.pose.orientation.y, location.pose.orientation.z);
    }

    // Once-a-second input diagnostics until Phase 3 is proven.
    static int diagCounter = 0;
    if (++diagCounter >= 72) {
        diagCounter = 0;
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = in.drawTrigger;
        XrActionStateFloat trig{XR_TYPE_ACTION_STATE_FLOAT};
        xrGetActionStateFloat(app.session, &gi, &trig);

        XrPath rightHand = XR_NULL_PATH;
        xrStringToPath(app.xrInstance, "/user/hand/right", &rightHand);
        XrInteractionProfileState profile{XR_TYPE_INTERACTION_PROFILE_STATE};
        xrGetCurrentInteractionProfile(app.session, rightHand, &profile);
        char profileName[XR_MAX_PATH_LENGTH] = "none";
        if (profile.interactionProfile != XR_NULL_PATH) {
            uint32_t len = 0;
            xrPathToString(app.xrInstance, profile.interactionProfile, sizeof(profileName), &len, profileName);
        }
        LOGI("input: sync=%d profile=%s trigActive=%d trig=%.2f aimValid=%d aim=(%.2f %.2f %.2f) state=%d",
             syncResult, profileName, trig.isActive ? 1 : 0, trig.currentState,
             in.aimValid ? 1 : 0, in.aimPos.x, in.aimPos.y, in.aimPos.z, app.sessionState);
        if (gEngineForDiag) {
            const SdfVolumeStats& s = gEngineForDiag->sdfEditor->volume.stats;
            LOGI("atlas: tail=%d free=%d used=%d capacity=%d (%.0f%%)",
                 s.tailBricks, s.freeBricks, s.usedBricks, s.capacityBricks, s.usage * 100.0f);
        }
    }
}

// -------------------------------------------------------- engine bring-up ---
// Mirrors main.cpp's initVulkan() wiring, with the Assimp-loaded cube mesh
// replaced by a procedural one (the brick proxy expects positions -2..+2,
// SdfBrickVS converts to the centered unit cube).

void initEngineScene(App& app, Engine& eng)
{
    VulkanBase* base = eng.base;
    VulkanPlatformHooks& hooks = base->platformHooks;

    hooks.presentExtentOverride = { static_cast<uint32_t>(app.swapchainWidth),
                                    static_cast<uint32_t>(app.swapchainHeight) };
    // Offscreen scene target is UNORM to match the desktop swapchain
    // (B8G8R8A8_UNORM): shaders write raw values there and the monitor
    // displays them as sRGB. Copying those raw bits into the sRGB XR
    // swapchain image makes the compositor interpret them the same way -
    // rendering with an sRGB attachment instead would re-encode on write
    // and wash all colors out.
    hooks.presentFormatOverride = VK_FORMAT_R8G8B8A8_UNORM;
    hooks.presentImageCount = static_cast<uint32_t>(app.swapchainImages.size());
    hooks.presentImages.clear();
    for (const XrSwapchainImageVulkan2KHR& image : app.swapchainImages) {
        hooks.presentImages.push_back(image.image);
    }
    base->createCommandBuffers();

    ResourceManager::setVulkanBase(base);

    eng.scene = new Scene(base);
    eng.renderer = new Renderer(base);
    eng.scene->renderer = eng.renderer;
    eng.scene->camera = &eng.camera;
    eng.scene->input = nullptr; // no desktop input on this platform
    eng.camera.viewportResolution = glm::vec2(float(app.swapchainWidth), float(app.swapchainHeight));

    eng.ctx.base = base;
    eng.ctx.scene = eng.scene;
    eng.ctx.renderer = eng.renderer;
    eng.ctx.camera = &eng.camera;
    eng.ctx.input = nullptr;

    eng.scene->init(); // framebuffer (multiview) + the 5 scene pipelines

    // Procedural brick-proxy cube, positions -2..+2 (what SdfBrickVS expects).
    Mesh mesh{};
    const float h = 2.0f;
    const glm::vec3 corners[8] = {
        {-h,-h,-h}, { h,-h,-h}, { h, h,-h}, {-h, h,-h},
        {-h,-h, h}, { h,-h, h}, { h, h, h}, {-h, h, h},
    };
    for (const glm::vec3& c : corners) {
        Vertex v{};
        v.pos = c;
        v.normal = glm::normalize(c);
        v.texCoord = glm::vec2(0.0f);
        mesh.vertices.push_back(v);
    }
    // Winding chosen so the cube's NEAR faces survive the pipeline's
    // back-face cull (under the engine's Vulkan-convention projection) -
    // SdfBrickFS declares layout(depth_greater), which is only valid when
    // marching starts at the near face. Wrong-side rasterization breaks the
    // depth contract and Adreno's LRZ turns silhouettes into confetti.
    mesh.indices = {
        6,5,4, 4,7,6,  // +Z
        3,0,1, 1,2,3,  // -Z
        2,1,5, 5,6,2,  // +X
        7,4,0, 0,3,7,  // -X
        6,7,3, 3,2,6,  // +Y
        0,4,5, 5,1,0,  // -Y
    };
    mesh.color = glm::vec4(1.0f);
    mesh.texID = -1;
    base->vbuffer->createVertexBuffer(mesh);
    base->vbuffer->createIndexBuffer(mesh);
    base->vbuffer->createInstanceIdBuffer(mesh, 64u * 64u * 64u);
    eng.scene->meshes.push_back(mesh);

    Object obj{};
    obj.name = "SdfChunk";
    obj.hasMesh = true;
    obj.hasAnimation = false;
    obj.textureID = 0;
    obj.meshID.push_back(0);
    obj.ModelMatrix = glm::mat4(1.0f);
    obj.finalMatrix = glm::mat4(1.0f);
    VulkanShader shader{};
    shader.graphicsPipeline = eng.scene->pipelines[4]; // volumetric brick pipeline
    shader.ubos = base->vbuffer->createUniformBuffers(shader.graphicsPipeline->bufferSize);
    obj.shader.push_back(shader);
    eng.scene->objects.push_back(obj);

    // SdfEditor locates the editable mesh by the volumetric pipeline and
    // builds the brick atlas + GPU draw list (same as desktop).
    eng.sdfEditor = new SdfEditor(eng.ctx);
    eng.scene->SBOs.push_back(eng.sdfEditor->chunks[0].sbo);
    eng.scene->brush = &eng.sdfEditor->brush;
    eng.scene->sdfVolume = &eng.sdfEditor->volume; // HUD stats + boundary cage
    eng.sdfEditor->brushInput = new QuestBrushInput(&app.input, &kWorldToStage);

    // Volumetric material: binding-2 sampler3D in SdfBrickFS.
    Material material{};
    material.type = TexturedMaterial;
    MaterialTexturedData* matData = new MaterialTexturedData();
#ifdef BAKEIT_SDF_TEXTURE_ATLAS
    // Texture-native field: bind to SdfEditor's sparse tile pool (it created and
    // registered the texture during construction). The brick FS samples it
    // (texMode 2 = ubo2.color.w, set by Scene::update).
    matData->DiffuseTexture = eng.sdfEditor->tilePoolTextureIndex;
#else
    // SSBO field: a 1x1x1 placeholder the brick FS never samples.
    eng.scene->ptextures.push_back(base->vbuffer->createTexture3DGeneral(1, 1, 1));
    matData->DiffuseTexture = static_cast<int>(eng.scene->ptextures.size()) - 1;
#endif
    // Binding 3 (coarse tile pool sampler): the SHARED coarse tile pool (all LOD
    // levels), sampled by the brick FS's coarse-LOD path. One pool for the whole
    // pyramid - the per-instance level picks its table region in the shader.
    matData->SecondaryTexture = eng.sdfEditor->coarseSharedPoolIndex;
    material.materialData = matData;
    eng.scene->materials.push_back(material);
    eng.scene->meshes[0].matID = 0;

    eng.scene->objects[0].shader[0].sbos = {
        eng.scene->SBOs[0][0],
        eng.scene->SBOs[0][1],
        eng.scene->SBOs[0][2],
        eng.scene->SBOs[0][3],
        eng.scene->SBOs[0][5],
        // Coarse-LOD bindings 9/10: shared coarse block table + dummy bricks
        // (mirror of the desktop wiring in main.cpp).
        eng.sdfEditor->coarseSharedSBO[1],
        eng.sdfEditor->coarseSharedSBO[0]
    };

    // No editor GUI on Quest: force the coarse-LOD level on so the coarse tile
    // pool actually renders (the direct analog of the desktop "Coarse LOD -> 2x2x2"
    // combo, EditorGui.cpp). 0 = off/fine, 2 = 2x2x2 coarse. Scene::update feeds
    // this to ubo2.color.z, which SdfBrickVS/FS read. TODO: drive from distance LOD.
    eng.scene->coarseLodLevel = 2;

    base->vbuffer->updateIndirectInstanceList(eng.scene->meshes[0], {});
    // Brush indicator: same as desktop - the post-process pass overlays the
    // shape outline + surface footprint, depth-tested against the scene.

    eng.scene->createSceneDescriptor();
    eng.scene->renderPass(); // record per-image command buffers (incl. XR copy)
    gEngineForDiag = &eng;
    LOGI("Engine scene ready");
}

// ------------------------------------------------------------ frame loop ---

// Vulkan-convention projection (y down, depth 0..1, right-handed) from the
// XR fov. Scene::update does NOT y-flip on this platform, and SdfEditor's
// frustum culling uses the same matrix - conventions stay consistent.
glm::mat4 projFromXrFov(const XrFovf& fov, float nearZ, float farZ)
{
    const float tl = std::tan(fov.angleLeft);
    const float tr = std::tan(fov.angleRight);
    const float td = std::tan(fov.angleDown);
    const float tu = std::tan(fov.angleUp);

    glm::mat4 p(0.0f);
    p[0][0] = 2.0f / (tr - tl);
    p[2][0] = (tr + tl) / (tr - tl);
    p[1][1] = -2.0f / (tu - td);
    p[2][1] = -(tu + td) / (tu - td);
    p[2][2] = farZ / (nearZ - farZ);
    p[3][2] = -(farZ * nearZ) / (farZ - nearZ);
    p[2][3] = -1.0f;
    return p;
}

void updateCameraFromXr(Engine& eng, const XrView views[2])
{
    const glm::mat4 stageFromWorld = glm::translate(glm::mat4(1.0f), kWorldToStage);
    for (int e = 0; e < 2; ++e) {
        const XrPosef& pose = views[e].pose;
        const glm::quat q(pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z);
        const glm::mat4 eyeToStage =
            glm::translate(glm::mat4(1.0f), glm::vec3(pose.position.x, pose.position.y, pose.position.z)) *
            glm::mat4_cast(q);
        eng.camera.view[e] = glm::inverse(eyeToStage) * stageFromWorld;
        eng.camera.proj[e] = projFromXrFov(views[e].fov, 0.05f, 100.0f);
    }
    // Combined-eye culling frustum for the GPU draw-list build: one frustum
    // from the center-eye pose whose per-side fov is the wider of the two
    // eyes, padded to absorb the eyes' positional (IPD) offset from the
    // center. Without this, culling uses eye 0 only and bricks vanish at the
    // right eye's temporal edge.
    const glm::vec3 posL(views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z);
    const glm::vec3 posR(views[1].pose.position.x, views[1].pose.position.y, views[1].pose.position.z);
    const glm::vec3 center = 0.5f * (posL + posR);
    const XrPosef& headPose = views[0].pose; // eye orientations are identical
    const glm::quat hq(headPose.orientation.w, headPose.orientation.x, headPose.orientation.y, headPose.orientation.z);
    const glm::mat4 centerToStage = glm::translate(glm::mat4(1.0f), center) * glm::mat4_cast(hq);
    eng.camera.cullView = glm::inverse(centerToStage) * glm::translate(glm::mat4(1.0f), kWorldToStage);

    // ~11 deg of slack per side fully covers the half-IPD parallax for
    // content farther than ~16cm from the face; closer than that a brick on
    // the very edge of one eye could pop, which Phase 4 can refine.
    const float kCullMarginRad = 0.2f;
    XrFovf cullFov;
    cullFov.angleLeft = std::min(views[0].fov.angleLeft, views[1].fov.angleLeft) - kCullMarginRad;
    cullFov.angleRight = std::max(views[0].fov.angleRight, views[1].fov.angleRight) + kCullMarginRad;
    cullFov.angleDown = std::min(views[0].fov.angleDown, views[1].fov.angleDown) - kCullMarginRad;
    cullFov.angleUp = std::max(views[0].fov.angleUp, views[1].fov.angleUp) + kCullMarginRad;
    eng.camera.cullProj = projFromXrFov(cullFov, 0.05f, 100.0f);

    // World-space head center for draw-list sorting and brush distance.
    eng.camera.position = center - kWorldToStage;
}

void renderFrame(App& app, Engine& eng)
{
    // USE_SCENE grant may land a few frames after launch (user taps Allow); finish
    // the deferred depth setup once it's granted.
    if (app.depthPendingPermission && app.depthProvider == XR_NULL_HANDLE
        && androidHasPermission(app.androidApp, kScenePermission)) {
        app.depthPendingPermission = false;
        setupEnvironmentDepth(app, eng);
    }

    // Bind the depth display texture to the post pass once both exist (the post pass
    // is created in initEngineScene, the depth target in setupEnvironmentDepth - either
    // can come first). Rebuilds one descriptor, so force a re-record afterward.
    if (app.depthVizReady && !app.depthBoundToPost && eng.renderer
        && eng.renderer->postprocessingManager && eng.renderer->postprocessingManager->postPass) {
        eng.renderer->postprocessingManager->postPass->setDepthDisplay(
            app.depthDisplayTex.imageView, app.depthDisplayTex.imageSampler);
        eng.renderer->renderPassDirty = true;
        app.depthBoundToPost = true;
        LOGI("Depth display bound to post pass");
    }

    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    XR_CHECK(xrWaitFrame(app.session, &waitInfo, &frameState));

    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    XR_CHECK(xrBeginFrame(app.session, &beginInfo));

    if (!app.haveBaseTime) {
        app.baseTime = frameState.predictedDisplayTime;
        app.haveBaseTime = true;
    }
    const float timeSec = static_cast<float>(frameState.predictedDisplayTime - app.baseTime) * 1e-9f;

    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView projViews[2] = {
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerPassthroughFB passthroughLayer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    passthroughLayer.layerHandle = app.passthroughLayer;
    const XrCompositionLayerBaseHeader* layers[2] = {};
    uint32_t layerCount = 0;
    if (app.passthroughSupported && app.passthroughLayer != XR_NULL_HANDLE) {
        // Passthrough underneath; the projection layer alpha-blends over it.
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&passthroughLayer);
        layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                           XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    }
    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    syncXrInput(app, frameState.predictedDisplayTime);

    if (frameState.shouldRender) {
        XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = app.localSpace;
        XrViewState viewState{XR_TYPE_VIEW_STATE};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t viewCount = 0;
        XR_CHECK(xrLocateViews(app.session, &locateInfo, &viewState, 2, &viewCount, views));

        // Environment depth (step 1): acquire the latest real-world depth map. The
        // returned swapchainIndex selects which depthImages[] VkImage holds this
        // frame's 2-layer depth array; nearZ/farZ + views[].fov/pose decode + reproject
        // it. For now just log to confirm the pipeline; step 2 samples it in the post pass.
        if (app.depthSupported && app.depthProvider != XR_NULL_HANDLE) {
            XrEnvironmentDepthImageAcquireInfoMETA dai{XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_ACQUIRE_INFO_META};
            dai.space = app.localSpace;
            dai.displayTime = frameState.predictedDisplayTime;
            XrEnvironmentDepthImageMETA depthImage{XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_META};
            // The runtime fills views[] but validates their type first - must be set by
            // the app (like XrView for xrLocateViews), else acquire -> VALIDATION_FAILURE.
            depthImage.views[0].type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META;
            depthImage.views[1].type = XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META;
            XrResult dr = app.pfnAcquireDepthImage(app.depthProvider, &dai, &depthImage);
            static int depthDbg = 0;
            if (dr == XR_SUCCESS) {
                if ((depthDbg++ % 72) == 0) {
                    LOGI("Depth acquired: idx=%u %ux%u nearZ=%.3f farZ=%.3f",
                         depthImage.swapchainIndex, app.depthWidth, app.depthHeight,
                         depthImage.nearZ, depthImage.farZ);
                }
                // Linearize this frame's depth into the display target: submit the
                // compute instance bound to this swapchainIndex, then wait it out so the
                // post pass reads completed data.
                // The post pass reprojects the depth into the eye view using the depth
                // camera's per-eye FOV - upload its tangents (tanL,tanR,tanD,tanU) so the
                // fullscreen depth lines up with what the cameras see. Done before
                // scene->update (which maps the post UBO) further down this frame.
                if (app.depthVizReady && eng.renderer && eng.renderer->postprocessingManager
                    && eng.renderer->postprocessingManager->postPass) {
                    for (int e = 0; e < 2; ++e) {
                        const XrFovf& f = depthImage.views[e].fov;
                        eng.renderer->postprocessingManager->postPass->depthFovTan[e] =
                            glm::vec4(tanf(f.angleLeft), tanf(f.angleRight),
                                      tanf(f.angleDown), tanf(f.angleUp));
                    }
                }
                if (app.depthVizReady && depthImage.swapchainIndex < app.depthVizComp.size()) {
                    DepthVizParams pu{};
                    pu.nearZ = depthImage.nearZ;
                    pu.farZ = depthImage.farZ;
                    pu.width = (int)app.depthWidth;
                    pu.height = (int)app.depthHeight;
                    // depthDisplayMode: 1 depth, 2 world pos, 3 normal -> shader 0/1/2.
                    pu.vizMode = (app.depthDisplayMode <= 1) ? 0 : (app.depthDisplayMode - 1);
                    for (int e = 0; e < 2; ++e) {
                        const XrFovf& f = depthImage.views[e].fov;
                        pu.fovTan[e] = glm::vec4(tanf(f.angleLeft), tanf(f.angleRight),
                                                 tanf(f.angleDown), tanf(f.angleUp));
                        const XrPosef& ps = depthImage.views[e].pose;
                        glm::quat q(ps.orientation.w, ps.orientation.x, ps.orientation.y, ps.orientation.z);
                        glm::vec3 t(ps.position.x, ps.position.y, ps.position.z);
                        pu.camToWorld[e] = glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(q);
                    }
                    app.depthParamsUbo[0].map(app.device, sizeof(pu), &pu);
                    ComputePipeline* cp = app.depthVizComp[depthImage.swapchainIndex];
                    cp->submit((app.depthWidth + 7) / 8, (app.depthHeight + 7) / 8, 1);
                    vkWaitForFences(app.device, 1, &cp->computeFence, VK_TRUE, UINT64_MAX);
                }
            } else if (dr != XR_ENVIRONMENT_DEPTH_NOT_AVAILABLE_META && (depthDbg++ % 72) == 0) {
                LOGE("xrAcquireEnvironmentDepthImageMETA failed: %d", dr);
            }
        }

        // The per-object UBOs are single-buffered: Scene::update overwrites
        // the matrices the in-flight command buffer is still reading, and on
        // a tiled GPU that splits a frame between old and new head poses
        // (tearing-like smears on rotation). Wait out the previous frame's
        // GPU work before touching the UBOs - xrWaitFrame already paces us,
        // so this costs almost nothing at 72Hz. (Proper per-frame UBO
        // buffering is part of the Phase 4 perf pass.)
        if (app.lastSubmittedFence != VK_NULL_HANDLE) {
            VK_CHECK(vkWaitForFences(app.device, 1, &app.lastSubmittedFence, VK_TRUE, UINT64_MAX));
        }

        // Safe spot for CPU writes to the SDF buffers: GPU is idle here.
        if (app.input.clearPressed) {
            eng.sdfEditor->clearScene();
            LOGI("Scene cleared");
        }

        // Y cycles the fullscreen debug overlay: off -> depth -> world pos -> normal.
        // Real-world occlusion stays on independently (depthVizActive = bound); this
        // only changes the debug overlay the compute writes / the post pass draws.
        if (app.input.depthModePressed && app.depthVizReady) {
            app.depthDisplayMode = (app.depthDisplayMode + 1) % 4;
            if (eng.renderer && eng.renderer->postprocessingManager
                && eng.renderer->postprocessingManager->postPass) {
                eng.renderer->postprocessingManager->postPass->depthVizMode = app.depthDisplayMode;
            }
            const char* names[4] = { "off", "depth", "world pos", "normal" };
            LOGI("Depth overlay: %s (occlusion always on)", names[app.depthDisplayMode]);
        }

        updateCameraFromXr(eng, views);

        const float dt = (app.lastFrameTimeSec >= 0.0f) ? (timeSec - app.lastFrameTimeSec) : 0.0f;
        app.lastFrameTimeSec = timeSec;

        // Smoothed fps for the HUD (Scene::fps feeds the post-pass digits).
        if (dt > 0.0f) {
            const float inst = 1.0f / dt;
            eng.scene->fps = (eng.scene->fps <= 0.0f) ? inst : glm::mix(eng.scene->fps, inst, 0.05f);
        }

        // World zoom: left stick Y (push up = zoom in) scales the sculpt
        // volume about its center (exponential so it feels even in both
        // directions). The engine's brush/raymarch/cull math is model-aware,
        // so sculpting keeps working at any zoom; the bounds cage and brush
        // indicator scale with it.
        const float zoomInput = stickDeadzone(app.input.stickL.y);
        if (zoomInput != 0.0f && dt > 0.0f) {
            app.worldScale = glm::clamp(app.worldScale * std::exp(zoomInput * 0.9f * dt), 0.3f, 3.0f);
        }
        const glm::vec3 pivot(-0.03125f); // chunk center: -0.5 + 30/64 per axis
        eng.scene->objects[0].ModelMatrix =
            glm::translate(glm::mat4(1.0f), pivot) *
            glm::scale(glm::mat4(1.0f), glm::vec3(app.worldScale)) *
            glm::translate(glm::mat4(1.0f), -pivot);

        // Hold left grip for the block-wireframe debug view.
        eng.scene->showBlockWireframe = app.input.wireframeDown;

        eng.scene->update(timeSec); // maps frame UBOs; records cmd bufs once

        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XR_CHECK(xrAcquireSwapchainImage(app.swapchain, &acquireInfo, &imageIndex));
        XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        imageWait.timeout = XR_INFINITE_DURATION;
        XR_CHECK(xrWaitSwapchainImage(app.swapchain, &imageWait));

        // Pre-recorded command buffer: multiview scene pass + copy into XR
        // image i. Per-image fence keeps re-submission off in-flight buffers.
        VkFence fence = app.frameFences[imageIndex];
        VK_CHECK(vkWaitForFences(app.device, 1, &fence, VK_TRUE, UINT64_MAX));
        VK_CHECK(vkResetFences(app.device, 1, &fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &eng.base->commandBuffers[imageIndex];
        VK_CHECK(vkQueueSubmit(app.queue, 1, &submit, fence));
        app.lastSubmittedFence = fence;

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
        endInfo.layerCount = layerCount;
        endInfo.layers = layers;
    }

    XR_CHECK(xrEndFrame(app.session, &endInfo));

    if (frameState.shouldRender) {
        // Compute (brush stamps + draw-list rebuild) after submission,
        // mirroring the desktop frame order (clines->update after present).
        eng.sdfEditor->update(timeSec);
    }
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
    Engine eng;
    app.androidApp = androidApp;
    androidApp->userData = &app;
    androidApp->onAppCmd = onAppCmd;

    LOGI("BakeItQuest starting (Phase 3: controller sculpting)");
    try {
        extractShaderAssets(androidApp);
        createXrInstance(app);
        initEngineVulkan(app, eng);
        createSessionAndSwapchain(app);
        createPassthrough(app);
        requestDepthPermissionOrSetup(app, eng);
        createXrInput(app);
        initEngineScene(app, eng);
    } catch (const std::exception& e) {
        LOGE("Engine init failed: %s", e.what());
        std::abort();
    }

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
            try {
                renderFrame(app, eng);
            } catch (const std::exception& e) {
                LOGE("Frame failed: %s", e.what());
                std::abort();
            }
        }
    }

    // Process teardown: the OS reclaims everything; just stop GPU work first
    // so the runtime isn't mid-read when the heap unwinds.
    if (app.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(app.device);
    }
    if (app.swapchain != XR_NULL_HANDLE) xrDestroySwapchain(app.swapchain);
    if (app.localSpace != XR_NULL_HANDLE) xrDestroySpace(app.localSpace);
    if (app.session != XR_NULL_HANDLE) xrDestroySession(app.session);
    if (app.xrInstance != XR_NULL_HANDLE) xrDestroyInstance(app.xrInstance);
    LOGI("BakeItQuest exiting");
}
