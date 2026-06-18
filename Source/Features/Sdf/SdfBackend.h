#pragma once
#include "Platform/Platform.h"

// SDF field backend switch -----------------------------------------------------
//
// When BAKEIT_SDF_TEXTURE_ATLAS is defined, the fine SDF field is stored in a
// SPARSE 3D-texture tile pool instead of the bricks[] SSBO:
//   - the brush (SdfBrushTexCS) imageStores each block's 9^3 into its own tile,
//   - the render (SdfBrickFS, texMode 2) samples that tile with hardware
//     trilinear,
//   - the bricks[] SSBO field path is unused (a dummy buffer keeps the render
//     binding valid).
// The block table (blocks[]) and the free-list brick allocator are reused as-is
// - a block's allocated "base" is reinterpreted as a tile INDEX. Each tile is
// self-contained (8 cells + 1 +pad), so there is no cross-tile boundary race.
//
// Comment a platform's define below to fall back to the SSBO field there. On
// Quest the tile pool ALSO escapes Adreno's 128 MB maxStorageBufferRange (the
// field is a texture, not an SSBO). It needs shaderStorageImageExtendedFormats
// for the R16F storage image - requested in VulkanDevice when the device
// supports it; if Adreno reports no R16F storage support, fall back to R32F in
// createTexture3DR16F.
#if defined(BAKEIT_PLATFORM_DESKTOP)
    #define BAKEIT_SDF_TEXTURE_ATLAS
#endif
#if defined(BAKEIT_PLATFORM_ANDROID)
    #define BAKEIT_SDF_TEXTURE_ATLAS
#endif

// Sparse tile pool geometry. Keep in sync with the constants in
// SdfBrickFS.frag and SdfBrushTexCS.comp.
//   TILES_PER_AXIS^3 tiles, each stored as 9^3 texels (8 cells + 1 pad).
//   32^3 = 32768 tiles -> 288^3 R16F pool ~= 48 MB. This is the cap on the
//   number of simultaneously-surface blocks (vs the SSBO atlas's ~184k).
#define BAKEIT_SDF_TILE_TILES_PER_AXIS 32
