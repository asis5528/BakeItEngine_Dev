#include "Pottery.h"
#include "../ResourceManager.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <vector>

 struct alignas(16) potteryUBO {
	glm::vec4 BrushData;
	glm::vec4 BrushColor;
	float BrushAction; // 1.0 add, 0.0 remove
	float time;
	float BrushShape; // 0.0 cube, 1.0 sphere
	float BrushSmoothness;
	float BrushEmissive; // 1.0 emissive paint mode on, 0.0 off
	float BrushRecolor; // 1.0 recolor existing blocks
	float _pad0 = 0.0f;
	float _pad1 = 0.0f;
	glm::ivec4 DispatchMeta = glm::ivec4(0); // xyz dispatch base block, w csg op count
	

};

namespace
{
struct alignas(16) BlockSpatialData
{
	glm::vec4 centerAndSize;
};

glm::vec3 ChunkLocalCenterOrigin(float blockSizeWS)
{
	return glm::vec3(-0.5f - 1.5f * blockSizeWS);
}

void BuildChunkSpatialData(
	std::vector<BlockSpatialData>& blockSpatial,
	uint32_t blocksPerAxis,
	float blockSizeWS,
	const glm::vec3& chunkTranslationWS)
{
	const glm::vec3 localCenterOrigin = ChunkLocalCenterOrigin(blockSizeWS);
	for (uint32_t z = 0; z < blocksPerAxis; ++z) {
		for (uint32_t y = 0; y < blocksPerAxis; ++y) {
			for (uint32_t x = 0; x < blocksPerAxis; ++x) {
				const uint32_t blockIndex = x + y * blocksPerAxis + z * blocksPerAxis * blocksPerAxis;
				const glm::vec3 localCenter = localCenterOrigin + glm::vec3(float(x), float(y), float(z)) * blockSizeWS;
				blockSpatial[blockIndex].centerAndSize = glm::vec4(localCenter + chunkTranslationWS, blockSizeWS);
			}
		}
	}
}

void BuildParentChunkSpatialData(
	std::vector<BlockSpatialData>& blockSpatial,
	uint32_t parentBlocksPerAxis,
	float parentBlockSizeWS,
	float childBlockSizeWS,
	const glm::vec3& chunkTranslationWS)
{
	const glm::vec3 childCenterOrigin = ChunkLocalCenterOrigin(childBlockSizeWS);
	const glm::vec3 parentCenterOrigin = childCenterOrigin + glm::vec3(0.5f * childBlockSizeWS);
	for (uint32_t z = 0; z < parentBlocksPerAxis; ++z) {
		for (uint32_t y = 0; y < parentBlocksPerAxis; ++y) {
			for (uint32_t x = 0; x < parentBlocksPerAxis; ++x) {
				const uint32_t blockIndex =
					x + y * parentBlocksPerAxis + z * parentBlocksPerAxis * parentBlocksPerAxis;
				const glm::vec3 localCenter =
					parentCenterOrigin + glm::vec3(float(x), float(y), float(z)) * parentBlockSizeWS;
				blockSpatial[blockIndex].centerAndSize = glm::vec4(localCenter + chunkTranslationWS, parentBlockSizeWS);
			}
		}
	}
}

void ComputeChunkBounds(
	uint32_t blocksPerAxis,
	float blockSizeWS,
	const glm::vec3& chunkTranslationWS,
	glm::vec3& outMinWS,
	glm::vec3& outMaxWS)
{
	const glm::vec3 localCenterOrigin = ChunkLocalCenterOrigin(blockSizeWS);
	const glm::vec3 localCenterLast =
		localCenterOrigin + glm::vec3(float(blocksPerAxis - 1u)) * blockSizeWS;
	const glm::vec3 halfExtent = glm::vec3(0.5f * blockSizeWS);
	outMinWS = chunkTranslationWS + localCenterOrigin - halfExtent;
	outMaxWS = chunkTranslationWS + localCenterLast + halfExtent;
}

void ComputeActiveBlockBounds(
	const std::vector<int32_t>& activeIds,
	uint32_t blocksPerAxis,
	float blockSizeWS,
	const glm::vec3& chunkTranslationWS,
	glm::vec3& outMinWS,
	glm::vec3& outMaxWS)
{
	if (activeIds.empty()) {
		ComputeChunkBounds(blocksPerAxis, blockSizeWS, chunkTranslationWS, outMinWS, outMaxWS);
		return;
	}

	const glm::vec3 localCenterOrigin = ChunkLocalCenterOrigin(blockSizeWS);
	const glm::vec3 halfExtent = glm::vec3(0.5f * blockSizeWS);
	outMinWS = glm::vec3(FLT_MAX);
	outMaxWS = glm::vec3(-FLT_MAX);

	for (int32_t rawId : activeIds) {
		if (rawId < 0) {
			continue;
		}
		const uint32_t id = static_cast<uint32_t>(rawId);
		const uint32_t x = id % blocksPerAxis;
		const uint32_t y = (id / blocksPerAxis) % blocksPerAxis;
		const uint32_t z = id / (blocksPerAxis * blocksPerAxis);
		const glm::vec3 localCenter =
			localCenterOrigin + glm::vec3(float(x), float(y), float(z)) * blockSizeWS;
		const glm::vec3 worldCenter = localCenter + chunkTranslationWS;
		outMinWS = glm::min(outMinWS, worldCenter - halfExtent);
		outMaxWS = glm::max(outMaxWS, worldCenter + halfExtent);
	}
}

constexpr float kSnapshotGravity = 0.35f;
constexpr float kSnapshotFloorY = -0.35f;
constexpr int kBrickSamplesPerBlock = 9 * 9 * 9;
constexpr int kBlockEmissiveFlag = (1 << 30);
constexpr int kBlockIndexMask = kBlockEmissiveFlag - 1;
constexpr int kAtlasDim = 512;
constexpr float kBrushCenterBias = 0.031f;
constexpr bool kForceCoarseLodDebug = false;
constexpr float kCsgFlagEmissive = 1.0f;
constexpr float kCsgFlagRecolor = 2.0f;
constexpr float kLodCoarseScreenRadiusPx = 320.0f;
constexpr float kLodFineScreenRadiusPx = 380.0f;
constexpr size_t kMinRenderableComponentBlocks = 3u;

void FilterTinyConnectedComponents(
	std::vector<int32_t>& activeIds,
	const std::vector<int32_t>& blockState,
	uint32_t blocksPerAxis,
	size_t minKeepSize)
{
	if (activeIds.size() <= minKeepSize || blockState.empty()) {
		return;
	}

	const uint32_t totalBlocks = static_cast<uint32_t>(blockState.size());
	std::vector<uint8_t> activeMask(totalBlocks, 0u);
	std::vector<uint8_t> visited(totalBlocks, 0u);
	for (int32_t rawId : activeIds) {
		if (rawId >= 0 && static_cast<uint32_t>(rawId) < totalBlocks) {
			activeMask[static_cast<uint32_t>(rawId)] = 1u;
		}
	}

	std::vector<int32_t> filtered;
	filtered.reserve(activeIds.size());
	std::vector<uint32_t> stack;
	std::vector<int32_t> component;

	const auto tryPushNeighbor = [&](int nx, int ny, int nz) {
		if (nx < 0 || ny < 0 || nz < 0) return;
		if (nx >= static_cast<int>(blocksPerAxis) || ny >= static_cast<int>(blocksPerAxis) || nz >= static_cast<int>(blocksPerAxis)) return;
		const uint32_t nidx =
			static_cast<uint32_t>(nx) +
			static_cast<uint32_t>(ny) * blocksPerAxis +
			static_cast<uint32_t>(nz) * blocksPerAxis * blocksPerAxis;
		if (activeMask[nidx] == 0u || visited[nidx] != 0u) {
			return;
		}
		visited[nidx] = 1u;
		stack.push_back(nidx);
	};

	for (int32_t rawId : activeIds) {
		if (rawId < 0) {
			continue;
		}
		const uint32_t start = static_cast<uint32_t>(rawId);
		if (start >= totalBlocks || visited[start] != 0u || activeMask[start] == 0u) {
			continue;
		}

		stack.clear();
		component.clear();
		visited[start] = 1u;
		stack.push_back(start);

		while (!stack.empty()) {
			const uint32_t idx = stack.back();
			stack.pop_back();
			component.push_back(static_cast<int32_t>(idx));

			const int x = static_cast<int>(idx % blocksPerAxis);
			const int y = static_cast<int>((idx / blocksPerAxis) % blocksPerAxis);
			const int z = static_cast<int>(idx / (blocksPerAxis * blocksPerAxis));

			tryPushNeighbor(x - 1, y, z);
			tryPushNeighbor(x + 1, y, z);
			tryPushNeighbor(x, y - 1, z);
			tryPushNeighbor(x, y + 1, z);
			tryPushNeighbor(x, y, z - 1);
			tryPushNeighbor(x, y, z + 1);
		}

		if (component.size() >= minKeepSize) {
			filtered.insert(filtered.end(), component.begin(), component.end());
		}
	}

	activeIds.swap(filtered);
}
}

