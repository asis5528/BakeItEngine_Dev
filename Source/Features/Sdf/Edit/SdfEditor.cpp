#include "SdfEditor.h"
#include "Features/Sdf/SdfBackend.h"
#include "Core/Resources/ResourceManager.h"
#include "Render/Renderer.h" // renderPassDirty: re-record after a coarse-level re-point
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <glm/gtc/matrix_transform.hpp> // glm::rotate (R-rotate operator)

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
	glm::ivec4 CoarseParams = glm::ivec4(0); // x: table-base offset into the shared coarse table/colors
	glm::mat4 BrushRot = glm::mat4(1.0f);    // world -> shape-local sample rotation (R-rotate operator)
};

// Mirrors WeldUbo in shaders/Sdf/SdfWeldCS.comp.
struct alignas(16) sdfWeldUBO {
	glm::mat4 aNormToBGrid;      // A voxel (chunk-normalized) -> B fine-grid coords
	glm::vec4 params;            // x: smin k, y: A grid cells, z: B grid cells, w: A field scale
	glm::ivec4 DispatchBaseBlock; // xyz: A block base, w: free-stack stable count
	glm::ivec4 CoarseParams = glm::ivec4(0); // x: A table-base offset (0 fine)
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
constexpr int32_t kBlockInterior = -999998;
// Block-table entry layout (matches the brush/weld shaders): bit 30 emissive,
// bit 29 MATERIAL (any negative voxel - band padding lacks it), low bits the
// tile index. Connectivity must only walk MATERIAL blocks: allocated
// band-only blocks otherwise form invisible webs that bridge every cut.
constexpr int32_t kBlockEmissiveFlag = 1 << 30;
constexpr int32_t kBlockMaterialFlag = 1 << 29;
constexpr int32_t kBlockIndexMask = kBlockMaterialFlag - 1;

// Mirrors CoarseUbo in shaders/Sdf/SdfDownsampleCS.comp.
struct alignas(16) CoarseDownsampleUBO
{
	glm::ivec4 dims;   // x: coarseN, y: coarseBlocksPerAxis, z: fineBlocksPerAxis
	glm::vec4  params; // x: clampVal (world-dist clamp ~= coarse block size)
};
}

// Brush parameters adjusted with keys; persist across strokes.
static int BrushShapeMode = 0; // 0 cube, 1 sphere, 2 plane
// Legacy 90-degree facing index. The R key is now the Blender-style rotate
// operator (SdfEditor::brushShapeRot carries the rotation as a matrix); this
// stays 0 and only rides along for UI/coarse-UBO compatibility.
static int BrushOrientation = 0;
static bool EmissiveMode = false;
static float leftBut = 1.0;
static float BrushRadius = 0.01;
static float BrushSmoothness = 0.001f;
static float BrushDistanceFromCamera = 0.2f;
// Surface-stick state: ray-t of the material surface under the cursor
// (-1 = aiming at air). Recomputed while hovering, FROZEN during a stroke.
static float surfaceStickT = -1.0f;
// R-rotate modal: confirm-by-click stroke suppression (the confirming click
// must not paint).
static bool suppressStrokeUntilRelease = false;

SdfEditor::SdfEditor(EngineContext& ctx)
{
	this->base = ctx.base;
	this->scene = ctx.scene;

	// One chunk today; reserve so later spawns never reallocate (chunk
	// references must stay valid across a stroke).
	chunks.reserve(kMaxChunks);
	chunks.emplace_back();
	SdfChunk& chunk = chunks[0];

	const int blocksPerAxis = static_cast<int>(chunk.blocksPerAxis);
#ifdef BAKEIT_SDF_TEXTURE_ATLAS
	// Texture-native field: the "atlas" is a sparse tile pool; capacity = tiles.
	const int kTilesPerAxis = BAKEIT_SDF_TILE_TILES_PER_AXIS;
	const int atlasCapacityBricks = kTilesPerAxis * kTilesPerAxis * kTilesPerAxis;
#else
	const int atlasCapacityBricks = (kAtlasDim * kAtlasDim * kAtlasDim) / kBrickSamplesPerBlock;
#endif

	// Shared fine-tile allocator (all chunks): tail counter + pop cursor,
	// free stack count, free stack. Mirrors the coarse pyramid's shared
	// counter - one pool, one allocator, chunk-agnostic tile indices.
	tileAllocSBO = base->vbuffer->createStorageBuffers({
		sizeof(int) * 2,                  // 0: atlas tail counter + free-stack pop cursor
		sizeof(int),                      // 1: free stack count
		sizeof(int) * atlasCapacityBricks // 2: free stack
	});
	int zeroInit = 0;
	int zeroCounters[2] = { 0, 0 };
	tileAllocSBO[0].map(base->device, sizeof(zeroCounters), zeroCounters);
	tileAllocSBO[1].map(base->device, sizeof(int), &zeroInit);

#ifdef BAKEIT_SDF_TEXTURE_ATLAS
	// Sparse tile pool: kTilesPerAxis^3 tiles, each 9^3 texels (8 cells + 1 pad).
	// GENERAL layout - bound as a storage image to the brush (imageStore) and as
	// a sampler3D to the render (binding 2). Allocated tiles are fully written by
	// the brush before being read, so no clear is needed. Texel format is R16F by
	// default; R8_SNORM under BAKEIT_SDF_TILE_R8 (8-bit precision experiment, the
	// shaders scale the field into [-1,1] - see SdfBackend.h). Created BEFORE the
	// first chunk's resources: every chunk's brush descriptors bind this pool.
	tilePoolDim = kTilesPerAxis * 9;
#ifdef BAKEIT_SDF_TILE_R8
	const VkFormat tilePoolFormat = VK_FORMAT_R8_SNORM;
#else
	const VkFormat tilePoolFormat = VK_FORMAT_R16_SFLOAT;
#endif
	ResourceTypes::Texture3D tilePoolTex =
		base->vbuffer->createTexture3DR16F(tilePoolDim, tilePoolDim, tilePoolDim, tilePoolFormat);
	tilePoolTexture = ResourceManager::getTexture(tilePoolTex.id);
	scene->ptextures.push_back(tilePoolTex);
	tilePoolTextureIndex = static_cast<int>(scene->ptextures.size()) - 1;
#endif

	// Primary chunk: field tables + its brush pipeline.
	createChunkResources(chunk);

	volume.stats.capacityBricks = atlasCapacityBricks;
	volume.stats.usedBricks = 0;
	volume.stats.tailBricks = 0;
	volume.stats.freeBricks = 0;
	volume.stats.usage = 0.0f;
	volume.blockSizeWS = chunk.blockSizeWS;
	ComputeChunkBounds(chunk.blocksPerAxis, chunk.blockSizeWS, chunk.translationWS,
		volume.boundsMinWS, volume.boundsMaxWS);

	uploadChunkSpatialData();

	// GPU-driven draw list for the scene mesh: counting-sorted by camera
	// distance (front-to-back for early-Z) straight from blocks[]; the mesh
	// draws via vkCmdDrawIndexedIndirect, so brush edits never need a CPU
	// readback or a command-buffer re-record to change what is drawn.
	// Chunk 0's proxy is the glb cube the Loader gave an instance-id buffer;
	// record its object/mesh slots so update() can drive per-chunk transforms.
	for (size_t oi = 0; oi < scene->objects.size() && chunk.objectIndex < 0; ++oi) {
		Object& obj = scene->objects[oi];
		if (!obj.hasMesh) continue;
		for (size_t si = 0; si < obj.meshID.size(); ++si) {
			if (si >= obj.shader.size()) continue;
			if (obj.shader[si].graphicsPipeline != scene->pipelines[4]) continue;
			chunk.objectIndex = static_cast<int>(oi);
			chunk.meshIndex = static_cast<int>(obj.meshID[si]);
			break;
		}
	}
	if (chunk.meshIndex >= 0) {
		Mesh& fineMesh = scene->meshes[chunk.meshIndex];
		if (fineMesh.hasInstanceIdBuffer) {
			base->vbuffer->createIndirectDrawBuffer(fineMesh);
			chunk.drawList = new SdfDrawList(
				base,
				chunk.sbo[1],
				fineMesh.instanceIdBuffer,
				sizeof(int32_t) * fineMesh.instanceCapacity,
				fineMesh.indirectDrawBuffer);
		}
	}

	// --- Coarse LOD pyramid in a SINGLE shared pool (desktop + Quest) ----
	// Levels N=2,4 share one tile pool + one concatenated table/colors + one tile
	// allocator. Each level owns a contiguous table region [tableBase, +(64/N)^3); the
	// shared counter hands out absolute tile indices so levels never collide, and the
	// pool keeps 32 tiles/axis (unchanged addressing). The render reads the shared
	// pool/table always (no per-level re-point). The stamp REUSES SdfBrushTexCS, with
	// the per-level UBO carrying that level's tableBase + coarse grid resolution.
	const int kCoarseTilesPerAxis = BAKEIT_SDF_TILE_TILES_PER_AXIS;
	const int coarseTileCapacity = kCoarseTilesPerAxis * kCoarseTilesPerAxis * kCoarseTilesPerAxis;
	const int kCoarseLevelN[kCoarseLevels] = { 2, 4 };
	coarseTableTotal = 0;
	for (int li = 0; li < kCoarseLevels; ++li) {
		CoarseLevel& lvl = coarseLevels[li];
		lvl.N = kCoarseLevelN[li];
		lvl.blocksPerAxis = blocksPerAxis / lvl.N;
		lvl.tableBase = coarseTableTotal;
		coarseTableTotal += lvl.blocksPerAxis * lvl.blocksPerAxis * lvl.blocksPerAxis;
	}
#ifdef BAKEIT_SDF_TILE_R8
	const VkFormat coarseTileFormat = VK_FORMAT_R8_SNORM;
#else
	const VkFormat coarseTileFormat = VK_FORMAT_R16_SFLOAT;
#endif
	// Shared SBO set (same 7-slot layout as the fine compute), table[1]/colors[3] sized
	// to the concatenation of all level regions; counter[2] is the single tile allocator.
	coarseSharedSBO = base->vbuffer->createStorageBuffers({
		sizeof(float) * 16,                          // 0: dummy bricks (FS binding 10 placeholder)
		sizeof(int) * coarseTableTotal,              // 1: concatenated block table
		sizeof(int) * 2,                             // 2: tail + alloc cursor (shared)
		sizeof(glm::vec4) * coarseTableTotal,        // 3: concatenated block colors
		sizeof(int),                                 // 4: dirty counter
		sizeof(int),                                 // 5: free brick count
		sizeof(int) * coarseTileCapacity             // 6: free stack
	});
	std::vector<int32_t> coarseBlocksInit(static_cast<size_t>(coarseTableTotal), kBlockEmpty);
	coarseSharedSBO[1].map(base->device, sizeof(int32_t) * coarseBlocksInit.size(), coarseBlocksInit.data());
	int coarseZeros2[2] = { 0, 0 };
	coarseSharedSBO[2].map(base->device, sizeof(coarseZeros2), coarseZeros2);
	int coarseZero = 0;
	coarseSharedSBO[4].map(base->device, sizeof(int), &coarseZero);
	coarseSharedSBO[5].map(base->device, sizeof(int), &coarseZero);

	// Shared tile pool (format follows the fine pool: R8_SNORM under BAKEIT_SDF_TILE_R8).
	coarseSharedPoolDim = kCoarseTilesPerAxis * 9;
	ResourceTypes::Texture3D coarseTileTex = base->vbuffer->createTexture3DR16F(
		coarseSharedPoolDim, coarseSharedPoolDim, coarseSharedPoolDim, coarseTileFormat);
	coarseSharedPool = ResourceManager::getTexture(coarseTileTex.id);
	scene->ptextures.push_back(coarseTileTex);
	coarseSharedPoolIndex = static_cast<int>(scene->ptextures.size()) - 1;

	// One stamp pipeline per level, all bound to the SHARED pool + SHARED SBOs; the
	// per-level UBO carries tableBase so its table/colors writes land in its region.
	// Bindings mirror SdfBrushTexCS: 0 UBO, 1 tile pool, 2-7 blocks/counter/colors/dirty/free.
	for (int li = 0; li < kCoarseLevels; ++li) {
		CoarseLevel& lvl = coarseLevels[li];
		lvl.ubo = base->vbuffer->createUniformBuffers({ sizeof(sdfEditUBO) });
		ComputeInput coarseBrushInput;
		coarseBrushInput.computeShaderPath = "shaders/Sdf/SdfBrushTexCS.spv";
		coarseBrushInput.descriptorTypes = {
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
		};
		lvl.brushComp = new ComputePipeline(base, coarseBrushInput);
		lvl.brushComp->setupDescriptors(lvl.ubo, { coarseSharedPool },
			{ coarseSharedSBO[1], coarseSharedSBO[2], coarseSharedSBO[3], coarseSharedSBO[4], coarseSharedSBO[5], coarseSharedSBO[6] });
		lvl.brushComp->dispatch(1, 1, 1); // create command buffer + signaled fence
	}

#ifndef BAKEIT_SDF_TEXTURE_ATLAS
	// --- Fine dense 3D texture (SSBO-vs-texture perf test) -----------------
	// One R16F volume at FINE resolution (chunkBlocksPerAxis*8 + 1 = 513),
	// filled from the SSBO brick atlas by SdfFineToTexCS. Wired to object 0's
	// material (binding 2 texSampler) so the brick FS can flip its fine fetch
	// between the SSBO and this texture. Cleared to +FAR (outside).
	// (Disabled in the texture-native build - the tile pool is the field there.)
	fineAtlasDim = static_cast<int>(chunk.blocksPerAxis) * 8 + 1;
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
	fineToTexComp->setupDescriptors({}, { fineAtlasTexture }, { chunk.sbo[1], chunk.sbo[0] });
	// Create the command buffer + signaled fence WITHOUT recording a dispatch:
	// this pipeline uses push constants, and dispatch(1,1,1) would record a
	// vkCmdDispatch with none set (validation VUID-...-maintenance4-08602). The
	// real recording (with push constants) happens in dispatchFineToTex().
	fineToTexComp->ensureCommandResources();
#endif // !BAKEIT_SDF_TEXTURE_ATLAS
}

void SdfEditor::createChunkResources(SdfChunk& chunk)
{
	const int numBlocks =
		static_cast<int>(chunk.blocksPerAxis * chunk.blocksPerAxis * chunk.blocksPerAxis);

	chunk.sbo = base->vbuffer->createStorageBuffers({
#ifdef BAKEIT_SDF_TEXTURE_ATLAS
		sizeof(float) * 16,                                  // 0: brick atlas DUMMY (field lives in the tile pool; render binding 4 just needs a valid buffer)
#else
		sizeof(float) * kAtlasDim * kAtlasDim * kAtlasDim,   // 0: brick atlas (PER CHUNK - the legacy SSBO build cannot afford >1 chunk)
#endif
		sizeof(int) * numBlocks,                             // 1: block table
		sizeof(int) * 2,                                     // 2: filler (ex tail counter; still bound as graphics binding 6)
		sizeof(glm::vec4) * numBlocks,                       // 3: block colors
		sizeof(int),                                         // 4: dirty counter
		sizeof(BlockSpatialData) * numBlocks,                // 5: block spatial data
		sizeof(int),                                         // 6: filler (allocator moved to shared tileAllocSBO)
		sizeof(int) * 4                                      // 7: filler (allocator moved to shared tileAllocSBO)
	});
	int zeroInit = 0;
	chunk.sbo[4].map(base->device, sizeof(int), &zeroInit);
	std::vector<int32_t> initialBlocks(static_cast<size_t>(numBlocks), kBlockEmpty);
	std::vector<glm::vec4> initialColors(static_cast<size_t>(numBlocks), glm::vec4(0.0f));
	chunk.sbo[1].map(base->device, sizeof(int32_t) * initialBlocks.size(), initialBlocks.data());
	chunk.sbo[3].map(base->device, sizeof(glm::vec4) * initialColors.size(), initialColors.data());

	// The chunk's own brush stamp pipeline, bound to its tables + the SHARED
	// pool/allocator. Mapped-at-dispatch UBO per chunk, so a stamp in flight
	// on one chunk never races another chunk's UBO write.
	chunk.brushUbo = base->vbuffer->createUniformBuffers({ sizeof(sdfEditUBO) });
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
	chunk.brushComp = new ComputePipeline(base, input);
	chunk.brushComp->setupDescriptors(chunk.brushUbo, { tilePoolTexture },
		{ chunk.sbo[1], tileAllocSBO[0], chunk.sbo[3], chunk.sbo[4], tileAllocSBO[1], tileAllocSBO[2] });
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
	chunk.brushComp = new ComputePipeline(base, input);
	chunk.brushComp->setupDescriptors(chunk.brushUbo, {},
		{ chunk.sbo[0], chunk.sbo[1], tileAllocSBO[0], chunk.sbo[3], chunk.sbo[4], tileAllocSBO[1], tileAllocSBO[2] });
#endif
	// Benign UBO for the warm-up dispatch below (zero radius, remove-mode:
	// touches nothing) - the buffer memory is otherwise undefined and the
	// one warm-up workgroup would read garbage dispatch coordinates.
	sdfEditUBO initUbo{};
	chunk.brushUbo[0].map(base->device, sizeof(sdfEditUBO), &initUbo);
	chunk.brushComp->dispatch(1, 1, 1); // create command buffer + signaled fence
}

