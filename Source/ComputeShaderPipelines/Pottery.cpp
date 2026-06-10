#include "Pottery.h"
#include "../ResourceManager.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

 struct alignas(16) potteryUBO {
	glm::vec4 BrushData;
	glm::vec4 BrushColor;
	float BrushAction; // 1.0 add, 0.0 remove
	float time;
	float BrushShape; // 0.0 cube, 1.0 sphere, 2.0 plane
	float BrushSmoothness;
	float BrushEmissive; // 1.0 emissive paint mode on, 0.0 off
	float BrushRecolor; // 1.0 recolor existing blocks
	float _pad0 = 0.0f;
	float _pad1 = 0.0f;
	glm::ivec4 DispatchBaseBlock = glm::ivec4(0);
	

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

constexpr float kSnapshotGravity = 0.35f;
constexpr float kSnapshotFloorY = -0.35f;
constexpr int kBrickSamplesPerBlock = 9 * 9 * 9;
constexpr uint32_t kFloorBlocksPerAxis = 64u;
constexpr int kBlockEmissiveFlag = (1 << 30);
constexpr int kBlockIndexMask = kBlockEmissiveFlag - 1;
constexpr int kAtlasDim = 512;
constexpr float kBrushCenterBias = 0.031f;
constexpr bool kForceCoarseLodDebug = false;

bool BlockHasRenderableSurfaceSamples(const float* bricks, int brickBase, float nearSurfaceThreshold)
{
	if (brickBase < 0 || bricks == nullptr) {
		return false;
	}

	bool hasNegative = false;
	bool hasPositive = false;
	int nearSurfaceCount = 0;

	for (int i = 0; i < kBrickSamplesPerBlock; ++i) {
		const float h = bricks[brickBase + i];
		if (h < 0.0f) hasNegative = true;
		if (h > 0.0f) hasPositive = true;
		if (std::abs(h) <= nearSurfaceThreshold) {
			nearSurfaceCount++;
		}
	}

	return (hasNegative && hasPositive) || (nearSurfaceCount >= 3);
}

std::vector<int32_t> BuildRenderableActiveIds(
	const std::vector<int32_t>& blockState,
	const float* bricks,
	float nearSurfaceThreshold)
{
	std::vector<int32_t> activeIds;
	activeIds.reserve(blockState.size());

	for (uint32_t i = 0; i < blockState.size(); ++i) {
		const int32_t blockRaw = blockState[i];
		if (blockRaw < 0) {
			continue;
		}

		const int brickBase = (blockRaw & kBlockIndexMask);
		if (BlockHasRenderableSurfaceSamples(bricks, brickBase, nearSurfaceThreshold)) {
			activeIds.push_back(static_cast<int32_t>(i));
		}
	}

	return activeIds;
}

std::vector<int32_t> BuildAllocatedActiveIds(const std::vector<int32_t>& blockState)
{
	std::vector<int32_t> activeIds;
	activeIds.reserve(blockState.size());
	for (uint32_t i = 0; i < blockState.size(); ++i) {
		if (blockState[i] >= 0) {
			activeIds.push_back(static_cast<int32_t>(i));
		}
	}
	return activeIds;
}

uint32_t FlattenBlockIndex(uint32_t x, uint32_t y, uint32_t z, uint32_t blocksPerAxis)
{
	return x + y * blocksPerAxis + z * blocksPerAxis * blocksPerAxis;
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
	input.computeShaderPath = "shaders/ComputeShaders/VolumetricBrushCS.spv";
	input.descriptorTypes = {
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
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
	lodInput.computeShaderPath = "shaders/ComputeShaders/VolumetricLod1CS.spv";
	lodInput.descriptorTypes = {
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
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
		sizeof(int) * atlasCapacityBricks
	});
	int zeroInit = 0;
	computeSBO[2].map(base->device, sizeof(int), &zeroInit);
	computeSBO[4].map(base->device, sizeof(int), &zeroInit);
	computeSBO[6].map(base->device, sizeof(int), &zeroInit);
	std::vector<int32_t> initialFineBlocks(static_cast<size_t>(numBlocks), -999999);
	std::vector<glm::vec4> initialFineColors(static_cast<size_t>(numBlocks), glm::vec4(0.0f));
	computeSBO[1].map(base->device, sizeof(int32_t) * initialFineBlocks.size(), initialFineBlocks.data());
	computeSBO[3].map(base->device, sizeof(glm::vec4) * initialFineColors.size(), initialFineColors.data());
	fineActiveIds.clear();
	scene->sdfAtlasCapacityBricks = atlasCapacityBricks;
	scene->sdfAtlasUsedBricks = 0;
	scene->sdfAtlasTailBricks = 0;
	scene->sdfAtlasFreeBricks = 0;
	scene->sdfAtlasUsage = 0.0f;

	uploadChunkSpatialData();

	std::vector<SBO> computeDescriptorSBOs = {
		computeSBO[0], // bricks
		computeSBO[1], // blocks
		computeSBO[2], // tail counter
		computeSBO[3], // block colors
		computeSBO[4], // dirty counter
		computeSBO[6], // free stack count
		computeSBO[7]  // free stack data
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
		computeSBO[0],
		computeSBO[1],
		computeSBO[3],
		lod1SBO[0],
		lod1SBO[1],
		lod1SBO[3]
	};
	lod1Builder->setupDescriptors({}, {}, lod1DescriptorSBOs);
	lod1Builder->dispatch(1, 1, 1);
	//glfwSetMouseButtonCallback(base->window, mouse_button_cb);
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
	pendingFullChunkResolve = false;
	pendingStrictSurfaceScan = false;
	pendingLod1Rebuild = false;
	lod1RebuildSubmitted = false;
	strictSurfaceScanCursor = 0;
	strictScanBlockState.clear();
	strictScanRenderableIds.clear();
	fineActiveIds.clear();
	lod1ActiveIds.clear();
}

void Pottery::rebuildLod1()
{
	lod1ActiveIds.clear();

	lod1Builder->submit(lod1BlocksPerAxis, lod1BlocksPerAxis, lod1BlocksPerAxis);
	vkWaitForFences(base->device, 1, &lod1Builder->computeFence, VK_TRUE, UINT64_MAX);

	const uint32_t totalBlocks = lod1BlocksPerAxis * lod1BlocksPerAxis * lod1BlocksPerAxis;
	std::vector<int32_t> coarseBlocks(totalBlocks, -999999);
	lod1SBO[1].read(base->device, sizeof(int32_t) * coarseBlocks.size(), coarseBlocks.data());
	void* mapped = nullptr;
	vkMapMemory(base->device, lod1SBO[0].bufferMemory, 0, lod1SBO[0].size, 0, &mapped);
	const float* coarseBricks = static_cast<const float*>(mapped);
	const float nearSurfaceThreshold = 2.0f / float(chunkBlocksPerAxis * chunkBlockDim);
	lod1ActiveIds = BuildRenderableActiveIds(coarseBlocks, coarseBricks, nearSurfaceThreshold);
	vkUnmapMemory(base->device, lod1SBO[0].bufferMemory);
}

void Pottery::updateLodSelection()
{
	if (lod1MeshIndex < 0 || lod1ObjectIndex < 0 || scene->objects.empty() || scene->objects[0].meshID.empty()) {
		return;
	}

	glm::vec3 boundsMin;
	glm::vec3 boundsMax;
	ComputeChunkBounds(chunkBlocksPerAxis, chunkBlockSizeWS, chunkTranslationWS, boundsMin, boundsMax);
	const glm::vec3 chunkCenter = 0.5f * (boundsMin + boundsMax);
	const float camDistance = glm::distance(scene->camPos, chunkCenter);
	const bool desiredCoarse = kForceCoarseLodDebug || (camDistance > 1.35f);

	Mesh& fineMesh = scene->meshes[scene->objects[0].meshID[0]];
	Mesh& coarseMesh = scene->meshes[static_cast<size_t>(lod1MeshIndex)];
	const uint32_t desiredFineCount = desiredCoarse ? 0u : static_cast<uint32_t>(fineActiveIds.size());
	const uint32_t desiredCoarseCount = desiredCoarse ? static_cast<uint32_t>(lod1ActiveIds.size()) : 0u;
	const bool needsRefresh =
		(coarseLodSelected != desiredCoarse) ||
		(fineMesh.instanceCount != desiredFineCount) ||
		(coarseMesh.instanceCount != desiredCoarseCount);

	if (!needsRefresh) {
		return;
	}

	base->vbuffer->updateInstanceIdBuffer(fineMesh, desiredCoarse ? std::vector<int32_t>{} : fineActiveIds);
	base->vbuffer->updateInstanceIdBuffer(coarseMesh, desiredCoarse ? lod1ActiveIds : std::vector<int32_t>{});
	coarseLodSelected = desiredCoarse;

	scene->renderPassDirty = true;
}

void Pottery::createStaticFloorObject()
{
	const Mesh& sourceMesh = scene->meshes[scene->objects[0].meshID[0]];
	const uint32_t totalBlocks = chunkBlocksPerAxis * chunkBlocksPerAxis * chunkBlocksPerAxis;
	const float blockSizeWS = chunkBlockSizeWS;
	const float floorCenterY = kSnapshotFloorY - 0.5f * blockSizeWS;
	const float startXZ = -0.5f + 0.5f * blockSizeWS;

	std::vector<int32_t> floorBlocks(totalBlocks, -999999);
	std::vector<glm::vec4> floorColors(totalBlocks, glm::vec4(0.0f));
	std::vector<BlockSpatialData> floorSpatial(totalBlocks);
	for (uint32_t i = 0; i < totalBlocks; ++i) {
		floorSpatial[i].centerAndSize = glm::vec4(0.0f, floorCenterY, 0.0f, blockSizeWS);
	}

	std::vector<int32_t> activeIds;
	activeIds.reserve(static_cast<size_t>(kFloorBlocksPerAxis) * static_cast<size_t>(kFloorBlocksPerAxis));

	for (uint32_t z = 0; z < kFloorBlocksPerAxis; ++z) {
		for (uint32_t x = 0; x < kFloorBlocksPerAxis; ++x) {
			const uint32_t blockIndex = x + z * chunkBlocksPerAxis;
			const int brickBase = static_cast<int>(activeIds.size()) * kBrickSamplesPerBlock;
			floorBlocks[blockIndex] = brickBase;
			floorColors[blockIndex] = glm::vec4(0.92f, 0.92f, 0.92f, 1.0f);
			floorSpatial[blockIndex].centerAndSize = glm::vec4(
				startXZ + float(x) * blockSizeWS,
				floorCenterY,
				startXZ + float(z) * blockSizeWS,
				blockSizeWS);
			activeIds.push_back(static_cast<int32_t>(blockIndex));
		}
	}

	std::vector<float> floorBricks(static_cast<size_t>(activeIds.size()) * kBrickSamplesPerBlock, -1.0f);
	std::vector<SBO> floorSbos = base->vbuffer->createStorageBuffers({
		sizeof(float) * floorBricks.size(),
		sizeof(int32_t) * floorBlocks.size(),
		sizeof(int),
		sizeof(glm::vec4) * floorColors.size(),
		sizeof(BlockSpatialData) * floorSpatial.size()
	});

	int zero = 0;
	floorSbos[0].map(base->device, sizeof(float) * floorBricks.size(), floorBricks.data());
	floorSbos[1].map(base->device, sizeof(int32_t) * floorBlocks.size(), floorBlocks.data());
	floorSbos[2].map(base->device, sizeof(int), &zero);
	floorSbos[3].map(base->device, sizeof(glm::vec4) * floorColors.size(), floorColors.data());
	floorSbos[4].map(base->device, sizeof(BlockSpatialData) * floorSpatial.size(), floorSpatial.data());
	snapshotChunkSBOs.push_back(std::move(floorSbos));

	Mesh floorMesh{};
	floorMesh.vertices = sourceMesh.vertices;
	floorMesh.indices = sourceMesh.indices;
	floorMesh.color = glm::vec4(1.0f);
	floorMesh.texID = sourceMesh.texID;
	floorMesh.matID = sourceMesh.matID;
	base->vbuffer->createVertexBuffer(floorMesh);
	base->vbuffer->createIndexBuffer(floorMesh);
	base->vbuffer->createInstanceIdBuffer(floorMesh, static_cast<uint32_t>(activeIds.size()));
	base->vbuffer->updateInstanceIdBuffer(floorMesh, activeIds);
	scene->meshes.push_back(floorMesh);

	Object floorObject{};
	floorObject.hasMesh = true;
	floorObject.hasAnimation = false;
	floorObject.ModelMatrix = glm::mat4(1.0f);
	floorObject.finalMatrix = glm::mat4(1.0f);
	floorObject.meshID.push_back(static_cast<uint32_t>(scene->meshes.size() - 1));
	floorObject.name = "StaticFloor";

	VulkanShader shader{};
	shader.graphicsPipeline = scene->pipelines[4];
	shader.ubos = base->vbuffer->createUniformBuffers(shader.graphicsPipeline->bufferSize);
	shader.sbos = snapshotChunkSBOs.back();
	floorObject.shader.push_back(shader);
	scene->objects.push_back(floorObject);
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
	scene->renderPassDirty = true;
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
static int BrushShapeMode = 0; // 0 cube, 1 sphere, 2 plane
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
		BrushShapeMode = (BrushShapeMode + 1) % 3;
		const char* shapeName = (BrushShapeMode == 0) ? "Cube" : ((BrushShapeMode == 1) ? "Sphere" : "Plane");
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

	if (isBrushEditing) {
		const float effectiveBrushRadius = BrushRadius * (leftBut < 0.5f ? 1.15f : 1.0f);
		const float smoothSupportPadding = glm::max(BrushSmoothness * 8.0f, chunkBlockSizeWS * 2.0f);
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
		const bool computeReady = (vkGetFenceStatus(base->device, comp->computeFence) == VK_SUCCESS);

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
		
		if (computeReady && targetMesh && targetMesh->hasInstanceIdBuffer) {
			const uint32_t totalBlocks = chunkBlocksPerAxis * chunkBlocksPerAxis * chunkBlocksPerAxis;
			std::vector<int32_t> blockState(totalBlocks, -1);
			computeSBO[1].read(base->device, sizeof(int32_t) * blockState.size(), blockState.data());
			fineActiveIds = BuildAllocatedActiveIds(blockState);
			base->vbuffer->updateInstanceIdBuffer(*targetMesh, fineActiveIds);
			scene->dirtyBlockCount = static_cast<int>(fineActiveIds.size());
			scene->renderPassDirty = true;
		}
		
		if (computeReady) {
			pubo.DispatchBaseBlock = glm::ivec4(dispatchBlockMin, 0);
			computeUBO[0].map(base->device, sizeof(potteryUBO), &pubo);
			int dirtyZero = 0;
			computeSBO[4].map(base->device, sizeof(int), &dirtyZero);
			comp->submit(
				static_cast<uint32_t>(dispatchBlockCount.x),
				static_cast<uint32_t>(dispatchBlockCount.y),
				static_cast<uint32_t>(dispatchBlockCount.z));
		}
	}

	if (editJustEnded) {
		pendingFullChunkResolve = true;
		pendingStrictSurfaceScan = false;
		pendingLod1Rebuild = false;
		lod1RebuildSubmitted = false;
		strictSurfaceScanCursor = 0;
		strictScanBlockState.clear();
		strictScanRenderableIds.clear();
	}

	if (!isBrushEditing && pendingFullChunkResolve) {
		if (vkGetFenceStatus(base->device, comp->computeFence) == VK_SUCCESS) {
			pubo.DispatchBaseBlock = glm::ivec4(0);
			computeUBO[0].map(base->device, sizeof(potteryUBO), &pubo);
			int dirtyZero = 0;
			computeSBO[4].map(base->device, sizeof(int), &dirtyZero);
			comp->submit(chunkBlocksPerAxis, chunkBlocksPerAxis, chunkBlocksPerAxis);
			pendingFullChunkResolve = false;
			pendingStrictSurfaceScan = true;
		}
	}

	if (!isBrushEditing && pendingStrictSurfaceScan) {
		if (vkGetFenceStatus(base->device, comp->computeFence) == VK_SUCCESS) {
			const uint32_t totalBlocks = chunkBlocksPerAxis * chunkBlocksPerAxis * chunkBlocksPerAxis;
			if (strictScanBlockState.empty()) {
				strictScanBlockState.resize(totalBlocks, -1);
				computeSBO[1].read(
					base->device,
					sizeof(int32_t) * strictScanBlockState.size(),
					strictScanBlockState.data());
				strictScanRenderableIds.clear();
				strictScanRenderableIds.reserve(fineActiveIds.size());
				strictSurfaceScanCursor = 0;
			}

			const uint32_t kBlocksPerFrame = 8192u;
			const uint32_t scanEnd = glm::min(
				strictSurfaceScanCursor + kBlocksPerFrame,
				totalBlocks);
			void* mapped = nullptr;
			vkMapMemory(base->device, computeSBO[0].bufferMemory, 0, computeSBO[0].size, 0, &mapped);
			const float* fineBricks = static_cast<const float*>(mapped);
			const float nearSurfaceThreshold = 2.0f / float(chunkBlocksPerAxis * chunkBlockDim);
			for (uint32_t i = strictSurfaceScanCursor; i < scanEnd; ++i) {
				const int32_t blockRaw = strictScanBlockState[i];
				if (blockRaw < 0) {
					continue;
				}
				const int brickBase = (blockRaw & kBlockIndexMask);
				if (BlockHasRenderableSurfaceSamples(fineBricks, brickBase, nearSurfaceThreshold)) {
					strictScanRenderableIds.push_back(static_cast<int32_t>(i));
				}
			}
			vkUnmapMemory(base->device, computeSBO[0].bufferMemory);
			strictSurfaceScanCursor = scanEnd;

			if (strictSurfaceScanCursor >= totalBlocks) {
				fineActiveIds.swap(strictScanRenderableIds);

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
				scene->dirtyBlockCount = static_cast<int>(fineActiveIds.size());

				int gpuValue = 0;
				int freeBrickCount = 0;
				computeSBO[2].read(base->device, sizeof(int), &gpuValue);
				computeSBO[6].read(base->device, sizeof(int), &freeBrickCount);
				scene->sdfAtlasTailBricks = gpuValue / kBrickSamplesPerBlock;
				scene->sdfAtlasFreeBricks = glm::max(freeBrickCount, 0);
				scene->sdfAtlasUsedBricks = glm::max(scene->sdfAtlasTailBricks - scene->sdfAtlasFreeBricks, 0);

				pendingStrictSurfaceScan = false;
				pendingLod1Rebuild = true;
				lod1RebuildSubmitted = false;
				strictSurfaceScanCursor = 0;
				strictScanBlockState.clear();
				strictScanRenderableIds.clear();
				scene->renderPassDirty = true;
			}
		}
	}

	if (!isBrushEditing && pendingLod1Rebuild) {
		if (!lod1RebuildSubmitted) {
			lod1Builder->submit(lod1BlocksPerAxis, lod1BlocksPerAxis, lod1BlocksPerAxis);
			lod1RebuildSubmitted = true;
		}
		else if (vkGetFenceStatus(base->device, lod1Builder->computeFence) == VK_SUCCESS) {
			const uint32_t totalBlocks = lod1BlocksPerAxis * lod1BlocksPerAxis * lod1BlocksPerAxis;
			std::vector<int32_t> coarseBlocks(totalBlocks, -999999);
			lod1SBO[1].read(base->device, sizeof(int32_t) * coarseBlocks.size(), coarseBlocks.data());
			void* mapped = nullptr;
			vkMapMemory(base->device, lod1SBO[0].bufferMemory, 0, lod1SBO[0].size, 0, &mapped);
			const float* coarseBricks = static_cast<const float*>(mapped);
			const float nearSurfaceThreshold = 2.0f / float(chunkBlocksPerAxis * chunkBlockDim);
			lod1ActiveIds = BuildRenderableActiveIds(coarseBlocks, coarseBricks, nearSurfaceThreshold);
			vkUnmapMemory(base->device, lod1SBO[0].bufferMemory);
			pendingLod1Rebuild = false;
			lod1RebuildSubmitted = false;
			scene->renderPassDirty = true;
		}
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
