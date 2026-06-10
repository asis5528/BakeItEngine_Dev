#pragma once
#include "Core/Vulkan/VulkanBase.h"
#include "World/Scene.h"
#include "Core/Pipelines/ComputePipeline.h"
#include "EngineContext.h"
class GaussianFluidSim
{

public:
    VulkanBase* base;
    Scene* scene;
    ResourceTypes::Texture3D computeTexture;
    std::vector<UBO> computeUBO;
    ComputePipeline* comp;

    ResourceTypes::Texture3D computeTexture2;
    std::vector<UBO> computeUBO2;
    ComputePipeline* comp2;

    ResourceTypes::Texture3D computeTexture3;
    std::vector<UBO> computeUBO3;
    ComputePipeline* comp3;

    int Frame = 0;
    GaussianFluidSim(EngineContext& ctx, ResourceTypes::Texture3D sdftex);
    ~GaussianFluidSim();
    void update(float time);
};