SdfChunk& SdfEditor::spawnChunk(const glm::vec3& translationWS, bool debugStamp, bool rebuildScene)
{
	// Recycle a retired slot first (weld-absorbed, erased-to-nothing, or
	// scene-cleared chunk): its GPU resources, mesh, object and descriptors
	// all still exist - only the bookkeeping needs resetting. Skipped for
	// debug spawns (init-time only, nothing to recycle yet).
	if (!debugStamp) {
		for (size_t i = 1; i < chunks.size(); ++i) {
			SdfChunk& c = chunks[i];
			if (c.active || c.meshIndex < 0 || c.objectIndex < 0 || !c.brushComp) {
				continue;
			}
			vkWaitForFences(base->device, 1, &c.brushComp->computeFence, VK_TRUE, UINT64_MAX);
			// Retired slots already have empty tables, but a scene-clear may
			// have wiped the shared allocator underneath a stale table - wipe
			// defensively (release path, one map).
			const uint32_t totalB = c.blocksPerAxis * c.blocksPerAxis * c.blocksPerAxis;
			std::vector<int32_t> initialBlocks(static_cast<size_t>(totalB), kBlockEmpty);
			std::vector<glm::vec4> initialColors(static_cast<size_t>(totalB), glm::vec4(0.0f));
			c.sbo[1].map(base->device, sizeof(int32_t) * initialBlocks.size(), initialBlocks.data());
			c.sbo[3].map(base->device, sizeof(glm::vec4) * initialColors.size(), initialColors.data());
			int zeroDirty = 0;
			c.sbo[4].map(base->device, sizeof(int), &zeroDirty);
			c.cpuTableDirty = true;
			c.translationWS = translationWS;
			c.contentMinOS = glm::vec3(0.0f);
			c.contentMaxOS = glm::vec3(-1.0f);
			c.physical = false;
			c.asleep = false;
			c.velocityWS = glm::vec3(0.0f);
			c.cutSuspects.clear();
			c.lastDrawListBlockToClip = glm::mat4(0.0f);
			scene->objects[c.objectIndex].ModelMatrix = glm::mat4(1.0f);
			base->vbuffer->updateIndirectInstanceList(scene->meshes[c.meshIndex], {});
			c.active = true;
			std::printf("Spawn: recycled chunk slot %d\n", static_cast<int>(i));
			return c;
		}
	}

	// NOTE: the render shaders hardcode BLOCKS_PER_AXIS = 64, so every chunk
	// is a 64^3 block grid for now (spawned chunks can shrink once the VS/FS
	// read the grid size per draw).
	SdfChunk& chunk = chunks.emplace_back();
	chunk.translationWS = translationWS;
	createChunkResources(chunk);

	// Proxy mesh: clone the CPU-side cube geometry of the primary proxy into
	// a mesh with its OWN GPU buffers (Scene's cleanup destroys buffers per
	// mesh, so sharing handles would double-free) + instance/indirect buffers.
	Mesh* proto = findEditableMesh();
	if (!proto) {
		return chunk; // no proxy in the scene yet - field-only chunk
	}
	Mesh mesh{};
	mesh.name = "SdfChunkProxy";
	mesh.vertices = proto->vertices;
	mesh.indices = proto->indices;
	mesh.color = proto->color;
	mesh.texID = proto->texID;
	mesh.matID = proto->matID; // tile-pool material (wired before spawning)
	base->vbuffer->createVertexBuffer(mesh);
	base->vbuffer->createIndexBuffer(mesh);
	const uint32_t totalBlocks = chunk.blocksPerAxis * chunk.blocksPerAxis * chunk.blocksPerAxis;
	base->vbuffer->createInstanceIdBuffer(mesh, totalBlocks);
	base->vbuffer->createIndirectDrawBuffer(mesh);
	base->vbuffer->updateIndirectInstanceList(mesh, {}); // start empty; the draw list fills it
	scene->meshes.push_back(mesh);
	chunk.meshIndex = static_cast<int>(scene->meshes.size()) - 1;

	Object object{};
	object.name = "SdfChunk";
	object.hasMesh = true;
	object.hasAnimation = false;
	object.textureID = 0;
	object.meshID.push_back(static_cast<uint32_t>(chunk.meshIndex));
	object.ModelMatrix = glm::mat4(1.0f);
	object.finalMatrix = glm::mat4(1.0f);
	VulkanShader shader{};
	shader.graphicsPipeline = scene->pipelines[4];
	shader.ubos = base->vbuffer->createUniformBuffers(shader.graphicsPipeline->bufferSize);
	shader.sbos = {
		chunk.sbo[0],       // binding 4: bricks (dummy in the tile build)
		chunk.sbo[1],       // binding 5: block table
		chunk.sbo[2],       // binding 6: filler
		chunk.sbo[3],       // binding 7: block colors
		chunk.sbo[5],       // binding 8: block spatial data
		coarseSharedSBO[1], // binding 9: shared coarse block table
		coarseSharedSBO[0]  // binding 10: coarse dummy bricks
	};
	object.shader.push_back(shader);
	scene->objects.push_back(object);
	chunk.objectIndex = static_cast<int>(scene->objects.size()) - 1;

	Mesh& stored = scene->meshes[chunk.meshIndex];
	chunk.drawList = new SdfDrawList(
		base,
		chunk.sbo[1],
		stored.instanceIdBuffer,
		sizeof(int32_t) * stored.instanceCapacity,
		stored.indirectDrawBuffer);

	// Mid-session spawn: allocate the new object's descriptor set from the
	// LIVE pool (created with spawn headroom - see Renderer::
	// kSpawnDescriptorHeadroom) and flag a command re-record. Nothing is
	// destroyed, so no device-idle wait: safe even mid-drag (stroke chaining).
	if (rebuildScene) {
		scene->renderer->createObjectDescriptor(*scene, chunk.objectIndex);
		scene->renderer->renderPassDirty = true;
	}

	if (!debugStamp) {
		return chunk;
	}

	// Debug content: stamp one smooth sphere at the chunk center so the new
	// chunk is visible immediately (same path a real stroke takes; the draw
	// list picks the blocks up on the next update).
	sdfEditUBO pubo;
	pubo.BrushData = glm::vec4(0.0f, 0.0f, 0.0f, 0.12f); // center (local), radius
	pubo.BrushColor = glm::vec4(0.9f, 0.75f, 0.5f, 1.0f);
	pubo.BrushAction = 1.0f;   // add
	pubo.time = 0.0f;
	pubo.BrushShape = 1.0f;    // sphere
	pubo.BrushSmoothness = 0.002f;
	pubo.BrushEmissive = 0.0f;
	pubo.BrushRecolor = 0.0f;
	// Dispatch region: blocks the stamp can affect (mirrors the stroke path).
	const float pad = glm::max(pubo.BrushSmoothness * 8.0f, chunk.blockSizeWS * 2.0f);
	const glm::vec3 centerCompute = glm::vec3(kBrushCenterBias); // brush at local origin
	const glm::vec3 extent = glm::vec3(pubo.BrushData.w + pad);
	glm::ivec3 blockMin = glm::ivec3(glm::floor((centerCompute - extent + 0.5f) / chunk.blockSizeWS)) - glm::ivec3(1);
	glm::ivec3 blockMax = glm::ivec3(glm::floor((centerCompute + extent + 0.5f) / chunk.blockSizeWS)) + glm::ivec3(1);
	blockMin = glm::clamp(blockMin, glm::ivec3(0), glm::ivec3(int(chunk.blocksPerAxis) - 1));
	blockMax = glm::clamp(blockMax, glm::ivec3(0), glm::ivec3(int(chunk.blocksPerAxis) - 1));
	const glm::ivec3 blockCount = blockMax - blockMin + glm::ivec3(1);
	pubo.DispatchBaseBlock = glm::ivec4(blockMin, 0); // no free-stack pops at init
	chunk.brushUbo[0].map(base->device, sizeof(sdfEditUBO), &pubo);
	chunk.brushComp->submit(
		static_cast<uint32_t>(blockCount.x),
		static_cast<uint32_t>(blockCount.y),
		static_cast<uint32_t>(blockCount.z));

	return chunk;
}

int SdfEditor::chunkForCell(const glm::ivec3& cell)
{
	// A cell's canvas chunk sits exactly at the cell origin with an identity
	// model. Physical or hand-moved chunks are OBJECTS now, not canvas - the
	// cell becomes empty again and can lazily respawn a fresh chunk.
	const glm::vec3 origin(cell);
	for (size_t i = 0; i < chunks.size(); ++i) {
		SdfChunk& c = chunks[i];
		if (!c.active || c.physical) {
			continue;
		}
		if (glm::any(glm::greaterThan(glm::abs(c.translationWS - origin), glm::vec3(1e-4f)))) {
			continue;
		}
		if (c.objectIndex >= 0
			&& scene->objects[c.objectIndex].ModelMatrix != glm::mat4(1.0f)) {
			continue;
		}
		return static_cast<int>(i);
	}
	return -1;
}

int SdfEditor::ensureCellChunk(const glm::ivec3& cell)
{
	const int existing = chunkForCell(cell);
	if (existing >= 0) {
		return existing;
	}
	bool canSpawn = chunks.size() < static_cast<size_t>(kMaxChunks);
	if (!canSpawn) {
		for (size_t i = 1; i < chunks.size(); ++i) { // a retired slot is as good as headroom
			if (!chunks[i].active && chunks[i].meshIndex >= 0 && chunks[i].objectIndex >= 0 && chunks[i].brushComp) {
				canSpawn = true;
				break;
			}
		}
	}
	if (!canSpawn) {
		return -1; // 32 live pieces: the stamp proceeds into existing chunks only
	}
	SdfChunk& c = spawnChunk(glm::vec3(cell), /*debugStamp=*/false, /*rebuildScene=*/true);
	const int idx = static_cast<int>(&c - chunks.data());
	std::printf("Canvas cell (%d,%d,%d) -> chunk %d\n", cell.x, cell.y, cell.z, idx);
	return idx;
}