Pottery::Pottery(VulkanBase* base, Scene* scene)
{
	this->base = base;
	this->scene = scene;
	//int size = 64*8;

	int blocksPerAxis = static_cast<int>(chunkBlocksPerAxis);
	int blockDim = static_cast<int>(chunkBlockDim);
	int size = blocksPerAxis * blockDim; // 64 * 8 = 512

	ComputeInput input;
	input.computeShaderPath = "shaders/ComputeShaders/sdfCompute _pottery.spv";
	input.descriptorTypes = {
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
	};
	comp = new ComputePipeline(base, input);
	ComputeInput lodInput;
	lodInput.computeShaderPath = "shaders/ComputeShaders/sdfCompute_lod1.spv";
	lodInput.descriptorTypes = {
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
	};
	lod1Builder = new ComputePipeline(base, lodInput);
	computeUBO = base->vbuffer->createUniformBuffers({ sizeof(potteryUBO) });
	
	int numBlocks = blocksPerAxis * blocksPerAxis * blocksPerAxis; // 64^3 = 262144
	int bricksPerBlock = kBrickSamplesPerBlock;                                     // 729
	const int atlasCapacityBricks = (kAtlasDim * kAtlasDim * kAtlasDim) / kBrickSamplesPerBlock;
	computeSBO = base->vbuffer->createStorageBuffers({
		sizeof(float) * kAtlasDim * kAtlasDim * kAtlasDim,
		sizeof(int) * numBlocks,
		sizeof(int),
		sizeof(glm::vec4) * numBlocks,
		sizeof(int),
		sizeof(BlockSpatialData) * numBlocks,
		sizeof(int),
		sizeof(int) * atlasCapacityBricks,
		sizeof(CsgOperationGpu) * MaxCsgOperations
	});
	int zeroInit = 0;
	computeSBO[2].map(base->device, sizeof(int), &zeroInit);
	computeSBO[4].map(base->device, sizeof(int), &zeroInit);
	computeSBO[6].map(base->device, sizeof(int), &zeroInit);
	std::vector<int32_t> initialFineBlocks(static_cast<size_t>(numBlocks), -999999);
	std::vector<glm::vec4> initialFineColors(static_cast<size_t>(numBlocks), glm::vec4(0.0f));
	computeSBO[1].map(base->device, sizeof(int32_t) * initialFineBlocks.size(), initialFineBlocks.data());
	computeSBO[3].map(base->device, sizeof(glm::vec4) * initialFineColors.size(), initialFineColors.data());
	cachedEditableBlockState.assign(static_cast<size_t>(numBlocks), -999999);
	fineActiveIds.clear();
	renderedFineIds.clear();
	renderedLod1Ids.clear();
	scene->sdfAtlasCapacityBricks = atlasCapacityBricks;
	scene->sdfAtlasUsedBricks = 0;
	scene->sdfAtlasTailBricks = 0;
	scene->sdfAtlasFreeBricks = 0;
	scene->sdfAtlasUsage = 0.0f;

	uploadChunkSpatialData();
	uploadCsgOperations();

	std::vector<SBO> computeDescriptorSBOs = {
		computeSBO[0], // bricks
		computeSBO[1], // blocks
		computeSBO[2], // tail counter
		computeSBO[3], // block colors
		computeSBO[4], // dirty counter
		computeSBO[6], // free stack count
		computeSBO[7], // free stack data
		computeSBO[8]  // csg operations
	};
	comp->setupDescriptors(computeUBO, {}, computeDescriptorSBOs);
	comp->dispatch(1, 1, 1);

	const uint32_t lod1TotalBlocks = lod1BlocksPerAxis * lod1BlocksPerAxis * lod1BlocksPerAxis;
	const uint32_t lod1AtlasCapacityBricks = lod1TotalBlocks;
	lod1SBO = base->vbuffer->createStorageBuffers({
		sizeof(float) * lod1AtlasCapacityBricks * kBrickSamplesPerBlock,
		sizeof(int32_t) * lod1TotalBlocks,
		sizeof(int),
		sizeof(glm::vec4) * lod1TotalBlocks,
		sizeof(BlockSpatialData) * lod1TotalBlocks
	});
	lod1SBO[2].map(base->device, sizeof(int), &zeroInit);
	std::vector<int32_t> initialLod1Blocks(static_cast<size_t>(lod1TotalBlocks), -999999);
	std::vector<glm::vec4> initialLod1Colors(static_cast<size_t>(lod1TotalBlocks), glm::vec4(0.0f));
	lod1SBO[1].map(base->device, sizeof(int32_t) * initialLod1Blocks.size(), initialLod1Blocks.data());
	lod1SBO[3].map(base->device, sizeof(glm::vec4) * initialLod1Colors.size(), initialLod1Colors.data());
	std::vector<BlockSpatialData> lod1Spatial(lod1TotalBlocks);
	BuildParentChunkSpatialData(lod1Spatial, lod1BlocksPerAxis, lod1BlockSizeWS, chunkBlockSizeWS, chunkTranslationWS);
	lod1SBO[4].map(
		base->device,
		sizeof(BlockSpatialData) * lod1Spatial.size(),
		lod1Spatial.data());
	std::vector<SBO> lod1DescriptorSBOs = {
		lod1SBO[0],
		lod1SBO[1],
		lod1SBO[3],
		computeSBO[8]
	};
	lod1Builder->setupDescriptors(computeUBO, {}, lod1DescriptorSBOs);
	lod1Builder->dispatch(1, 1, 1);

}

