#include "Renderer.h"
#include "World/Scene.h"
#include "Core/Vulkan/VulkanUtils.h"
#include "Core/Resources/ResourceManager.h"
#include <stdexcept>

Renderer::Renderer(VulkanBase* base)
{
	this->base = base;
}

void Renderer::init(Scene* scene)
{
	standardSampler = base->vbuffer->createTextureSampler(1);

	Framebuffer* frameBuffer = new Framebuffer(base->vulkandevice, base->presentExtent().width, base->presentExtent().height, base->vulkandevice->getMaxUsableSampleCount(), base->presentFormat(), utils::findDepthFormat(base->vulkandevice->physicalDevice), 2);
	framebuffers.push_back(frameBuffer);
	postprocessingManager = new PostProcessingManager(base, scene, framebuffers[0]);
	scenePipelines = new ScenePipelines(base, framebuffers[0]->renderPass);
	pipelines = scenePipelines->pipelines;
	scene->pipelines = pipelines; // legacy mirror; consumers index scene->pipelines
}

void Renderer::recordCommandBuffers(Scene& scene)
{
	// The command buffers being re-recorded may still be executing on the
	// GPU (two frames in flight); a command buffer must not be in the
	// pending state when vkBeginCommandBuffer is called
	// (VUID-vkBeginCommandBuffer-commandBuffer-00049). The fences are
	// created signaled, so this returns immediately when the GPU is idle.
	if (!base->inFlightFences.empty()) {
		vkWaitForFences(base->device, static_cast<uint32_t>(base->inFlightFences.size()), base->inFlightFences.data(), VK_TRUE, UINT64_MAX);
	}

	for (size_t i = 0; i < base->commandBuffers.size(); i++) {
		VkCommandBufferBeginInfo beginInfo{};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

		if (vkBeginCommandBuffer(base->commandBuffers[i], &beginInfo) != VK_SUCCESS) {
			throw std::runtime_error("failed to begin recording command buffer!");
		}

		framebuffers[0]->beginRenderPass(base->commandBuffers[i]);

		for (unsigned int j = 0; j < scene.objects.size(); j++) {
			if (scene.objects[j].hasMesh) {
				for (int meshI = 0; meshI < scene.objects[j].meshID.size(); meshI++) {
					Mesh mesh = scene.meshes[scene.objects[j].meshID[meshI]];
					vkCmdBindPipeline(base->commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, scene.objects[j].shader[meshI].graphicsPipeline->graphicsPipeline);
					vkCmdBindDescriptorSets(base->commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, scene.objects[j].shader[meshI].graphicsPipeline->pipelineLayout, 0, 1, &scene.objects[j].shader[meshI].descriptorSet, 0, nullptr);
					mesh.draw(base->commandBuffers[i]);
				}
			}
		}

		vkCmdEndRenderPass(base->commandBuffers[i]);

		postprocessingManager->renderPass(base->commandBuffers[i]);

		base->swapChain->beginRenderPass(base->commandBuffers[i], i);
		vkCmdEndRenderPass(base->commandBuffers[i]);

		if (vkEndCommandBuffer(base->commandBuffers[i]) != VK_SUCCESS) {
			throw std::runtime_error("failed to record command buffer!");
		}
	}
}

void Renderer::createSceneDescriptor(Scene& scene)
{
	unsigned int objectWithMeshSize = scene.objects.size();
	for (Object& object : scene.objects) {
		if (!object.hasMesh) {
			objectWithMeshSize -= 1;
		}
		else {
			objectWithMeshSize += object.meshID.size() - 1;
		}
	}
	base->vulkandescriptor->createDescriptorPool(objectWithMeshSize,
		{
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
		},
		objectWithMeshSize + 10, &descriptorPool);

	std::vector<VkDescriptorSetLayout> layouts;
	for (int i = 0; i < scene.objects.size(); i++) {
		if (scene.objects[i].hasMesh) {
			for (int meshID = 0; meshID < scene.objects[i].meshID.size(); meshID++) {
				layouts.push_back(scene.objects[i].shader[meshID].graphicsPipeline->descriptorSetLayout);
			}
		}
	}
	std::vector<VkDescriptorSet> descriptorSets = base->vulkandescriptor->createDescriptorSets(objectWithMeshSize, layouts, descriptorPool);

	int j = 0;
	for (int i = 0; i < scene.objects.size(); i++) {
		if (scene.objects[i].hasMesh) {
			for (int meshID = 0; meshID < scene.objects[i].meshID.size(); meshID++) {
				scene.objects[i].shader[meshID].descriptorSet = descriptorSets[j];
				j += 1;
			}
		}
	}

	for (size_t i = 0; i < scene.objects.size(); i++) {
		if (scene.objects[i].hasMesh) {
			Object* object = &scene.objects[i];

			for (int meshID = 0; meshID < scene.objects[i].meshID.size(); meshID++) {
				std::vector<VulkanTexture> descriptorsTextures{};
				Material mat = scene.materials[scene.meshes[object->meshID[meshID]].matID];
				if (mat.type == TexturedMaterial) {
					MaterialTexturedData* dat = (struct MaterialTexturedData*)mat.materialData;
					descriptorsTextures.push_back(ResourceManager::getTexture(scene.ptextures[dat->DiffuseTexture].id));
				}
				base->vulkandescriptor->updateDescriptorSets(object->shader[meshID].descriptorSet, { object->shader[meshID].ubos }, descriptorsTextures, object->shader[meshID].sbos);
			}
		}
	}
	postprocessingManager->createDescriptors();

	scene.rootNode.updateTransoformation(glm::mat4(1.0), scene.objects);
}

void Renderer::cleanDescriptors()
{
	vkDestroyDescriptorPool(base->device, descriptorPool, nullptr);
}

void Renderer::update()
{
	postprocessingManager->update();
}

void Renderer::recreate(Scene& scene)
{
	VkExtent2D ex;
	ex.width = scene.camera->viewportResolution.x;
	ex.height = scene.camera->viewportResolution.y;

	for (Framebuffer*& framebuffer : framebuffers) {
		framebuffer->clean();
		framebuffer->width = ex.width;
		framebuffer->height = ex.height;
		framebuffer->recreate();
	}

	scenePipelines->recreate(framebuffers[0]->renderPass, ex);
	pipelines = scenePipelines->pipelines;
	scene.pipelines = pipelines;

	postprocessingManager->recreate(ex);

	cleanDescriptors();
	createSceneDescriptor(scene);
}

void Renderer::clean()
{
	delete scenePipelines;
	delete postprocessingManager;
	for (Framebuffer*& fb : framebuffers) {
		delete fb;
	}
}
