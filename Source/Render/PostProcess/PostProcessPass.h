#pragma once
#include <vector>
#include <glm/glm.hpp>
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
		bool depthVizActive = false; // Quest depth bound to binding 4 (enables occlusion)
		int  depthVizMode = 0;       // fullscreen debug overlay: 0 off, 1 depth, 2 world pos, 3 normal
		// Depth-camera FOV tangents (tanL, tanR, tanD, tanU) per eye, set each frame
		// from the acquired XR depth views. The FS remaps each eye-ray through these
		// to fullscreen-reproject the depth so it lines up with the camera view.
		glm::vec4 depthFovTan[2] = { glm::vec4(0.0f), glm::vec4(0.0f) };
		PostProcessPass(VulkanBase* base, Scene *scene, Framebuffer* previousFB);
		void createDescriptors();
		void update();
		void renderPass(VkCommandBuffer commandBuffer);
		void recreate(VkExtent2D ex);
		// Bind the Quest environment-depth texture (linear metric depth) to binding 4.
		void setDepthDisplay(VkImageView view, VkSampler sampler);
		~PostProcessPass();
};
