#pragma once
#include <vulkan/vulkan.h>
#include <functional>
#include <vector>

// Injection points for platform-owned Vulkan bootstrap (see Platform/Platform.h).
//
// Desktop leaves everything defaulted: VulkanBase creates the instance and
// device itself, scans for a physical device, and drives a GLFW surface +
// VkSwapchainKHR. The Quest entry point fills these from OpenXR
// (XR_KHR_vulkan_enable2), which must wrap creation and dictates the GPU:
//   createInstance     -> xrCreateVulkanInstanceKHR
//   pickPhysicalDevice -> xrGetVulkanGraphicsDevice2KHR
//   createDevice       -> xrCreateVulkanDeviceKHR
//   deviceExtensionOverride -> no VK_KHR_swapchain on Quest (the XrSwapchain
//                              provides the images), multiview still required.
struct VulkanPlatformHooks
{
	// Called instead of vkCreateInstance when set. The create info is fully
	// populated by VulkanBase (layers, debug messenger, instance extensions).
	std::function<VkResult(const VkInstanceCreateInfo&, VkInstance&)> createInstance;

	// Returns the physical device the platform requires. When set, replaces
	// the desktop enumeration scan.
	std::function<VkPhysicalDevice(VkInstance)> pickPhysicalDevice;

	// Called instead of vkCreateDevice when set.
	std::function<VkResult(VkPhysicalDevice, const VkDeviceCreateInfo&, VkDevice&)> createDevice;

	// When non-empty, replaces the default device-extension list wholesale.
	std::vector<const char*> deviceExtensionOverride;
};
