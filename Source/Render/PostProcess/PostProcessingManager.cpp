#include "PostProcessingManager.h"
#include "World/Scene.h"

PostProcessingManager::PostProcessingManager(VulkanBase* base, Scene* scene, Framebuffer* previousFB)
{
	this->base = base;
	this->previousFB = previousFB;
	scene->camera->view[0] = glm::mat4(1.0f);
	postPass = new PostProcessPass(base, scene, previousFB);
	// Expose only the final output framebuffer (what the Editor viewport shows).
	if (!postPass->framebuffers.empty()) {
		framebuffers.push_back(postPass->framebuffers[0]);
	}
	bufferSize = framebuffers.size();
}

void PostProcessingManager::createDescriptors()
{
	postPass->createDescriptors();
}

void PostProcessingManager::renderPass(VkCommandBuffer commandBuffer)
{
	postPass->renderPass(commandBuffer);
}
void PostProcessingManager::update()
{
	postPass->update();
}
void PostProcessingManager::recreate(VkExtent2D ex)
{
	postPass->recreate(ex);
}

PostProcessingManager::~PostProcessingManager()
{
	delete postPass;
}
