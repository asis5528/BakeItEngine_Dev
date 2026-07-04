#pragma once
#include "Core/Pipelines/Shader.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

class SdfDrawList;
class ComputePipeline;

// One SDF chunk: a self-contained block grid with its own field tables and
// draw list. The tile pools and their allocators stay on SdfEditor and are
// SHARED by every chunk (a block's allocated base indexes the one pool), so
// moving a block between chunks is pure table surgery - the basis for the
// planned split (cut a piece loose) and weld (stamp a chunk into another).
struct SdfChunk
{
	// False once this chunk was absorbed by a weld: tables empty, tiles
	// returned to the pool, mesh renders nothing. Skipped by stroke
	// targeting; the slot can be recycled by a future spawn.
	bool active = true;

	// Grid description: where the chunk sits (object space) and how fine it is.
	glm::vec3 translationWS = glm::vec3(0.0f);
	glm::vec3 uploadedTranslationWS = glm::vec3(1e9f); // spatial-SBO upload cache
	uint32_t blocksPerAxis = 64u;
	uint32_t blockDim = 8u;
	float blockSizeWS = 1.0f / 64.0f;

	// Per-chunk field tables, keeping the historical 8-slot layout:
	// [0] bricks (dummy under BAKEIT_SDF_TEXTURE_ATLAS), [1] block table,
	// [3] block colors, [4] dirty counter, [5] block spatial data.
	// Slots [2]/[6]/[7] were the tile allocator, which is now SHARED across
	// chunks (SdfEditor::tileAllocSBO); they stay allocated only as descriptor
	// filler ([2] is bound as graphics binding 6, which no shader reads).
	std::vector<SBO> sbo;

	// GPU-driven draw list (count/scan/scatter) for this chunk's proxy mesh.
	SdfDrawList* drawList = nullptr;

	// Per-chunk brush stamp pipeline + UBO, bound to THIS chunk's tables and
	// the shared pool/allocator (the CoarseLevel::brushComp pattern: one
	// shader, one descriptor set per consumer - retargeting a stroke to
	// another chunk never re-points descriptors). The UBO is mapped only at
	// dispatch time, after this pipeline's fence confirms the previous stamp.
	ComputePipeline* brushComp = nullptr;
	std::vector<UBO> brushUbo;

	// Scene slots of this chunk's proxy-cube object/mesh (indices, not
	// pointers - scene vectors reallocate as models load).
	int objectIndex = -1;
	int meshIndex = -1;

	// Draw-list rebuild cache: camera matrix + LOD params baked into the last
	// build; the list is rebuilt only when one of them changes (or a stamp
	// ran), so idle frames skip the build submit and the HUD readback.
	glm::mat4 lastDrawListBlockToClip = glm::mat4(0.0f);
	float lastCoarseSplitDist = -1.0f;
	float lastCoarseSplitDist2 = -1.0f;
	int lastCoarseLodLevelForList = -1;
};