void Pottery::createLodObjects()
{
	if (lod1ObjectIndex >= 0 || scene->objects.empty() || scene->objects[0].meshID.empty()) {
		return;
	}

	const Mesh& sourceMesh = scene->meshes[scene->objects[0].meshID[0]];
	Mesh coarseMesh{};
	coarseMesh.vertices = sourceMesh.vertices;
	coarseMesh.indices = sourceMesh.indices;
	coarseMesh.color = sourceMesh.color;
	coarseMesh.texID = sourceMesh.texID;
	coarseMesh.matID = sourceMesh.matID;
	base->vbuffer->createVertexBuffer(coarseMesh);
	base->vbuffer->createIndexBuffer(coarseMesh);
	base->vbuffer->createInstanceIdBuffer(
		coarseMesh,
		lod1BlocksPerAxis * lod1BlocksPerAxis * lod1BlocksPerAxis);
	base->vbuffer->updateInstanceIdBuffer(coarseMesh, {});
	scene->meshes.push_back(coarseMesh);
	lod1MeshIndex = static_cast<int>(scene->meshes.size() - 1);

	Object coarseObject{};
	coarseObject.hasMesh = true;
	coarseObject.hasAnimation = false;
	coarseObject.ModelMatrix = glm::mat4(1.0f);
	coarseObject.finalMatrix = glm::mat4(1.0f);
	coarseObject.meshID.push_back(static_cast<uint32_t>(lod1MeshIndex));
	coarseObject.name = "VolumetricLOD1";

	VulkanShader shader{};
	shader.graphicsPipeline = scene->pipelines[4];
	shader.ubos = base->vbuffer->createUniformBuffers(shader.graphicsPipeline->bufferSize);
	shader.sbos = {
		lod1SBO[0],
		lod1SBO[1],
		lod1SBO[2],
		lod1SBO[3],
		lod1SBO[4]
	};
	coarseObject.shader.push_back(shader);
	scene->objects.push_back(coarseObject);
	lod1ObjectIndex = static_cast<int>(scene->objects.size() - 1);

}

void Pottery::uploadChunkSpatialData()
{
	if (chunkTranslationWS == uploadedChunkTranslationWS) {
		return;
	}

	const uint32_t totalBlocks = chunkBlocksPerAxis * chunkBlocksPerAxis * chunkBlocksPerAxis;
	std::vector<BlockSpatialData> blockSpatial(static_cast<size_t>(totalBlocks));
	BuildChunkSpatialData(blockSpatial, chunkBlocksPerAxis, chunkBlockSizeWS, chunkTranslationWS);
	computeSBO[5].map(
		base->device,
		sizeof(BlockSpatialData) * blockSpatial.size(),
		blockSpatial.data());

	if (lod1SBO.size() >= 5) {
		const uint32_t lod1TotalBlocks = lod1BlocksPerAxis * lod1BlocksPerAxis * lod1BlocksPerAxis;
		std::vector<BlockSpatialData> lod1Spatial(static_cast<size_t>(lod1TotalBlocks));
		BuildParentChunkSpatialData(lod1Spatial, lod1BlocksPerAxis, lod1BlockSizeWS, chunkBlockSizeWS, chunkTranslationWS);
		lod1SBO[4].map(
			base->device,
			sizeof(BlockSpatialData) * lod1Spatial.size(),
			lod1Spatial.data());
	}
	uploadedChunkTranslationWS = chunkTranslationWS;
}

void Pottery::uploadCsgOperations()
{
	std::vector<CsgOperationGpu> uploadData = csgOperations;
	uploadData.resize(MaxCsgOperations);
	computeSBO[8].map(
		base->device,
		sizeof(CsgOperationGpu) * uploadData.size(),
		uploadData.data());
	scene->csgOperations.clear();
	scene->csgOperations.reserve(csgOperations.size());
	for (const CsgOperationGpu& op : csgOperations) {
		CsgOperationDebug debugOp{};
		debugOp.centerLocal = glm::vec3(op.centerRadius);
		debugOp.radius = op.centerRadius.w;
		debugOp.color = op.color;
		debugOp.action = (op.meta.x >= 0.5f) ? 1 : 0;
		debugOp.shape = static_cast<int>(op.meta.y + 0.5f);
		debugOp.smoothness = op.meta.z;
		const int flags = static_cast<int>(op.meta.w + 0.5f);
		debugOp.emissive = (flags & int(kCsgFlagEmissive)) != 0;
		debugOp.recolor = (flags & int(kCsgFlagRecolor)) != 0;
		scene->csgOperations.push_back(debugOp);
	}
}

