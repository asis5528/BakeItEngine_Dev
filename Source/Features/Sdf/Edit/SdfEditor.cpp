#include "SdfEditor.h"
#include "Features/Sdf/SdfBackend.h"
#include "Core/Resources/ResourceManager.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <vector>

 struct alignas(16) sdfEditUBO {
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

bool IntersectAabb(const glm::vec3& ro, const glm::vec3& rd, const glm::vec3& bmin, const glm::vec3& bmax, float& tEnter, float& tExit)
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

constexpr int kBrickSamplesPerBlock = 9 * 9 * 9;
#ifdef BAKEIT_PLATFORM_ANDROID
// Adreno's maxStorageBufferRange caps SSBO ranges at 128 MiB (desktop's
// 512^3 float atlas is 512 MiB; over-limit ranges read as garbage/zero - the
// "flat cubes" look). 300^3 = ~103 MiB / ~37k bricks fills the budget while
// staying under the limit; the tail allocator runs out sooner than desktop's
// 184k bricks, so watch stats.tailBricks vs capacity. Phase 4's FP16/3D-
// texture atlas relieves this. Shaders index the atlas linearly by brick,
// so capacity is a CPU-side decision only.
constexpr int kAtlasDim = 300;
#else
constexpr int kAtlasDim = 512;
#endif
constexpr float kBrushCenterBias = 0.031f;
constexpr int32_t kBlockEmpty = -999999;

// Mirrors CoarseUbo in shaders/Sdf/SdfDownsampleCS.comp.
struct alignas(16) CoarseDownsampleUBO
{
	glm::ivec4 dims;   // x: coarseN, y: coarseBlocksPerAxis, z: fineBlocksPerAxis
	glm::vec4  params; // x: clampVal (world-dist clamp ~= coarse block size)
};
}

// Brush parameters adjusted with keys; persist across strokes.
static int BrushShapeMode = 0; // 0 cube, 1 sphere, 2 plane
static bool EmissiveMode = false;
static float leftBut = 1.0;
static float BrushRadius = 0.01;
static float BrushSmoothness = 0.001f;
static float BrushDistanceFromCamera = 0.2f;

