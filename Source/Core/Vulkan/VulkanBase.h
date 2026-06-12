#pragma once
#include "Platform/Platform.h"
#ifdef BAKEIT_PLATFORM_DESKTOP
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#else
#include <vulkan/vulkan.h>
struct GLFWwindow; // opaque; init(nullptr) on platforms without GLFW
#endif
#include <vector>
#include "VulkanPlatformHooks.h"
#include "VulkanDevice.h"
#include "SwapChain.h"
#include "Framebuffer.h"
#include "Core/Pipelines/GraphicsPipeline.h"
#include "Core/Resources/VulkanBuffer.h"
#include "Core/Resources/VulkanDescriptor.h"
class VulkanBase {

public:
	// Platform-owned bootstrap hooks; fill before init() (Quest/OpenXR).
	// Left defaulted on desktop - see VulkanPlatformHooks.h.
	VulkanPlatformHooks platformHooks;

	VkInstance vkinstance;
	VkSurfaceKHR vksurface;
	VkPhysicalDevice vkPhysicalDevice;
	VkDevice device;
	VkQueue queue;
	GLFWwindow* window;
	VkDebugUtilsMessengerEXT vkDebugMessenger;
	VkSampleCountFlagBits msaaSamples;
	VulkanDevice* vulkandevice;
	SwapChain* swapChain = nullptr; // desktop only; stays null on Quest
	//Framebuffer* offscreenbuffer;
	VkPipelineCache pipelineCache;
//	GraphicsPipeline* objectPipeline;
	//GraphicsPipeline* quadPipeline;
	VBuffer* vbuffer;
	VulkanDescriptor* vulkandescriptor;
	std::vector<VkSemaphore> imageAvailableSemaphores;
	std::vector<VkSemaphore> renderFinishedSemaphores;
	std::vector<VkFence> inFlightFences;
	std::vector<VkFence> imagesInFlight;
	std::vector<VkCommandBuffer>  commandBuffers;
	size_t currentFrame = 0;
	uint32_t imageIndex;
	std::vector<VkCommandBuffer> submitCommandBuffers;
	// Presentation-surface properties. Engine code must use these instead of
	// dereferencing swapChain: on the Quest build the extent/format come from
	// the OpenXR swapchain, not VkSwapchainKHR (see Platform/Platform.h).
	// Direct swapChain access is reserved for VulkanBase internals and
	// desktop-only subsystems (ImGui, the OpenVR path).
	VkExtent2D presentExtent() const;
	VkFormat presentFormat() const;

	void init(GLFWwindow* window);
	void setupSurface();
	// Public for the Quest path: the XR swapchain (whose image count sizes
	// the command-buffer array) can only be created after init() has built
	// the device, so the platform layer fills platformHooks.presentImage*
	// and then calls this. Inside init() it no-ops while the count is 0.
	void createCommandBuffers();
	void beginCommandBuffers(unsigned int index);
	uint32_t getFrameIndex();
	void presentQueue(VkResult &result);
	void recreate();
	VulkanBase();
	~VulkanBase();

private:
	
	const int MAX_FRAMES_IN_FLIGHT = 2;
	
	void createInstance();
	void pickPhysicalDevice();
	bool checkValidationLayerSupport();
	std::vector<const char*> getRequiredExtensions();
	void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo);
	void setupDebugMessenger();
	VkResult CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger);
	void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator);
	void createSyncObjects();
};