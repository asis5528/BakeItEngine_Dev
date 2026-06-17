#pragma once
#include "World/Scene.h"
#include "Core/Pipelines/ComputePipeline.h"
#include "Features/Sdf/SdfBackend.h"
#include "Features/Sdf/SdfVolume.h"
#include "Features/Sdf/Edit/BrushState.h"
#include "Features/Sdf/Edit/BrushInputSource.h"
#include "Features/Sdf/Edit/SdfDrawList.h"
#include "EngineContext.h"
#include <glm/glm.hpp>
#include <vector>

// SdfEditor - the volumetric brush editor.
//
// The brick atlas holds the one persistent scene SDF. Every brush stamp is a
// complete boolean update (smooth union / subtraction of the analytic brush
// SDF with the stored field), applied by SdfBrushCS over only the blocks the
// brush bounds touch. Stamps are final: there is no settle/resolve pass.
// Block storage classes: surface blocks own a brick; fully-interior and
// empty blocks store a sentinel in blocks[] and no brick.
// Visibility is fully GPU-driven: SdfDrawList compacts active blocks into
// the indirect draw list (sorted front-to-back) right behind each stamp.
class SdfEditor
{
public:
	SdfVolume volume;
	BrushState brush;

	// Brush ray + intents; platform-owned (desktop: DesktopBrushInput,
	// quest: OpenXR controller actions). Wired in main.cpp; update() is a
	// no-op for brushing while unset.
	BrushInputSource* brushInput = nullptr;

	std::vector<UBO> computeUBO;
	std::vector<SBO> computeSBO;
	ComputePipeline* comp;
	SdfDrawList* drawList = nullptr;
	VulkanBase* base;
	Scene* scene;

	// Coarse LOD atlas - the brush stamps it DIRECTLY at coarse resolution (the
	// same SdfBrushCS run on a coarser grid), so it is a clean native low-res
	// SDF, not a downsampled (aliased) copy of the fine field. coarseSBO mirrors
	// the fine atlas buffers (bricks, block table, counter, colors, dirty, free
	// count, free stack) sized for the coarse grid. The render reads [0] bricks
	// and [1] block table.
	ComputePipeline* coarseBrushComp = nullptr;
	std::vector<UBO> coarseUBO;
	std::vector<SBO> coarseSBO;
	int coarseN = 2;                 // coarse block = N^3 fine blocks (dense-tex test level)
	int coarseBlocksPerAxis = 32;    // chunkBlocksPerAxis / coarseN

	// Dense coarse field as a 3D texture (TEST path): the coarse compute writes
	// it via imageStore, the render samples it as binding-2 sampler3D (hardware
	// trilinear). coarseAtlasDim = coarseBlocksPerAxis*8 + 1 (boundary sample).
	// coarseAtlasTextureIndex is the scene->ptextures slot wired to object 0's
	// material so the brick FS's texSampler resolves to this image.
	VulkanTexture coarseAtlasTexture{};
	int coarseAtlasTextureIndex = -1;
	int coarseAtlasDim = 257; // coarseBlocksPerAxis*8 + 1 (set at construction)

	// Fine dense field as a 3D texture (SSBO-vs-texture perf TEST, desktop only).
	// SdfFineToTexCS converts the sparse SSBO brick atlas into this dense 513^3
	// R16F volume; binding 2 (object 0's material) is wired here so the brick FS
	// can flip its FINE fetch between the SSBO and this texture (ubo2.color.w via
	// Scene::fineTexFetch). The convert runs on idle frames while the toggle is
	// on and the field is dirty (a stamp ran / scene cleared / toggle just on).
	VulkanTexture fineAtlasTexture{};
	int fineAtlasTextureIndex = -1;
	int fineAtlasDim = 513;          // chunkBlocksPerAxis*8 + 1
	ComputePipeline* fineToTexComp = nullptr;
	bool fineTexDirty = true;        // field changed since the last convert

	// Texture-native field (BAKEIT_SDF_TEXTURE_ATLAS): the SPARSE tile pool. The
	// brush imageStores each surface block's 9^3 into its own tile (tile index =
	// the block's allocated brick base); the render samples it at binding 2.
	VulkanTexture tilePoolTexture{};
	int tilePoolTextureIndex = -1;
	int tilePoolDim = 0;             // kTilesPerAxis * 9
	// Accumulated dirty block region (union of brush stamps since the last
	// convert), so the rebuild only touches the edited footprint, not the whole
	// 64^3 grid. min > max means empty. Reset after each convert.
	glm::ivec3 fineTexDirtyMin = glm::ivec3(1000000);
	glm::ivec3 fineTexDirtyMax = glm::ivec3(-1000000);

	glm::vec3 chunkTranslationWS = glm::vec3(0.0f);
	glm::vec3 uploadedChunkTranslationWS = glm::vec3(1e9f);
	uint32_t chunkBlocksPerAxis = 64u;
	uint32_t chunkBlockDim = 8u;
	float chunkBlockSizeWS = 1.0f / 64.0f;

	// Allocatable (stable) free-stack entries for the next stamp: the kernel
	// may only pop entries that existed before its dispatch (same-dispatch
	// reuse races); update() compacts the stack and refreshes this snapshot.
	int freeStackStableCount = 0;

	// Camera matrix of the last draw-list build: the list is rebuilt only
	// when this changes (or a stamp ran), so idle frames skip the build
	// submit and the HUD readback entirely.
	glm::mat4 lastDrawListBlockToClip = glm::mat4(0.0f);

	// Stroke gesture state. A stroke lasts as long as the button is held;
	// dispatches happen when the cursor has moved far enough since the last
	// executed dispatch (deltas accumulate across skipped frames).
	bool wasBrushPointerDown = false;
	bool strokeNeedsInitialDispatch = false;
	glm::dvec2 previousBrushCursorPos = glm::dvec2(0.0);

	SdfEditor(EngineContext& ctx);
	~SdfEditor();
	void update(float time);
	void uploadChunkSpatialData();
	void clearScene();
	// Reset the dense coarse 3D-texture field to +FAR (init and on clear-scene).
	void clearCoarseAtlas();
	// Reset the dense fine 3D-texture field to +FAR (init and on clear-scene).
	void clearFineAtlas();
	// Rebuild the dense fine 3D texture from the SSBO atlas: clears to +FAR then
	// runs SdfFineToTexCS (surface/interior writes), in one non-blocking submit.
	void dispatchFineToTex();
	Mesh* findEditableMesh();
	// The object owning the editable mesh; its ModelMatrix carries the
	// pottery-wheel rotation. Brush math runs in OBJECT space (so a held
	// brush carves a ring on a spinning pot) and the draw-list cull/sort
	// matrices include the model transform.
	Object* findEditableObject();
};
