#pragma once
#include <vector>
#include <glm/glm.hpp>
#include "../Graphics/Framebuffer.h"
#include "../quad.h"

class Scene;
class VolumeRendering
{
	public:
		std::vector<Quad*> quads;
		std::vector<Framebuffer*> framebuffers;
		VulkanBase *base;
		Scene* scene;
		std::vector<UBO> volumeUBO;
		glm::mat4 prevViewProj = glm::mat4(1.0f);
		glm::mat4 prevViewProjNoJitter = glm::mat4(1.0f);
		glm::vec2 prevJitter = glm::vec2(0.0f);
		uint32_t taaFrame = 0;
		bool hasTaaHistory = false;
		Framebuffer* previousFB;
		VolumeRendering(VulkanBase* base, Scene *scene, Framebuffer* previousFB);
		void createDescriptors();
		void update();
		void renderPass(VkCommandBuffer commandBuffer);
		void recreate(VkExtent2D ex);
		~VolumeRendering();
};