SdfEditor::SdfEditor(EngineContext& ctx)
{
	this->base = ctx.base;
	this->scene = ctx.scene;

	const int blocksPerAxis = static_cast<int>(chunkBlocksPerAxis);
	const int numBlocks = blocksPerAxis * blocksPerAxis * blocksPerAxis;
#ifdef BAKEIT_SDF_TEXTURE_ATLAS
	// Texture-native field: the "atlas" is a sparse tile pool; capacity = tiles.
	const int kTilesPerAxis = BAKEIT_SDF_TILE_TILES_PER_AXIS;
	const int atlasCapacityBricks = kTilesPerAxis * kTilesPerAxis * kTilesPerAxis;
#else
	const int atlasCapacityBricks = (kAtlasDim * kAtlasDim * kAtlasDim) / kBrickSamplesPerBlock;
#endif

	ComputeInput input;
#ifdef BAKEIT_SDF_TEXTURE_ATLAS
	// Brush writes the field into the sparse tile pool (binding 1 = storage
	// image) instead of bricks[]; the other SSBOs (block table, counter,
	// colors, dirty, free-list) are identical to the SSBO build.
	input.computeShaderPath = "shaders/Sdf/SdfBrushTexCS.spv";
	input.descriptorTypes = {
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
	};
#else
	input.computeShaderPath = "shaders/Sdf/SdfBrushCS.spv";
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
#endif
	comp = new ComputePipeline(base, input);
	computeUBO = base->vbuffer->createUniformBuffers({ sizeof(sdfEditUBO) });

	computeSBO = base->vbuffer->createStorageBuffers({
#ifdef BAKEIT_SDF_TEXTURE_ATLAS
		sizeof(float) * 16,                                  // 0: brick atlas DUMMY (field lives in the tile pool; render binding 3 just needs a valid buffer)
#else
		sizeof(float) * kAtlasDim * kAtlasDim * kAtlasDim,   // 0: brick atlas
#endif
		sizeof(int) * numBlocks,                             // 1: block table
		sizeof(int) * 2,                                     // 2: atlas tail counter + free-stack pop cursor
		sizeof(glm::vec4) * numBlocks,                       // 3: block colors
		sizeof(int),                                         // 4: dirty counter
		sizeof(BlockSpatialData) * numBlocks,                // 5: block spatial data
		sizeof(int),                                         // 6: free stack count
		sizeof(int) * atlasCapacityBricks                    // 7: free stack
	});
	int zeroInit = 0;
	int zeroCounters[2] = { 0, 0 };
	computeSBO[2].map(base->device, sizeof(zeroCounters), zeroCounters);
	computeSBO[4].map(base->device, sizeof(int), &zeroInit);
	computeSBO[6].map(base->device, sizeof(int), &zeroInit);
	std::vector<int32_t> initialBlocks(static_cast<size_t>(numBlocks), kBlockEmpty);
	std::vector<glm::vec4> initialColors(static_cast<size_t>(numBlocks), glm::vec4(0.0f));
	computeSBO[1].map(base->device, sizeof(int32_t) * initialBlocks.size(), initialBlocks.data());
	computeSBO[3].map(base->device, sizeof(glm::vec4) * initialColors.size(), initialColors.data());

	volume.stats.capacityBricks = atlasCapacityBricks;
	volume.stats.usedBricks = 0;
	volume.stats.tailBricks = 0;
	volume.stats.freeBricks = 0;
	volume.stats.usage = 0.0f;
	volume.blockSizeWS = chunkBlockSizeWS;
	ComputeChunkBounds(chunkBlocksPerAxis, chunkBlockSizeWS, chunkTranslationWS,
		volume.boundsMinWS, volume.boundsMaxWS);

	uploadChunkSpatialData();

#ifdef BAKEIT_SDF_TEXTURE_ATLAS
	// Sparse tile pool: kTilesPerAxis^3 tiles, each 9^3 texels (8 cells + 1 pad).
	// R16F, GENERAL layout - bound as a storage image to the brush (imageStore)
	// and as a sampler3D to the render (binding 2). Allocated tiles are fully
	// written by the brush before being read, so no clear is needed.
	tilePoolDim = kTilesPerAxis * 9;
	ResourceTypes::Texture3D tilePoolTex =
		base->vbuffer->createTexture3DR16F(tilePoolDim, tilePoolDim, tilePoolDim);
	tilePoolTexture = ResourceManager::getTexture(tilePoolTex.id);
	scene->ptextures.push_back(tilePoolTex);
	tilePoolTextureIndex = static_cast<int>(scene->ptextures.size()) - 1;

	// Brush bindings: 0 UBO, 1 tile pool (image), 2-7 block table / counter /
	// colors / dirty / free-count / free-stack (bricks[] is NOT bound here).
	comp->setupDescriptors(computeUBO, { tilePoolTexture },
		{ computeSBO[1], computeSBO[2], computeSBO[3], computeSBO[4], computeSBO[6], computeSBO[7] });
	comp->dispatch(1, 1, 1);
#else
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
#endif

	// GPU-driven draw list for the scene mesh: counting-sorted by camera
	// distance (front-to-back for early-Z) straight from blocks[]; the mesh
	// draws via vkCmdDrawIndexedIndirect, so brush edits never need a CPU
	// readback or a command-buffer re-record to change what is drawn.
	if (Mesh* fineMesh = findEditableMesh()) {
		if (fineMesh->hasInstanceIdBuffer) {
			base->vbuffer->createIndirectDrawBuffer(*fineMesh);
			drawList = new SdfDrawList(
				base,
				computeSBO[1],
				fineMesh->instanceIdBuffer,
				sizeof(int32_t) * fineMesh->instanceCapacity,
				fineMesh->indirectDrawBuffer);
		}
	}

	// --- Coarse LOD atlas: stamped directly by the brush (desktop only) ----
#ifndef BAKEIT_PLATFORM_ANDROID
	coarseBlocksPerAxis = blocksPerAxis / coarseN;
	const int numCoarseBlocks = coarseBlocksPerAxis * coarseBlocksPerAxis * coarseBlocksPerAxis;
	coarseUBO = base->vbuffer->createUniformBuffers({ sizeof(sdfEditUBO) });
	// Slot 0/6 (legacy SSBO brick atlas + its free stack) are UNUSED now - the
	// dense 3D texture replaces them - so allocate them tiny. They only stay so
	// the render's binding-9 descriptor (unread) has a valid buffer; at the N=2
	// coarse grid a full brick atlas would otherwise be ~287 MB.
	coarseSBO = base->vbuffer->createStorageBuffers({
		sizeof(float) * 16,                                       // 0: (legacy brick atlas - UNUSED)
		sizeof(int) * numCoarseBlocks,                            // 1: coarse block table
		sizeof(int) * 2,                                          // 2: tail + alloc cursor
		sizeof(glm::vec4) * numCoarseBlocks,                      // 3: coarse block colors
		sizeof(int),                                              // 4: dirty counter
		sizeof(int),                                              // 5: free brick count
		sizeof(int) * 16                                          // 6: (legacy free stack - UNUSED)
	});
	std::vector<int32_t> coarseBlocksInit(static_cast<size_t>(numCoarseBlocks), kBlockEmpty);
	coarseSBO[1].map(base->device, sizeof(int32_t) * coarseBlocksInit.size(), coarseBlocksInit.data());
	int coarseZeros2[2] = { 0, 0 };
	coarseSBO[2].map(base->device, sizeof(coarseZeros2), coarseZeros2);
	int coarseZero = 0;
	coarseSBO[4].map(base->device, sizeof(int), &coarseZero);
	coarseSBO[5].map(base->device, sizeof(int), &coarseZero);

	// Dense coarse field as a 3D texture (TEST path). One R16F volume covers the
	// whole coarse domain (coarseBlocksPerAxis*8 + 1 boundary sample), cleared to
	// +FAR so the first stamp's smin/boolean starts clean. Wired to object 0's
	// material so the brick FS samples it through binding-2 texSampler.
	coarseAtlasDim = coarseBlocksPerAxis * 8 + 1;
	ResourceTypes::Texture3D coarseTex =
		base->vbuffer->createTexture3DR16F(coarseAtlasDim, coarseAtlasDim, coarseAtlasDim);
	coarseAtlasTexture = ResourceManager::getTexture(coarseTex.id);
	scene->ptextures.push_back(coarseTex);
	coarseAtlasTextureIndex = static_cast<int>(scene->ptextures.size()) - 1;
	clearCoarseAtlas();

	// Coarse stamp compute writes the dense texture (imageStore) + the coarse
	// block table; no brick atlas / allocation / free list. Bindings: 0 UBO,
	// 1 storage image (the dense field), 2 coarse block table.
	ComputeInput coarseBrushInput;
	coarseBrushInput.computeShaderPath = "shaders/Sdf/SdfCoarseBrushCS.spv";
	coarseBrushInput.descriptorTypes = {
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
	};
	coarseBrushComp = new ComputePipeline(base, coarseBrushInput);
	coarseBrushComp->setupDescriptors(coarseUBO, { coarseAtlasTexture }, { coarseSBO[1] });
	coarseBrushComp->dispatch(1, 1, 1); // create command buffer + signaled fence

#ifndef BAKEIT_SDF_TEXTURE_ATLAS
	// --- Fine dense 3D texture (SSBO-vs-texture perf test) -----------------
	// One R16F volume at FINE resolution (chunkBlocksPerAxis*8 + 1 = 513),
	// filled from the SSBO brick atlas by SdfFineToTexCS. Wired to object 0's
	// material (binding 2 texSampler) so the brick FS can flip its fine fetch
	// between the SSBO and this texture. Cleared to +FAR (outside).
	// (Disabled in the texture-native build - the tile pool is the field there.)
	fineAtlasDim = static_cast<int>(chunkBlocksPerAxis) * 8 + 1;
	ResourceTypes::Texture3D fineTex =
		base->vbuffer->createTexture3DR16F(fineAtlasDim, fineAtlasDim, fineAtlasDim);
	fineAtlasTexture = ResourceManager::getTexture(fineTex.id);
	scene->ptextures.push_back(fineTex);
	fineAtlasTextureIndex = static_cast<int>(scene->ptextures.size()) - 1;
	clearFineAtlas();

	// Convert pass: blocks[] + bricks[] -> dense texture (imageStore). No UBO.
	// Bindings (setupDescriptors order = [images][sbos]): 0 storage image,
	// 1 block table, 2 brick atlas.
	ComputeInput fineToTexInput;
	fineToTexInput.computeShaderPath = "shaders/Sdf/SdfFineToTexCS.spv";
	fineToTexInput.descriptorTypes = {
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
	};
	fineToTexInput.pushConstantSize = 2 * sizeof(glm::ivec4); // regionBase + phase
	fineToTexComp = new ComputePipeline(base, fineToTexInput);
	fineToTexComp->setupDescriptors({}, { fineAtlasTexture }, { computeSBO[1], computeSBO[0] });
	// Create the command buffer + signaled fence WITHOUT recording a dispatch:
	// this pipeline uses push constants, and dispatch(1,1,1) would record a
	// vkCmdDispatch with none set (validation VUID-...-maintenance4-08602). The
	// real recording (with push constants) happens in dispatchFineToTex().
	fineToTexComp->ensureCommandResources();
#endif // !BAKEIT_SDF_TEXTURE_ATLAS
#endif
}

