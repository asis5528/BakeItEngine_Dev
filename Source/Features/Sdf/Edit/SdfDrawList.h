#pragma once
#include "Core/Pipelines/ComputePipeline.h"
#include <glm/glm.hpp>

// GPU-built, approximately front-to-back draw list for the editable SDF chunk.
//
// Three compute passes (counting sort by camera distance, 256 buckets):
//   1. count   - each active block (blocks[i] >= 0) inside the view frustum
//                increments its bucket
//   2. scan    - prefix-sums bucket counts into start offsets and writes the
//                total into the indirect draw command's instanceCount
//   3. scatter - each surviving block writes its ID into its bucket's slot
//
// Blocks outside the view frustum are culled in count/scatter (bounding-
// sphere vs. the 6 planes extracted from the blockToClip matrix), so the
// camera is baked into the list: rebuild whenever it moves, not only after
// stamps.
//
// The instance buffer comes out ordered near-to-far, so the brick cubes
// rasterize roughly front-to-back and early-Z (depth_greater in SdfBrickFS)
// rejects occluded bricks before their raymarch runs. Distance from the
// camera is monotonic along any view ray, so bucket order is per-ray correct;
// order never affects correctness (grid blocks don't overlap along a ray),
// only performance.
//
// All passes + barriers are recorded once into one command buffer and
// re-submitted per build; only the camera UBO changes between builds.
class SdfDrawList
{
public:
	SdfDrawList(
		VulkanBase* base,
		const SBO& blocksSbo,
		VkBuffer instanceIdBuffer,
		uint64_t instanceIdBufferSize,
		VkBuffer indirectBuffer);
	~SdfDrawList();

	// camBlockUnits: camera position expressed in block-grid units
	// (so that distance(blockCoord + 0.5, camBlockUnits) is the camera
	// distance in blocks). blockToClip: proj * view * blockUnits->world;
	// the frustum-cull planes are extracted from it (Gribb-Hartmann).
	// Distance-band LOD thresholds in block units: split1 = fine->2 boundary, split2 =
	// 2->4 boundary (farther). Pass a huge value to disable a band. maxLevel (0/2/4)
	// caps the coarsest band. Waits for the previous build, then submits.
	void build(const glm::vec3& camBlockUnits, const glm::mat4& blockToClip,
		float split1DistBlocks, float split2DistBlocks, int maxLevel);

private:
	void record();

	VulkanBase* base;
	ComputePipeline* countPipe;
	ComputePipeline* scanPipe;
	ComputePipeline* scatterPipe;
	std::vector<UBO> sortParams; // vec4: xyz camera (block units), w bucket scale
	std::vector<SBO> bucketSbo;  // NUM_BUCKETS uints: counts -> offsets
	std::vector<SBO> anchorSbo;  // one uint per coarse block (finest level): anchor claim
	VkBuffer indirectBuffer;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
};
