cmake_minimum_required(VERSION 3.22.1)
project(bakeit_quest)

# Phase 1 skeleton: OpenXR + Vulkan clear-color in the headset. Phase 2 adds
# the engine sources from ../../../../../Source (Core/Features/World render
# core only - desktop subsystems stay out, see Source/Platform/Platform.h).

find_package(OpenXR REQUIRED CONFIG)

add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(bakeit_quest SHARED
    main_android.cpp)

target_compile_features(bakeit_quest PRIVATE cxx_std_17)
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