void SdfEditor::uploadChunkSpatialData()
{
	for (SdfChunk& chunk : chunks) {
		if (chunk.translationWS == chunk.uploadedTranslationWS) {
			continue;
		}

		const uint32_t totalBlocks = chunk.blocksPerAxis * chunk.blocksPerAxis * chunk.blocksPerAxis;
		std::vector<BlockSpatialData> blockSpatial(static_cast<size_t>(totalBlocks));
		BuildChunkSpatialData(blockSpatial, chunk.blocksPerAxis, chunk.blockSizeWS, chunk.translationWS);
		chunk.sbo[5].map(
			base->device,
			sizeof(BlockSpatialData) * blockSpatial.size(),
			blockSpatial.data());
		chunk.uploadedTranslationWS = chunk.translationWS;
	}
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

void SdfEditor::weldChunkInto(int srcIndex, int dstIndex)
{
	if (srcIndex == dstIndex
		|| srcIndex < 0 || srcIndex >= static_cast<int>(chunks.size())
		|| dstIndex < 0 || dstIndex >= static_cast<int>(chunks.size())) {
		return;
	}
	SdfChunk& src = chunks[srcIndex]; // B: the chunk being absorbed
	SdfChunk& dst = chunks[dstIndex]; // A: the chunk that receives the field
	if (!src.active || !dst.active) {
		return;
	}
	if (src.blockSizeWS != dst.blockSizeWS || src.blocksPerAxis != dst.blocksPerAxis) {
		std::printf("Weld: mismatched chunk grids (unsupported)\n");
		return;
	}

	// The weld and the CPU absorb below rewrite tables + the shared
	// allocator: wait out every in-flight stamp first. Synchronous on
	// purpose - welding is a discrete user action (release-path semantics),
	// never part of a drag.
	for (SdfChunk& c : chunks) {
		if (c.brushComp) {
			vkWaitForFences(base->device, 1, &c.brushComp->computeFence, VK_TRUE, UINT64_MAX);
		}
	}

	const glm::mat4 modelA = (dst.objectIndex >= 0)
		? scene->objects[dst.objectIndex].ModelMatrix : glm::mat4(1.0f);
	const glm::mat4 modelB = (src.objectIndex >= 0)
		? scene->objects[src.objectIndex].ModelMatrix : glm::mat4(1.0f);
	const glm::mat4 invModelA = glm::inverse(modelA);
	const glm::mat4 invModelB = glm::inverse(modelB);

	// A-block region overlapped by B: B's bounds corners into A's "compute
	// space" (object - translation + center bias: the brush's block mapping).
	glm::vec3 bMin, bMax;
	ComputeChunkBounds(src.blocksPerAxis, src.blockSizeWS, src.translationWS, bMin, bMax);
	glm::vec3 lo(1e30f), hi(-1e30f);
	for (int corner = 0; corner < 8; ++corner) {
		const glm::vec3 c(
			(corner & 1) ? bMax.x : bMin.x,
			(corner & 2) ? bMax.y : bMin.y,
			(corner & 4) ? bMax.z : bMin.z);
		const glm::vec3 objA = glm::vec3(invModelA * modelB * glm::vec4(c, 1.0f));
		const glm::vec3 v = objA - dst.translationWS + glm::vec3(kBrushCenterBias);
		lo = glm::min(lo, v);
		hi = glm::max(hi, v);
	}
	const glm::ivec3 lastBlock = glm::ivec3(static_cast<int>(dst.blocksPerAxis) - 1);
	const glm::ivec3 blockMinU = glm::ivec3(glm::floor((lo + 0.5f) / dst.blockSizeWS)) - glm::ivec3(1);
	const glm::ivec3 blockMaxU = glm::ivec3(glm::floor((hi + 0.5f) / dst.blockSizeWS)) + glm::ivec3(1);
	if (glm::any(glm::lessThan(blockMaxU, glm::ivec3(0)))
		|| glm::any(glm::greaterThan(blockMinU, lastBlock))) {
		std::printf("Weld: chunk %d does not overlap chunk %d - move it closer first\n", srcIndex, dstIndex);
		return;
	}
	const glm::ivec3 blockMin = glm::clamp(blockMinU, glm::ivec3(0), lastBlock);
	const glm::ivec3 blockMax = glm::clamp(blockMaxU, glm::ivec3(0), lastBlock);
	const glm::ivec3 blockCount = blockMax - blockMin + glm::ivec3(1);

	// A voxel-normalized -> B fine grid as one affine chain (matches the
	// brush's voxel<->object mapping on both sides; see SdfWeldCS.comp).
	const float bCells = static_cast<float>(src.blocksPerAxis * 8u);
	glm::mat4 t1(1.0f);
	t1[3] = glm::vec4(dst.translationWS - glm::vec3(kBrushCenterBias), 1.0f);
	glm::mat4 t2(1.0f);
	t2[3] = glm::vec4(glm::vec3(0.5f + kBrushCenterBias) - src.translationWS, 1.0f);
	glm::mat4 sc(1.0f);
	sc[0][0] = sc[1][1] = sc[2][2] = bCells;

	sdfWeldUBO wubo;
	wubo.aNormToBGrid = sc * t2 * invModelB * modelA * t1;
	wubo.params = glm::vec4(
		glm::max(BrushSmoothness, 0.001f),               // blend width = brush smoothness knob
		static_cast<float>(dst.blocksPerAxis * 8u),
		bCells,
		0.0f);                                           // A field scale: 0 -> fine default
	wubo.DispatchBaseBlock = glm::ivec4(blockMin, freeStackStableCount);

	// One-shot weld pipelines (fresh descriptors per src/dst pair): the fine
	// weld, plus - when the destination owns the coarse pyramid (chunk 0) -
	// one re-stamp per coarse level so the welded material survives the
	// distance-band LOD (mirrors the coarse brush stamps).
	ComputeInput weldInput;
	weldInput.computeShaderPath = "shaders/Sdf/SdfWeldCS.spv";
	weldInput.descriptorTypes = {
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  // 1: A pool (written)
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // 2: A block table
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // 3: A tail counter
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // 4: A block colors
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // 5: A dirty counter
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // 6: A free count
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // 7: A free stack
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // 8: B block table
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE   // 9: B pool (read, always fine)
	};
	std::vector<ComputePipeline*> passes;
	std::vector<std::vector<UBO>> passUbos;

	{
		ComputePipeline* weld = new ComputePipeline(base, weldInput);
		std::vector<UBO> weldUbo = base->vbuffer->createUniformBuffers({ sizeof(sdfWeldUBO) });
		weldUbo[0].map(base->device, sizeof(sdfWeldUBO), &wubo);
		weld->setupDescriptors(weldUbo, { tilePoolTexture, tilePoolTexture },
			{ dst.sbo[1], tileAllocSBO[0], dst.sbo[3], dst.sbo[4],
			  tileAllocSBO[1], tileAllocSBO[2], src.sbo[1] });
		int zero = 0;
		dst.sbo[4].map(base->device, sizeof(int), &zero);
		weld->ensureCommandResources(); // command buffer + SIGNALED fence: submit() waits on it first
		weld->submit(
			static_cast<uint32_t>(blockCount.x),
			static_cast<uint32_t>(blockCount.y),
			static_cast<uint32_t>(blockCount.z));
		passes.push_back(weld);
		passUbos.push_back(std::move(weldUbo));
	}

	if (dstIndex == 0 && !coarseSharedSBO.empty()) {
		// Coarse re-stamp: same weld (the normalized A voxel space is
		// resolution-independent, so the matrix carries over), at each
		// level's grid with its table region + widened R8 scale, into the
		// coarse pool. Bump-only alloc like the coarse brush (w = 0).
		int coarseFreeZero = 0;
		coarseSharedSBO[5].map(base->device, sizeof(int), &coarseFreeZero);
		for (CoarseLevel& lvl : coarseLevels) {
			if (!lvl.brushComp) {
				continue;
			}
			vkWaitForFences(base->device, 1, &lvl.brushComp->computeFence, VK_TRUE, UINT64_MAX);
			const glm::ivec3 cMin = blockMin / lvl.N;
			const glm::ivec3 cMax = blockMax / lvl.N;
			const glm::ivec3 cCount = cMax - cMin + glm::ivec3(1);
			sdfWeldUBO cubo = wubo;
			cubo.params.y = static_cast<float>(lvl.blocksPerAxis * 8);
			cubo.params.w = 82.0f / static_cast<float>(lvl.N);
			cubo.DispatchBaseBlock = glm::ivec4(cMin, 0);
			cubo.CoarseParams = glm::ivec4(lvl.tableBase, 0, 0, 0);

			ComputePipeline* cweld = new ComputePipeline(base, weldInput);
			std::vector<UBO> cUbo = base->vbuffer->createUniformBuffers({ sizeof(sdfWeldUBO) });
			cUbo[0].map(base->device, sizeof(sdfWeldUBO), &cubo);
			cweld->setupDescriptors(cUbo, { coarseSharedPool, tilePoolTexture },
				{ coarseSharedSBO[1], coarseSharedSBO[2], coarseSharedSBO[3], coarseSharedSBO[4],
				  coarseSharedSBO[5], coarseSharedSBO[6], src.sbo[1] });
			cweld->ensureCommandResources();
			cweld->submit(
				static_cast<uint32_t>(cCount.x),
				static_cast<uint32_t>(cCount.y),
				static_cast<uint32_t>(cCount.z));
			passes.push_back(cweld);
			passUbos.push_back(std::move(cUbo));
		}
	}

	for (ComputePipeline* p : passes) {
		vkWaitForFences(base->device, 1, &p->computeFence, VK_TRUE, UINT64_MAX);
	}

	// Absorb B: collect every tile it owned back into the shared free stack.
	// First compact the stack exactly like update() does - the weld may have
	// POPPED entries (alloc cursor) that must not be double-allocated later.
	const int kIndexMask = kBlockIndexMask;
	const uint32_t numB = src.blocksPerAxis * src.blocksPerAxis * src.blocksPerAxis;
	std::vector<int32_t> bTable(static_cast<size_t>(numB));
	src.sbo[1].read(base->device, sizeof(int32_t) * bTable.size(), bTable.data());
	std::vector<int> freed;
	for (int32_t raw : bTable) {
		if (raw >= 0) {
			freed.push_back(raw & kIndexMask);
		}
	}
	int counters[2] = { 0, 0 };
	tileAllocSBO[0].read(base->device, sizeof(counters), counters);
	int freeCount = 0;
	tileAllocSBO[1].read(base->device, sizeof(int), &freeCount);
	std::vector<int> stack(static_cast<size_t>(glm::max(freeCount, 0)));
	if (freeCount > 0) {
		tileAllocSBO[2].read(base->device, sizeof(int) * stack.size(), stack.data());
	}
	const int consumed = std::min(counters[1], freeStackStableCount);
	if (consumed > 0 && freeCount >= consumed) {
		stack.erase(stack.begin(), stack.begin() + consumed);
	}
	if (counters[1] != 0) {
		counters[1] = 0;
		tileAllocSBO[0].map(base->device, sizeof(counters), counters);
	}
	stack.insert(stack.end(), freed.begin(), freed.end());
	freeCount = static_cast<int>(stack.size());
	if (!stack.empty()) {
		tileAllocSBO[2].map(base->device, sizeof(int) * stack.size(), stack.data());
	}
	tileAllocSBO[1].map(base->device, sizeof(int), &freeCount);
	freeStackStableCount = freeCount;
	volume.stats.freeBricks = freeCount;
	volume.stats.tailBricks = counters[0];
	volume.stats.usedBricks = glm::max(counters[0] - freeCount, 0);

	// Empty B's table (its draw list compacts to 0 instances next build).
	std::fill(bTable.begin(), bTable.end(), kBlockEmpty);
	src.sbo[1].map(base->device, sizeof(int32_t) * bTable.size(), bTable.data());
	int srcDirtyZero = 0;
	src.sbo[4].map(base->device, sizeof(int), &srcDirtyZero);
	src.cutSuspects.clear(); // absorbed: no material left to be suspicious about
	src.active = false;
	if (editableChunkIndex == srcIndex) {
		editableChunkIndex = dstIndex;
	}

	// Both draw lists changed underneath their caches: force rebuilds.
	dst.lastDrawListBlockToClip = glm::mat4(0.0f);
	src.lastDrawListBlockToClip = glm::mat4(0.0f);

	for (ComputePipeline* p : passes) {
		delete p;
	}
	for (std::vector<UBO>& ubos : passUbos) {
		for (UBO& u : ubos) {
			vkDestroyBuffer(base->device, u.buffer, nullptr);
			vkFreeMemory(base->device, u.bufferMemory, nullptr);
		}
	}
	dst.cpuTableDirty = true;
	src.cpuTableDirty = true;
	std::printf("Welded chunk %d into chunk %d (%d tiles freed)\n",
		srcIndex, dstIndex, static_cast<int>(freed.size()));
}

void SdfEditor::retireChunkSlot(int chunkIndex)
{
	if (chunkIndex <= 0 || chunkIndex >= static_cast<int>(chunks.size())) {
		return; // chunk 0 is the primary canvas: cleared, never retired
	}
	SdfChunk& c = chunks[chunkIndex];
	if (!c.active) {
		return;
	}
	if (c.brushComp) {
		vkWaitForFences(base->device, 1, &c.brushComp->computeFence, VK_TRUE, UINT64_MAX);
	}

	// Recycle every allocated tile into the shared free stack (same
	// pop-cursor compaction the weld absorb does: drop the consumed bottom
	// first so the stack only holds live entries).
	const uint32_t numB = c.blocksPerAxis * c.blocksPerAxis * c.blocksPerAxis;
	std::vector<int32_t> cTable(static_cast<size_t>(numB));
	c.sbo[1].read(base->device, sizeof(int32_t) * cTable.size(), cTable.data());
	std::vector<int> freed;
	for (int32_t raw : cTable) {
		if (raw >= 0) {
			freed.push_back(raw & kBlockIndexMask);
		}
	}
	int counters[2] = { 0, 0 };
	tileAllocSBO[0].read(base->device, sizeof(counters), counters);
	int freeCount = 0;
	tileAllocSBO[1].read(base->device, sizeof(int), &freeCount);
	std::vector<int> stack(static_cast<size_t>(glm::max(freeCount, 0)));
	if (freeCount > 0) {
		tileAllocSBO[2].read(base->device, sizeof(int) * stack.size(), stack.data());
	}
	const int consumed = std::min(counters[1], freeStackStableCount);
	if (consumed > 0 && freeCount >= consumed) {
		stack.erase(stack.begin(), stack.begin() + consumed);
	}
	if (counters[1] != 0) {
		counters[1] = 0;
		tileAllocSBO[0].map(base->device, sizeof(counters), counters);
	}
	stack.insert(stack.end(), freed.begin(), freed.end());
	freeCount = static_cast<int>(stack.size());
	if (!stack.empty()) {
		tileAllocSBO[2].map(base->device, sizeof(int) * stack.size(), stack.data());
	}
	tileAllocSBO[1].map(base->device, sizeof(int), &freeCount);
	freeStackStableCount = freeCount;
	volume.stats.freeBricks = freeCount;
	volume.stats.tailBricks = counters[0];
	volume.stats.usedBricks = glm::max(counters[0] - freeCount, 0);

	std::fill(cTable.begin(), cTable.end(), kBlockEmpty);
	c.sbo[1].map(base->device, sizeof(int32_t) * cTable.size(), cTable.data());
	c.cpuTableDirty = true;
	int dirtyZero = 0;
	c.sbo[4].map(base->device, sizeof(int), &dirtyZero);
	c.cutSuspects.clear();
	c.physical = false;
	c.asleep = false;
	c.velocityWS = glm::vec3(0.0f);
	c.contentMinOS = glm::vec3(0.0f);
	c.contentMaxOS = glm::vec3(-1.0f);
	c.lastDrawListBlockToClip = glm::mat4(0.0f);
	if (c.objectIndex >= 0) {
		scene->objects[c.objectIndex].ModelMatrix = glm::mat4(1.0f);
	}
	if (c.meshIndex >= 0) {
		base->vbuffer->updateIndirectInstanceList(scene->meshes[c.meshIndex], {});
	}
	c.active = false;
	if (editableChunkIndex == chunkIndex) {
		editableChunkIndex = 0;
	}
	std::printf("Chunk %d retired (%d tiles freed) - slot reusable\n",
		chunkIndex, static_cast<int>(freed.size()));
}

bool SdfEditor::computeChunkContentBounds(SdfChunk& c)
{
	// Content AABB from the block table (synchronous readback - drop and
	// stroke-release are discrete user actions, weld semantics). Occupied =
	// surface (owns a tile, raw >= 0) or fully-interior sentinel; empty
	// blocks don't hold the chunk up.
	if (c.brushComp) {
		vkWaitForFences(base->device, 1, &c.brushComp->computeFence, VK_TRUE, UINT64_MAX);
	}
	const uint32_t bpa = c.blocksPerAxis;
	std::vector<int32_t> table(static_cast<size_t>(bpa) * bpa * bpa);
	c.sbo[1].read(base->device, sizeof(int32_t) * table.size(), table.data());
	glm::ivec3 bMin(static_cast<int>(bpa));
	glm::ivec3 bMax(-1);
	for (uint32_t z = 0; z < bpa; ++z) {
		for (uint32_t y = 0; y < bpa; ++y) {
			for (uint32_t x = 0; x < bpa; ++x) {
				const int32_t raw = table[x + y * bpa + z * bpa * bpa];
				// Real material only (interior, or surface with the material
				// flag) - allocated band padding must not hold the body up.
				if (raw == kBlockInterior || (raw >= 0 && (raw & kBlockMaterialFlag) != 0)) {
					bMin = glm::min(bMin, glm::ivec3(x, y, z));
					bMax = glm::max(bMax, glm::ivec3(x, y, z));
				}
			}
		}
	}
	if (bMax.x < bMin.x) {
		c.contentMinOS = glm::vec3(0.0f);
		c.contentMaxOS = glm::vec3(-1.0f);
		return false;
	}
	// Block b's object-space center = translation + localCenterOrigin + b*bs
	// (BuildChunkSpatialData's mapping); the AABB extends half a block out.
	const glm::vec3 origin = c.translationWS + ChunkLocalCenterOrigin(c.blockSizeWS);
	c.contentMinOS = origin + glm::vec3(bMin) * c.blockSizeWS - glm::vec3(0.5f * c.blockSizeWS);
	c.contentMaxOS = origin + glm::vec3(bMax) * c.blockSizeWS + glm::vec3(0.5f * c.blockSizeWS);
	return true;
}

void SdfEditor::makeChunkPhysical(int chunkIndex)
{
	if (chunkIndex < 0 || chunkIndex >= static_cast<int>(chunks.size())) {
		return;
	}
	SdfChunk& c = chunks[chunkIndex];
	if (!c.active || c.objectIndex < 0 || c.physical) {
		return;
	}
	if (!computeChunkContentBounds(c)) {
		std::printf("Physics: chunk %d has no content, staying sketch\n", chunkIndex);
		return;
	}
	c.velocityWS = glm::vec3(0.0f);
	c.physical = true;
	c.asleep = false;
	std::printf("Physics: chunk %d is now physical\n", chunkIndex);
}

void SdfEditor::commitPhysics()
{
	// "P": drain the unassigned queue - every sketch chunk becomes physical
	// and starts acting immediately. Already-physical chunks are untouched.
	int promoted = 0;
	for (size_t i = 0; i < chunks.size(); ++i) {
		SdfChunk& c = chunks[i];
		if (!c.active || c.physical || c.objectIndex < 0) {
			continue;
		}
		makeChunkPhysical(static_cast<int>(i));
		if (c.physical) {
			++promoted;
		}
	}
	std::printf("Physics commit: %d chunk(s) promoted\n", promoted);
}

void SdfEditor::splitChunkIslands(int chunkIndex, bool strokeWasCut)
{
	if (chunkIndex < 0 || chunkIndex >= static_cast<int>(chunks.size())) {
		return;
	}
	SdfChunk& parent = chunks[chunkIndex]; // chunks is reserved: spawns below never move it
	if (!parent.active) {
		return;
	}
	if (parent.brushComp) {
		vkWaitForFences(base->device, 1, &parent.brushComp->computeFence, VK_TRUE, UINT64_MAX);
	}

	const int bpa = static_cast<int>(parent.blocksPerAxis);
	const int total = bpa * bpa * bpa;
	std::vector<int32_t> table(static_cast<size_t>(total));
	parent.sbo[1].read(base->device, sizeof(int32_t) * table.size(), table.data());

	// Connectivity only walks REAL material: interior blocks and surface
	// blocks whose stamp saw negative voxels (kBlockMaterialFlag). Allocated
	// band-only blocks (near-surface padding for render quality) are NOT
	// substance - treating them as material let invisible band webs bridge
	// every cut (the "second cut never detected" bug).
	auto isMaterial = [](int32_t raw) {
		return raw == kBlockInterior || (raw >= 0 && (raw & kBlockMaterialFlag) != 0);
	};
	auto cellPos = [bpa](int cell) {
		return glm::ivec3(cell % bpa, (cell / bpa) % bpa, cell / (bpa * bpa));
	};

	// ---- Stage 1: prune the cut suspects (blocks the eraser touched,
	// accumulated across strokes). Blocks that ended up EMPTY are natural
	// barriers, INTERIOR blocks are solid, and band-only blocks hold no
	// material - none of them can hide a cut.
	for (auto it = parent.cutSuspects.begin(); it != parent.cutSuspects.end(); ) {
		const int32_t raw = table[static_cast<size_t>(*it)];
		if (raw >= 0 && (raw & kBlockMaterialFlag) != 0) {
			++it;
		} else {
			it = parent.cutSuspects.erase(it);
		}
	}
	auto isSuspect = [&](int cell) { return parent.cutSuspects.count(cell) != 0; };
	const bool anySuspects = !parent.cutSuspects.empty();

	// ---- Stage 2: provisional flood fill (6-connectivity) over material
	// blocks with the suspects EXCLUDED as barriers. One group -> the object
	// is connected no matter what the suspects hide (the usual case, ~zero
	// cost, no readback). Several groups -> a thin cut may have severed it
	// INSIDE a suspect block, and only then does stage 3 read field data.
	std::vector<int32_t> provIsland(static_cast<size_t>(total), -1);
	int provCount = 0;
	{
		std::vector<int> stack;
		for (int seed = 0; seed < total; ++seed) {
			if (provIsland[seed] >= 0 || !isMaterial(table[seed]) || isSuspect(seed)) {
				continue;
			}
			provIsland[seed] = provCount;
			stack.push_back(seed);
			while (!stack.empty()) {
				const int cell = stack.back();
				stack.pop_back();
				const glm::ivec3 p = cellPos(cell);
				const int nbr[6][3] = { {p.x-1,p.y,p.z}, {p.x+1,p.y,p.z}, {p.x,p.y-1,p.z},
					{p.x,p.y+1,p.z}, {p.x,p.y,p.z-1}, {p.x,p.y,p.z+1} };
				for (const int* n : nbr) {
					if (n[0] < 0 || n[1] < 0 || n[2] < 0 || n[0] >= bpa || n[1] >= bpa || n[2] >= bpa) {
						continue;
					}
					const int ni = n[0] + n[1] * bpa + n[2] * bpa * bpa;
					if (provIsland[ni] < 0 && isMaterial(table[ni]) && !isSuspect(ni)) {
						provIsland[ni] = provCount;
						stack.push_back(ni);
					}
				}
			}
			++provCount;
		}
	}

	// ---- Stage 3 state (kept alive through the move loop: the mixed-block
	// duplication needs the voxel labels).
#ifdef BAKEIT_SDF_TILE_R8
	constexpr int kVoxBytes = 1;
#else
	constexpr int kVoxBytes = 2;
#endif
	const size_t tileStride = static_cast<size_t>(((729 * kVoxBytes + 3) / 4) * 4); // 4-aligned copy offsets
	const int tilesPerAxis = tilePoolDim / 9;
	bool verifyRan = false;
	std::vector<int> sCells;                 // suspect cells (sorted), voxel-labeled
	std::vector<int32_t> sIdxOfCell;         // cell -> sCells index (-1 none)
	std::vector<int> rbCells;                // cells whose tile was read back
	std::vector<int32_t> rbIdxOfCell;        // cell -> rbCells index (-1 none)
	std::vector<uint8_t> voxels;             // read-back tile bytes (tileStride per tile)
	std::vector<int16_t> localLabel;         // per suspect: 8^3 component labels (-1 air)
	std::vector<int> compBase;               // per suspect: first global component id
	int compCount = 0;
	std::vector<int> compIsland;             // component -> final island (-1 = crumb, gets DELETED)
	std::vector<int32_t> blockIsland(static_cast<size_t>(total), -1);
	std::vector<std::vector<int>> islandsPresentOf; // per suspect: distinct chunk-backed islands inside it
	std::vector<char> blockHasCrumb;         // per suspect: holds sub-threshold debris to erase
	// Allocated-but-materialless blocks (render band) never seed or bridge
	// islands, but they must TRAVEL with whichever piece they pad (the
	// cross-brick sampling at a piece's surface needs them). Adopt each from
	// a neighboring assigned block; unattached leftovers (stale band litter)
	// stay in the parent, harmless and invisible.
	auto adoptBandBlocks = [&]() {
		for (bool changed = true; changed; ) {
			changed = false;
			for (int cell = 0; cell < total; ++cell) {
				if (blockIsland[cell] >= 0 || table[cell] < 0) {
					continue; // assigned already, or not an allocated surface block
				}
				const glm::ivec3 p = cellPos(cell);
				const int nbr[6][3] = { {p.x-1,p.y,p.z}, {p.x+1,p.y,p.z}, {p.x,p.y-1,p.z},
					{p.x,p.y+1,p.z}, {p.x,p.y,p.z-1}, {p.x,p.y,p.z+1} };
				for (const int* n : nbr) {
					if (n[0] < 0 || n[1] < 0 || n[2] < 0 || n[0] >= bpa || n[1] >= bpa || n[2] >= bpa) {
						continue;
					}
					const int ni = n[0] + n[1] * bpa + n[2] * bpa * bpa;
					if (blockIsland[ni] >= 0) {
						blockIsland[cell] = blockIsland[ni];
						changed = true;
						break;
					}
				}
			}
		}
	};
	auto voxInside = [&](size_t off) {
#ifdef BAKEIT_SDF_TILE_R8
		return static_cast<int8_t>(voxels[off]) <= -77; // snorm: v/127 < -0.6 (surface bias)
#else
		const uint16_t bits = static_cast<uint16_t>(voxels[off])
			| (static_cast<uint16_t>(voxels[off + 1]) << 8);
		return (bits & 0x8000u) != 0u && (bits & 0x7FFFu) != 0u; // half < 0
#endif
	};
	auto sampleOff = [&](int rbIdx, int x, int y, int z) {
		return static_cast<size_t>(rbIdx) * tileStride
			+ static_cast<size_t>(x + y * 9 + z * 81) * kVoxBytes;
	};

	std::vector<std::vector<int>> islands;

	if (!anySuspects && provCount <= 1) {
		// Definitely one object (or definitely nothing), no cuts pending.
		if (provCount == 1) {
			islands.emplace_back();
			for (int cell = 0; cell < total; ++cell) {
				if (isMaterial(table[cell])) {
					islands[0].push_back(cell);
				}
			}
		}
	} else if (!anySuspects) {
		// Two+ groups separated purely by EMPTY blocks: the split is decided
		// at block level, still no readback (the fat-brush cut case).
		for (int cell = 0; cell < total; ++cell) {
			if (isMaterial(table[cell])) {
				blockIsland[cell] = provIsland[cell];
			}
		}
		adoptBandBlocks();
		islands.assign(static_cast<size_t>(provCount), {});
		for (int cell = 0; cell < total; ++cell) {
			if (blockIsland[cell] >= 0) {
				islands[static_cast<size_t>(blockIsland[cell])].push_back(cell);
			}
		}
	} else {
		// Suspects pending -> ALWAYS voxel-verify, even when the provisional
		// fill found a single group (or none). Suspect blocks never seed the
		// fill, so cuts placed close together swallow the piece BETWEEN them
		// whole - the fill only sees the big remainder, "one connected group",
		// and a real severed segment hides entirely inside the suspect set.
		// The verify promotes exactly those (suspect-only components >= 16
		// voxels become islands). Cost is bounded: the readback covers only
		// suspect tiles + their face neighbors, and the honest-block pruning
		// below shrinks the suspect set again right after.
		// ---- Stage 3: voxel verify. Read back ONLY the suspect tiles (plus
		// their surface neighbors, for exact face contact), flood-fill the
		// material inside each suspect block (8^3 voxels), and stitch: a
		// suspect component attaches to every provisional group whose
		// material it actually touches face-to-face. Bridging material
		// re-unites the groups (deep scratch, false alarm); a real cut
		// leaves them apart. This is the "does material still reach both
		// sides of the block" test, run on the stored field.
		verifyRan = true;
		sIdxOfCell.assign(static_cast<size_t>(total), -1);
		rbIdxOfCell.assign(static_cast<size_t>(total), -1);
		auto addRb = [&](int cell) {
			if (rbIdxOfCell[cell] < 0 && table[cell] >= 0) {
				rbIdxOfCell[cell] = static_cast<int32_t>(rbCells.size());
				rbCells.push_back(cell);
			}
		};
		sCells.assign(parent.cutSuspects.begin(), parent.cutSuspects.end());
		std::sort(sCells.begin(), sCells.end());
		for (size_t si = 0; si < sCells.size(); ++si) {
			const int cell = sCells[si];
			sIdxOfCell[cell] = static_cast<int32_t>(si);
			addRb(cell);
			const glm::ivec3 p = cellPos(cell);
			const int nbr[6][3] = { {p.x-1,p.y,p.z}, {p.x+1,p.y,p.z}, {p.x,p.y-1,p.z},
				{p.x,p.y+1,p.z}, {p.x,p.y,p.z-1}, {p.x,p.y,p.z+1} };
			for (const int* n : nbr) {
				if (n[0] < 0 || n[1] < 0 || n[2] < 0 || n[0] >= bpa || n[1] >= bpa || n[2] >= bpa) {
					continue;
				}
				addRb(n[0] + n[1] * bpa + n[2] * bpa * bpa);
			}
		}

		// One copy command, one wait: release-path semantics, like the weld.
		{
			const uint64_t bytes = static_cast<uint64_t>(rbCells.size()) * tileStride;
			std::vector<SBO> staging = base->vbuffer->createStorageBuffers({ bytes });
			std::vector<VkBufferImageCopy> regions(rbCells.size());
			for (size_t i = 0; i < rbCells.size(); ++i) {
				const int tile = table[rbCells[i]] & kBlockIndexMask;
				VkBufferImageCopy& r = regions[i];
				r = {};
				r.bufferOffset = i * tileStride;
				r.bufferRowLength = 9;
				r.bufferImageHeight = 9;
				r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				r.imageOffset = { (tile % tilesPerAxis) * 9,
					((tile / tilesPerAxis) % tilesPerAxis) * 9,
					(tile / (tilesPerAxis * tilesPerAxis)) * 9 };
				r.imageExtent = { 9, 9, 9 };
			}
			VkCommandBuffer cmd = base->vulkandevice->beginSingleTimeCommands();
			vkCmdCopyImageToBuffer(cmd, tilePoolTexture.image, VK_IMAGE_LAYOUT_GENERAL,
				staging[0].buffer, static_cast<uint32_t>(regions.size()), regions.data());
			base->vulkandevice->endSingleTimeCommands(cmd);
			voxels.resize(bytes);
			staging[0].read(base->device, bytes, voxels.data());
			vkDestroyBuffer(base->device, staging[0].buffer, nullptr);
			vkFreeMemory(base->device, staging[0].bufferMemory, nullptr);
		}

		// Local components per suspect block (flood fill over its 8^3 core).
		localLabel.assign(sCells.size() * 512, -1);
		compBase.assign(sCells.size(), 0);
		std::vector<int> compVoxels;
		{
			std::vector<int> vstack;
			for (size_t si = 0; si < sCells.size(); ++si) {
				compBase[si] = compCount;
				const int rb = rbIdxOfCell[sCells[si]];
				for (int v = 0; v < 512; ++v) {
					const int vx = v & 7, vy = (v >> 3) & 7, vz = v >> 6;
					if (localLabel[si * 512 + v] >= 0 || !voxInside(sampleOff(rb, vx, vy, vz))) {
						continue;
					}
					const int16_t lbl = static_cast<int16_t>(compCount - compBase[si]);
					int voxCount = 0;
					localLabel[si * 512 + v] = lbl;
					vstack.push_back(v);
					while (!vstack.empty()) {
						const int c = vstack.back();
						vstack.pop_back();
						++voxCount;
						const int cx = c & 7, cy = (c >> 3) & 7, cz = c >> 6;
						static const int D[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
						for (const int* d : D) {
							const int nx = cx + d[0], ny = cy + d[1], nz = cz + d[2];
							if (nx < 0 || ny < 0 || nz < 0 || nx > 7 || ny > 7 || nz > 7) {
								continue;
							}
							const int n = nx | (ny << 3) | (nz << 6);
							if (localLabel[si * 512 + n] < 0 && voxInside(sampleOff(rb, nx, ny, nz))) {
								localLabel[si * 512 + n] = lbl;
								vstack.push_back(n);
							}
						}
					}
					compVoxels.push_back(voxCount);
					++compCount;
				}
			}
		}

		if (compCount == 0) {
			// No material voxels found inside ANY suspect tile: either every
			// suspect is pure render-band padding, or the readback returned
			// garbage. Shattering the object on missing data is the worst
			// outcome - fall back to plain block-level connectivity (suspects
			// connect, the pre-verify rule) and start the suspect set fresh.
			std::printf("Split verify: no material voxels in %d suspect tiles - falling back to block-level islands\n",
				static_cast<int>(sCells.size()));
			parent.cutSuspects.clear();
			splitChunkIslands(chunkIndex, strokeWasCut); // recurses once: no suspects left
			return;
		}

		// Union-find over provisional groups [0,provCount) + components.
		std::vector<int> uf(static_cast<size_t>(provCount + compCount));
		for (size_t i = 0; i < uf.size(); ++i) {
			uf[i] = static_cast<int>(i);
		}
		auto ufFind = [&uf](int a) {
			while (uf[a] != a) { uf[a] = uf[uf[a]]; a = uf[a]; }
			return a;
		};
		auto ufUnite = [&](int a, int b) {
			a = ufFind(a); b = ufFind(b);
			if (a != b) uf[b] = a;
		};
		auto compAt = [&](int si, const glm::ivec3& v) { // -1 = air at that voxel
			const int16_t l = localLabel[static_cast<size_t>(si) * 512 + (v.x | (v.y << 3) | (v.z << 6))];
			return (l < 0) ? -1 : compBase[si] + l;
		};

		// Stitch across block faces: adjacent inside-voxels on BOTH sides.
		for (size_t si = 0; si < sCells.size(); ++si) {
			const int cell = sCells[si];
			const glm::ivec3 p = cellPos(cell);
			for (int axis = 0; axis < 3; ++axis)
			for (int dir = -1; dir <= 1; dir += 2) {
				glm::ivec3 q = p;
				q[axis] += dir;
				if (q[axis] < 0 || q[axis] >= bpa) continue;
				const int n = q.x + q.y * bpa + q.z * bpa * bpa;
				const int32_t rawN = table[n];
				if (!isMaterial(rawN)) continue;
				const int nSi = sIdxOfCell[n];
				if (nSi >= 0 && dir < 0) continue; // suspect pair handled once, from the +side
				const int u = (axis == 0) ? 1 : 0;
				const int w = (axis == 2) ? 1 : 2;
				for (int a = 0; a < 8; ++a)
				for (int b = 0; b < 8; ++b) {
					glm::ivec3 va(0), vb(0);
					va[axis] = (dir > 0) ? 7 : 0; va[u] = a; va[w] = b;
					vb[axis] = (dir > 0) ? 0 : 7; vb[u] = a; vb[w] = b;
					const int cComp = compAt(static_cast<int>(si), va);
					if (cComp < 0) continue;
					if (nSi >= 0) {
						const int nComp = compAt(nSi, vb);
						if (nComp >= 0) ufUnite(provCount + cComp, provCount + nComp);
					} else if (rawN == kBlockInterior) {
						ufUnite(provCount + cComp, provIsland[n]);
					} else if (voxInside(sampleOff(rbIdxOfCell[n], vb.x, vb.y, vb.z))) {
						ufUnite(provCount + cComp, provIsland[n]);
					}
				}
			}
		}

		// Final islands: every provisional group's root, plus roots made ONLY
		// of suspect components when they hold enough material to be an
		// object (>= 16 voxels). Smaller leftovers are CRUMBS - cutting
		// debris that gets erased outright (a clean knife leaves no dust).
		std::vector<int> islandOfRoot(static_cast<size_t>(provCount + compCount), -1);
		int islandCount = 0;
		for (int i = 0; i < provCount; ++i) {
			const int r = ufFind(i);
			if (islandOfRoot[r] < 0) islandOfRoot[r] = islandCount++;
		}
		std::vector<int> rootVoxels(static_cast<size_t>(provCount + compCount), 0);
		for (int c = 0; c < compCount; ++c) {
			rootVoxels[ufFind(provCount + c)] += compVoxels[c];
		}
		for (int c = 0; c < compCount; ++c) {
			const int r = ufFind(provCount + c);
			if (islandOfRoot[r] < 0 && rootVoxels[r] >= 16) {
				islandOfRoot[r] = islandCount++;
			}
		}
		compIsland.assign(static_cast<size_t>(compCount), -1);
		for (int c = 0; c < compCount; ++c) {
			compIsland[c] = islandOfRoot[ufFind(provCount + c)];
		}

		// Per-block assignment. Non-suspects follow their provisional group;
		// suspects go to the island owning MOST of their voxels (mixed blocks
		// remember every island present - the duplication step needs them).
		for (int cell = 0; cell < total; ++cell) {
			if (isMaterial(table[cell]) && sIdxOfCell[cell] < 0) {
				blockIsland[cell] = islandOfRoot[ufFind(provIsland[cell])];
			}
		}
		islandsPresentOf.assign(sCells.size(), {});
		blockHasCrumb.assign(sCells.size(), 0);
		std::vector<int> counts(static_cast<size_t>(glm::max(islandCount, 1)), 0);
		for (size_t si = 0; si < sCells.size(); ++si) {
			std::fill(counts.begin(), counts.end(), 0);
			const int nLocal = ((si + 1 < sCells.size()) ? compBase[si + 1] : compCount) - compBase[si];
			for (int l = 0; l < nLocal; ++l) {
				const int isl = compIsland[compBase[si] + l];
				if (isl >= 0) counts[isl] += compVoxels[compBase[si] + l];
				else blockHasCrumb[si] = 1;
			}
			int owner = -1, ownerVox = 0;
			for (int i = 0; i < islandCount; ++i) {
				if (counts[i] > 0) islandsPresentOf[si].push_back(i);
				if (counts[i] > ownerVox) { owner = i; ownerVox = counts[i]; }
			}
			blockIsland[sCells[si]] = owner; // -1 (band-only block) -> adopted below
		}
		// Band blocks (suspect or not) travel with whichever piece they pad.
		adoptBandBlocks();

		islands.assign(static_cast<size_t>(glm::max(islandCount, 0)), {});
		for (int cell = 0; cell < total; ++cell) {
			if (blockIsland[cell] >= 0) {
				islands[static_cast<size_t>(blockIsland[cell])].push_back(cell);
			}
		}
		{
			int insideTotal = 0, crumbComps = 0;
			for (int c2 = 0; c2 < compCount; ++c2) {
				insideTotal += compVoxels[c2];
				if (compIsland[c2] < 0) ++crumbComps;
			}
			std::printf("Split verify: %d suspects, %d tiles read, %d inside voxels, %d comp(s) (%d crumb), %d group(s) -> %d island(s)\n",
				static_cast<int>(sCells.size()), static_cast<int>(rbCells.size()),
				insideTotal, compCount, crumbComps, provCount, islandCount);
		}
	}

	if (islands.empty()) {
		// Erased to nothing: no object left. Spawned chunks give their slot
		// back (band-block leftovers freed with it); chunk 0 just resets.
		if (chunkIndex != 0) {
			retireChunkSlot(chunkIndex);
			return;
		}
		parent.contentMinOS = glm::vec3(0.0f);
		parent.contentMaxOS = glm::vec3(-1.0f);
		if (parent.physical) {
			parent.physical = false;
			parent.asleep = false;
			parent.velocityWS = glm::vec3(0.0f);
		}
		return;
	}

	auto islandBBox = [bpa](const std::vector<int>& cells, glm::ivec3& mn, glm::ivec3& mx) {
		mn = glm::ivec3(bpa);
		mx = glm::ivec3(-1);
		for (int cell : cells) {
			const glm::ivec3 p(cell % bpa, (cell / bpa) % bpa, cell / (bpa * bpa));
			mn = glm::min(mn, p);
			mx = glm::max(mx, p);
		}
	};
	auto contentBoundsFor = [](const SdfChunk& c, const glm::ivec3& mn, const glm::ivec3& mx,
		glm::vec3& outMin, glm::vec3& outMax) {
		const glm::vec3 origin = c.translationWS + ChunkLocalCenterOrigin(c.blockSizeWS);
		outMin = origin + glm::vec3(mn) * c.blockSizeWS - glm::vec3(0.5f * c.blockSizeWS);
		outMax = origin + glm::vec3(mx) * c.blockSizeWS + glm::vec3(0.5f * c.blockSizeWS);
	};

	// Which island keeps the parent's chunk (and with it, the parent's
	// state)? Cut strokes: the biggest island ("the branch is smaller than
	// the tree"). Draw strokes: the island overlapping the PRE-stroke content
	// bounds - the original object - so a freshly drawn blob, however big,
	// can never steal a physical object's state. Refresh the parent's content
	// bounds from the kept island, and wake a physical parent so it
	// re-settles on its (possibly reshaped) bottom.
	size_t keep = 0;
	for (size_t i = 1; i < islands.size(); ++i) {
		if (islands[i].size() > islands[keep].size()) {
			keep = i;
		}
	}
	if (!strokeWasCut && parent.contentMaxOS.x >= parent.contentMinOS.x) {
		// Pre-stroke content bounds -> block range (inverse of contentBoundsFor).
		const glm::vec3 pOrigin = parent.translationWS + ChunkLocalCenterOrigin(parent.blockSizeWS);
		const glm::ivec3 oldMin = glm::ivec3(glm::round(
			(parent.contentMinOS - pOrigin + glm::vec3(0.5f * parent.blockSizeWS)) / parent.blockSizeWS));
		const glm::ivec3 oldMax = glm::ivec3(glm::round(
			(parent.contentMaxOS - pOrigin - glm::vec3(0.5f * parent.blockSizeWS)) / parent.blockSizeWS));
		size_t best = islands.size();
		size_t bestCells = 0;
		for (size_t i = 0; i < islands.size(); ++i) {
			glm::ivec3 mn, mx;
			islandBBox(islands[i], mn, mx);
			const bool overlaps = glm::all(glm::lessThanEqual(mn, oldMax))
				&& glm::all(glm::greaterThanEqual(mx, oldMin));
			if (overlaps && islands[i].size() > bestCells) {
				best = i;
				bestCells = islands[i].size();
			}
		}
		if (best < islands.size()) {
			keep = best;
		}
	}
	glm::ivec3 kMin, kMax;
	islandBBox(islands[keep], kMin, kMax);
	contentBoundsFor(parent, kMin, kMax, parent.contentMinOS, parent.contentMaxOS);
	if (parent.physical) {
		parent.asleep = false;
	}

	// How many components does suspect block si hold for the given island?
	// (Crumb components count toward their block's assigned island.) Two or
	// more = the block still hides internally separated material and must
	// stay suspicious for future strokes.
	auto compCountFor = [&](size_t si, int island) {
		const int nLocal = ((si + 1 < sCells.size()) ? compBase[si + 1] : compCount) - compBase[si];
		int cnt = 0;
		for (int l = 0; l < nLocal; ++l) {
			if (compIsland[compBase[si] + l] == island) ++cnt; // crumbs (-1) never count
		}
		return cnt;
	};

	if (islands.size() == 1) {
		if (verifyRan) {
			// Deep scratch, not a cut. Keep only suspects that still hide
			// several separate material pieces; solid single-component
			// blocks are proven honest at block level again.
			std::unordered_set<int32_t> keepSet;
			for (size_t si = 0; si < sCells.size(); ++si) {
				if (compCountFor(si, blockIsland[sCells[si]]) >= 2) {
					keepSet.insert(sCells[si]);
				}
			}
			parent.cutSuspects = std::move(keepSet);
		}
		return; // one connected object - 99% of strokes end here
	}

	// Every other island becomes its own chunk: fresh 64^3 domain centered on
	// the island (block-aligned with the parent, so the voxel data is reused
	// untouched), blocks moved by table surgery - tiles never move, only
	// ownership changes. State inherited (rules doc §2).
	std::vector<glm::vec4> colors(static_cast<size_t>(total));
	parent.sbo[3].read(base->device, sizeof(glm::vec4) * colors.size(), colors.data());
	const glm::mat4 parentModel = (parent.objectIndex >= 0)
		? scene->objects[parent.objectIndex].ModelMatrix : glm::mat4(1.0f);

	// Which islands actually get a chunk (budget), so the mixed-block cleanup
	// only erases voxels that end up in a DIFFERENT chunk.
	std::vector<char> moveable(islands.size(), 0);
	{
		int budget = kMaxChunks - static_cast<int>(chunks.size());
		for (size_t i = 1; i < chunks.size(); ++i) { // retired slots are reusable (spawnChunk recycles)
			if (!chunks[i].active && chunks[i].meshIndex >= 0 && chunks[i].objectIndex >= 0 && chunks[i].brushComp) {
				++budget;
			}
		}
		for (size_t i = 0; i < islands.size(); ++i) {
			if (i == keep || islands[i].empty()) continue;
			if (budget > 0) {
				moveable[i] = 1;
				--budget;
			}
		}
	}
	auto islandHasChunk = [&](int i) {
		return i == static_cast<int>(keep)
			|| (i >= 0 && i < static_cast<int>(moveable.size()) && moveable[static_cast<size_t>(i)] != 0);
	};

	// ---- Mixed suspect blocks: a cut ran INSIDE them, so several islands
	// own voxels of the same block. The block's tile is DUPLICATED into each
	// island's chunk, and every copy keeps only its own island's voxels
	// (core AND the 1-voxel pads that mirror the neighbors) - both cut faces
	// stay intact, nothing is lost, and no sliver rides along with the wrong
	// piece.
	std::vector<std::vector<std::pair<int, int32_t>>> childExtraOf(islands.size());
	std::vector<std::pair<int, int32_t>> parentExtras;
	bool tableFlagsDirty = false;
	if (verifyRan) {
		std::vector<uint8_t> upBytes;
		std::vector<VkBufferImageCopy> upRegions;
		int dupsAllocated = 0;
		int allocCounters[2] = { 0, 0 };
		tileAllocSBO[0].read(base->device, sizeof(allocCounters), allocCounters);
		const int tileCapacity = tilesPerAxis * tilesPerAxis * tilesPerAxis;

		// Final island of one voxel sample (-2 = air / no material there).
		auto vIslandAt = [&](int cell, const glm::ivec3& lv) -> int {
			const int32_t raw = table[cell];
			if (!isMaterial(raw)) return -2;
			if (raw == kBlockInterior) return blockIsland[cell];
			const int si2 = sIdxOfCell[cell];
			if (si2 >= 0) {
				const int16_t l = localLabel[static_cast<size_t>(si2) * 512 + (lv.x | (lv.y << 3) | (lv.z << 6))];
				if (l < 0) return -2;
				const int isl = compIsland[compBase[si2] + l];
				return (isl >= 0) ? isl : -3; // crumbs: cutting debris, erased from every copy
			}
			const int rb2 = rbIdxOfCell[cell];
			if (rb2 >= 0) return voxInside(sampleOff(rb2, lv.x, lv.y, lv.z)) ? blockIsland[cell] : -2;
			return blockIsland[cell];
		};
		auto pushUpload = [&](int tile, const uint8_t* bytes) {
			const size_t off = upBytes.size();
			upBytes.insert(upBytes.end(), bytes, bytes + 729 * kVoxBytes);
			upBytes.resize(off + tileStride, 0);
			VkBufferImageCopy r{};
			r.bufferOffset = off;
			r.bufferRowLength = 9;
			r.bufferImageHeight = 9;
			r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			r.imageOffset = { (tile % tilesPerAxis) * 9,
				((tile / tilesPerAxis) % tilesPerAxis) * 9,
				(tile / (tilesPerAxis * tilesPerAxis)) * 9 };
			r.imageExtent = { 9, 9, 9 };
			upRegions.push_back(r);
		};

		for (size_t si = 0; si < sCells.size(); ++si) {
			const bool isMixed = islandsPresentOf[si].size() >= 2;
			if (!isMixed && !blockHasCrumb[si]) continue;
			const int cell = sCells[si];
			const glm::ivec3 p = cellPos(cell);
			const int owner = blockIsland[cell];
			const int32_t rawC = table[cell];
			const int origTile = rawC & kBlockIndexMask;
			const int32_t flags = rawC & kBlockEmissiveFlag;
			const uint8_t* src = &voxels[static_cast<size_t>(rbIdxOfCell[cell]) * tileStride];

			// Crumb-only blocks (no island material) still rewrite their own
			// tile so the debris disappears - X = -999 matches nothing, so
			// every crumb voxel goes to air.
			std::vector<int> targets = islandsPresentOf[si];
			if (targets.empty()) {
				targets.push_back((owner >= 0) ? owner : -999);
			}
			for (int X : targets) {
				if (X != -999 && !islandHasChunk(X)) continue;
				std::vector<uint8_t> copy(src, src + 729 * kVoxBytes);
				for (int sz2 = 0; sz2 < 9; ++sz2)
				for (int sy2 = 0; sy2 < 9; ++sy2)
				for (int sx2 = 0; sx2 < 9; ++sx2) {
					glm::ivec3 o = p;
					glm::ivec3 lv(sx2, sy2, sz2);
					if (sx2 == 8) { o.x += 1; lv.x = 0; }
					if (sy2 == 8) { o.y += 1; lv.y = 0; }
					if (sz2 == 8) { o.z += 1; lv.z = 0; }
					if (o.x >= bpa || o.y >= bpa || o.z >= bpa) continue;
					const int J = vIslandAt(o.x + o.y * bpa + o.z * bpa * bpa, lv);
					if (J == -3 || (J >= 0 && J != X && islandHasChunk(J))) {
						const size_t boff = static_cast<size_t>(sx2 + sy2 * 9 + sz2 * 81) * kVoxBytes;
#ifdef BAKEIT_SDF_TILE_R8
						copy[boff] = 0x7F;                        // +1.0 snorm: air
#else
						copy[boff] = 0x40; copy[boff + 1] = 0x56; // 100.0 half: FAR
#endif
					}
				}
				if (X == owner || X == -999) {
					pushUpload(origTile, copy.data()); // owner keeps the original tile, cleaned
					// The cleaned tile may have lost its last material voxel
					// (crumb-only blocks always do). Recompute the flag from
					// the cleaned CORE so the table never claims substance
					// the voxels no longer hold - a stale material flag here
					// bridges block connectivity across future cuts.
					bool coreInside = false;
					for (int vz2 = 0; vz2 < 8 && !coreInside; ++vz2)
					for (int vy2 = 0; vy2 < 8 && !coreInside; ++vy2)
					for (int vx2 = 0; vx2 < 8 && !coreInside; ++vx2) {
						const size_t boff2 = static_cast<size_t>(vx2 + vy2 * 9 + vz2 * 81) * kVoxBytes;
#ifdef BAKEIT_SDF_TILE_R8
						coreInside = static_cast<int8_t>(copy[boff2]) <= -77;
#else
						const uint16_t bits2 = static_cast<uint16_t>(copy[boff2])
							| (static_cast<uint16_t>(copy[boff2 + 1]) << 8);
						coreInside = (bits2 & 0x8000u) != 0u && (bits2 & 0x7FFFu) != 0u;
#endif
					}
					const int32_t newRaw = origTile | flags | (coreInside ? kBlockMaterialFlag : 0);
					if (newRaw != table[cell]) {
						table[cell] = newRaw;
						tableFlagsDirty = true;
					}
				} else {
					if (allocCounters[0] + dupsAllocated >= tileCapacity) {
						std::printf("Split: tile pool full, island %d loses its sliver of block %d\n", X, cell);
						continue;
					}
					const int dupTile = allocCounters[0] + dupsAllocated;
					++dupsAllocated;
					pushUpload(dupTile, copy.data());
					// The copy holds island X's material by construction.
					const int32_t entry = dupTile | flags | kBlockMaterialFlag;
					if (X == static_cast<int>(keep)) {
						parentExtras.push_back({ cell, entry });
					} else {
						childExtraOf[static_cast<size_t>(X)].push_back({ cell, entry });
					}
				}
			}
		}
		if (dupsAllocated > 0) {
			allocCounters[0] += dupsAllocated;
			tileAllocSBO[0].map(base->device, sizeof(allocCounters), allocCounters);
			volume.stats.tailBricks = allocCounters[0];
			volume.stats.usedBricks = glm::max(volume.stats.tailBricks - volume.stats.freeBricks, 0);
		}
		if (!upRegions.empty()) {
			std::vector<SBO> staging = base->vbuffer->createStorageBuffers({ static_cast<uint64_t>(upBytes.size()) });
			staging[0].map(base->device, upBytes.size(), upBytes.data());
			VkCommandBuffer cmd = base->vulkandevice->beginSingleTimeCommands();
			vkCmdCopyBufferToImage(cmd, staging[0].buffer, tilePoolTexture.image, VK_IMAGE_LAYOUT_GENERAL,
				static_cast<uint32_t>(upRegions.size()), upRegions.data());
			base->vulkandevice->endSingleTimeCommands(cmd);
			vkDestroyBuffer(base->device, staging[0].buffer, nullptr);
			vkFreeMemory(base->device, staging[0].bufferMemory, nullptr);
		}
	}

	// Pre-surgery table snapshot: the cut-face pad cleanup below must know
	// which cells HELD material before the move loop empties them.
	const std::vector<int32_t> preMove(table);
	int moved = 0;
	for (size_t i = 0; i < islands.size(); ++i) {
		if (i == keep || islands[i].empty()) {
			continue;
		}
		if (!moveable[i]) {
			std::printf("Split: chunk cap reached, island of %d blocks stays in chunk %d\n",
				static_cast<int>(islands[i].size()), chunkIndex);
			continue;
		}
		glm::ivec3 mn, mx;
		islandBBox(islands[i], mn, mx);
		for (const auto& e : childExtraOf[i]) { // duplicated cut blocks can sit 1 block outside
			const glm::ivec3 ep = cellPos(e.first);
			mn = glm::min(mn, ep);
			mx = glm::max(mx, ep);
		}
		// Domain offset (in parent blocks): island centered, clamped to fit.
		glm::ivec3 offset;
		for (int a = 0; a < 3; ++a) {
			offset[a] = glm::clamp((mn[a] + mx[a]) / 2 - bpa / 2, mx[a] - (bpa - 1), mn[a]);
		}

		SdfChunk& child = spawnChunk(
			parent.translationWS + glm::vec3(offset) * parent.blockSizeWS,
			/*debugStamp=*/false, /*rebuildScene=*/true);
		if (child.objectIndex >= 0) {
			// Inherit the parent's transform: nothing visibly moves at the
			// moment of the split.
			scene->objects[child.objectIndex].ModelMatrix = parentModel;
		}

		std::vector<int32_t> childTable(static_cast<size_t>(total), kBlockEmpty);
		std::vector<glm::vec4> childColors(static_cast<size_t>(total), glm::vec4(0.0f));
		auto childCellIndex = [&](int cell) {
			const glm::ivec3 np = cellPos(cell) - offset;
			return np.x + np.y * bpa + np.z * bpa * bpa;
		};
		for (int cell : islands[i]) {
			const int ni = childCellIndex(cell);
			childTable[ni] = table[cell];
			childColors[ni] = colors[cell];
			table[cell] = kBlockEmpty;
			if (verifyRan && sIdxOfCell[cell] >= 0
				&& compCountFor(static_cast<size_t>(sIdxOfCell[cell]), static_cast<int>(i)) >= 2) {
				child.cutSuspects.insert(ni); // still hides separated material
			}
		}
		for (const auto& e : childExtraOf[i]) {
			const int ni = childCellIndex(e.first);
			childTable[ni] = e.second;
			childColors[ni] = colors[e.first];
			if (sIdxOfCell[e.first] >= 0
				&& compCountFor(static_cast<size_t>(sIdxOfCell[e.first]), static_cast<int>(i)) >= 2) {
				child.cutSuspects.insert(ni);
			}
		}
		child.sbo[1].map(base->device, sizeof(int32_t) * childTable.size(), childTable.data());
		child.sbo[3].map(base->device, sizeof(glm::vec4) * childColors.size(), childColors.data());
		child.cpuTableDirty = true;

		contentBoundsFor(child, mn - offset, mx - offset, child.contentMinOS, child.contentMaxOS);
		// Cut off a physical object -> falls instantly. Islands created by
		// DRAWING are always born sketch, even inside a physical chunk's
		// domain - new material waits for the next "P". "Cut" is a property
		// of the ISLAND, not of the detecting stroke: suspects accumulate, so
		// a severed piece may only be recognized on a LATER release (even a
		// draw stroke's). Only the eraser creates suspects - an island that
		// touches the suspect set was cut free, whoever detected it.
		bool bornOfCut = strokeWasCut;
		if (!bornOfCut && verifyRan) {
			for (int cell : islands[i]) {
				if (sIdxOfCell[cell] >= 0) { bornOfCut = true; break; }
				const glm::ivec3 cp = cellPos(cell);
				const int nbr[6][3] = { {cp.x-1,cp.y,cp.z}, {cp.x+1,cp.y,cp.z}, {cp.x,cp.y-1,cp.z},
					{cp.x,cp.y+1,cp.z}, {cp.x,cp.y,cp.z-1}, {cp.x,cp.y,cp.z+1} };
				for (const int* n : nbr) {
					if (n[0] < 0 || n[1] < 0 || n[2] < 0 || n[0] >= bpa || n[1] >= bpa || n[2] >= bpa) {
						continue;
					}
					if (sIdxOfCell[n[0] + n[1] * bpa + n[2] * bpa * bpa] >= 0) { bornOfCut = true; break; }
				}
				if (bornOfCut) break;
			}
		}
		child.physical = parent.physical && bornOfCut;
		child.asleep = false;
		child.velocityWS = glm::vec3(0.0f);
		child.lastDrawListBlockToClip = glm::mat4(0.0f);
		++moved;
		std::printf("Split: island of %d blocks -> chunk %d (%s)\n",
			static_cast<int>(islands[i].size()),
			static_cast<int>(&child - chunks.data()), // recycled slots keep their old index
			child.physical ? "physical" : "sketch");
	}
	if (moved == 0 && parentExtras.empty() && !tableFlagsDirty) {
		return;
	}

	// The kept island's cleaned copies of duplicated cut blocks.
	for (const auto& e : parentExtras) {
		table[e.first] = e.second;
	}

	// ---- Cut-face pad cleanup. A tile's +axis pad layer (local index 8)
	// mirrors the NEIGHBOR block's first voxel layer; table surgery moves
	// ownership without touching voxels, so a surviving block facing a
	// departed cell keeps a pad full of the OTHER piece's material - it
	// renders as a thin disc floating at the block boundary near the cut.
	// (Band blocks are the worst carriers: no material of their own, so the
	// mixed-block dup cleanup never rewrites them.) Overwrite those pad
	// layers with air on BOTH sides of the split - the parent's phantom of
	// the child and the child's phantom of the parent - via sub-tile copies
	// from one shared 81-texel air buffer, one submit.
	{
		std::unordered_map<int, int> padAxesOf; // tile -> bitmask of +axes to air out
		auto neighborIndex = [&](int cell, int axis) -> int {
			glm::ivec3 q = cellPos(cell);
			q[axis] += 1;
			return (q[axis] >= bpa) ? -1 : (q.x + q.y * bpa + q.z * bpa * bpa);
		};

		// Parent side: pads facing cells whose material left the parent.
		for (int cell = 0; cell < total; ++cell) {
			if (table[cell] < 0) continue;
			for (int a = 0; a < 3; ++a) {
				const int ni = neighborIndex(cell, a);
				if (ni >= 0 && isMaterial(preMove[ni]) && table[ni] == kBlockEmpty) {
					padAxesOf[table[cell] & kBlockIndexMask] |= 1 << a;
				}
			}
		}
		// Child side: pads facing parent-space cells that did not come along.
		for (size_t i = 0; i < islands.size(); ++i) {
			if (i == keep || !moveable[i]) continue;
			std::unordered_set<int> mine(islands[i].begin(), islands[i].end());
			for (const auto& e : childExtraOf[i]) {
				mine.insert(e.first);
			}
			for (int cell : islands[i]) {
				if (preMove[cell] < 0) continue; // interior/sentinel: no tile to patch
				for (int a = 0; a < 3; ++a) {
					const int ni = neighborIndex(cell, a);
					if (ni >= 0 && isMaterial(preMove[ni]) && mine.count(ni) == 0) {
						padAxesOf[preMove[cell] & kBlockIndexMask] |= 1 << a;
					}
				}
			}
		}

		// Exact non-overlapping regions per tile (overlapping copy regions in
		// one vkCmdCopyBufferToImage are invalid): the x=8 plane full, the
		// y=8 plane minus the x=8 line when both are written, the z=8 plane
		// minus both covered lines.
		std::vector<VkBufferImageCopy> padRegions;
		for (const auto& kv : padAxesOf) {
			const int tile = kv.first;
			const int mask = kv.second;
			const glm::ivec3 to((tile % tilesPerAxis) * 9,
				((tile / tilesPerAxis) % tilesPerAxis) * 9,
				(tile / (tilesPerAxis * tilesPerAxis)) * 9);
			for (int a = 0; a < 3; ++a) {
				if ((mask & (1 << a)) == 0) continue;
				VkBufferImageCopy r{};
				r.bufferOffset = 0; // every region reads the same air texels
				r.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				glm::ivec3 off(0);
				glm::ivec3 ext(9);
				off[a] = 8;
				ext[a] = 1;
				if (a > 0 && (mask & 1) != 0) ext.x = 8;                   // x=8 line already covered
				if (a > 1 && (mask & 2) != 0) ext.y = 8;                   // y=8 line already covered
				r.imageOffset = { to.x + off.x, to.y + off.y, to.z + off.z };
				r.imageExtent = { static_cast<uint32_t>(ext.x), static_cast<uint32_t>(ext.y), static_cast<uint32_t>(ext.z) };
				r.bufferRowLength = static_cast<uint32_t>(ext.x);
				r.bufferImageHeight = static_cast<uint32_t>(ext.y);
				padRegions.push_back(r);
			}
		}
		if (!padRegions.empty()) {
			std::vector<uint8_t> air(81 * kVoxBytes);
#ifdef BAKEIT_SDF_TILE_R8
			std::fill(air.begin(), air.end(), 0x7F);              // +1.0 snorm: air
#else
			for (size_t b2 = 0; b2 < air.size(); b2 += 2) {
				air[b2] = 0x40; air[b2 + 1] = 0x56;               // 100.0 half: FAR
			}
#endif
			std::vector<SBO> staging = base->vbuffer->createStorageBuffers({ static_cast<uint64_t>(air.size()) });
			staging[0].map(base->device, air.size(), air.data());
			VkCommandBuffer cmd = base->vulkandevice->beginSingleTimeCommands();
			vkCmdCopyBufferToImage(cmd, staging[0].buffer, tilePoolTexture.image, VK_IMAGE_LAYOUT_GENERAL,
				static_cast<uint32_t>(padRegions.size()), padRegions.data());
			base->vulkandevice->endSingleTimeCommands(cmd);
			vkDestroyBuffer(base->device, staging[0].buffer, nullptr);
			vkFreeMemory(base->device, staging[0].bufferMemory, nullptr);
		}
	}

	if (verifyRan) {
		// Suspects that stay in the parent remain suspicious only while they
		// still hide several separate pieces of the KEPT island's material.
		std::unordered_set<int32_t> keepSet;
		for (size_t si = 0; si < sCells.size(); ++si) {
			const int cell = sCells[si];
			if (table[cell] >= 0 && compCountFor(si, static_cast<int>(keep)) >= 2) {
				keepSet.insert(cell);
			}
		}
		parent.cutSuspects = std::move(keepSet);
	}

	parent.sbo[1].map(base->device, sizeof(int32_t) * table.size(), table.data());
	parent.cpuTableDirty = true;
	parent.lastDrawListBlockToClip = glm::mat4(0.0f);
	uploadChunkSpatialData(); // the children's spatial SBOs, before their first draw-list build

	// Chunk 0 owns the coarse LOD pyramid: blank every coarse block whose
	// fine region is now completely empty, so the moved island doesn't ghost
	// in the distance bands. Coarse blocks PARTIALLY covered by the island
	// sit at the cut, where the erase stroke's coarse stamp already cleaned
	// up. (Coarse tiles are leaked for now - the coarse allocator is
	// bump-only; acceptable until idle-path compaction exists.)
	if (chunkIndex == 0 && !coarseSharedSBO.empty() && coarseTableTotal > 0) {
		std::vector<int32_t> coarseTable(static_cast<size_t>(coarseTableTotal));
		coarseSharedSBO[1].read(base->device, sizeof(int32_t) * coarseTable.size(), coarseTable.data());
		bool changed = false;
		for (CoarseLevel& lvl : coarseLevels) {
			const int cbpa = lvl.blocksPerAxis;
			for (int cz = 0; cz < cbpa; ++cz)
			for (int cy = 0; cy < cbpa; ++cy)
			for (int cx = 0; cx < cbpa; ++cx) {
				const int ci = lvl.tableBase + cx + cy * cbpa + cz * cbpa * cbpa;
				if (coarseTable[ci] == kBlockEmpty) {
					continue;
				}
				bool anyFine = false;
				for (int fz = cz * lvl.N; fz < (cz + 1) * lvl.N && !anyFine; ++fz)
				for (int fy = cy * lvl.N; fy < (cy + 1) * lvl.N && !anyFine; ++fy)
				for (int fx = cx * lvl.N; fx < (cx + 1) * lvl.N && !anyFine; ++fx) {
					anyFine = isMaterial(table[fx + fy * bpa + fz * bpa * bpa]);
				}
				if (!anyFine) {
					coarseTable[ci] = kBlockEmpty;
					changed = true;
				}
			}
		}
		if (changed) {
			coarseSharedSBO[1].map(base->device, sizeof(int32_t) * coarseTable.size(), coarseTable.data());
		}
	}
}

void SdfEditor::updateChunkPhysics(float time)
{
	const float rawDt = (lastPhysicsTime >= 0.0f) ? (time - lastPhysicsTime) : 0.0f;
	lastPhysicsTime = time;
	// Clamp: a hitch (shader recompile, weld, breakpoint) must not turn into
	// one giant tunneling step.
	const float dt = glm::clamp(rawDt, 0.0f, 1.0f / 30.0f);
	if (dt <= 0.0f) {
		return;
	}

	for (SdfChunk& c : chunks) {
		if (!c.active || !c.physical || c.asleep || c.objectIndex < 0) {
			continue;
		}
		glm::mat4& m = scene->objects[c.objectIndex].ModelMatrix;

		c.velocityWS.y -= physicsGravity * dt;
		m[3] += glm::vec4(c.velocityWS * dt, 0.0f);

		// Ground contact: lowest transformed corner of the content AABB vs the
		// plane (8 corners - correct under any pottery rotation the object may
		// carry, and exact while the matrix is a pure translation).
		float lowestY = 1e30f;
		for (int corner = 0; corner < 8; ++corner) {
			const glm::vec3 p(
				(corner & 1) ? c.contentMaxOS.x : c.contentMinOS.x,
				(corner & 2) ? c.contentMaxOS.y : c.contentMinOS.y,
				(corner & 4) ? c.contentMaxOS.z : c.contentMinOS.z);
			lowestY = glm::min(lowestY, (m * glm::vec4(p, 1.0f)).y);
		}
		if (lowestY < physicsGroundY) {
			m[3].y += physicsGroundY - lowestY;
			if (c.velocityWS.y < 0.0f) {
				c.velocityWS.y = -c.velocityWS.y * physicsRestitution;
			}
			// Grounded: kill residual bounce + slide, then sleep. A future
			// drop (or the split feature re-dropping a piece) wakes it.
			if (c.velocityWS.y < 0.05f) {
				c.velocityWS.y = 0.0f;
			}
			c.velocityWS.x *= glm::max(1.0f - 8.0f * dt, 0.0f);
			c.velocityWS.z *= glm::max(1.0f - 8.0f * dt, 0.0f);
			if (glm::dot(c.velocityWS, c.velocityWS) < 1e-6f) {
				c.velocityWS = glm::vec3(0.0f);
				c.asleep = true;
			}
		}
		// The draw list re-culls/sorts on its own: its rebuild cache compares
		// the block-to-clip matrix, which contains the model transform.
	}
}

bool SdfEditor::chunkContainsPoint(const SdfChunk& c, const glm::vec3& pWS, float margin)
{
	const glm::mat4 cModel = (c.objectIndex >= 0)
		? scene->objects[c.objectIndex].ModelMatrix
		: glm::mat4(1.0f);
	const glm::vec3 p = glm::vec3(glm::inverse(cModel) * glm::vec4(pWS, 1.0f));
	glm::vec3 bmin, bmax;
	ComputeChunkBounds(c.blocksPerAxis, c.blockSizeWS, c.translationWS, bmin, bmax);
	return glm::all(glm::greaterThanEqual(p, bmin + margin))
		&& glm::all(glm::lessThanEqual(p, bmax - margin));
}

float SdfEditor::raycastChunkMaterial(SdfChunk& c, const glm::vec3& rayOriginWS, const glm::vec3& rayDirWS)
{
	if (!c.active || c.contentMaxOS.x < c.contentMinOS.x) {
		return -1.0f;
	}
	const int bpa = static_cast<int>(c.blocksPerAxis);
	const size_t totalCells = static_cast<size_t>(bpa) * bpa * bpa;

	const glm::mat4 cModel = (c.objectIndex >= 0)
		? scene->objects[c.objectIndex].ModelMatrix
		: glm::mat4(1.0f);
	const glm::mat4 inv = glm::inverse(cModel);
	const glm::vec3 ro = glm::vec3(inv * glm::vec4(rayOriginWS, 1.0f));
	const glm::vec3 rd = glm::normalize(glm::mat3(inv) * rayDirWS);

	// Cheap rejects before touching the table: padded content box + domain.
	float tEnter = 0.0f, tExit = 0.0f;
	const glm::vec3 pad(2.0f * c.blockSizeWS);
	if (!IntersectAabb(ro, rd, c.contentMinOS - pad, c.contentMaxOS + pad, tEnter, tExit) || tExit < 0.0f) {
		return -1.0f;
	}
	glm::vec3 bmin, bmax;
	ComputeChunkBounds(c.blocksPerAxis, c.blockSizeWS, c.translationWS, bmin, bmax);
	float tDomEnter = 0.0f, tDomExit = 0.0f;
	if (!IntersectAabb(ro, rd, bmin, bmax, tDomEnter, tDomExit) || tDomExit < 0.0f) {
		return -1.0f;
	}

	// Lazy CPU-mirror refresh: only when a table writer flagged it AND the
	// chunk's stamps are idle (a mid-stroke query keeps the stale mirror -
	// one stamp of staleness is fine for aiming).
	if (c.cpuTableDirty || c.cpuTable.size() != totalCells) {
		const bool stampIdle = !c.brushComp
			|| vkGetFenceStatus(base->device, c.brushComp->computeFence) == VK_SUCCESS;
		if (stampIdle) {
			c.cpuTable.resize(totalCells);
			c.sbo[1].read(base->device, sizeof(int32_t) * totalCells, c.cpuTable.data());
			c.cpuTableDirty = false;
		} else if (c.cpuTable.size() != totalCells) {
			return -1.0f; // no mirror yet and the table is being written
		}
	}

	auto isMaterialRaw = [](int32_t raw) {
		return raw == kBlockInterior || (raw >= 0 && (raw & kBlockMaterialFlag) != 0);
	};

	// 3D-DDA through the block grid over the clipped t-range.
	const glm::vec3 gridOrigin =
		c.translationWS + ChunkLocalCenterOrigin(c.blockSizeWS) - glm::vec3(0.5f * c.blockSizeWS);
	float tCur = glm::max(glm::max(tEnter, tDomEnter), 0.0f) + 1e-5f;
	tExit = glm::min(tExit, tDomExit);
	const glm::vec3 g0 = (ro + rd * tCur - gridOrigin) / c.blockSizeWS;
	glm::ivec3 cell = glm::clamp(glm::ivec3(glm::floor(g0)), glm::ivec3(0), glm::ivec3(bpa - 1));
	glm::ivec3 stepDir;
	glm::vec3 tMax, tDelta;
	for (int a = 0; a < 3; ++a) {
		if (rd[a] > 1e-8f) {
			stepDir[a] = 1;
			tDelta[a] = c.blockSizeWS / rd[a];
			tMax[a] = tCur + ((float(cell[a] + 1) - g0[a]) * c.blockSizeWS) / rd[a];
		} else if (rd[a] < -1e-8f) {
			stepDir[a] = -1;
			tDelta[a] = -c.blockSizeWS / rd[a];
			tMax[a] = tCur + ((g0[a] - float(cell[a])) * c.blockSizeWS) / -rd[a];
		} else {
			stepDir[a] = 0;
			tDelta[a] = 1e30f;
			tMax[a] = 1e30f;
		}
	}
	for (int it = 0; it < 3 * bpa + 3; ++it) {
		if (isMaterialRaw(c.cpuTable[cell.x + cell.y * bpa + cell.z * bpa * bpa])) {
			return tCur;
		}
		const int a = (tMax.x < tMax.y)
			? ((tMax.x < tMax.z) ? 0 : 2)
			: ((tMax.y < tMax.z) ? 1 : 2);
		tCur = tMax[a];
		if (tCur > tExit) break;
		cell[a] += stepDir[a];
		if (cell[a] < 0 || cell[a] >= bpa) break;
		tMax[a] += tDelta[a];
	}
	return -1.0f;
}

int SdfEditor::pickChunkIndexByRay(const glm::vec3& rayOriginWS, const glm::vec3& rayDirWS)
{
	// Three-tier pick, each chunk tested in its own object space (models are
	// rigid + uniform scale, so object-space t values are comparable enough):
	//
	//   Tier 0 - MATERIAL: DDA-march the ray through the chunk's block table
	//   (CPU read; stroke start only) and take the chunk whose material the
	//   ray strikes FIRST. Content-AABB entry distance alone mis-targets once
	//   pieces nest - an L-shaped keep's bbox CONTAINS the segments cut off
	//   its concave side, and with the camera inside the big padded bbox its
	//   clamped t = 0 won every pick, leaving split children uneditable.
	//
	//   Tier 1 - CONTENT AABBs (inflated by the brush footprint), when the ray
	//   hits no material anywhere: strokes into the empty space right around a
	//   piece still favor that piece over a bare overlapping domain.
	//
	//   Tier 2 - full domain boxes: aiming at empty canvas space inside a
	//   domain still targets that chunk (that is how material gets drawn in
	//   the first place).
	//
	// Miss everything -> -1 (spawn cue).
	int bestMaterial = -1, bestContent = -1, bestDomain = -1;
	float bestMaterialT = 1e30f, bestContentT = 1e30f, bestDomainT = 1e30f;
	for (size_t i = 0; i < chunks.size(); ++i) {
		SdfChunk& c = chunks[i];
		if (!c.active) continue; // absorbed by a weld: empty bounds, never a target

		const float tMat = raycastChunkMaterial(c, rayOriginWS, rayDirWS);
		if (tMat >= 0.0f && tMat < bestMaterialT) {
			bestMaterialT = tMat;
			bestMaterial = static_cast<int>(i);
		}

		const glm::mat4 cModel = (c.objectIndex >= 0)
			? scene->objects[c.objectIndex].ModelMatrix
			: glm::mat4(1.0f);
		const glm::mat4 inv = glm::inverse(cModel);
		const glm::vec3 ro = glm::vec3(inv * glm::vec4(rayOriginWS, 1.0f));
		const glm::vec3 rd = glm::normalize(glm::mat3(inv) * rayDirWS);
		float tEnter = 0.0f, tExit = 0.0f;

		if (c.contentMaxOS.x >= c.contentMinOS.x) {
			const glm::vec3 pad(BrushRadius + 2.0f * c.blockSizeWS);
			if (IntersectAabb(ro, rd, c.contentMinOS - pad, c.contentMaxOS + pad, tEnter, tExit)
				&& tExit >= 0.0f) {
				const float t = glm::max(tEnter, 0.0f);
				if (t < bestContentT) {
					bestContentT = t;
					bestContent = static_cast<int>(i);
				}
			}
		}

		glm::vec3 bmin, bmax;
		ComputeChunkBounds(c.blocksPerAxis, c.blockSizeWS, c.translationWS, bmin, bmax);
		if (IntersectAabb(ro, rd, bmin, bmax, tEnter, tExit) && tExit >= 0.0f) {
			const float t = glm::max(tEnter, 0.0f);
			if (t < bestDomainT) {
				bestDomainT = t;
				bestDomain = static_cast<int>(i);
			}
		}
	}
	if (bestMaterial >= 0) return bestMaterial;
	return (bestContent >= 0) ? bestContent : bestDomain;
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
	const glm::ivec3 lo = glm::clamp(fineTexDirtyMin, glm::ivec3(0), glm::ivec3(int(chunks[0].blocksPerAxis) - 1));
	const glm::ivec3 hi = glm::clamp(fineTexDirtyMax, glm::ivec3(0), glm::ivec3(int(chunks[0].blocksPerAxis) - 1));
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
	int zero = 0;
	int zeroCounters[2] = { 0, 0 };
	for (SdfChunk& chunk : chunks) {
		const uint32_t totalBlocks = chunk.blocksPerAxis * chunk.blocksPerAxis * chunk.blocksPerAxis;
		std::vector<int32_t> clearedBlocks(totalBlocks, kBlockEmpty);
		std::vector<glm::vec4> clearedColors(totalBlocks, glm::vec4(0.0f));
		chunk.sbo[1].map(base->device, sizeof(int32_t) * clearedBlocks.size(), clearedBlocks.data());
		chunk.sbo[3].map(base->device, sizeof(glm::vec4) * clearedColors.size(), clearedColors.data());
		chunk.sbo[4].map(base->device, sizeof(int), &zero);
		chunk.cutSuspects.clear();
		chunk.cpuTableDirty = true;
		// Physics + transform reset; spawned chunks also give their SLOT back
		// (active=false -> spawnChunk recycles it; without this a session of
		// cuts permanently ate the kMaxChunks budget - "chunk cap reached").
		chunk.physical = false;
		chunk.asleep = false;
		chunk.velocityWS = glm::vec3(0.0f);
		chunk.contentMinOS = glm::vec3(0.0f);
		chunk.contentMaxOS = glm::vec3(-1.0f);
		chunk.lastDrawListBlockToClip = glm::mat4(0.0f);
		if (chunk.objectIndex >= 0) {
			scene->objects[chunk.objectIndex].ModelMatrix = glm::mat4(1.0f);
		}
		if (&chunk != &chunks[0]) {
			if (chunk.meshIndex >= 0) {
				base->vbuffer->updateIndirectInstanceList(scene->meshes[chunk.meshIndex], {});
			}
			chunk.active = false;
		}
	}
	editableChunkIndex = 0;
	// Shared allocator: reset once (every chunk's tiles come from the one pool).
	tileAllocSBO[0].map(base->device, sizeof(zeroCounters), zeroCounters);
	tileAllocSBO[1].map(base->device, sizeof(int), &zero);
	freeStackStableCount = 0;
	if (Mesh* fineMesh = findEditableMesh()) {
		base->vbuffer->setIndirectInstanceCount(*fineMesh, 0);
	}
	volume.stats.dirtyBlockCount = 0;

	// Coarse path: empty the shared coarse table + reset the shared bump allocator so
	// a cleared scene shows no stale coarse geometry (all levels, both platforms).
	if (!coarseSharedSBO.empty()) {
		std::vector<int32_t> coarseEmpty(static_cast<size_t>(coarseTableTotal), kBlockEmpty);
		coarseSharedSBO[1].map(base->device, sizeof(int32_t) * coarseEmpty.size(), coarseEmpty.data());
		int zc[2] = { 0, 0 };
		coarseSharedSBO[2].map(base->device, sizeof(zc), zc);  // tail + cursor
		int z = 0;
		coarseSharedSBO[5].map(base->device, sizeof(int), &z); // free count
	}
#ifndef BAKEIT_PLATFORM_ANDROID
	clearCoarseAtlas();
	clearFineAtlas(); // also marks fineTexDirty so the next idle frame re-converts
#endif
}

void SdfEditor::update(float time)
{
	uploadChunkSpatialData();
	// Physics before everything: dropped chunks fall this frame's dt, and the
	// draw-list build below then sees their moved matrices. Runs regardless of
	// brush input (a chunk keeps falling while nobody sculpts).
	updateChunkPhysics(time);

	// Coarse-LOD pyramid lives in ONE shared pool (binding 3) + concatenated table
	// (binding 9), so the rendered level is selected per-instance in the VS/FS by the
	// distance band - no descriptor re-point needed at all.

	if (!brushInput) {
		return;
	}
	BrushInput bin = brushInput->poll();
	const glm::dvec2 currentCursorPos = bin.strokeCursor;

	if (bin.shapeTogglePressed) {
		BrushShapeMode = (BrushShapeMode + 1) % kBrushShapeCount;
		std::printf("Brush Shape: %s\n", BrushShapeName(BrushShapeMode));
	}
	if (bin.emissiveTogglePressed) {
		EmissiveMode = !EmissiveMode;
		std::printf("Emissive Mode: %s\n", EmissiveMode ? "ON" : "OFF");
	}
	if (bin.physicsCommitPressed) {
		commitPhysics();
	}
	// R-rotate operator (Blender): R enters the modal; X/Y/Z picks the world
	// axis; MOVING THE MOUSE rotates interactively (cursor's angular motion
	// around the brush's screen position); typing digits switches to exact
	// numeric entry. LMB / Enter applies (accumulates into the brush
	// rotation), RMB / Esc / R cancels. Live preview via the indicator.
	if (bin.rotateTogglePressed && bin.interactionAllowed) {
		rotateModalActive = !rotateModalActive;
		rotateModalAxis = 1;
		rotateModalNumber.clear();
		rotateModalMouseDeg = 0.0f;
		std::printf("Brush rotate: %s\n",
			rotateModalActive ? "R  (X/Y/Z: axis, arrows or digits: angle, LMB/Enter: apply, RMB/Esc: cancel)" : "cancelled");
	}
	if (rotateModalActive) {
		for (char tc : bin.typedChars) {
			if (tc == 'x')      rotateModalAxis = 0;
			else if (tc == 'y') rotateModalAxis = 1;
			else if (tc == 'z') rotateModalAxis = 2;
			else                rotateModalNumber.push_back(tc);
		}

		// Interactive angle: left/right arrows step the rotation (tap = one
		// step, hold = OS key-repeat; Alt = fine). Typed digits override.
		if (bin.rotateSteps != 0) {
			const float step = bin.fineModifier ? 1.0f : 5.0f;
			rotateModalMouseDeg += step * static_cast<float>(bin.rotateSteps);
		}

		const float modalDeg = rotateModalNumber.empty()
			? rotateModalMouseDeg
			: static_cast<float>(std::atof(rotateModalNumber.c_str()));
		static const char* kAxisName[3] = { "X", "Y", "Z" };
		const bool confirm = bin.confirmPressed || bin.drawDown;
		const bool cancel = bin.cancelPressed || bin.eraseDown;
		if (cancel) {
			rotateModalActive = false;
			suppressStrokeUntilRelease = bin.eraseDown;
			std::printf("Brush rotate: cancelled\n");
		} else if (confirm) {
			glm::vec3 axis(0.0f);
			axis[rotateModalAxis] = 1.0f;
			brushShapeRot = glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(modalDeg), axis)) * brushShapeRot;
			rotateModalActive = false;
			suppressStrokeUntilRelease = bin.drawDown;
			std::printf("Brush rotate: %+.1f deg around %s\n", modalDeg, kAxisName[rotateModalAxis]);
		}
		// The modal owns the mouse buttons and keyboard: swallow strokes
		// (everything else - hover, stick, snap - keeps working).
		bin.drawDown = false;
		bin.eraseDown = false;
	}
	// The confirming/cancelling click must not start a stroke when the
	// button is still held on the next frames.
	if (suppressStrokeUntilRelease) {
		if (!bin.drawDown && !bin.eraseDown) {
			suppressStrokeUntilRelease = false;
		} else {
			bin.drawDown = false;
			bin.eraseDown = false;
		}
	}

	if (bin.interactionAllowed) {
		if (bin.eraseDown) leftBut = 0.0;
		if (bin.drawDown) leftBut = 1.0;
		BrushRadius += 0.0001f * bin.radiusDelta;
		BrushSmoothness += 0.0001f * bin.smoothnessDelta;
		// W/S: with the brush stuck to a surface they nudge the depth BIAS
		// (push into / pull off the material); aiming at air they drive the
		// free camera-distance depth as before.
		if (surfaceStickEnabled && surfaceStickT >= 0.0f) {
			surfaceBias = glm::clamp(surfaceBias + 0.002f * bin.distanceDelta, -0.25f, 0.5f);
		} else {
			BrushDistanceFromCamera += 0.005f * bin.distanceDelta;
		}

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

	// Stroke targeting: on the stroke's first frame, ray-test every chunk's
	// bounds and edit the nearest hit for the WHOLE stroke (a stroke never
	// migrates mid-drag). A DRAW stroke into empty air spawns a fresh chunk
	// at the brush pose (erase into air stays a no-op on the old target).
	// wasBrushPointerDown still holds last frame's state here - the gesture
	// block below updates it.
	{
		const bool pointerDownNow = bin.interactionAllowed && (bin.drawDown || bin.eraseDown);
		if (pointerDownNow && !wasBrushPointerDown) {
			strokeAbsoluteBrushDepth = false;
			int picked = pickChunkIndexByRay(bin.rayOrigin, bin.rayDir);
			if (picked < 0 && bin.drawDown && airDrawSpawnEnabled) {
				// Draw into empty air: target the world-grid cell under the
				// brush (lazy chunk-grid canvas). GRID-ALIGNED, never brush-
				// centered - cells tile space exactly, so nothing gets cut at
				// bounds; each stamp also dispatches into every neighboring
				// cell the footprint overlaps (see the stamp block).
				const float airDepth = (surfaceStickEnabled && surfaceStickT >= 0.0f)
					? surfaceStickT + surfaceBias
					: BrushDistanceFromCamera;
				const glm::vec3 p = bin.rayOrigin + bin.rayDir * airDepth;
				picked = ensureCellChunk(glm::ivec3(glm::floor(p + 0.5f)));
			}
			if (picked >= 0 && picked != editableChunkIndex) {
				editableChunkIndex = picked;
				std::printf("Brush target: chunk %d\n", picked);
			}
		}
		// (The old spawn-stroke chunk chaining is gone: the stamp block now
		// dispatches into every grid cell the footprint overlaps, so long air
		// strokes flow across cells without retargeting.)
	}
	SdfChunk& chunk = chunks[editableChunkIndex];

	// Pottery rotation: the EDITED chunk's object ModelMatrix (rigid). All
	// brush math runs in OBJECT space - transform the platform's world-space
	// pointing ray with the inverse model, so a held brush carves a ring on
	// a spinning pot. Identity model (desktop today) reduces to the old path.
	Object* editableObject = (chunk.objectIndex >= 0)
		? &scene->objects[chunk.objectIndex]
		: findEditableObject();
	const glm::mat4 model = editableObject ? editableObject->ModelMatrix : glm::mat4(1.0f);
	const glm::mat4 invModel = glm::inverse(model);

	// Brush placement: the platform's pointing ray, pushed into the chunk
	// bounds (desktop: cursor ray; quest: controller aim pose).
	const glm::vec3 rayDir = glm::normalize(glm::mat3(invModel) * bin.rayDir);
	const glm::vec3 rayOrigin = glm::vec3(invModel * glm::vec4(bin.rayOrigin, 1.0f));

	// Blender-style surface stick (default on, GUI-toggleable): while
	// hovering, the depth is the nearest MATERIAL hit under the cursor across
	// all chunks - the brush rides on what you see, no W/S driving needed.
	// The stuck depth FREEZES for the length of a stroke (classic sculpt:
	// carving toward the camera must not drag the brush along); W/S nudges a
	// bias around it. Aiming at air falls back to the camera-distance depth.
	{
		const bool pointerHeld = bin.interactionAllowed && (bin.drawDown || bin.eraseDown);
		if (surfaceStickEnabled && (!pointerHeld || !wasBrushPointerDown)) {
			surfaceStickT = -1.0f;
			for (SdfChunk& sc : chunks) {
				const float t = raycastChunkMaterial(sc, bin.rayOrigin, bin.rayDir);
				if (t >= 0.0f && (surfaceStickT < 0.0f || t < surfaceStickT)) {
					surfaceStickT = t;
				}
			}
		} else if (!surfaceStickEnabled) {
			surfaceStickT = -1.0f;
		}
	}
	const bool surfaceStuck = surfaceStickEnabled && surfaceStickT >= 0.0f;
	const float wantedDepth = surfaceStuck
		? glm::max(surfaceStickT + surfaceBias, 0.0f)
		: BrushDistanceFromCamera;

	// The brush lives at its own depth, UNCLAMPED. With the chunk-grid canvas
	// the stamp dispatches into every cell the footprint overlaps, so nothing
	// is lost outside the target's domain - the old clamp (which pinned the
	// brush against the domain wall when aiming past it) is gone.
	glm::vec3 brushCenter = rayOrigin + rayDir * wantedDepth;
	glm::vec3 chunkBoundsMin;
	glm::vec3 chunkBoundsMax;
	ComputeChunkBounds(chunk.blocksPerAxis, chunk.blockSizeWS, chunk.translationWS, chunkBoundsMin, chunkBoundsMax);
	glm::vec3 brushCenterLocal = brushCenter - chunk.translationWS;
	if (bin.snapModifier) {
		// Blender-style Ctrl snap: quantize to half-block steps of the target
		// chunk's voxel grid (grid lines at local -0.5 + k/128), so aligned
		// stamps and cuts land exactly on block boundaries or centers.
		const float snapStep = 0.5f * chunk.blockSizeWS;
		brushCenterLocal = glm::round((brushCenterLocal + glm::vec3(0.5f)) / snapStep) * snapStep - glm::vec3(0.5f);
		brushCenter = brushCenterLocal + chunk.translationWS; // keep the readout consistent
	}

	// World -> shape-local sample rotation: the accumulated R-rotate result,
	// plus the LIVE modal preview while one is up (the indicator rotates as
	// the mouse sweeps / digits are typed; confirm bakes the same value, so
	// the preview can never lie). Rotating the SHAPE by R means sampling it
	// at R^-1 * d, and R^-1 = R^T for rotations.
	glm::mat3 effectiveShapeRot = brushShapeRot;
	if (rotateModalActive) {
		const float previewDeg = rotateModalNumber.empty()
			? rotateModalMouseDeg
			: static_cast<float>(std::atof(rotateModalNumber.c_str()));
		glm::vec3 axis(0.0f);
		axis[rotateModalAxis] = 1.0f;
		effectiveShapeRot = glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(previewDeg), axis)) * brushShapeRot;
	}
	const glm::mat3 sampleRot = glm::transpose(effectiveShapeRot);

	sdfEditUBO pubo;
	pubo.time = time * 0.05f;
	pubo.BrushData = glm::vec4(-brushCenterLocal, BrushRadius);
	pubo.BrushColor = brush.color;
	pubo.BrushAction = leftBut;
	pubo.BrushShape = static_cast<float>(BrushShapeMode);
	pubo.BrushSmoothness = BrushSmoothness;
	pubo.BrushEmissive = EmissiveMode ? 1.0f : 0.0f;
	pubo.BrushRecolor = (bin.recolorModifier && bin.drawDown && bin.interactionAllowed) ? 1.0f : 0.0f;
	pubo.BrushRot = glm::mat4(sampleRot);

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
	brush.orientation = BrushOrientation;
	brush.radius = BrushRadius;
	brush.smoothness = BrushSmoothness;
	brush.zAxis = BrushDistanceFromCamera;
	brush.action = leftBut;
	brush.emissive = EmissiveMode;
	brush.worldPos = glm::vec3(model * glm::vec4(brushCenter, 1.0f));
	brush.surfaceStuck = surfaceStuck;
	brush.snapped = bin.snapModifier;
	brush.rot = glm::mat4(sampleRot);
	// NOTE: the brush UBO is intentionally only mapped at dispatch time
	// (after the compute fence confirms the previous stamp finished) -
	// mapping it every frame overwrites DispatchBaseBlock while the
	// in-flight compute is reading it, silently voiding stamps.

	// Stroke gesture: a stroke lasts as long as the button is held; stamps
	// happen when the cursor moved far enough since the last executed stamp
	// (deltas accumulate across skipped frames).
	const bool brushPointerDown = bin.interactionAllowed && (bin.drawDown || bin.eraseDown);
	const bool isInitialBrushClick = brushPointerDown && !wasBrushPointerDown;
	const bool strokeReleased = !brushPointerDown && wasBrushPointerDown;
	wasBrushPointerDown = brushPointerDown;
	if (isInitialBrushClick) {
		strokeNeedsInitialDispatch = true;
		strokeHadErase = false;
		strokeTouchedChunks.clear();
	}
	if (brushPointerDown && bin.eraseDown) {
		strokeHadErase = true; // a cut: release-split children inherit physical
	}
	if (!brushPointerDown) {
		previousBrushCursorPos = currentCursorPos;
		strokeAbsoluteBrushDepth = false; // spawn-stroke depth mode ends with the stroke
	}
	if (strokeReleased) {
		// The connectivity rule (BAKEIT_PHYSICS_RULES.md §1): every stroke
		// release flood-fills EVERY chunk the stroke stamped (a stroke can
		// span several canvas cells); disconnected islands become their own
		// chunks. Pieces CUT off a physical object fall instantly; islands
		// merely DRAWN are born sketch. Also refreshes content bounds and
		// wakes a physical chunk so it re-settles on its new shape.
		if (strokeTouchedChunks.empty()) {
			splitChunkIslands(editableChunkIndex, strokeHadErase);
		} else {
			for (const int touched : strokeTouchedChunks) {
				splitChunkIslands(touched, strokeHadErase);
			}
			strokeTouchedChunks.clear();
		}
	}
	const glm::dvec2 cursorDelta = currentCursorPos - previousBrushCursorPos;
	const bool movedEnough = glm::dot(cursorDelta, cursorDelta) > 0.25;
	const bool wantsDispatch = brushPointerDown && (strokeNeedsInitialDispatch || movedEnough);

	bool stampedThisFrame = false;
	// Gate on BOTH the target chunk's fence and the chunk that stamped last:
	// the free-stack compaction below rewrites the SHARED allocator, which the
	// in-flight stamp of another chunk may still be popping.
	const bool lastStampIdle = (lastStampChunkIndex < 0)
		|| (vkGetFenceStatus(base->device, chunks[lastStampChunkIndex].brushComp->computeFence) == VK_SUCCESS);
	if (wantsDispatch && lastStampIdle
		&& vkGetFenceStatus(base->device, chunk.brushComp->computeFence) == VK_SUCCESS) {
		stampedThisFrame = true;
		lastStampChunkIndex = editableChunkIndex;
		if (&chunk == &chunks[0]) {
			fineTexDirty = true; // SSBO field changed -> dense fine texture is stale (chunk-0 debug path)
		}
		// Stats + free-stack maintenance from the previous stamp (stable:
		// the compute is idle now). The kernel pops the stable bottom of the
		// stack via a cursor and pushes frees on top - same-dispatch reuse
		// races (garbage brick indices, the "glitch cubes"). Compact here so
		// freshly freed bricks become allocatable for the next stamp.
		int counters[2] = { 0, 0 }; // [0] atlas tail (floats), [1] pop cursor
		int freeBrickCount = 0;
		tileAllocSBO[0].read(base->device, sizeof(counters), counters);
		tileAllocSBO[1].read(base->device, sizeof(int), &freeBrickCount);

		const int consumed = std::min(counters[1], freeStackStableCount);
		if (consumed > 0 && freeBrickCount >= consumed) {
			std::vector<int> stack(static_cast<size_t>(freeBrickCount));
			tileAllocSBO[2].read(base->device, sizeof(int) * stack.size(), stack.data());
			stack.erase(stack.begin(), stack.begin() + consumed);
			freeBrickCount -= consumed;
			if (!stack.empty()) {
				tileAllocSBO[2].map(base->device, sizeof(int) * stack.size(), stack.data());
			}
			tileAllocSBO[1].map(base->device, sizeof(int), &freeBrickCount);
		}
		if (counters[1] != 0) {
			counters[1] = 0;
			tileAllocSBO[0].map(base->device, sizeof(counters), counters);
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
		const float smoothSupportPadding = glm::max(BrushSmoothness * 8.0f, chunk.blockSizeWS * 2.0f);
		glm::vec3 brushExtent = glm::vec3(effectiveBrushRadius + smoothSupportPadding);
		if (BrushShapeMode == 2) {
			// Plane: a thin slab, not a cube - only pad around its thin axis.
			// The cubic extent would mark THOUSANDS of blocks per stamp as
			// dispatch region + cut suspects for a wide cutter. The thin
			// direction in world space is the vector the sample rotation maps
			// onto shape-local +Y (matrix row 1); blend each axis' extent
			// between thin and wide by that amount (exact for axis-aligned
			// rotations, conservative in between).
			const glm::vec3 thinW = glm::abs(
				glm::vec3(sampleRot[0].y, sampleRot[1].y, sampleRot[2].y));
			const glm::vec3 wide(effectiveBrushRadius + smoothSupportPadding);
			const glm::vec3 thin(0.002f + smoothSupportPadding);
			brushExtent = glm::mix(wide, thin, thinW);
		}

		// Stamp targets: the stroke target plus every world-grid cell the
		// footprint overlaps (the lazy chunk-grid canvas - cells tile space,
		// so a stamp straddling a boundary writes BOTH sides and the field
		// stays seamless by construction). Draw strokes create missing cells
		// on the fly; erase only touches what exists. All previous stamps are
		// proven done by the lastStampIdle gate (one queue: fences signal in
		// submission order, so the LAST fence covers them all).
		std::vector<int> stampTargets;
		stampTargets.push_back(editableChunkIndex);
		if (airDrawSpawnEnabled) {
			const glm::vec3 pad(2.0f * chunk.blockSizeWS);
			const glm::ivec3 cellMin = glm::ivec3(glm::floor(brushCenterWorld - brushExtent - pad + 0.5f));
			const glm::ivec3 cellMax = glm::ivec3(glm::floor(brushCenterWorld + brushExtent + pad + 0.5f));
			for (int cz = cellMin.z; cz <= cellMax.z; ++cz)
			for (int cy = cellMin.y; cy <= cellMax.y; ++cy)
			for (int cx = cellMin.x; cx <= cellMax.x; ++cx) {
				int idx = chunkForCell(glm::ivec3(cx, cy, cz));
				if (idx < 0 && leftBut >= 0.5f) {
					idx = ensureCellChunk(glm::ivec3(cx, cy, cz));
				}
				if (idx >= 0 && std::find(stampTargets.begin(), stampTargets.end(), idx) == stampTargets.end()) {
					stampTargets.push_back(idx);
				}
			}
		}

		for (const int ti : stampTargets) {
			SdfChunk& tc = chunks[ti];
			const glm::mat4 tModel = (tc.objectIndex >= 0)
				? scene->objects[tc.objectIndex].ModelMatrix
				: glm::mat4(1.0f);
			const glm::vec3 tLocal = glm::vec3(glm::inverse(tModel) * glm::vec4(brushCenterWorld, 1.0f)) - tc.translationWS;
			const glm::vec3 tCompute = tLocal + glm::vec3(kBrushCenterBias);
			const int bpa = static_cast<int>(tc.blocksPerAxis);
			const glm::vec3 blockCoordMinF = ((tCompute - brushExtent) + 0.5f) / tc.blockSizeWS;
			const glm::vec3 blockCoordMaxF = ((tCompute + brushExtent) + 0.5f) / tc.blockSizeWS;
			glm::ivec3 dispatchBlockMin = glm::ivec3(glm::floor(blockCoordMinF)) - glm::ivec3(1);
			glm::ivec3 dispatchBlockMax = glm::ivec3(glm::floor(blockCoordMaxF)) + glm::ivec3(1);
			if (glm::any(glm::lessThan(dispatchBlockMax, glm::ivec3(0)))
				|| glm::any(glm::greaterThan(dispatchBlockMin, glm::ivec3(bpa - 1)))) {
				continue; // the footprint misses this chunk's domain entirely
			}
			dispatchBlockMin = glm::clamp(dispatchBlockMin, glm::ivec3(0), glm::ivec3(bpa - 1));
			dispatchBlockMax = glm::clamp(dispatchBlockMax, glm::ivec3(0), glm::ivec3(bpa - 1));
			const glm::ivec3 dispatchBlockCount = dispatchBlockMax - dispatchBlockMin + glm::ivec3(1);

			// ERASE stamps mark every touched block as a cut suspect: the release
			// connectivity pass treats them as barriers and voxel-verifies them
			// only when a separation is actually suspected (rules doc §1).
			// The suspect box is INFLATED 3 blocks beyond the dispatch box: the
			// erase leaves allocated near-surface AIR blocks (keep-band, no
			// material crossing) hugging the cut faces and the object's surface.
			if (leftBut < 0.5f) {
				const glm::ivec3 sMin = glm::clamp(dispatchBlockMin - glm::ivec3(3), glm::ivec3(0), glm::ivec3(bpa - 1));
				const glm::ivec3 sMax = glm::clamp(dispatchBlockMax + glm::ivec3(3), glm::ivec3(0), glm::ivec3(bpa - 1));
				for (int bz = sMin.z; bz <= sMax.z; ++bz)
				for (int by = sMin.y; by <= sMax.y; ++by)
				for (int bx = sMin.x; bx <= sMax.x; ++bx) {
					tc.cutSuspects.insert(bx + by * bpa + bz * bpa * bpa);
				}
			}

			// Grow the dense-texture dirty region (chunk-0 debug path only).
			if (ti == 0) {
				fineTexDirty = true;
				fineTexDirtyMin = glm::min(fineTexDirtyMin, dispatchBlockMin);
				fineTexDirtyMax = glm::max(fineTexDirtyMax, dispatchBlockMax);
			}

			sdfEditUBO tpubo = pubo;
			tpubo.BrushData = glm::vec4(-tLocal, BrushRadius);
			tpubo.DispatchBaseBlock = glm::ivec4(dispatchBlockMin, freeStackStableCount);
			tc.brushUbo[0].map(base->device, sizeof(sdfEditUBO), &tpubo);
			int dirtyZero = 0;
			tc.sbo[4].map(base->device, sizeof(int), &dirtyZero);
			tc.cpuTableDirty = true; // surface-stick/pick mirror is stale after this stamp
			tc.lastDrawListBlockToClip = glm::mat4(0.0f); // force this chunk's draw-list rebuild
			tc.brushComp->submit(
				static_cast<uint32_t>(dispatchBlockCount.x),
				static_cast<uint32_t>(dispatchBlockCount.y),
				static_cast<uint32_t>(dispatchBlockCount.z));
			lastStampChunkIndex = ti;
			if (std::find(strokeTouchedChunks.begin(), strokeTouchedChunks.end(), ti) == strokeTouchedChunks.end()) {
				strokeTouchedChunks.push_back(ti);
			}

			if (ti != 0) {
				continue;
			}
			// Coarse LOD pyramid (PRIMARY chunk only): stamp the SAME brush op
			// into every coarse level at its resolution (ubo._pad0 = that
			// level's coarse grid cells) into the SHARED pool, each writing
			// its own table region (CoarseParams.x = tableBase). Bump-only
			// alloc: w=0 free-stack threshold so SdfBrushTexCS always
			// tail-allocates; reset the (shared) free count once.
			int coarseFreeZero = 0;
			coarseSharedSBO[5].map(base->device, sizeof(int), &coarseFreeZero);
			for (CoarseLevel& lvl : coarseLevels) {
			if (!lvl.brushComp || vkGetFenceStatus(base->device, lvl.brushComp->computeFence) != VK_SUCCESS) {
				continue;
			}
			const glm::ivec3 coarseMin = dispatchBlockMin / lvl.N;
			const glm::ivec3 coarseMax = dispatchBlockMax / lvl.N;
			const glm::ivec3 coarseCount = coarseMax - coarseMin + glm::ivec3(1);
			sdfEditUBO cubo = tpubo;                                // same brush params
			cubo._pad0 = static_cast<float>(lvl.blocksPerAxis * 8); // coarse grid (256/128)
			// Widen the R8 field range for the coarse cube (base scale 82/N, same
			// asymmetric -0.6 surface bias) so the coarse march can skip an empty
			// coarse cube in ~1 step (else it crawls ~N/2). Base must match the fine
			// tileFieldScale() default (82) in SdfBrushTexCS.comp.
			cubo._pad1 = 82.0f / static_cast<float>(lvl.N);
			cubo.DispatchBaseBlock = glm::ivec4(coarseMin, 0);
			cubo.CoarseParams = glm::ivec4(lvl.tableBase, BrushOrientation, 0, 0); // x: this level's table region, y: shape up-axis
			lvl.ubo[0].map(base->device, sizeof(sdfEditUBO), &cubo);
			lvl.brushComp->submit(
				static_cast<uint32_t>(coarseCount.x),
				static_cast<uint32_t>(coarseCount.y),
				static_cast<uint32_t>(coarseCount.z));
			}
		}
		strokeNeedsInitialDispatch = false;
		previousBrushCursorPos = currentCursorPos;
	}

	// Rebuild the draw list when its inputs changed: right behind each stamp
	// (blocks[] changed) and whenever the camera moved - the frustum cull and
	// the front-to-back bucket order bake the camera into the list. Skipping
	// unchanged frames avoids a per-frame fence wait + queue submit, and the
	// HUD instance-count readback (GPU -> CPU) runs only on stamp frames so
	// idle/orbit frames never sync back to the CPU.
	// Rebuild each chunk's draw list when its inputs changed: right behind a
	// stamp into that chunk (blocks[] changed) and whenever the camera moved -
	// the frustum cull and the front-to-back bucket order bake the camera into
	// the list. Each chunk culls/sorts in its OWN object/block space through
	// its proxy object's model matrix. VR platforms set the camera's combined-
	// eye cull override (one frustum containing both eyes); desktop leaves it
	// zeroed and eye 0 is used.
	const bool hasCullOverride = scene->camera->cullView != glm::mat4(0.0f);
	const glm::mat4 cullViewProj = hasCullOverride
		? scene->camera->cullProj * scene->camera->cullView
		: scene->camera->proj[0] * scene->camera->view[0];
	for (SdfChunk& c : chunks) {
		if (!c.drawList || c.meshIndex < 0) continue;
		Mesh& chunkMesh = scene->meshes[c.meshIndex];
		if (!chunkMesh.useIndirectDraw) continue;

		// Distance sort runs in object/block space: bring the camera into
		// this chunk's object space first (matters once the pot rotates).
		const glm::mat4 cModel = (c.objectIndex >= 0)
			? scene->objects[c.objectIndex].ModelMatrix
			: glm::mat4(1.0f);
		const glm::vec3 cameraObject = glm::vec3(glm::inverse(cModel) * glm::vec4(scene->camera->position, 1.0f));
		const glm::vec3 camBlockUnits =
			(cameraObject - c.translationWS - ChunkLocalCenterOrigin(c.blockSizeWS)) / c.blockSizeWS + glm::vec3(0.5f);

		// Block-units -> world is a uniform scale + translation (inverse of
		// the camBlockUnits mapping above); chained with proj * view it gives
		// the matrix the frustum planes are extracted from. Model goes in
		// front of blockToWorld: block units live in object space, the
		// frustum in world space.
		const float s = c.blockSizeWS;
		const glm::vec3 origin =
			c.translationWS + ChunkLocalCenterOrigin(s) - glm::vec3(0.5f * s);
		glm::mat4 blockToWorld(1.0f);
		blockToWorld[0][0] = s;
		blockToWorld[1][1] = s;
		blockToWorld[2][2] = s;
		blockToWorld[3] = glm::vec4(origin, 1.0f);
		const glm::mat4 blockToClip = cullViewProj * cModel * blockToWorld;

		// Distance bands: fine -> 2 past coarseLodDistance, 2 -> 4 past coarseLodDistance2
		// (block units). coarseLodLevel caps the coarsest band (0 off / 2 / 4). A huge
		// distance disables a band (all fine / no 4). The coarse pyramid's tables hold
		// only the PRIMARY chunk's field, so other chunks render all-fine until the
		// pyramid goes per-chunk.
		const int maxLevel = (&c == &chunks[0]) ? scene->coarseLodLevel : 0;
		const float split1Dist = (maxLevel >= 2) ? scene->coarseLodDistance : 1e9f;
		const float split2Dist = (maxLevel >= 4) ? scene->coarseLodDistance2 : 1e9f;
		const bool stampedThisChunk = stampedThisFrame && (&c == &chunk);
		if (stampedThisChunk || blockToClip != c.lastDrawListBlockToClip
			|| split1Dist != c.lastCoarseSplitDist || split2Dist != c.lastCoarseSplitDist2
			|| maxLevel != c.lastCoarseLodLevelForList) {
			c.drawList->build(camBlockUnits, blockToClip, split1Dist, split2Dist, maxLevel);
			c.lastDrawListBlockToClip = blockToClip;
			c.lastCoarseSplitDist = split1Dist;
			c.lastCoarseSplitDist2 = split2Dist;
			c.lastCoarseLodLevelForList = maxLevel;
		}
	}
	if (stampedThisFrame && chunk.meshIndex >= 0) {
		// HUD mirror: last completed list's count (host-visible).
		volume.stats.dirtyBlockCount = static_cast<int>(
			base->vbuffer->readIndirectInstanceCount(scene->meshes[chunk.meshIndex]));
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
		&& vkGetFenceStatus(base->device, chunks[0].brushComp->computeFence) == VK_SUCCESS
		&& vkGetFenceStatus(base->device, fineToTexComp->computeFence) == VK_SUCCESS) {
		dispatchFineToTex(); // clear to +FAR, then SdfFineToTexCS over the 64^3 grid
		fineTexDirty = false;
	}
#endif
}

