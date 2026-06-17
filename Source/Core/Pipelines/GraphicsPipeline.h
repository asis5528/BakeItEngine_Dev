#pragma once
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <array>
#include <string>
#include <vector>
#include "World/Mesh.h"


   
    struct descriptorTypes {
        VkDescriptorType descriptorType;
        VkShaderStageFlagBits shaderStage;
    };

    struct PipelineInput {
        std::string vertexShaderpath;
        std::string fragmentShaderpath;
        VkRenderPass renderPass;
        VkExtent2D viewportExtent;
        VkSampleCountFlagBits msaaSamples;
        int MultiviewLayer = 1;
        VkDevice device;
        bool skinned = false;
        bool useInstanceIdAttribute = false;
        uint32_t colorAttachmentCount = 1;
      //  VkPipelineCache pipelineCache;
        std::vector<descriptorTypes> descriptorTypes;

    };
    struct PipelineInfoLocal {       
        VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
        VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
        std::vector<VkPipelineShaderStageCreateInfo> shaderStagess;
        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        VkVertexInputBindingDescription bindingDescription;
        std::vector<VkVertexInputBindingDescription> bindingDescriptions;
       // std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions;
        std::vector< VkVertexInputAttributeDescription> attributeDescriptions;
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        VkViewport viewport{};
        VkRect2D scissor{};
        VkPipelineViewportStateCreateInfo viewportState{};
        VkPipelineRasterizationStateCreateInfo rasterizer{};
        VkPipelineMultisampleStateCreateInfo multisampling{};
        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments;
        VkPipelineColorBlendStateCreateInfo colorBlending{};

    };
class GraphicsPipeline
{

public:
    PipelineInput pipelineInput;
    VkDescriptorSetLayout descriptorSetLayout;
    VkPipelineLayout pipelineLayout;
    VkGraphicsPipelineCreateInfo pipelineInfo ;
    VkPipeline graphicsPipeline;
    PipelineInfoLocal infoLocal;
    std::vector<uint64_t> bufferSize;
    bool frontFaceCull = 0;
   // void k(PipelineInput* pipelineInput);
    ~GraphicsPipeline();
    void clean();
    void setupPipeline(PipelineInput pipelineInput);
    void recreatePipeline(VkExtent2D resolution,VkRenderPass renderPass);
    void createGraphicsPipeline();
    // Live shader reload: re-read the (freshly recompiled) .spv files, swap the
    // shader modules and rebuild only the pipeline. Layout / descriptor set
    // layout / fixed-function state are untouched. Used by ShaderHotReload.
    void reloadShaders();
private:
    void createDescriptorSetLayout();
    VkShaderModule createShaderModule(const std::vector<char>& code);
};