void SdfEditor::uploadChunkSpatialData()
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
	uploadedChunkTranslationWS = chunkTranslationWS;
}

Mesh* SdfEditor::findEditableMesh()
{
	for (Object& obj : scene->objects) {
		if (!obj.hasMesh) continue;
		for (size_t i = 0; i < obj.meshID.size(); ++i) {
			if (i >= obj.shader.size()) continue;
			if (obj.shader[i].graphicsPipeline != scene->pipelines[4]) continue;
			return &scene->meshes[obj.meshID[i]];
		}
	}
	return nullptr;
}

Object* SdfEditor::findEditableObject()
{
	for (Object& obj : scene->objects) {
		if (!obj.hasMesh) continue;
		for (size_t i = 0; i < obj.meshID.size(); ++i) {
			if (i >= obj.shader.size()) continue;
			if (obj.shader[i].graphicsPipeline != scene->pipelines[4]) continue;
			return &obj;
		}
	}
	return nullptr;
}

void SdfEditor::clearCoarseAtlas()
{
#ifndef BAKEIT_PLATFORM_ANDROID
	if (coarseAtlasTexture.image == VK_NULL_HANDLE) {
		return;
	}
	VkCommandBuffer cmd = base->vulkandevice->beginSingleTimeCommands();
	VkClearColorValue clearVal{};
	clearVal.float32[0] = 100.01f; // FAR_VALUE (matches SdfCoarseBrushCS)
	VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCmdClearColorImage(cmd, coarseAtlasTexture.image, VK_IMAGE_LAYOUT_GENERAL, &clearVal, 1, &range);
	base->vulkandevice->endSingleTimeCommands(cmd);
#endif
}

