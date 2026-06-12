#pragma once
#include "Platform/Platform.h"
#include <cstdint>
#include <vulkan/vulkan.h>
#include <array>
#include "Core/Resources/VulkanImage.h"
#include "VulkanDevice.h"
class Framebuffer
{
	public:
		uint32_t width;
		uint32_t height;
		VulkanDevice *vulkandevice;
		//VkDevice device;
		//VkPhysicalDevice physicalDevice;
		VkSampleCountFlagBits msaaSamples;
		VkRenderPass renderPass;
		VkFormat colorFormat;
		VkFormat depthFormat;
		VulkanImage* ColorImage;
		VulkanImage* AuxColorImage;
		VulkanImage* MotionVectorImage;
		VulkanImage* DepthImage;
		VulkanImage* MultisampledColorImage;
		VulkanImage* MultisampledAuxColorImage;
		VulkanImage* MultisampledMotionVectorImage;
		VulkanImage* MultisampledDepthImage;
		VkFramebuffer framebuffer;
		// Single-layer 2D view of the display color image (layer 0). Use this
		// for consumers whose shaders sample a plain sampler2D (e.g. the ImGui
		// viewport) - the attachment views are 2D_ARRAY when multiLayerView > 1.
		VkImageView displayImageView = VK_NULL_HANDLE;
		int multiLayerView;
		// True multiview rendering (subpass viewMask 0b11): one draw rasterizes
		// into both array layers, gl_ViewIndex selecting the eye. Desktop leaves
		// this off (layered images exist but only view 0 is drawn); the Quest
		// path turns it on so both eyes render in a single pass.
		bool multiviewEnable = false;
		Framebuffer(VulkanDevice *vulkandevice , uint32_t width, uint32_t height,VkSampleCountFlagBits msaaSamples,VkFormat colorFormat, VkFormat depthFormat, int MultiLayerView = 1, bool multiviewEnable = false);
		~Framebuffer();
		void clean();
		void recreate();
		void createRenderPass();
		void createFrameBuffer();
		void beginRenderPass(VkCommandBuffer commandBuffer);
};

