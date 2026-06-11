#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include "VulkanPlatformHooks.h"

struct VulkanDevice
{
	// Set by VulkanBase before createLogicalDevice(); may carry a device
	// creation wrapper and an extension-list override (Quest/OpenXR).
	const VulkanPlatformHooks* platformHooks = nullptr;

	VkPhysicalDevice physicalDevice;
	VulkanDevice(VkPhysicalDevice physicalDevice);
	~VulkanDevice();
	VkSampleCountFlagBits getMaxUsableSampleCount();
	uint32_t findQueueFamilies(VkPhysicalDevice device);
	void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory);
	void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
	VkCommandBuffer beginSingleTimeCommands();
	void endSingleTimeCommands(VkCommandBuffer commandBuffer);
	void createLogicalDevice();
	void createCommandPool();
	VkDevice device;
	VkQueue Queue;
	uint32_t queueID;
	VkCommandPool commandPool;

	
};

