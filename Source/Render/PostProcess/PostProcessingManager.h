#pragma once
#include <vector>
#include "Core/Vulkan/Framebuffer.h"
#include "Quad.h"
#include "PostProcessPass.h"
class Scene;
class PostProcessingManager
{
	public:
		std::vector<Quad*> quads;
		Scene* scene;
		Framebuffer* previousFB;
		std::vector<Framebuffer*> framebuffers;
		VulkanBase* base;
		PostProcessPass *postPass;
		int bufferSize;

		PostProcessingManager(VulkanBase *base,Scene *scene,Framebuffer *previousFB);
		void renderPass(VkCommandBuffer commandBuffer);
		void createDescriptors();
		void update();
		void recreate(VkExtent2D ex);
		~PostProcessingManager();
};
