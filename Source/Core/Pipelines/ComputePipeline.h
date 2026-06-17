#pragma once
#include "Core/Vulkan/VulkanBase.h"
#include "World/Scene.h"

struct ComputeInput {
    std::string computeShaderPath;
    std::vector<VkDescriptorType> descriptorTypes;
    // Optional push-constant block size (bytes), VK_SHADER_STAGE_COMPUTE_BIT,
    // offset 0. 0 = none. Lets a single pipeline be re-dispatched with small
    // per-dispatch params (e.g. a region offset + phase) without a UBO rebind.
    uint32_t pushConstantSize = 0;
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

