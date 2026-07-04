#include "SdfEditor.h"
#include "Features/Sdf/SdfBackend.h"
#include "Core/Resources/ResourceManager.h"
#include "Render/Renderer.h" // renderPassDirty: re-record after a coarse-level re-point
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
	glm::ivec4 CoarseParams = glm::ivec4(0); // x: table-base offset into the shared coarse table/colors
};

// Mirrors WeldUbo in shaders/Sdf/SdfWeldCS.comp.
struct alignas(16) sdfWeldUBO {
	glm::mat4 aNormToBGrid;      // A voxel (chunk-normalized) -> B fine-grid coords
	glm::vec4 params;            // x: smin k, y: A grid cells, z: B grid cells
	glm::ivec4 DispatchBaseBlock; // xyz: A block base, w: free-stack stable count
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
		0.0f);
	wubo.DispatchBaseBlock = glm::ivec4(blockMin, freeStackStableCount);

	// One-shot weld pipeline: fresh descriptors for this src/dst pair, torn
	// down right after the (synchronous) dispatch.
	ComputeInput weldInput;
	weldInput.computeShaderPath = "shaders/Sdf/SdfWeldCS.spv";
	weldInput.descriptorTypes = {
		VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  // shared tile pool (B read, A written)
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // 2: A block table
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // 3: shared tail counter
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // 4: A block colors
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // 5: A dirty counter
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // 6: shared free count
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // 7: shared free stack
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER  // 8: B block table
	};
	ComputePipeline* weld = new ComputePipeline(base, weldInput);
	std::vector<UBO> weldUbo = base->vbuffer->createUniformBuffers({ sizeof(sdfWeldUBO) });
	weldUbo[0].map(base->device, sizeof(sdfWeldUBO), &wubo);
	weld->setupDescriptors(weldUbo, { tilePoolTexture },
		{ dst.sbo[1], tileAllocSBO[0], dst.sbo[3], dst.sbo[4],
		  tileAllocSBO[1], tileAllocSBO[2], src.sbo[1] });
	int zero = 0;
	dst.sbo[4].map(base->device, sizeof(int), &zero);
	weld->ensureCommandResources(); // command buffer + SIGNALED fence: submit() waits on it first
	weld->submit(
		static_cast<uint32_t>(blockCount.x),
		static_cast<uint32_t>(blockCount.y),
		static_cast<uint32_t>(blockCount.z));
	vkWaitForFences(base->device, 1, &weld->computeFence, VK_TRUE, UINT64_MAX);

	// Absorb B: collect every tile it owned back into the shared free stack.
	// First compact the stack exactly like update() does - the weld may have
	// POPPED entries (alloc cursor) that must not be double-allocated later.
	const int kIndexMask = (1 << 30) - 1;
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
	src.sbo[4].map(base->device, sizeof(int), &zero);
	src.active = false;
	if (editableChunkIndex == srcIndex) {
		editableChunkIndex = dstIndex;
	}

	// Both draw lists changed underneath their caches: force rebuilds.
	dst.lastDrawListBlockToClip = glm::mat4(0.0f);
	src.lastDrawListBlockToClip = glm::mat4(0.0f);

	delete weld;
	for (UBO& u : weldUbo) {
		vkDestroyBuffer(base->device, u.buffer, nullptr);
		vkFreeMemory(base->device, u.bufferMemory, nullptr);
	}
	std::printf("Welded chunk %d into chunk %d (%d tiles freed)\n",
		srcIndex, dstIndex, static_cast<int>(freed.size()));
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

