#pragma once
#include "Platform/Platform.h"

// SDF field backend switch (DESKTOP TEST) --------------------------------------
//
// When BAKEIT_SDF_TEXTURE_ATLAS is defined, the fine SDF field is stored in a
// SPARSE 3D-texture tile pool instead of the bricks[] SSBO atlas:
//   - the brush (SdfBrushTexCS) imageStores each block's 9^3 into its own tile,
//   - the render (SdfBrickFS, tex path) samples that tile with hardware
//     trilinear,
//   - the bricks[] SSBO field path is unused (a dummy buffer keeps the render
//     binding valid).
// The block table (blocks[]) and the free-list brick allocator are reused as-is
// - a block's allocated "base" is reinterpreted as a tile INDEX. Each tile is
// self-contained (8 cells + 1 +pad), so there is no cross-tile boundary race.
//
// Comment out the define below to fall back to the SSBO field (current
// default behaviour). Never defined on Android/Quest (the dense desktop test
// volumes don't fit the Adreno budget; Quest needs a sized sparse pool later).
#if defined(BAKEIT_PLATFORM_DESKTOP)
    //#define BAKEIT_SDF_TEXTURE_ATLAS
#endif

// Sparse tile pool geometry. Keep in sync with the constants in
// SdfBrickFS.frag and SdfBrushTexCS.comp.
//   TILES_PER_AXIS^3 tiles, each stored as 9^3 texels (8 cells + 1 pad).
//   32^3 = 32768 tiles -> 288^3 R16F pool ~= 48 MB. This is the cap on the
//   number of simultaneously-surface blocks (vs the SSBO atlas's ~184k).
#define BAKEIT_SDF_TILE_TILES_PER_AXIS 32
