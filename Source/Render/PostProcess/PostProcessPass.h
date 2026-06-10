#pragma once
#include <vector>
#include "Core/Vulkan/Framebuffer.h"
#include "Quad.h"

class Scene;

// The post-processing pass. Currently a plain passthrough (offscreen color
// copied to the output framebuffer the Editor viewport displays); effects
// plug in here as fullscreen quads when they earn their place.
class PostProcessPass
{
	public:
		std::vector<Quad*> quads;
		std::vector<Framebuffer*> framebuffers;
		VulkanBase *base;
		Scene* scene;
		Framebuffer* previousFB;
		PostProcessPass(VulkanBase* base, Scene *scene, Framebuffer* previousFB);
		void createDescriptors();
		void update();
		void renderPass(VkCommandBuffer commandBuffer);
		void recreate(VkExtent2D ex);
		~PostProcessPass();
};
