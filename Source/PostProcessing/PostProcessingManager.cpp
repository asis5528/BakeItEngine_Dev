#include "PostProcessingManager.h"
#include "../Scene/Scene.h"

PostProcessingManager::PostProcessingManager(VulkanBase* base, Scene* scene, Framebuffer* previousFB)
{
	this->base = base;
	this->previousFB = previousFB;
	scene->view[0] = glm::mat4(1.0f);
	volumeRendering = new VolumeRendering(base, scene,previousFB);
	// Expose only final output framebuffer; history buffers stay internal to VolumeRendering.
	if (!volumeRendering->framebuffers.empty()) {
		framebuffers.push_back(volumeRendering->framebuffers[0]);
	}
	bufferSize = framebuffers.size();
}

void PostProcessingManager::createDescriptors()
{
	volumeRendering->createDescriptors();
}

void PostProcessingManager::renderPass(VkCommandBuffer commandBuffer)
{
	volumeRendering->renderPass(commandBuffer);
}
void PostProcessingManager::update()
{
	//update UBOs if any
	volumeRendering->update();
}
void PostProcessingManager::recreate(VkExtent2D ex)
{
	volumeRendering->recreate(ex);
}

PostProcessingManager::~PostProcessingManager()
{
	delete volumeRendering;
}
