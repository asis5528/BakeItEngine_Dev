#pragma once
#include "../Scene/Scene.h"
#include "../Graphics/ComputePipeline.h"
#include <glm/glm.hpp>
#include <vector>
class Pottery
{
public:
	struct alignas(16) CsgOperationGpu
	{
		glm::vec4 centerRadius = glm::vec4(0.0f);
		glm::vec4 color = glm::vec4(1.0f);
		glm::vec4 meta = glm::vec4(0.0f); // x action, y shape, z smoothness, w flags
	};

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
	static constexpr uint32_t MaxCsgOperations = 16384u;
	bool wasBrushEditing = false;
	bool wasBrushPointerDown = false;
	bool pendingEditableSync = true;
	uint32_t liveEditRefreshCounter = 0;
	glm::dvec2 previousBrushCursorPos = glm::dvec2(0.0);
	std::vector<int32_t> cachedEditableBlockState;
	std::vector<int32_t> fineActiveIds;
	std::vector<int32_t> lod1ActiveIds;
	std::vector<int32_t> renderedFineIds;
	std::vector<int32_t> renderedLod1Ids;
	int lod1ObjectIndex = -1;
	int lod1MeshIndex = -1;
	bool coarseLodSelected = false;
	std::vector<CsgOperationGpu> csgOperations;
	std::vector<SnapshotChunk> snapshotChunks;
	std::vector<std::vector<SBO>> snapshotChunkSBOs;
	Pottery(VulkanBase* base, Scene* scene);
	~Pottery();
	void update(float time);
	void uploadChunkSpatialData();
	void createLodObjects();
	void rebuildLod1();
	void updateLodSelection();
	void uploadCsgOperations();
	void rebuildEditableChunkFromCsg();
	void finalizeStrokeChunk(float time);
	void updateSnapshotChunks(float time);
	void clearEditableChunk();
};