SdfEditor::~SdfEditor() {
	delete fineToTexComp;
	for (SdfChunk& chunk : chunks) {
		delete chunk.drawList;
		delete chunk.brushComp;
		for (UBO& ubo : chunk.brushUbo) {
			vkDestroyBuffer(base->device, ubo.buffer, nullptr);
			vkFreeMemory(base->device, ubo.bufferMemory, nullptr);
		}
		for (SBO& sbo : chunk.sbo) {
			vkDestroyBuffer(base->device, sbo.buffer, nullptr);
			vkFreeMemory(base->device, sbo.bufferMemory, nullptr);
		}
	}
	for (SBO& sbo : tileAllocSBO) {
		vkDestroyBuffer(base->device, sbo.buffer, nullptr);
		vkFreeMemory(base->device, sbo.bufferMemory, nullptr);
	}
	for (CoarseLevel& lvl : coarseLevels) {
		delete lvl.brushComp;
		for (UBO& ubo : lvl.ubo) {
			vkDestroyBuffer(base->device, ubo.buffer, nullptr);
			vkFreeMemory(base->device, ubo.bufferMemory, nullptr);
		}
	}
	for (SBO& sbo : coarseSharedSBO) {
		vkDestroyBuffer(base->device, sbo.buffer, nullptr);
		vkFreeMemory(base->device, sbo.bufferMemory, nullptr);
	}
}

