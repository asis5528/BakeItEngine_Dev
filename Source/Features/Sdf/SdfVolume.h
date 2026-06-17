#pragma once
#include <glm/glm.hpp>

// Public contract for the sparse SDF volume (brick atlas).
//
// Owner: Features/Volumetric. SdfEditor (Edit/) writes it; other features and
// the Editor read it through this header instead of reaching into SdfEditor
// internals or Scene fields.
//
// Grows as consumers appear. Next candidates to migrate here:
// grid constants (chunkBlocksPerAxis / chunkBlockDim / chunkBlockSizeWS),
// atlas buffers, block tables and free lists - once Physics/Fluid need them.

struct SdfVolumeStats
{
	int dirtyBlockCount = 0;
	int usedBricks = 0;
	int capacityBricks = 0;
	int tailBricks = 0;
	int freeBricks = 0;
	float usage = 0.0f;
};

struct SdfVolume
{
	SdfVolumeStats stats;

	// Sculptable chunk bounds in WORLD space (model transform applied).
	// Refreshed by SdfEditor every update; the post pass draws them as the
	// boundary cage so the user can see where stamps land.
	glm::vec3 boundsMinWS = glm::vec3(0.0f);
	glm::vec3 boundsMaxWS = glm::vec3(0.0f);

	// World-space edge length of one block (8x8x8 cells). Constant for the
	// chunk; the deferred-normal post pass needs it to scale the brick-less
	// extrapolation bias when it samples the field gradient.
	float blockSizeWS = 0.0f;
};
