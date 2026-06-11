#pragma once

// Platform seam for the Windows-desktop / Quest-3-standalone split.
//
// BAKEIT_PLATFORM_DESKTOP - Windows editor build: GLFW window + VkSwapchainKHR,
//                           mouse/keyboard input, ImGui editor, Assimp loader,
//                           shaders recompiled at startup via compile.bat.
// BAKEIT_PLATFORM_ANDROID - Quest standalone build: no window (OpenXR owns the
//                           XrSwapchain), controller input via OpenXR actions,
//                           features-demo only (no ImGui/Assimp), precompiled
//                           .spv shipped as APK assets.
//
// Rule of thumb when porting: code under Core/, Features/, World/ and the
// shaders must stay platform-free; anything that needs an #ifdef here should
// live behind an interface in Platform/ instead, with main.cpp (desktop) or
// the Android entry point choosing the implementation.

#if defined(__ANDROID__)
    #define BAKEIT_PLATFORM_ANDROID 1
#else
    #define BAKEIT_PLATFORM_DESKTOP 1
#endif
