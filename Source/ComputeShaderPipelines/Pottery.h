#pragma once
#include "../Scene/Scene.h"
#include "Core/Pipelines/ComputePipeline.h"
#include <glm/glm.hpp>
#include <vector>
class Pottery
{
public:
	struct SnapshotChunk
	{
		size_t objectIndex = 0;
		size_t meshIndex = 0;
		size_t sboIndex = 0;
		int usedBricks = 0;
		glm::vec3 baseShadowCenterWS = glm::vec3(0.0f);
		float shadowRadius = 0.0f;
		glm::vec3 bounceColor = glm::vec3(0.0f);
		std::vector<glm::vec4> baseSpatial;
		float spawnTime = 0.0f;
		float stopOffsetY = 0.0f;
	};

	std::vector<UBO> computeUBO;
	std::vector<SBO> computeSBO;
	std::vector<SBO> lod1SBO;
	ComputePipeline* comp;
	ComputePipeline* lod1Builder;
	VulkanBase* base;
	Scene* scene;
	glm::vec3 chunkTranslationWS = glm::vec3(0.0f);
	glm::vec3 uploadedChunkTranslationWS = glm::vec3(1e9f);
	uint32_t chunkBlocksPerAxis = 64u;
	uint32_t chunkBlockDim = 8u;
	float chunkBlockSizeWS = 1.0f / 64.0f;
	uint32_t lod1BlocksPerAxis = 32u;
	float lod1BlockSizeWS = 2.0f / 64.0f;
	bool wasBrushEditing = false;
	bool wasBrushPointerDown = false;
	glm::dvec2 previousBrushCursorPos = glm::dvec2(0.0);
	bool pendingFullChunkResolve = true;
	bool pendingStrictSurfaceScan = false;
	bool pendingLod1Rebuild = false;
	bool lod1RebuildSubmitted = false;
	uint32_t strictSurfaceScanCursor = 0;
	std::vector<int32_t> strictScanBlockState;
	std::vector<int32_t> strictScanRenderableIds;
	std::vector<int32_t> fineActiveIds;
	std::vector<int32_t> lod1ActiveIds;
	int lod1ObjectIndex = -1;
	int lod1MeshIndex = -1;
	bool coarseLodSelected = false;
	std::vector<SnapshotChunk> snapshotChunks;
	std::vector<std::vector<SBO>> snapshotChunkSBOs;
	Pottery(VulkanBase* base, Scene* scene);
	~Pottery();
	void update(float time);
	void uploadChunkSpatialData();
	void createLodObjects();
	void rebuildLod1();
	void updateLodSelection();
	void createStaticFloorObject();
	void finalizeStrokeChunk(float time);
	void updateSnapshotChunks(float time);
	void clearEditableChunk();
};

