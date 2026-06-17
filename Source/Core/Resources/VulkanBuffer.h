#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include "Core/Pipelines/GraphicsPipeline.h"
#include "World/Mesh.h"
#include "Core/Pipelines/Shader.h"
#include "VulkanImage.h"
#include "Core/Vulkan/VulkanDevice.h"
#include "Core/Resources/ResourceTypes.h"

struct VBuffer {
	VulkanDevice* vulkandevice;
	VBuffer(VulkanDevice* vulkandevice);
	void createVertexBuffer(Mesh& mesh); 
	void createSkinnedVertexBuffer(Mesh& mesh);
	void createIndexBuffer(Mesh& mesh);
	void createInstanceIdBuffer(Mesh& mesh, uint32_t instanceCount);
	void updateInstanceIdBuffer(Mesh& mesh, const std::vector<int32_t>& instanceIds);
	// GPU-driven draw list: host-visible VkDrawIndexedIndirectCommand the
	// compaction compute (or the CPU helpers below) writes the instance
	// count into; Mesh::draw consumes it via vkCmdDrawIndexedIndirect.
	void createIndirectDrawBuffer(Mesh& mesh);
	void setIndirectInstanceCount(Mesh& mesh, uint32_t count);
	uint32_t readIndirectInstanceCount(Mesh& mesh);
	void updateIndirectInstanceList(Mesh& mesh, const std::vector<int32_t>& instanceIds);
	std::vector<UBO> createUniformBuffers(std::vector<uint64_t> Buffersize);
	std::vector<SBO> createStorageBuffers(std::vector<uint64_t> bufferSizes);
//	std::vector 
	ResourceTypes::Texture createTexturefromBuffer(VulkanTexture& texture, void* pixels);
	ResourceTypes::Texture3D createTexture3DGeneral(int w,int h,int d);
	// Single-channel FP16 3D texture in GENERAL layout, usable as both a storage
	// image (compute write) and a linear-filtered sampler3D (render read). Used
	// for the dense coarse-LOD SDF field. TRANSFER_DST so it can be cleared.
	ResourceTypes::Texture3D createTexture3DR16F(int w,int h,int d);
	ResourceTypes::Texture3D createTexture3DfromBuffer(VulkanTexture3D& texture, void* pixels);
	void transitionImageLayout(VkImage image, VkImageAspectFlags aspectMask, VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t mipLevels, uint32_t layerCount = 1);
	void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height,uint32_t depth);
	void generateMipmaps(VkImage image, VkFormat imageFormat, int32_t texWidth, int32_t texHeight, uint32_t mipLevels);
	VkSampler createTextureSampler(uint32_t miplevels);

};



