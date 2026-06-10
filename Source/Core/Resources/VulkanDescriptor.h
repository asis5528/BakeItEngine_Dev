#pragma once
#include "Core/Vulkan/VulkanDevice.h"
#include <array>
#include <vector>
#include "World/Mesh.h"
#include "Core/Pipelines/Shader.h"
#include "Core/Pipelines/GraphicsPipeline.h"
#include "VulkanImage.h"


struct VulkanDescriptor
{
	VulkanDevice * vulkandevice;
	VulkanDescriptor (VulkanDevice *vulkandevice);
	void createDescriptorPool(uint32_t size,std::vector<VkDescriptorType> descriptorTypes,  uint32_t maxsets,VkDescriptorPool * descriptorPool);
	std::vector<VkDescriptorSet> createDescriptorSets(uint32_t size, std::vector<VkDescriptorSetLayout> layouts,VkDescriptorPool descriptorPool);
	void updateDescriptorSets(VkDescriptorSet descriptorSet,std::vector<UBO> ubos,std::vector<VulkanTexture> textures, std::vector<SBO> sbos);
}
;