void SdfEditor::clearFineAtlas()
{
#ifndef BAKEIT_PLATFORM_ANDROID
	if (fineAtlasTexture.image == VK_NULL_HANDLE) {
		return;
	}
	VkCommandBuffer cmd = base->vulkandevice->beginSingleTimeCommands();
	VkClearColorValue clearVal{};
	clearVal.float32[0] = 100.01f; // FAR_VALUE (matches SdfFineToTexCS)
	VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCmdClearColorImage(cmd, fineAtlasTexture.image, VK_IMAGE_LAYOUT_GENERAL, &clearVal, 1, &range);
	base->vulkandevice->endSingleTimeCommands(cmd);
	// The whole texture is now +FAR (empty field). Callers (init / clear-scene)
	// also empty the SSBO, so the two are coherent - drop any pending region.
	fineTexDirty = false;
	fineTexDirtyMin = glm::ivec3(1000000);
	fineTexDirtyMax = glm::ivec3(-1000000);
#endif
}

void SdfEditor::dispatchFineToTex()
{
#ifndef BAKEIT_PLATFORM_ANDROID
	if (!fineToTexComp || fineAtlasTexture.image == VK_NULL_HANDLE) {
		return;
	}
	// Localized rebuild over the accumulated dirty block region only. Empty box
	// (nothing stamped since the last convert) -> nothing to do.
	const glm::ivec3 lo = glm::clamp(fineTexDirtyMin, glm::ivec3(0), glm::ivec3(int(chunkBlocksPerAxis) - 1));
	const glm::ivec3 hi = glm::clamp(fineTexDirtyMax, glm::ivec3(0), glm::ivec3(int(chunkBlocksPerAxis) - 1));
	if (glm::any(glm::greaterThan(lo, hi))) {
		return;
	}
	const glm::ivec3 count = hi - lo + glm::ivec3(1);

	VkDevice device = base->device;
	// Caller already checked the fence is signaled; this wait is instant.
	vkWaitForFences(device, 1, &fineToTexComp->computeFence, VK_TRUE, UINT64_MAX);
	vkResetFences(device, 1, &fineToTexComp->computeFence);

	VkCommandBuffer cmd = fineToTexComp->computeCommandBuffer;
	vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo bi{};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vkBeginCommandBuffer(cmd, &bi);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fineToTexComp->computePipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
		fineToTexComp->pipelineLayout, 0, 1, &fineToTexComp->descriptorSet, 0, nullptr);

	// Push constants: regionBase (dirty-region min block) + phase.
	struct { glm::ivec4 regionBase; glm::ivec4 phase; } pc{};
	pc.regionBase = glm::ivec4(lo, 0);

	// Phase 0: clear the region to +FAR. Phase 1: write surface/interior data.
	// A barrier in between makes phase 1's surface pads win over phase 0's +FAR.
	for (int phase = 0; phase < 2; ++phase) {
		pc.phase = glm::ivec4(phase, 0, 0, 0);
		vkCmdPushConstants(cmd, fineToTexComp->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
			0, sizeof(pc), &pc);
		vkCmdDispatch(cmd,
			static_cast<uint32_t>(count.x),
			static_cast<uint32_t>(count.y),
			static_cast<uint32_t>(count.z));
		if (phase == 0) {
			VkImageMemoryBarrier imb{};
			imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			imb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			imb.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			imb.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
			imb.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			imb.image = fineAtlasTexture.image;
			imb.subresourceRange = VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			vkCmdPipelineBarrier(cmd,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				0, 0, nullptr, 0, nullptr, 1, &imb);
		}
	}

	vkEndCommandBuffer(cmd);

	VkSubmitInfo si{};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	vkQueueSubmit(base->queue, 1, &si, fineToTexComp->computeFence);

	// Region consumed; start a fresh accumulation for the next edits.
	fineTexDirtyMin = glm::ivec3(1000000);
	fineTexDirtyMax = glm::ivec3(-1000000);
#endif
}

