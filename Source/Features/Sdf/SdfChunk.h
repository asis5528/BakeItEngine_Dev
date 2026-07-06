#pragma once
#include "Core/Pipelines/Shader.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <unordered_set>
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

	// Object state (BAKEIT_PHYSICS_RULES.md §2): sketch (physical == false,
	// hangs where drawn, never moves) or physical (assigned to the physical
	// world by the "P" commit or inherited on split - falls, lands, rests).
	// One-way: once physical, always physical. Integration is linear-only for
	// now and writes the proxy object's ModelMatrix translation - the same
	// channel the GUI drag and the future VR grab use - so the draw list, the
	// brush's inverse-model math and the weld all follow a falling chunk for
	// free. Rotation/contacts are later stages (see the rules doc).
	bool physical = false;         // integrated by SdfEditor::updateChunkPhysics
	bool asleep = false;           // resting on the ground, integration skipped
	glm::vec3 velocityWS = glm::vec3(0.0f);
	// Object-space AABB of the OCCUPIED blocks (surface + interior) - the
	// object's real extent, vs the mostly-empty 64^3 domain. Refreshed by the
	// stroke-release connectivity pass and at physical promotion; used for
	// ground contact and content-first ray picking. min > max = no content.
	glm::vec3 contentMinOS = glm::vec3(0.0f);
	glm::vec3 contentMaxOS = glm::vec3(-1.0f);

	// Cut suspects (BAKEIT_PHYSICS_RULES.md §1): block cells the ERASER
	// touched, accumulated across strokes. The release connectivity pass
	// treats them as barriers in its block-level flood fill; only when that
	// reports a possible separation are these blocks' voxels read back and
	// verified ("does material still bridge?"). Pruned when a block goes
	// empty/interior; cleared/migrated by the verify itself.
	std::unordered_set<int32_t> cutSuspects;

	// CPU mirror of the block table for ray queries (surface-stick brush
	// placement, material-first stroke picking). Refreshed lazily by
	// SdfEditor::raycastChunkMaterial whenever dirty AND the chunk's stamps
	// are idle; every table writer (stamp, split, weld, retire, clear) sets
	// the dirty flag. One stamp of staleness is fine for aiming.
	std::vector<int32_t> cpuTable;
	bool cpuTableDirty = true;

	// Draw-list rebuild cache: camera matrix + LOD params baked into the last
	// build; the list is rebuilt only when one of them changes (or a stamp
	// ran), so idle frames skip the build submit and the HUD readback.
	glm::mat4 lastDrawListBlockToClip = glm::mat4(0.0f);
	float lastCoarseSplitDist = -1.0f;
	float lastCoarseSplitDist2 = -1.0f;
	int lastCoarseLodLevelForList = -1;
};
