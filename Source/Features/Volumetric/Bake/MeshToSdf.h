#pragma once
#include "World/Scene.h"
#include "Core/Pipelines/ComputePipeline.h"
#include "EngineContext.h"
class MeshToSdf
{
public:
	std::vector<SBO> computeSBOVertices;
	PGEResourceTypes::Texture3D computeTexture;
	ComputePipeline* comp;
	VulkanBase* base;
	Scene* scene;
	MeshToSdf(EngineContext& ctx);
	~MeshToSdf();
	void update(float time);
};

