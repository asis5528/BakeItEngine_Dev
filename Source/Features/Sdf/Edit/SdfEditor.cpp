#include "SdfEditor.h"
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
constexpr int kAtlasDim = 512;
constexpr float kBrushCenterBias = 0.031f;
constexpr int32_t kBlockEmpty = -999999;
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
	const int atlasCapacityBricks = (kAtlasDim * kAtlasDim * kAtlasDim) / kBrickSamplesPerBlock;

	ComputeInput input;
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
	comp = new ComputePipeline(base, input);
	computeUBO = base->vbuffer->createUniformBuffers({ sizeof(sdfEditUBO) });

	computeSBO = base->vbuffer->createStorageBuffers({
		sizeof(float) * kAtlasDim * kAtlasDim * kAtlasDim,   // 0: brick atlas
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
}

void SdfEditor::update(float time)
{
	uploadChunkSpatialData();

	const Input& in = *scene->input;
	const glm::dvec2 currentCursorPos = in.mousePos;

	if (in.uPressed) {
		BrushShapeMode = (BrushShapeMode + 1) % 3;
		const char* shapeName = (BrushShapeMode == 0) ? "Cube" : ((BrushShapeMode == 1) ? "Sphere" : "Plane");
		std::printf("Brush Shape: %s\n", shapeName);
	}
	if (in.iPressed) {
		EmissiveMode = !EmissiveMode;
		std::printf("Emissive Mode: %s\n", EmissiveMode ? "ON" : "OFF");
	}

	const float altSpeed = in.altDown ? 0.1f : 1.0f;
	const float brushRadiusStep = 0.0001f * altSpeed;
	const float brushSmoothnessStep = 0.0001f * altSpeed;
	const float brushDistanceStep = 0.005f * altSpeed;
	float msx = in.mousePos.x / base->presentExtent().width;
	float msy = in.mousePos.y / base->presentExtent().height;

	if (!in.cameraMoveMode) {
		if (in.rightButtonDown) leftBut = 0.0;
		if (in.leftButtonDown) leftBut = 1.0;
		if (in.aDown) { BrushRadius += brushRadiusStep; }
		if (in.dDown) { BrushRadius -= brushRadiusStep; }
		if (in.qDown) { BrushSmoothness += brushSmoothnessStep; }
		if (in.eDown) { BrushSmoothness -= brushSmoothnessStep; }
		if (in.wDown) BrushDistanceFromCamera += brushDistanceStep;
		if (in.sDown) BrushDistanceFromCamera -= brushDistanceStep;

		if (BrushRadius < 0.001f) BrushRadius = 0.001f;
		if (BrushSmoothness < 0.0001f) BrushSmoothness = 0.0001f;
		// Upper bound keeps the kernel's keepBand (2x smoothness) - and with
		// it the number of retained near-surface bricks - within budget.
		if (BrushSmoothness > 0.005f) BrushSmoothness = 0.005f;
		if (BrushDistanceFromCamera < 0.02f) BrushDistanceFromCamera = 0.02f;
		if (BrushDistanceFromCamera > 3.0f) BrushDistanceFromCamera = 3.0f;
	}

	// Brush placement: ray through the cursor, pushed into the chunk bounds.
	glm::mat4 invProj = glm::inverse(scene->camera->proj[0]);
	glm::mat4 invView = glm::inverse(scene->camera->view[0]);
	float ndcX = msx * 2.0f - 1.0f;
	float ndcY = 1.0f - msy * 2.0f;
	glm::vec4 rayClip(ndcX, ndcY, -1.0f, 1.0f);
	glm::vec4 rayView = invProj * rayClip;
	rayView = glm::vec4(rayView.x, rayView.y, -1.0f, 0.0f);
	glm::vec3 rayDir = glm::normalize(glm::vec3(invView * rayView));
	glm::vec3 rayOrigin = scene->camera->position;

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
	pubo.BrushRecolor = (in.shiftDown && in.leftButtonDown && !in.cameraMoveMode) ? 1.0f : 0.0f;

	// Editor-facing brush state mirror.
	brush.data = glm::vec4(-brushCenter, BrushRadius);
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
	const bool brushPointerDown = !in.cameraMoveMode && (in.leftButtonDown || in.rightButtonDown);
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

		volume.stats.tailBricks = counters[0] / kBrickSamplesPerBlock;
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
	}

	// Rebuild the draw list when its inputs changed: right behind each stamp
	// (blocks[] changed) and whenever the camera moved - the frustum cull and
	// the front-to-back bucket order bake the camera into the list. Skipping
	// unchanged frames avoids a per-frame fence wait + queue submit, and the
	// HUD instance-count readback (GPU -> CPU) runs only on stamp frames so
	// idle/orbit frames never sync back to the CPU.
	Mesh* targetMesh = findEditableMesh();
	if (drawList && targetMesh && targetMesh->useIndirectDraw) {
		const glm::vec3 camBlockUnits =
			(scene->camera->position - chunkTranslationWS - ChunkLocalCenterOrigin(chunkBlockSizeWS)) / chunkBlockSizeWS + glm::vec3(0.5f);

		// Block-units -> world is a uniform scale + translation (inverse of
		// the camBlockUnits mapping above); chained with proj * view it gives
		// the matrix the frustum planes are extracted from. (VR note: eye 0
		// only - revisit when VR is re-enabled.)
		const float s = chunkBlockSizeWS;
		const glm::vec3 origin =
			chunkTranslationWS + ChunkLocalCenterOrigin(s) - glm::vec3(0.5f * s);
		glm::mat4 blockToWorld(1.0f);
		blockToWorld[0][0] = s;
		blockToWorld[1][1] = s;
		blockToWorld[2][2] = s;
		blockToWorld[3] = glm::vec4(origin, 1.0f);
		const glm::mat4 blockToClip =
			scene->camera->proj[0] * scene->camera->view[0] * blockToWorld;

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
}

SdfEditor::~SdfEditor() {
	delete drawList;
	delete comp;
	for (UBO& ubo : computeUBO) {
		vkDestroyBuffer(base->device, ubo.buffer, nullptr);
		vkFreeMemory(base->device, ubo.bufferMemory, nullptr);
	}
	for (SBO& sbo : computeSBO) {
		vkDestroyBuffer(base->device, sbo.buffer, nullptr);
		vkFreeMemory(base->device, sbo.bufferMemory, nullptr);
	}
}