void Pottery::rebuildEditableChunkFromCsg()
{
	const uint32_t totalBlocks = chunkBlocksPerAxis * chunkBlocksPerAxis * chunkBlocksPerAxis;
	std::vector<int32_t> clearedBlocks(totalBlocks, -999999);
	std::vector<glm::vec4> clearedColors(totalBlocks, glm::vec4(0.0f));
	int zero = 0;
	computeSBO[1].map(base->device, sizeof(int32_t) * clearedBlocks.size(), clearedBlocks.data());
	computeSBO[2].map(base->device, sizeof(int), &zero);
	computeSBO[3].map(base->device, sizeof(glm::vec4) * clearedColors.size(), clearedColors.data());
	computeSBO[4].map(base->device, sizeof(int), &zero);
	computeSBO[6].map(base->device, sizeof(int), &zero);
	cachedEditableBlockState.assign(totalBlocks, -999999);
	fineActiveIds.clear();
	lod1ActiveIds.clear();
	renderedFineIds.clear();
	renderedLod1Ids.clear();
	coarseLodSelected = false;

	potteryUBO pubo{};
	pubo.DispatchMeta = glm::ivec4(0, 0, 0, static_cast<int>(csgOperations.size()));
	computeUBO[0].map(base->device, sizeof(potteryUBO), &pubo);

	if (!csgOperations.empty()) {
		comp->submit(chunkBlocksPerAxis, chunkBlocksPerAxis, chunkBlocksPerAxis);
		vkWaitForFences(base->device, 1, &comp->computeFence, VK_TRUE, UINT64_MAX);
	}

	std::vector<int32_t> blockState(totalBlocks, -1);
	computeSBO[1].read(base->device, sizeof(int32_t) * blockState.size(), blockState.data());

	int activeBlockCount = 0;
	fineActiveIds.reserve(totalBlocks);
	for (uint32_t i = 0; i < totalBlocks; ++i) {
		const int32_t newRaw = blockState[static_cast<size_t>(i)];
		if (newRaw >= 0) {
			activeBlockCount++;
			fineActiveIds.push_back(static_cast<int32_t>(i));
		}
	}
	FilterTinyConnectedComponents(fineActiveIds, blockState, chunkBlocksPerAxis, kMinRenderableComponentBlocks);
	cachedEditableBlockState = blockState;
	scene->dirtyBlockCount = static_cast<int>(fineActiveIds.size());
	rebuildLod1();
	int gpuValue = 0;
	int freeBrickCount = 0;
	computeSBO[2].read(base->device, sizeof(int), &gpuValue);
	computeSBO[6].read(base->device, sizeof(int), &freeBrickCount);
	scene->sdfAtlasTailBricks = gpuValue / kBrickSamplesPerBlock;
	scene->sdfAtlasFreeBricks = glm::max(freeBrickCount, 0);
	scene->sdfAtlasUsedBricks = glm::max(scene->sdfAtlasTailBricks - scene->sdfAtlasFreeBricks, 0);
	pendingEditableSync = false;

	Mesh* targetMesh = nullptr;
	for (Object& obj : scene->objects) {
		if (!obj.hasMesh) continue;
		for (size_t i = 0; i < obj.meshID.size(); ++i) {
			if (i >= obj.shader.size()) continue;
			if (obj.shader[i].graphicsPipeline != scene->pipelines[4]) continue;
			targetMesh = &scene->meshes[obj.meshID[i]];
			break;
		}
		if (targetMesh) break;
	}

	if (targetMesh && targetMesh->hasInstanceIdBuffer) {
		base->vbuffer->updateInstanceIdBuffer(*targetMesh, fineActiveIds);
	}
	if (lod1MeshIndex >= 0) {
		base->vbuffer->updateInstanceIdBuffer(scene->meshes[static_cast<size_t>(lod1MeshIndex)], {});
	}

	updateLodSelection();
	if (!coarseLodSelected) {
		vkQueueWaitIdle(base->queue);
		scene->renderPass();
	}
}

void Pottery::clearEditableChunk()
{
	const uint32_t totalBlocks = chunkBlocksPerAxis * chunkBlocksPerAxis * chunkBlocksPerAxis;
	std::vector<int32_t> clearedBlocks(totalBlocks, -999999);
	std::vector<glm::vec4> clearedColors(totalBlocks, glm::vec4(0.0f));
	int zero = 0;
	computeSBO[1].map(base->device, sizeof(int32_t) * clearedBlocks.size(), clearedBlocks.data());
	computeSBO[2].map(base->device, sizeof(int), &zero);
	computeSBO[3].map(base->device, sizeof(glm::vec4) * clearedColors.size(), clearedColors.data());
	computeSBO[4].map(base->device, sizeof(int), &zero);
	computeSBO[6].map(base->device, sizeof(int), &zero);
	cachedEditableBlockState.assign(totalBlocks, -999999);
	fineActiveIds.clear();
	lod1ActiveIds.clear();
	renderedFineIds.clear();
	renderedLod1Ids.clear();
	coarseLodSelected = false;
	csgOperations.clear();
	uploadCsgOperations();
}

void Pottery::rebuildLod1()
{
	lod1ActiveIds.clear();

	lod1Builder->submit(lod1BlocksPerAxis, lod1BlocksPerAxis, lod1BlocksPerAxis);
	vkWaitForFences(base->device, 1, &lod1Builder->computeFence, VK_TRUE, UINT64_MAX);

	const uint32_t totalBlocks = lod1BlocksPerAxis * lod1BlocksPerAxis * lod1BlocksPerAxis;
	std::vector<int32_t> coarseBlocks(totalBlocks, -999999);
	lod1SBO[1].read(base->device, sizeof(int32_t) * coarseBlocks.size(), coarseBlocks.data());

	lod1ActiveIds.reserve(totalBlocks);
	for (uint32_t i = 0; i < totalBlocks; ++i) {
		if (coarseBlocks[i] >= 0) {
			lod1ActiveIds.push_back(static_cast<int32_t>(i));
		}
	}
	FilterTinyConnectedComponents(lod1ActiveIds, coarseBlocks, lod1BlocksPerAxis, kMinRenderableComponentBlocks);
}

void Pottery::updateLodSelection()
{
	if (lod1MeshIndex < 0 || lod1ObjectIndex < 0 || scene->objects.empty() || scene->objects[0].meshID.empty()) {
		return;
	}

	Mesh& fineMesh = scene->meshes[scene->objects[0].meshID[0]];
	Mesh& coarseMesh = scene->meshes[static_cast<size_t>(lod1MeshIndex)];
	glm::vec3 boundsMin;
	glm::vec3 boundsMax;
	ComputeActiveBlockBounds(fineActiveIds, chunkBlocksPerAxis, chunkBlockSizeWS, chunkTranslationWS, boundsMin, boundsMax);
	const glm::vec3 chunkCenter = 0.5f * (boundsMin + boundsMax);
	const float chunkRadius = 0.5f * glm::length(boundsMax - boundsMin);
	const float camDistance = glm::distance(scene->camPos, chunkCenter);
	float projectedRadiusPx = FLT_MAX;
	if (camDistance > chunkRadius + 1e-4f) {
		const float safeFovDegrees = glm::max(scene->camFovDegrees, 1.0f);
		const float tanHalfFov = std::tan(glm::radians(safeFovDegrees) * 0.5f);
		if (tanHalfFov > 1e-5f) {
			projectedRadiusPx =
				(chunkRadius / (camDistance * tanHalfFov)) *
				(0.5f * static_cast<float>(base->swapChain->swapChainExtent.height));
		}
	}
	const float coarseThresholdPx = coarseLodSelected ? kLodFineScreenRadiusPx : kLodCoarseScreenRadiusPx;
	const bool desiredCoarse = kForceCoarseLodDebug || (projectedRadiusPx < coarseThresholdPx);
	scene->lodProjectedRadiusPx = projectedRadiusPx;
	scene->lodCoarseActive = desiredCoarse;
	std::vector<int32_t> desiredFineIds = desiredCoarse ? std::vector<int32_t>{} : fineActiveIds;
	std::vector<int32_t> desiredCoarseIds = desiredCoarse ? lod1ActiveIds : std::vector<int32_t>{};
	const bool needsRefresh =
		(coarseLodSelected != desiredCoarse) ||
		(renderedFineIds != desiredFineIds) ||
		(renderedLod1Ids != desiredCoarseIds) ||
		(fineMesh.instanceCount != static_cast<uint32_t>(desiredFineIds.size())) ||
		(coarseMesh.instanceCount != static_cast<uint32_t>(desiredCoarseIds.size()));

	if (!needsRefresh) {
		return;
	}

	base->vbuffer->updateInstanceIdBuffer(fineMesh, desiredFineIds);
	base->vbuffer->updateInstanceIdBuffer(coarseMesh, desiredCoarseIds);
	renderedFineIds = std::move(desiredFineIds);
	renderedLod1Ids = std::move(desiredCoarseIds);
	coarseLodSelected = desiredCoarse;

	vkQueueWaitIdle(base->queue);
	scene->renderPass();
}

