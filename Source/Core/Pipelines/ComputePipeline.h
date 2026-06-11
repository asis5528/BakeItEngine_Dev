#pragma once
#include "Core/Vulkan/VulkanBase.h"
#include "World/Scene.h"

struct ComputeInput {
    std::string computeShaderPath;
    std::vector<VkDescriptorType> descriptorTypes;

};


class ComputePipeline
{

public:
    VkDescriptorSetLayout descriptorSetLayout;
    VkPipelineLayout pipelineLayout;
    VkDescriptorSet descriptorSet;
    VkPipeline computePipeline;
    VkCommandPool commandPool;
    VkDescriptorPool computeDescriptorPool;
    VkCommandBuffer computeCommandBuffer;
    VkFence computeFence;
    VkShaderModule shaderModule;
    VulkanBase *base;


    ComputeInput m_input;
    bool commandResourcesCreated = false;
     ComputePipeline( VulkanBase* base, ComputeInput input);
     ~ComputePipeline();
     void setupDescriptors(std::vector <UBO> ubos, std::vector<VulkanTexture> textures,std::vector<SBO> sbos);
     void ensureCommandResources();
     void dispatch(uint32_t group_x, uint32_t group_y, uint32_t group_z);
     void submit();
     void submit(uint32_t group_x, uint32_t group_y, uint32_t group_z);
};

