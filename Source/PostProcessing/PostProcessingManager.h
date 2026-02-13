#pragma once
#include <vector>
#include "../Graphics/Framebuffer.h"
#include "../quad.h"
#include "VolumeRendering.h"
class Scene;
class PostProcessingManager
{
	public:
		std::vector<Quad*> quads;
		Scene* scene;
		Framebuffer* previousFB;
		std::vector<Framebuffer*> framebuffers;
		VulkanBase* base;
		VolumeRendering *volumeRendering;
		int bufferSize;

		PostProcessingManager(VulkanBase *base,Scene *scene,Framebuffer *previousFB);
		void renderPass(VkCommandBuffer commandBuffer);
		void createDescriptors();
		void update();
		void recreate(VkExtent2D ex);
		~PostProcessingManager();
};