void Pottery::updateSnapshotChunks(float time)
{
	scene->floorBlobShadowCount = 0;
	for (const SnapshotChunk& snapshot : snapshotChunks) {
		if (snapshot.objectIndex >= scene->objects.size()) {
			continue;
		}

		const float t = glm::max(0.0f, time - snapshot.spawnTime);
		const float fallOffsetY = glm::max(-0.5f * kSnapshotGravity * t * t, snapshot.stopOffsetY);
		scene->objects[snapshot.objectIndex].ModelMatrix =
			glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, fallOffsetY, 0.0f));
		if (scene->floorBlobShadowCount < Scene::MaxFloorBlobShadows) {
			const glm::vec3 shadowCenter =
				snapshot.baseShadowCenterWS + glm::vec3(0.0f, fallOffsetY, 0.0f);
			scene->floorBlobShadows[scene->floorBlobShadowCount] =
				glm::vec4(shadowCenter, snapshot.shadowRadius);
			scene->floorBlobBounceColors[scene->floorBlobShadowCount] =
				glm::vec4(snapshot.bounceColor, 1.0f);
			scene->floorBlobShadowCount++;
		}
	}
}

void Pottery::finalizeStrokeChunk(float time)
{
	const uint32_t totalBlocks = chunkBlocksPerAxis * chunkBlocksPerAxis * chunkBlocksPerAxis;
	std::vector<int32_t> blockState(totalBlocks, -1);
	computeSBO[1].read(base->device, sizeof(int32_t) * blockState.size(), blockState.data());

	std::vector<int32_t> activeIds;
	for (uint32_t i = 0; i < totalBlocks; ++i) {
		if (blockState[i] >= 0) {
			activeIds.push_back(static_cast<int32_t>(i));
		}
	}
	if (activeIds.empty()) {
		return;
	}

	std::vector<glm::vec4> blockColors(totalBlocks, glm::vec4(0.0f));
	std::vector<glm::vec4> blockSpatial(totalBlocks, glm::vec4(0.0f));
	computeSBO[3].read(base->device, sizeof(glm::vec4) * blockColors.size(), blockColors.data());
	computeSBO[5].read(base->device, sizeof(glm::vec4) * blockSpatial.size(), blockSpatial.data());

	std::vector<int32_t> snapshotBlocks = blockState;
	std::vector<float> snapshotBricks(static_cast<size_t>(activeIds.size()) * 9u * 9u * 9u, 100.01f);
	for (size_t activeIndex = 0; activeIndex < activeIds.size(); ++activeIndex) {
		const uint32_t blockIndex = static_cast<uint32_t>(activeIds[activeIndex]);
		const int oldBlockRaw = blockState[blockIndex];
		const int oldBrickBase = oldBlockRaw & 0x3fffffff;
		const int newBrickBase = static_cast<int>(activeIndex * 9u * 9u * 9u);
		snapshotBlocks[blockIndex] = newBrickBase | (oldBlockRaw & (1 << 30));

		void* src = nullptr;
		vkMapMemory(
			base->device,
			computeSBO[0].bufferMemory,
			static_cast<VkDeviceSize>(oldBrickBase) * sizeof(float),
			static_cast<VkDeviceSize>(9u * 9u * 9u) * sizeof(float),
			0,
			&src);
		memcpy(
			snapshotBricks.data() + newBrickBase,
			src,
			static_cast<size_t>(9u * 9u * 9u) * sizeof(float));
		vkUnmapMemory(base->device, computeSBO[0].bufferMemory);
	}

	snapshotChunkSBOs.push_back(base->vbuffer->createStorageBuffers({
		sizeof(float) * snapshotBricks.size(),
		sizeof(int32_t) * snapshotBlocks.size(),
		sizeof(int),
		sizeof(glm::vec4) * blockColors.size(),
		sizeof(glm::vec4) * blockSpatial.size()
	}));
	std::vector<SBO>& sbos = snapshotChunkSBOs.back();
	int zero = 0;
	sbos[0].map(base->device, sizeof(float) * snapshotBricks.size(), snapshotBricks.data());
	sbos[1].map(base->device, sizeof(int32_t) * snapshotBlocks.size(), snapshotBlocks.data());
	sbos[2].map(base->device, sizeof(int), &zero);
	sbos[3].map(base->device, sizeof(glm::vec4) * blockColors.size(), blockColors.data());
	sbos[4].map(base->device, sizeof(glm::vec4) * blockSpatial.size(), blockSpatial.data());

	const Mesh& sourceMesh = scene->meshes[scene->objects[0].meshID[0]];
	Mesh snapshotMesh{};
	snapshotMesh.vertices = sourceMesh.vertices;
	snapshotMesh.indices = sourceMesh.indices;
	snapshotMesh.color = sourceMesh.color;
	snapshotMesh.texID = sourceMesh.texID;
	snapshotMesh.matID = sourceMesh.matID;
	base->vbuffer->createVertexBuffer(snapshotMesh);
	base->vbuffer->createIndexBuffer(snapshotMesh);
	base->vbuffer->createInstanceIdBuffer(snapshotMesh, static_cast<uint32_t>(activeIds.size()));
	base->vbuffer->updateInstanceIdBuffer(snapshotMesh, activeIds);
	scene->meshes.push_back(snapshotMesh);
	const size_t newMeshIndex = scene->meshes.size() - 1;

	Object snapshotObject{};
	snapshotObject.hasMesh = true;
	snapshotObject.hasAnimation = false;
	snapshotObject.ModelMatrix = glm::mat4(1.0f);
	snapshotObject.finalMatrix = glm::mat4(1.0f);
	snapshotObject.meshID.push_back(static_cast<uint32_t>(newMeshIndex));
	snapshotObject.name = "SnapshotChunk";

	VulkanShader shader{};
	shader.graphicsPipeline = scene->pipelines[4];
	shader.ubos = base->vbuffer->createUniformBuffers(shader.graphicsPipeline->bufferSize);
	shader.sbos = sbos;
	snapshotObject.shader.push_back(shader);
	scene->objects.push_back(snapshotObject);
	const size_t newObjectIndex = scene->objects.size() - 1;

	float minY = FLT_MAX;
	glm::vec3 boundsMin(FLT_MAX);
	glm::vec3 boundsMax(-FLT_MAX);
	glm::vec3 colorAccum(0.0f);
	for (int32_t id : activeIds) {
		const glm::vec4 centerAndSize = blockSpatial[static_cast<size_t>(id)];
		const glm::vec3 center = glm::vec3(centerAndSize);
		const glm::vec3 halfExtent(centerAndSize.w * 0.5f);
		minY = std::min(minY, center.y);
		boundsMin = glm::min(boundsMin, center - halfExtent);
		boundsMax = glm::max(boundsMax, center + halfExtent);
		colorAccum += glm::vec3(blockColors[static_cast<size_t>(id)]);
	}

	SnapshotChunk snapshot{};
	snapshot.objectIndex = newObjectIndex;
	snapshot.meshIndex = newMeshIndex;
	snapshot.sboIndex = snapshotChunkSBOs.size() - 1;
	snapshot.usedBricks = static_cast<int>(activeIds.size());
	snapshot.baseShadowCenterWS = 0.5f * (boundsMin + boundsMax);
	snapshot.shadowRadius = glm::max(
		0.5f * glm::length(glm::vec2(boundsMax.x - boundsMin.x, boundsMax.z - boundsMin.z)),
		chunkBlockSizeWS * 0.75f);
	snapshot.bounceColor = colorAccum / glm::max(float(activeIds.size()), 1.0f);
	snapshot.baseSpatial = blockSpatial;
	snapshot.spawnTime = time;
	snapshot.stopOffsetY = (kSnapshotFloorY + 0.5f * chunkBlockSizeWS) - minY;
	snapshotChunks.push_back(std::move(snapshot));

	clearEditableChunk();
	base->vbuffer->updateInstanceIdBuffer(scene->meshes[scene->objects[0].meshID[0]], {});

	vkDeviceWaitIdle(base->device);
	scene->cleanDescriptors();
	scene->createSceneDescriptor();
	scene->renderPass();
}