void SdfEditor::clearScene()
{
	const uint32_t totalBlocks = chunkBlocksPerAxis * chunkBlocksPerAxis * chunkBlocksPerAxis;
	std::vector<int32_t> clearedBlocks(totalBlocks, kBlockEmpty);
	std::vector<glm::vec4> clearedColors(totalBlocks, glm::vec4(0.0f));
	int zero = 0;
	int zeroCounters[2] = { 0, 0 };
	computeSBO[1].map(base->device, sizeof(int32_t) * clearedBlocks.size(), clearedBlocks.data());
	computeSBO[2].map(base->device, sizeof(zeroCounters), zeroCounters);
	computeSBO[3].map(base->device, sizeof(glm::vec4) * clearedColors.size(), clearedColors.data());
	computeSBO[4].map(base->device, sizeof(int), &zero);
	computeSBO[6].map(base->device, sizeof(int), &zero);
	freeStackStableCount = 0;
	if (Mesh* fineMesh = findEditableMesh()) {
		base->vbuffer->setIndirectInstanceCount(*fineMesh, 0);
	}
	volume.stats.dirtyBlockCount = 0;

	// Coarse path: empty the coarse block table and reset the dense field so a
	// cleared scene shows no stale coarse geometry.
#ifndef BAKEIT_PLATFORM_ANDROID
	if (!coarseSBO.empty()) {
		const int numCoarseBlocks = coarseBlocksPerAxis * coarseBlocksPerAxis * coarseBlocksPerAxis;
		std::vector<int32_t> coarseEmpty(static_cast<size_t>(numCoarseBlocks), kBlockEmpty);
		coarseSBO[1].map(base->device, sizeof(int32_t) * coarseEmpty.size(), coarseEmpty.data());
	}
	clearCoarseAtlas();
	clearFineAtlas(); // also marks fineTexDirty so the next idle frame re-converts
#endif
}

