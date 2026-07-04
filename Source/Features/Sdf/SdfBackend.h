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

// Tile-pool texel PRECISION experiment ----------------------------------------
// Define BAKEIT_SDF_TILE_R8 to store the field in R8_SNORM (8-bit fixed point,
// half the bandwidth/footprint of R16F: 288^3 -> ~24 MB) instead of R16_SFLOAT,
// to evaluate whether 8-bit precision is enough for the surface/normals.
//
// There is no 8-bit float in Vulkan; R8_SNORM holds [-1,1]. The field is a
// signed distance in NORMALIZED chunk space whose near-surface magnitude is
// ~<= one block diagonal (~0.03), so the shaders scale it by
// BAKEIT_SDF_TILE_FIELD_SCALE (=1/0.03125 = 32: maps [-1,1] onto +/-1 block
// diagonal, no clamping of real data) on store and unscale on sample. The same
// scale constant MUST be set in SdfBrushTexCS.comp and SdfBrickFS.frag (the
// shaders can't see this header) - search SDF_TILE_R8 there.
//
// Needs shaderStorageImageExtendedFormats (R8_SNORM is a non-mandatory storage
// format) - already requested in VulkanDevice.
//
// 8-bit IS enough with an ASYMMETRIC encoding. The first cut mapped the field
// symmetrically (surface at 0, [-1,1] = +/-1-block-diagonal) and wasted half its
// 256 codes on deep-interior distances the march never samples (it stops at the
// surface; only the normal's inside tap + the zero-crossing cell read <~2 cells
// inside). That left only ~8 codes spanning the one cell the finite-difference
// normal straddles -> visibly bumpy/mottled shading. Fix (Option A): bias the
// surface to stored value -0.6 (20% of the range inside, 80% outside) and tighten
// the scale to ~82, giving ~2.5 cells inside / ~10 outside, ~20 codes/cell
// (~2.5x finer normals) - still R8 / 48 MB. The bias cancels in the gradient, so
// only the scale drives smoothness; the outside reach is a pure march-speed knob
// (a clamped-short distance only shortens the sphere-trace step, never overshoots).
// Constants live in the SDF_TILE_R8 blocks of SdfBrushTexCS.comp + SdfBrickFS.frag
// (TILE_FIELD_BIAS / scale 82) and the coarse scale 82/N in SdfEditor.cpp.
#define BAKEIT_SDF_TILE_R8