Pottery::~Pottery() {
	delete comp;
	delete lod1Builder;
	for (UBO& ubo : computeUBO) {
		vkDestroyBuffer(base->device, ubo.buffer, nullptr);
		vkFreeMemory(base->device, ubo.bufferMemory, nullptr);
	}
	for (SBO& sbo : computeSBO) {
		vkDestroyBuffer(base->device, sbo.buffer, nullptr);
		vkFreeMemory(base->device, sbo.bufferMemory, nullptr);
	}
	for (SBO& sbo : lod1SBO) {
		vkDestroyBuffer(base->device, sbo.buffer, nullptr);
		vkFreeMemory(base->device, sbo.bufferMemory, nullptr);
	}
	for (std::vector<SBO>& sboSet : snapshotChunkSBOs) {
		for (SBO& sbo : sboSet) {
			vkDestroyBuffer(base->device, sbo.buffer, nullptr);
			vkFreeMemory(base->device, sbo.bufferMemory, nullptr);
		}
	}
	//computeTexture.destroy(base->device);
	//vkDestroySampler(base->device, computeTexture.imageSampler, nullptr);
}

static bool KeyPressed = false;
static bool WKeyDown = false;
static bool SKeyDown = false;
static bool AKeyDown = false;
static bool DKeyDown = false;
static bool QKeyDown = false;
static bool EKeyDown = false;
static bool LeftArrowDown = false;
static bool RightArrowDown = false;
static bool AltKeyDown = false;
static bool ShiftKeyDown = false;
static bool UndoRequested = false;
static int BrushShapeMode = 0; // 0 cube, 1 sphere
static bool EmissiveMode = false;
static void key_cb(GLFWwindow* win, int key, int scancode, int action, int mods)
{
	const char* act = (action == GLFW_PRESS) ? "down" :
		(action == GLFW_RELEASE) ? "up" : "repeat";

	std::printf("Key %d %s (mods=%d)\n", key, act, mods);

	// Track general press
	KeyPressed = (action == GLFW_PRESS);
	if (key == GLFW_KEY_LEFT_ALT) {
		AltKeyDown = (action != GLFW_RELEASE);
	}
	if (key == GLFW_KEY_LEFT_SHIFT || key == GLFW_KEY_RIGHT_SHIFT) {
		ShiftKeyDown = (action != GLFW_RELEASE);
	}
	if (key == GLFW_KEY_Z && action == GLFW_PRESS && (mods & GLFW_MOD_CONTROL)) {
		UndoRequested = true;
	}
	// Track specific keys
	if (key == GLFW_KEY_W)
		WKeyDown = (action != GLFW_RELEASE);
	if (key == GLFW_KEY_S)
		SKeyDown = (action != GLFW_RELEASE);
	if (key == GLFW_KEY_A)
		AKeyDown = (action != GLFW_RELEASE);
	if (key == GLFW_KEY_D)
		DKeyDown = (action != GLFW_RELEASE);
	if (key == GLFW_KEY_Q)
		QKeyDown = (action != GLFW_RELEASE);
	if (key == GLFW_KEY_E)
		EKeyDown = (action != GLFW_RELEASE);
	if (key == GLFW_KEY_LEFT)
		LeftArrowDown = (action != GLFW_RELEASE);
	if (key == GLFW_KEY_RIGHT)
		RightArrowDown = (action != GLFW_RELEASE);
	if (key == GLFW_KEY_U && action == GLFW_PRESS) {
		BrushShapeMode = (BrushShapeMode + 1) % 2;
		const char* shapeName = (BrushShapeMode == 0) ? "Cube" : "Sphere";
		std::printf("Brush Shape: %s\n", shapeName);
	}
	if (key == GLFW_KEY_I && action == GLFW_PRESS) {
		EmissiveMode = !EmissiveMode;
		std::printf("Emissive Mode: %s\n", EmissiveMode ? "ON" : "OFF");
	}
}

static float leftBut = 1.0;
static float BrushRadius = 0.01;
static float BrushSmoothness = 0.001f;
static float BrushDistanceFromCamera = 0.2f;

