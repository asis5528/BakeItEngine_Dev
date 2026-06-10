#pragma once
#include <vector>
#include "Core/Pipelines/GraphicsPipeline.h"
#include "Core/Vulkan/VulkanBase.h"
class ScenePipelines
{
public:
	std::vector<GraphicsPipeline*> pipelines;
	VulkanBase* base;
	//VkRenderPass renderPass;
	ScenePipelines(VulkanBase *base , VkRenderPass renderPass);
	void recreate(VkRenderPass renderPass, VkExtent2D extent);
	~ScenePipelines();
};

