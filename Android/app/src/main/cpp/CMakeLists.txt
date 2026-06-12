cmake_minimum_required(VERSION 3.22.1)
project(bakeit_quest)

# Phase 2: the engine's render core compiled into the APK. The Quest entry
# point (main_android.cpp) drives VulkanBase through VulkanPlatformHooks and
# renders the SDF brick scene into the XrSwapchain via multiview.

find_package(OpenXR REQUIRED CONFIG)

# Engine sources live in the repo's Source/ tree; this file sits at
# Android/app/src/main/cpp, so the repo root is five levels up.
get_filename_component(ENGINE_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. ABSOLUTE)
set(ENGINE_SRC ${ENGINE_ROOT}/Source)

add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

# Engine modules compiled into the Quest app. Excluded by design (see
# Source/Platform/Platform.h): SwapChain (VkSwapchainKHR is desktop-only),
# Loader/Model/MathUtils (Assimp), Editor (ImGui), Platform/Desktop (GLFW
# input), ComputeShaderPipelines + Fluid + MeshToSdf (desktop feature wiring;
# the Quest entry point instantiates SdfEditor directly).
set(BAKEIT_ENGINE_SOURCES
    ${ENGINE_SRC}/Core/Pipelines/ComputePipeline.cpp
    ${ENGINE_SRC}/Core/Pipelines/GraphicsPipeline.cpp
    ${ENGINE_SRC}/Core/Resources/ResourceManager.cpp
    ${ENGINE_SRC}/Core/Resources/VulkanBuffer.cpp
    ${ENGINE_SRC}/Core/Resources/VulkanDescriptor.cpp
    ${ENGINE_SRC}/Core/Resources/VulkanImage.cpp
    ${ENGINE_SRC}/Core/Vulkan/Framebuffer.cpp
    ${ENGINE_SRC}/Core/Vulkan/VulkanBase.cpp
    ${ENGINE_SRC}/Core/Vulkan/VulkanDevice.cpp
    ${ENGINE_SRC}/Core/Vulkan/VulkanUtils.cpp
    ${ENGINE_SRC}/Features/Sdf/Edit/SdfDrawList.cpp
    ${ENGINE_SRC}/Features/Sdf/Edit/SdfEditor.cpp
    ${ENGINE_SRC}/World/Animation.cpp
    ${ENGINE_SRC}/World/Material.cpp
    ${ENGINE_SRC}/World/Mesh.cpp
    ${ENGINE_SRC}/World/Object.cpp
    ${ENGINE_SRC}/World/Scene.cpp
    ${ENGINE_SRC}/World/Camera/Camera.cpp
    ${ENGINE_SRC}/Render/Renderer.cpp
    ${ENGINE_SRC}/Render/ScenePipelines.cpp
    ${ENGINE_SRC}/Render/PostProcess/PostProcessingManager.cpp
    ${ENGINE_SRC}/Render/PostProcess/PostProcessPass.cpp
    ${ENGINE_SRC}/Render/PostProcess/Quad.cpp
)

add_library(bakeit_quest SHARED
    main_android.cpp
    ${BAKEIT_ENGINE_SOURCES})

target_include_directories(bakeit_quest PRIVATE
    ${ENGINE_SRC}
    ${ENGINE_ROOT}/ThirdParty/Include)

target_compile_features(bakeit_quest PRIVATE cxx_std_17)
# The engine uses C++ exceptions (std::runtime_error) for fatal errors.
target_compile_options(bakeit_quest PRIVATE -fexceptions -frtti)

target_compile_definitions(bakeit_quest PRIVATE
    XR_USE_PLATFORM_ANDROID
    XR_USE_GRAPHICS_API_VULKAN
    VK_USE_PLATFORM_ANDROID_KHR)

# NativeActivity entry point lives in the glue; keep its exported symbol.
set_target_properties(bakeit_quest PROPERTIES LINK_FLAGS "-u ANativeActivity_onCreate")

target_link_libraries(bakeit_quest
    OpenXR::openxr_loader
    native_app_glue
    vulkan
    android
    log)