int SdfEditor::pickChunkIndexByRay(const glm::vec3& rayOriginWS, const glm::vec3& rayDirWS)
{
	// Nearest chunk whose AABB the ray enters, each tested in its own object
	// space (models are rigid + uniform scale, so object-space t values are
	// comparable enough for picking). Miss everything -> -1 (spawn cue).
	int best = -1;
	float bestT = 1e30f;
	bool anyHit = false;
	for (size_t i = 0; i < chunks.size(); ++i) {
		SdfChunk& c = chunks[i];
		if (!c.active) continue; // absorbed by a weld: empty bounds, never a target
		const glm::mat4 cModel = (c.objectIndex >= 0)
			? scene->objects[c.objectIndex].ModelMatrix
			: glm::mat4(1.0f);
		const glm::mat4 inv = glm::inverse(cModel);
		const glm::vec3 ro = glm::vec3(inv * glm::vec4(rayOriginWS, 1.0f));
		const glm::vec3 rd = glm::normalize(glm::mat3(inv) * rayDirWS);
		glm::vec3 bmin, bmax;
		ComputeChunkBounds(c.blocksPerAxis, c.blockSizeWS, c.translationWS, bmin, bmax);
		float tEnter = 0.0f, tExit = 0.0f;
		if (!IntersectAabb(ro, rd, bmin, bmax, tEnter, tExit)) continue;
		if (tExit < 0.0f) continue;             // chunk entirely behind the ray
		const float t = glm::max(tEnter, 0.0f); // camera inside the box -> distance 0
		if (!anyHit || t < bestT) {
			anyHit = true;
			bestT = t;
			best = static_cast<int>(i);
		}
	}
	return best;
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
	}
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

	// Coarse-LOD pyramid lives in ONE shared pool (binding 3) + concatenated table
	// (binding 9), so the rendered level is selected per-instance in the VS/FS by the
	// distance band - no descriptor re-point needed at all.

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
			if (picked < 0 && bin.drawDown && airDrawSpawnEnabled
				&& chunks.size() < static_cast<size_t>(kMaxChunks)) {
				const glm::vec3 spawnPos = bin.rayOrigin + bin.rayDir * BrushDistanceFromCamera;
				spawnChunk(spawnPos, /*debugStamp=*/false, /*rebuildScene=*/true);
				picked = static_cast<int>(chunks.size()) - 1;
				// The chunk is centered at camera + D: measure this stroke's
				// depth from the camera so the stamps land AT the center, not
				// clamped onto the far boundary (spheres cut in half).
				strokeAbsoluteBrushDepth = true;
				lastSpawnStrokeBrushWS = spawnPos;
				std::printf("Spawned chunk %d\n", picked);
			}
			if (picked >= 0 && picked != editableChunkIndex) {
				editableChunkIndex = picked;
				std::printf("Brush target: chunk %d\n", picked);
			}
		} else if (pointerDownNow && strokeAbsoluteBrushDepth && bin.drawDown && airDrawSpawnEnabled) {
			// Spawn-stroke chunk chaining: the brush rides at camera + D
			// (world). When it nears the current chunk's boundary - margin =
			// the brush footprint, so no stamp ever gets clipped by the
			// domain edge - hop to the chunk that contains the point, and
			// when none does, spawn one placed AHEAD along the stroke so each
			// chained chunk hosts a full run of stamps before the next hop.
			// Long air strokes span as many chunks as they need.
			const glm::vec3 desired = bin.rayOrigin + bin.rayDir * BrushDistanceFromCamera;
			const float margin = BrushRadius
				+ glm::max(BrushSmoothness * 8.0f, 3.0f * chunks[editableChunkIndex].blockSizeWS);
			if (!chunkContainsPoint(chunks[editableChunkIndex], desired, margin)) {
				int idx = -1;
				for (size_t i = 0; i < chunks.size(); ++i) {
					if (chunks[i].active && chunkContainsPoint(chunks[i], desired, margin)) {
						idx = static_cast<int>(i);
						break;
					}
				}
				if (idx < 0 && chunks.size() < static_cast<size_t>(kMaxChunks)) {
					const glm::vec3 dir = desired - lastSpawnStrokeBrushWS;
					const float len = glm::length(dir);
					const glm::vec3 ahead = (len > 1e-5f)
						? (dir / len) * glm::max(0.5f - margin - 0.05f, 0.0f)
						: glm::vec3(0.0f);
					spawnChunk(desired + ahead, /*debugStamp=*/false, /*rebuildScene=*/true);
					idx = static_cast<int>(chunks.size()) - 1;
					std::printf("Spawned chunk %d (stroke chain)\n", idx);
				}
				if (idx >= 0 && idx != editableChunkIndex) {
					editableChunkIndex = idx;
					strokeNeedsInitialDispatch = true; // stamp promptly in the new chunk
				}
			}
			lastSpawnStrokeBrushWS = desired;
		}
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

	float tEnter = 0.0f, tExit = 0.0f;
	glm::vec3 brushCenter = rayOrigin + rayDir * BrushDistanceFromCamera; // fallback
	glm::vec3 chunkBoundsMin;
	glm::vec3 chunkBoundsMax;
	ComputeChunkBounds(chunk.blocksPerAxis, chunk.blockSizeWS, chunk.translationWS, chunkBoundsMin, chunkBoundsMax);
	if (IntersectAabb(rayOrigin, rayDir, chunkBoundsMin, chunkBoundsMax, tEnter, tExit)) {
		float tStart = std::max(tEnter, 0.0f);
		// Entry-relative depth normally; camera-absolute for the stroke that
		// spawned this chunk (see strokeAbsoluteBrushDepth in the header).
		float tCenter = strokeAbsoluteBrushDepth
			? glm::clamp(BrushDistanceFromCamera, tStart, tExit - 0.001f)
			: std::min(tStart + BrushDistanceFromCamera, tExit - 0.001f);
		if (tCenter >= tStart) {
			brushCenter = rayOrigin + rayDir * tCenter;
		}
	}
	const glm::vec3 brushCenterLocal = brushCenter - chunk.translationWS;

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
		strokeAbsoluteBrushDepth = false; // spawn-stroke depth mode ends with the stroke
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
		const glm::vec3 brushCenterCompute = brushCenterLocal + glm::vec3(kBrushCenterBias);
		const glm::vec3 brushExtent = glm::vec3(effectiveBrushRadius + smoothSupportPadding);
		const glm::vec3 blockCoordMinF = ((brushCenterCompute - brushExtent) + 0.5f) / chunk.blockSizeWS;
		const glm::vec3 blockCoordMaxF = ((brushCenterCompute + brushExtent) + 0.5f) / chunk.blockSizeWS;
		glm::ivec3 dispatchBlockMin = glm::ivec3(glm::floor(blockCoordMinF)) - glm::ivec3(1);
		glm::ivec3 dispatchBlockMax = glm::ivec3(glm::floor(blockCoordMaxF)) + glm::ivec3(1);
		dispatchBlockMin = glm::clamp(dispatchBlockMin, glm::ivec3(0), glm::ivec3(int(chunk.blocksPerAxis) - 1));
		dispatchBlockMax = glm::clamp(dispatchBlockMax, glm::ivec3(0), glm::ivec3(int(chunk.blocksPerAxis) - 1));
		const glm::ivec3 dispatchBlockCount = dispatchBlockMax - dispatchBlockMin + glm::ivec3(1);

		// Grow the dense-texture dirty region so the localized rebuild covers
		// every block this stamp touched (accumulates across stamps until the
		// next convert consumes + resets it). Chunk-0 debug path only.
		if (&chunk == &chunks[0]) {
			fineTexDirtyMin = glm::min(fineTexDirtyMin, dispatchBlockMin);
			fineTexDirtyMax = glm::max(fineTexDirtyMax, dispatchBlockMax);
		}

		pubo.DispatchBaseBlock = glm::ivec4(dispatchBlockMin, freeStackStableCount);
		chunk.brushUbo[0].map(base->device, sizeof(sdfEditUBO), &pubo);
		int dirtyZero = 0;
		chunk.sbo[4].map(base->device, sizeof(int), &dirtyZero);
		chunk.brushComp->submit(
			static_cast<uint32_t>(dispatchBlockCount.x),
			static_cast<uint32_t>(dispatchBlockCount.y),
			static_cast<uint32_t>(dispatchBlockCount.z));
		strokeNeedsInitialDispatch = false;
		previousBrushCursorPos = currentCursorPos;

		// Coarse LOD pyramid: stamp the SAME brush op into EVERY coarse level at its
		// resolution (ubo._pad0 = that level's coarse grid cells = blocksPerAxis*8)
		// into the SHARED pool, each writing its own table region (CoarseParams.x =
		// tableBase). All levels stay populated so distance bands can switch freely.
		// Cheap/local (only the coarse blocks the brush touches) and non-blocking (own
		// fence). Bump-only alloc: w=0 free-stack threshold so SdfBrushTexCS always
		// tail-allocates from the shared counter; reset the (shared) free count once.
		int coarseFreeZero = 0;
		coarseSharedSBO[5].map(base->device, sizeof(int), &coarseFreeZero);
		for (CoarseLevel& lvl : coarseLevels) {
			// Coarse tables mirror only the PRIMARY chunk's field; strokes into
			// spawned chunks (all-fine render) skip the pyramid.
			if (&chunk != &chunks[0]) {
				break;
			}
			if (!lvl.brushComp || vkGetFenceStatus(base->device, lvl.brushComp->computeFence) != VK_SUCCESS) {
				continue;
			}
			const glm::ivec3 coarseMin = dispatchBlockMin / lvl.N;
			const glm::ivec3 coarseMax = dispatchBlockMax / lvl.N;
			const glm::ivec3 coarseCount = coarseMax - coarseMin + glm::ivec3(1);
			sdfEditUBO cubo = pubo;                                 // same brush params
			cubo._pad0 = static_cast<float>(lvl.blocksPerAxis * 8); // coarse grid (256/128)
			// Widen the R8 field range for the coarse cube (base scale 82/N, same
			// asymmetric -0.6 surface bias) so the coarse march can skip an empty
			// coarse cube in ~1 step (else it crawls ~N/2). Base must match the fine
			// tileFieldScale() default (82) in SdfBrushTexCS.comp.
			cubo._pad1 = 82.0f / static_cast<float>(lvl.N);
			cubo.DispatchBaseBlock = glm::ivec4(coarseMin, 0);
			cubo.CoarseParams = glm::ivec4(lvl.tableBase, 0, 0, 0); // this level's table region
			lvl.ubo[0].map(base->device, sizeof(sdfEditUBO), &cubo);
			lvl.brushComp->submit(
				static_cast<uint32_t>(coarseCount.x),
				static_cast<uint32_t>(coarseCount.y),
				static_cast<uint32_t>(coarseCount.z));
		}
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

