#pragma once
#include "../Scene/Scene.h"
#include "../Graphics/ComputePipeline.h"
class Pottery
{
public:
	std::vector<UBO> computeUBO;
	std::vector<SBO> computeSBO;
	PGEResourceTypes::Texture3D computeTexture;
	ComputePipeline* comp;
	VulkanBase* base;
	Scene* scene;
	Pottery(VulkanBase* base, Scene* scene);
	~Pottery();
	void update(float time);
};

