#pragma once

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
};
