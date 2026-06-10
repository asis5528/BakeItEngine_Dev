#pragma once
#include "Core/Vulkan/VulkanBase.h"
#include "World/Scene.h"
#include "Core/Pipelines/ComputePipeline.h"
#include "Features/Fluid/GaussianFluidSim.h"
#include "Features/Volumetric/Bake/MeshToSdf.h"
#include "Features/Volumetric/Edit/Pottery.h"
class ComputeShaderPipelines
{


public:
    VulkanBase* base;
    Scene* scene;
   // VulkanTexture ct3D;
  //  VulkanTexture3D ct3D2;

    //VulkanTexture3D ex3D;
   // VulkanTexture3D ex3D2;
    std::vector<UBO> computeUBO;
    std::vector<UBO> computeUBO2;
    ComputePipeline* comp;
    ComputePipeline* comp2;
    int Frame = 0;

    GaussianFluidSim *fluidSim;
    MeshToSdf *meshtosdf;
    Pottery* pottery;
    ComputeShaderPipelines(VulkanBase* base, Scene* scene);
    ~ComputeShaderPipelines();
    void update(float time);
    void createLodObjects();
    void createStaticFloorObject();



   
};