static bool intersectAabb(const glm::vec3& ro, const glm::vec3& rd, const glm::vec3& bmin, const glm::vec3& bmax, float& tEnter, float& tExit)
{
	tEnter = -1e30f;
	tExit = 1e30f;

	for (int axis = 0; axis < 3; ++axis) {
		float o = ro[axis];
		float d = rd[axis];
		float mn = bmin[axis];
		float mx = bmax[axis];

		if (std::fabs(d) < 1e-6f) {
			if (o < mn || o > mx) return false;
			continue;
		}

		float t0 = (mn - o) / d;
		float t1 = (mx - o) / d;
		if (t0 > t1) std::swap(t0, t1);

		tEnter = std::max(tEnter, t0);
		tExit = std::min(tExit, t1);
		if (tExit < tEnter) return false;
	}
	return true;
}

void Pottery::update(float time)
{
	uploadChunkSpatialData();
	scene->floorBlobShadowCount = 0;

	double mouseX, mouseY;
	glfwGetCursorPos(base->window, &mouseX, &mouseY);
	const glm::dvec2 currentCursorPos(mouseX, mouseY);
	
	glfwSetKeyCallback(base->window, key_cb);
	scene->slowZoom = AltKeyDown;
	const float altSpeed = AltKeyDown ? 0.1f : 1.0f;
	const float brushRadiusStep = 0.0001f * altSpeed;
	const float brushSmoothnessStep = 0.0001f * altSpeed;
	const float brushDistanceStep = 0.005f * altSpeed;
	float msx = mouseX / base->swapChain->swapChainExtent.width;
	float msy = mouseY / base->swapChain->swapChainExtent.height;
	// Example: print to console
	//std::printf("Mouse: (%.1f, %.1f)\n", msx-0.5, msy-0.5);
	potteryUBO pubo;
	pubo.time = time*0.05;
	
	scene->WKeyDown = WKeyDown;
	scene->AKeyDown = AKeyDown;
	scene->SKeyDown = SKeyDown;
	scene->DKeyDown = DKeyDown;
	scene->LeftArrowDown = LeftArrowDown;
	scene->RightArrowDown = RightArrowDown;
	if(!scene->cameraMoveMode){
		 if (scene->RightButtonDown) leftBut = 0.0;
		if (scene->LeftButtonDown) leftBut = 1.0;
		if (AKeyDown) { BrushRadius += brushRadiusStep; std::cout << BrushRadius << "\n"; }
		if (DKeyDown) { BrushRadius -= brushRadiusStep; std::cout << BrushRadius << "\n"; }
		if (QKeyDown) { BrushSmoothness += brushSmoothnessStep; }
		if (EKeyDown) { BrushSmoothness -= brushSmoothnessStep; }
		if (WKeyDown) BrushDistanceFromCamera += brushDistanceStep;
		if (SKeyDown) BrushDistanceFromCamera -= brushDistanceStep;

		if (BrushRadius < 0.001f) BrushRadius = 0.001f;
		if (BrushSmoothness < 0.0001f) BrushSmoothness = 0.0001f;
		if (BrushDistanceFromCamera < 0.02f) BrushDistanceFromCamera = 0.02f;
		if (BrushDistanceFromCamera > 3.0f) BrushDistanceFromCamera = 3.0f;
	}
	glm::mat4 invProj = glm::inverse(scene->projNoJitter[0]);
	glm::mat4 invView = glm::inverse(scene->view[0]);
	float ndcX = msx * 2.0f - 1.0f;
	float ndcY = 1.0f - msy * 2.0f;

	glm::vec4 rayClip(ndcX, ndcY, -1.0f, 1.0f);
	glm::vec4 rayView = invProj * rayClip;
	rayView = glm::vec4(rayView.x, rayView.y, -1.0f, 0.0f);
	glm::vec3 rayDir = glm::normalize(glm::vec3(invView * rayView));
	glm::vec3 rayOrigin = scene->camPos;

	float tEnter = 0.0f, tExit = 0.0f;
	glm::vec3 brushCenter = rayOrigin + rayDir * BrushDistanceFromCamera; // fallback
	glm::vec3 chunkBoundsMin;
	glm::vec3 chunkBoundsMax;
	ComputeChunkBounds(chunkBlocksPerAxis, chunkBlockSizeWS, chunkTranslationWS, chunkBoundsMin, chunkBoundsMax);
	if (intersectAabb(rayOrigin, rayDir, chunkBoundsMin, chunkBoundsMax, tEnter, tExit)) {
		float tStart = std::max(tEnter, 0.0f);
		float tCenter = std::min(tStart + BrushDistanceFromCamera, tExit - 0.001f);
		if (tCenter >= tStart) {
			brushCenter = rayOrigin + rayDir * tCenter;
		}
	}
	const glm::vec3 brushCenterLocal = brushCenter - chunkTranslationWS;
	pubo.BrushData = glm::vec4(-brushCenterLocal, BrushRadius);
	pubo.BrushColor = scene->brushColor;
	pubo.BrushAction = leftBut; // 1.0 add, 0.0 remove
	pubo.BrushShape = static_cast<float>(BrushShapeMode);
	pubo.BrushSmoothness = BrushSmoothness;
	pubo.BrushEmissive = EmissiveMode ? 1.0f : 0.0f;
	pubo.BrushRecolor = (ShiftKeyDown && scene->LeftButtonDown && !scene->cameraMoveMode) ? 1.0f : 0.0f;
	pubo.DispatchMeta = glm::ivec4(0, 0, 0, static_cast<int>(csgOperations.size()));
	scene->brushData = glm::vec4(-brushCenter, BrushRadius);
	scene->brushUseSphere = (BrushShapeMode == 1);
	scene->brushShapeMode = BrushShapeMode;
	scene->brushRadius = BrushRadius;
	scene->brushSmoothness = BrushSmoothness;
	scene->brushZAxis = BrushDistanceFromCamera;
	scene->brushAction = leftBut;
	scene->ShiftDown = ShiftKeyDown;
	scene->emissiveMode = EmissiveMode;
	computeUBO[0].map(base->device, sizeof(potteryUBO), &pubo);


	const int dim = 8;
	const int total = dim * dim * dim; // 512

	// CPU-side storage
	std::vector<int> gpuArray(total);

	// Read from GPU buffer
	//computeSBO[1].read(base->device, sizeof(int) * gpuArray.size(), gpuArray.data());

	
	//std::cout << "gpuArray[0]: " << gpuArray[10] << std::endl;

	const bool brushPointerDown = !scene->cameraMoveMode && (scene->LeftButtonDown || scene->RightButtonDown);
	const glm::dvec2 cursorDelta = currentCursorPos - previousBrushCursorPos;
	const bool cursorDragged =
		wasBrushPointerDown &&
		glm::dot(cursorDelta, cursorDelta) > 0.25;
	const bool isInitialBrushClick = brushPointerDown && !wasBrushPointerDown;
	const bool isBrushEditing = brushPointerDown && (isInitialBrushClick || cursorDragged);
	const bool editJustEnded = wasBrushEditing && !isBrushEditing;
	wasBrushEditing = isBrushEditing;
	wasBrushPointerDown = brushPointerDown;
	previousBrushCursorPos = currentCursorPos;

	if (UndoRequested && !isBrushEditing) {
		UndoRequested = false;
		if (!csgOperations.empty()) {
			csgOperations.pop_back();
			uploadCsgOperations();
			rebuildEditableChunkFromCsg();
		}
	}

	if (isBrushEditing) {
		const float effectiveBrushRadius = BrushRadius * (leftBut < 0.5f ? 1.15f : 1.0f);
		const float smoothSupportPadding = glm::max(BrushSmoothness * 4.0f, chunkBlockSizeWS);
		const glm::vec3 brushCenterCompute = brushCenterLocal + glm::vec3(kBrushCenterBias);
		const glm::vec3 brushExtent = glm::vec3(effectiveBrushRadius + smoothSupportPadding);
		const glm::vec3 blockCoordMinF =
			((brushCenterCompute - brushExtent) + 0.5f) / chunkBlockSizeWS;
		const glm::vec3 blockCoordMaxF =
			((brushCenterCompute + brushExtent) + 0.5f) / chunkBlockSizeWS;
		glm::ivec3 dispatchBlockMin = glm::ivec3(glm::floor(blockCoordMinF)) - glm::ivec3(1);
		glm::ivec3 dispatchBlockMax = glm::ivec3(glm::floor(blockCoordMaxF)) + glm::ivec3(1);
		dispatchBlockMin = glm::clamp(dispatchBlockMin, glm::ivec3(0), glm::ivec3(int(chunkBlocksPerAxis) - 1));
		dispatchBlockMax = glm::clamp(dispatchBlockMax, glm::ivec3(0), glm::ivec3(int(chunkBlocksPerAxis) - 1));
		const glm::ivec3 dispatchBlockCount = dispatchBlockMax - dispatchBlockMin + glm::ivec3(1);
		CsgOperationGpu op{};
		op.centerRadius = glm::vec4(brushCenterLocal + glm::vec3(kBrushCenterBias), BrushRadius);
		op.color = scene->brushColor;
		op.meta.x = leftBut;
		op.meta.y = static_cast<float>(BrushShapeMode);
		op.meta.z = BrushSmoothness;
		int flags = 0;
		if (EmissiveMode) flags |= static_cast<int>(kCsgFlagEmissive);
		if (pubo.BrushRecolor > 0.5f) flags |= static_cast<int>(kCsgFlagRecolor);
		op.meta.w = static_cast<float>(flags);
		if (csgOperations.size() < MaxCsgOperations) {
			csgOperations.push_back(op);
			uploadCsgOperations();
		}

		pubo.DispatchMeta = glm::ivec4(dispatchBlockMin, static_cast<int>(csgOperations.size()));
		computeUBO[0].map(base->device, sizeof(potteryUBO), &pubo);

		int dirtyZero = 0;
		computeSBO[4].map(base->device, sizeof(int), &dirtyZero);
		comp->submit(
			static_cast<uint32_t>(dispatchBlockCount.x),
			static_cast<uint32_t>(dispatchBlockCount.y),
			static_cast<uint32_t>(dispatchBlockCount.z));

		vkWaitForFences(base->device, 1, &comp->computeFence, VK_TRUE, UINT64_MAX);

		Mesh* targetMesh = nullptr;
		for (Object& obj : scene->objects) {
			if (!obj.hasMesh) continue;
			for (size_t i = 0; i < obj.meshID.size(); ++i) {
				if (i >= obj.shader.size()) continue;
				if (obj.shader[i].graphicsPipeline != scene->pipelines[4]) continue;
				targetMesh = &scene->meshes[obj.meshID[i]];
				break;
			}
			if (targetMesh) break;
		}

		if (targetMesh && targetMesh->hasInstanceIdBuffer) {
			const uint32_t totalBlocks = chunkBlocksPerAxis * chunkBlocksPerAxis * chunkBlocksPerAxis;
			std::vector<int32_t> blockState(totalBlocks, -1);
			computeSBO[1].read(base->device, sizeof(int32_t) * blockState.size(), blockState.data());

			std::vector<int32_t> activeIds;
			activeIds.reserve(static_cast<size_t>(scene->dirtyBlockCount > 0 ? scene->dirtyBlockCount : 0));
			for (uint32_t i = 0; i < totalBlocks; ++i) {
				if (blockState[i] >= 0) {
					activeIds.push_back(static_cast<int32_t>(i));
				}
			}
			FilterTinyConnectedComponents(activeIds, blockState, chunkBlocksPerAxis, kMinRenderableComponentBlocks);
			fineActiveIds = activeIds;

			uint32_t oldCount = targetMesh->instanceCount;
			uint32_t oldCoarseCount = 0;
			if (lod1MeshIndex >= 0) {
				oldCoarseCount = scene->meshes[static_cast<size_t>(lod1MeshIndex)].instanceCount;
			}
			base->vbuffer->updateInstanceIdBuffer(*targetMesh, activeIds);
			uint32_t newCount = targetMesh->instanceCount;
			scene->dirtyBlockCount = static_cast<int>(newCount);

			if (coarseLodSelected && lod1MeshIndex >= 0) {
				Mesh& coarseMesh = scene->meshes[static_cast<size_t>(lod1MeshIndex)];
				base->vbuffer->updateInstanceIdBuffer(coarseMesh, {});
			}
			coarseLodSelected = false;
			uint32_t newCoarseCount = oldCoarseCount;
			if (lod1MeshIndex >= 0) {
				newCoarseCount = scene->meshes[static_cast<size_t>(lod1MeshIndex)].instanceCount;
			}

			if (newCount != oldCount || newCoarseCount != oldCoarseCount) {
				vkQueueWaitIdle(base->queue);
				scene->renderPass();
			}
		}
	}

	if (editJustEnded) {
		pendingEditableSync = true;
	}

	if (pendingEditableSync && !isBrushEditing) {
		rebuildEditableChunkFromCsg();
	}

	if (!isBrushEditing) {
		updateLodSelection();
	}

	if (scene->sdfAtlasCapacityBricks > 0) {
		scene->sdfAtlasUsage = glm::clamp(
			static_cast<float>(scene->sdfAtlasUsedBricks) /
			static_cast<float>(scene->sdfAtlasCapacityBricks),
			0.0f,
			1.0f);
	}
	else {
		scene->sdfAtlasUsage = 0.0f;
	}
}
