#pragma once
#include <vector>
#include "Core/Vulkan/Framebuffer.h"
#include "Quad.h"

class Scene;

// The post-processing pass. Passthrough of the offscreen scene color to the
// output framebuffer the Editor viewport displays, plus the brush indicator
// overlay (analytic outline of the active brush shape, ray-traced in
// PostPassFS); further effects plug in here as fullscreen quads when they
// earn their place.
class PostProcessPass
{
	public:
		std::vector<Quad*> quads;
		std::vector<Framebuffer*> framebuffers;
		std::vector<UBO> indicatorUbo; // camera + brush data for the overlay
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