void SdfEditor::update(float time)
{
	uploadChunkSpatialData();

	if (!brushInput) {
		return;
	}
	const BrushInput bin = brushInput->poll();
	const glm::dvec2 currentCursorPos = bin.strokeCursor;

	if (bin.shapeTogglePressed) {
		BrushShapeMode = (BrushShapeMode + 1) % kBrushShapeCount;
		std::printf("Brush Shape: %s\n", BrushShapeName(BrushShapeMode));
	}
	if (bin.emissiveTogglePressed) {
		EmissiveMode = !EmissiveMode;
		std::printf("Emissive Mode: %s\n", EmissiveMode ? "ON" : "OFF");
	}

	if (bin.interactionAllowed) {
		if (bin.eraseDown) leftBut = 0.0;
		if (bin.drawDown) leftBut = 1.0;
		BrushRadius += 0.0001f * bin.radiusDelta;
		BrushSmoothness += 0.0001f * bin.smoothnessDelta;
		BrushDistanceFromCamera += 0.005f * bin.distanceDelta;

		if (BrushRadius < 0.001f) BrushRadius = 0.001f;
		// Upper bound: a fifth of the chunk. Unbounded growth makes stamps
		// (and the indicator) engulf the camera - easy to hit by holding an
		// analog input (the VR thumbstick).
		if (BrushRadius > 0.2f) BrushRadius = 0.2f;
		if (BrushSmoothness < 0.0001f) BrushSmoothness = 0.0001f;
		// Upper bound keeps the kernel's keepBand (2x smoothness) - and with
		// it the number of retained near-surface bricks - within budget.
		if (BrushSmoothness > 0.005f) BrushSmoothness = 0.005f;
		if (BrushDistanceFromCamera < 0.02f) BrushDistanceFromCamera = 0.02f;
		if (BrushDistanceFromCamera > 3.0f) BrushDistanceFromCamera = 3.0f;
	}

	// Pottery rotation: the editable object's ModelMatrix (rigid). All brush
	// math runs in OBJECT space - transform the platform's world-space
	// pointing ray with the inverse model, so a held brush carves a ring on
	// a spinning pot. Identity model (desktop today) reduces to the old path.
	Object* editableObject = findEditableObject();
	const glm::mat4 model = editableObject ? editableObject->ModelMatrix : glm::mat4(1.0f);
	const glm::mat4 invModel = glm::inverse(model);

	// Brush placement: the platform's pointing ray, pushed into the chunk
	// bounds (desktop: cursor ray; quest: controller aim pose).
	const glm::vec3 rayDir = glm::normalize(glm::mat3(invModel) * bin.rayDir);
	const glm::vec3 rayOrigin = glm::vec3(invModel * glm::vec4(bin.rayOrigin, 1.0f));

	float tEnter = 0.0f, tExit = 0.0f;
	glm::vec3 brushCenter = rayOrigin + rayDir * BrushDistanceFromCamera; // fallback
	glm::vec3 chunkBoundsMin;
	glm::vec3 chunkBoundsMax;
	ComputeChunkBounds(chunkBlocksPerAxis, chunkBlockSizeWS, chunkTranslationWS, chunkBoundsMin, chunkBoundsMax);
	if (IntersectAabb(rayOrigin, rayDir, chunkBoundsMin, chunkBoundsMax, tEnter, tExit)) {
		float tStart = std::max(tEnter, 0.0f);
		float tCenter = std::min(tStart + BrushDistanceFromCamera, tExit - 0.001f);
		if (tCenter >= tStart) {
			brushCenter = rayOrigin + rayDir * tCenter;
		}
	}
	const glm::vec3 brushCenterLocal = brushCenter - chunkTranslationWS;

	sdfEditUBO pubo;
	pubo.time = time * 0.05f;
	pubo.BrushData = glm::vec4(-brushCenterLocal, BrushRadius);
	pubo.BrushColor = brush.color;
	pubo.BrushAction = leftBut;
	pubo.BrushShape = static_cast<float>(BrushShapeMode);
	pubo.BrushSmoothness = BrushSmoothness;
	pubo.BrushEmissive = EmissiveMode ? 1.0f : 0.0f;
	pubo.BrushRecolor = (bin.recolorModifier && bin.drawDown && bin.interactionAllowed) ? 1.0f : 0.0f;

	// Editor-facing brush state mirror. The indicator overlay draws in world
	// space, so mirror the WORLD center and radius (brushCenter/BrushRadius
	// are object space; the model may carry a uniform world-zoom scale).
	const float modelScale = glm::length(glm::vec3(model[0]));
	const glm::vec3 brushCenterWorld = glm::vec3(model * glm::vec4(brushCenter, 1.0f));
	brush.data = glm::vec4(-brushCenterWorld, BrushRadius * modelScale);

	// World-space chunk bounds for the boundary cage: AABB of the 8
	// transformed corners (exact for scale+translate, conservative under
	// rotation).
	{
		glm::vec3 bmin(1e30f), bmax(-1e30f);
		for (int corner = 0; corner < 8; ++corner) {
			const glm::vec3 c(
				(corner & 1) ? chunkBoundsMax.x : chunkBoundsMin.x,
				(corner & 2) ? chunkBoundsMax.y : chunkBoundsMin.y,
				(corner & 4) ? chunkBoundsMax.z : chunkBoundsMin.z);
			const glm::vec3 w = glm::vec3(model * glm::vec4(c, 1.0f));
			bmin = glm::min(bmin, w);
			bmax = glm::max(bmax, w);
		}
		volume.boundsMinWS = bmin;
		volume.boundsMaxWS = bmax;
	}
	brush.useSphere = (BrushShapeMode == 1);
	brush.shapeMode = BrushShapeMode;
	brush.radius = BrushRadius;
	brush.smoothness = BrushSmoothness;
	brush.zAxis = BrushDistanceFromCamera;
	brush.action = leftBut;
	brush.emissive = EmissiveMode;
	// NOTE: the brush UBO is intentionally only mapped at dispatch time
	// (after the compute fence confirms the previous stamp finished) -
	// mapping it every frame overwrites DispatchBaseBlock while the
	// in-flight compute is reading it, silently voiding stamps.

	// Stroke gesture: a stroke lasts as long as the button is held; stamps
	// happen when the cursor moved far enough since the last executed stamp
	// (deltas accumulate across skipped frames).
	const bool brushPointerDown = bin.interactionAllowed && (bin.drawDown || bin.eraseDown);
	const bool isInitialBrushClick = brushPointerDown && !wasBrushPointerDown;
	wasBrushPointerDown = brushPointerDown;
	if (isInitialBrushClick) {
		strokeNeedsInitialDispatch = true;
	}
	if (!brushPointerDown) {
		previousBrushCursorPos = currentCursorPos;
	}
	const glm::dvec2 cursorDelta = currentCursorPos - previousBrushCursorPos;
	const bool movedEnough = glm::dot(cursorDelta, cursorDelta) > 0.25;
	const bool wantsDispatch = brushPointerDown && (strokeNeedsInitialDispatch || movedEnough);

	bool stampedThisFrame = false;
	if (wantsDispatch && vkGetFenceStatus(base->device, comp->computeFence) == VK_SUCCESS) {
		stampedThisFrame = true;
		fineTexDirty = true; // SSBO field changed -> dense fine texture is stale
		// Stats + free-stack maintenance from the previous stamp (stable:
		// the compute is idle now). The kernel pops the stable bottom of the
		// stack via a cursor and pushes frees on top - same-dispatch reuse
		// races (garbage brick indices, the "glitch cubes"). Compact here so
		// freshly freed bricks become allocatable for the next stamp.
		int counters[2] = { 0, 0 }; // [0] atlas tail (floats), [1] pop cursor
		int freeBrickCount = 0;
		computeSBO[2].read(base->device, sizeof(counters), counters);
		computeSBO[6].read(base->device, sizeof(int), &freeBrickCount);

		const int consumed = std::min(counters[1], freeStackStableCount);
		if (consumed > 0 && freeBrickCount >= consumed) {
			std::vector<int> stack(static_cast<size_t>(freeBrickCount));
			computeSBO[7].read(base->device, sizeof(int) * stack.size(), stack.data());
			stack.erase(stack.begin(), stack.begin() + consumed);
			freeBrickCount -= consumed;
			if (!stack.empty()) {
				computeSBO[7].map(base->device, sizeof(int) * stack.size(), stack.data());
			}
			computeSBO[6].map(base->device, sizeof(int), &freeBrickCount);
		}
		if (counters[1] != 0) {
			counters[1] = 0;
			computeSBO[2].map(base->device, sizeof(counters), counters);
		}
		freeStackStableCount = freeBrickCount;

#ifdef BAKEIT_SDF_TEXTURE_ATLAS
		volume.stats.tailBricks = counters[0]; // counter is in TILE units (1 per block)
#else
		volume.stats.tailBricks = counters[0] / kBrickSamplesPerBlock;
#endif
		volume.stats.freeBricks = std::max(freeBrickCount, 0);
		volume.stats.usedBricks = std::max(volume.stats.tailBricks - volume.stats.freeBricks, 0);

		// Dispatch region: blocks the brush stamp can affect.
		const float effectiveBrushRadius = BrushRadius * (leftBut < 0.5f ? 1.15f : 1.0f);
		const float smoothSupportPadding = glm::max(BrushSmoothness * 8.0f, chunkBlockSizeWS * 2.0f);
		const glm::vec3 brushCenterCompute = brushCenterLocal + glm::vec3(kBrushCenterBias);
		const glm::vec3 brushExtent = glm::vec3(effectiveBrushRadius + smoothSupportPadding);
		const glm::vec3 blockCoordMinF = ((brushCenterCompute - brushExtent) + 0.5f) / chunkBlockSizeWS;
		const glm::vec3 blockCoordMaxF = ((brushCenterCompute + brushExtent) + 0.5f) / chunkBlockSizeWS;
		glm::ivec3 dispatchBlockMin = glm::ivec3(glm::floor(blockCoordMinF)) - glm::ivec3(1);
		glm::ivec3 dispatchBlockMax = glm::ivec3(glm::floor(blockCoordMaxF)) + glm::ivec3(1);
		dispatchBlockMin = glm::clamp(dispatchBlockMin, glm::ivec3(0), glm::ivec3(int(chunkBlocksPerAxis) - 1));
		dispatchBlockMax = glm::clamp(dispatchBlockMax, glm::ivec3(0), glm::ivec3(int(chunkBlocksPerAxis) - 1));
		const glm::ivec3 dispatchBlockCount = dispatchBlockMax - dispatchBlockMin + glm::ivec3(1);

		// Grow the dense-texture dirty region so the localized rebuild covers
		// every block this stamp touched (accumulates across stamps until the
		// next convert consumes + resets it).
		fineTexDirtyMin = glm::min(fineTexDirtyMin, dispatchBlockMin);
		fineTexDirtyMax = glm::max(fineTexDirtyMax, dispatchBlockMax);

		pubo.DispatchBaseBlock = glm::ivec4(dispatchBlockMin, freeStackStableCount);
		computeUBO[0].map(base->device, sizeof(sdfEditUBO), &pubo);
		int dirtyZero = 0;
		computeSBO[4].map(base->device, sizeof(int), &dirtyZero);
		comp->submit(
			static_cast<uint32_t>(dispatchBlockCount.x),
			static_cast<uint32_t>(dispatchBlockCount.y),
			static_cast<uint32_t>(dispatchBlockCount.z));
		strokeNeedsInitialDispatch = false;
		previousBrushCursorPos = currentCursorPos;

		// Coarse LOD: stamp the SAME brush op into the coarse atlas at coarse
		// resolution (ubo._pad0 = coarse grid cells). A clean native low-res SDF
		// instead of a downsampled (aliased) copy, and local/cheap - only the
		// coarse blocks the brush touches. Bump-only allocation: reset the free
		// count each stamp so the free stack can't overflow (erased coarse bricks
		// leak until the free-list compaction lands). Own fence -> non-blocking.
#ifndef BAKEIT_PLATFORM_ANDROID
		if (coarseBrushComp && vkGetFenceStatus(base->device, coarseBrushComp->computeFence) == VK_SUCCESS) {
			const glm::ivec3 coarseMin = dispatchBlockMin / coarseN;
			const glm::ivec3 coarseMax = dispatchBlockMax / coarseN;
			const glm::ivec3 coarseCount = coarseMax - coarseMin + glm::ivec3(1);
			sdfEditUBO cubo = pubo;                              // same brush params
			cubo._pad0 = static_cast<float>(coarseBlocksPerAxis * 8); // coarse grid (128)
			cubo.DispatchBaseBlock = glm::ivec4(coarseMin, 0);   // w unused by the dense shader
			coarseUBO[0].map(base->device, sizeof(sdfEditUBO), &cubo);
			coarseBrushComp->submit(
				static_cast<uint32_t>(coarseCount.x),
				static_cast<uint32_t>(coarseCount.y),
				static_cast<uint32_t>(coarseCount.z));
		}
#endif
	}

	// Rebuild the draw list when its inputs changed: right behind each stamp
	// (blocks[] changed) and whenever the camera moved - the frustum cull and
	// the front-to-back bucket order bake the camera into the list. Skipping
	// unchanged frames avoids a per-frame fence wait + queue submit, and the
	// HUD instance-count readback (GPU -> CPU) runs only on stamp frames so
	// idle/orbit frames never sync back to the CPU.
	Mesh* targetMesh = findEditableMesh();
	if (drawList && targetMesh && targetMesh->useIndirectDraw) {
		// Distance sort runs in object/block space: bring the camera into
		// object space first (matters once the pot rotates).
		const glm::vec3 cameraObject = glm::vec3(invModel * glm::vec4(scene->camera->position, 1.0f));
		const glm::vec3 camBlockUnits =
			(cameraObject - chunkTranslationWS - ChunkLocalCenterOrigin(chunkBlockSizeWS)) / chunkBlockSizeWS + glm::vec3(0.5f);

		// Block-units -> world is a uniform scale + translation (inverse of
		// the camBlockUnits mapping above); chained with proj * view it gives
		// the matrix the frustum planes are extracted from. VR platforms set
		// the camera's combined-eye cull override (one frustum containing
		// both eyes); desktop leaves it zeroed and eye 0 is used.
		const float s = chunkBlockSizeWS;
		const glm::vec3 origin =
			chunkTranslationWS + ChunkLocalCenterOrigin(s) - glm::vec3(0.5f * s);
		glm::mat4 blockToWorld(1.0f);
		blockToWorld[0][0] = s;
		blockToWorld[1][1] = s;
		blockToWorld[2][2] = s;
		blockToWorld[3] = glm::vec4(origin, 1.0f);
		const bool hasCullOverride = scene->camera->cullView != glm::mat4(0.0f);
		const glm::mat4 cullViewProj = hasCullOverride
			? scene->camera->cullProj * scene->camera->cullView
			: scene->camera->proj[0] * scene->camera->view[0];
		// Model goes in front of blockToWorld: block units live in object
		// space, the frustum in world space.
		const glm::mat4 blockToClip = cullViewProj * model * blockToWorld;

		if (stampedThisFrame || blockToClip != lastDrawListBlockToClip) {
			drawList->build(camBlockUnits, blockToClip);
			lastDrawListBlockToClip = blockToClip;
		}
		if (stampedThisFrame) {
			// HUD mirror: last completed list's count (host-visible).
			volume.stats.dirtyBlockCount = static_cast<int>(
				base->vbuffer->readIndirectInstanceCount(*targetMesh));
		}
	}

	if (volume.stats.capacityBricks > 0) {
		volume.stats.usage = glm::clamp(
			static_cast<float>(volume.stats.usedBricks) /
			static_cast<float>(volume.stats.capacityBricks),
			0.0f,
			1.0f);
	}
	else {
		volume.stats.usage = 0.0f;
	}

	// Fine SSBO -> dense texture convert (SSBO-vs-texture test). Only while the
	// toggle is on and the field changed; runs on an IDLE frame (no stamp this
	// frame) once the brush compute has finished, so it reads a settled SSBO.
	// Non-blocking: its own fence gates the next submit. One workgroup per block
	// (9^3 threads) over the 64^3 grid.
#ifndef BAKEIT_PLATFORM_ANDROID
	if (scene->fineTexFetch && fineToTexComp && fineTexDirty && !stampedThisFrame
		&& vkGetFenceStatus(base->device, comp->computeFence) == VK_SUCCESS
		&& vkGetFenceStatus(base->device, fineToTexComp->computeFence) == VK_SUCCESS) {
		dispatchFineToTex(); // clear to +FAR, then SdfFineToTexCS over the 64^3 grid
		fineTexDirty = false;
	}
#endif
}

SdfEditor::~SdfEditor() {
	delete drawList;
	delete comp;
	delete coarseBrushComp;
	delete fineToTexComp;
	for (UBO& ubo : computeUBO) {
		vkDestroyBuffer(base->device, ubo.buffer, nullptr);
		vkFreeMemory(base->device, ubo.bufferMemory, nullptr);
	}
	for (SBO& sbo : computeSBO) {
		vkDestroyBuffer(base->device, sbo.buffer, nullptr);
		vkFreeMemory(base->device, sbo.bufferMemory, nullptr);
	}
	for (UBO& ubo : coarseUBO) {
		vkDestroyBuffer(base->device, ubo.buffer, nullptr);
		vkFreeMemory(base->device, ubo.bufferMemory, nullptr);
	}
	for (SBO& sbo : coarseSBO) {
		vkDestroyBuffer(base->device, sbo.buffer, nullptr);
		vkFreeMemory(base->device, sbo.bufferMemory, nullptr);
	}
}
