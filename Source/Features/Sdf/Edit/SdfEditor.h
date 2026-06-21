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

	// Coarse LOD pyramid in a SINGLE shared pool. The brush stamps each level (N=2,4)
	// directly at its coarse resolution (SdfBrushTexCS on a coarser grid) into ONE
	// shared sparse tile pool + ONE concatenated block table/colors: level i occupies
	// table region [tableBase_i, tableBase_i + (64/N_i)^3). A single allocation counter
	// hands out absolute tile indices, so levels never collide and the 32-tiles/axis
	// pool addressing is unchanged. The render always reads the shared pool (binding 3)
	// + table (binding 9) - no per-level re-point; the per-instance level picks its
	// tableBase in the VS/FS. Distance bands (Scene): fine -> 2 -> 4 with two thresholds.
	struct CoarseLevel {
		int N = 0;                   // coarse block = N^3 fine blocks
		int blocksPerAxis = 0;       // chunkBlocksPerAxis / N
		int tableBase = 0;           // start of this level's region in the shared table/colors
		ComputePipeline* brushComp = nullptr; // bound to the shared pool + shared SBOs
		std::vector<UBO> ubo;        // carries tableBase + coarse grid + field scale
	};
	static const int kCoarseLevels = 2; // N = 2, 4
	CoarseLevel coarseLevels[kCoarseLevels];

	// Shared coarse storage (one pool + one SBO set for ALL levels). table[1]/colors[3]
	// span the concatenation of every level's region; counter[2] is the single tile
	// allocator. Same 7-SBO layout as the fine compute.
	VulkanTexture coarseSharedPool{};
	int coarseSharedPoolIndex = -1;
	int coarseSharedPoolDim = 0;
	std::vector<SBO> coarseSharedSBO; // [0] dummy bricks, [1] table, [2] counter,
	                                  // [3] colors, [4] dirty, [5] free count, [6] free stack
	int coarseTableTotal = 0;         // total table entries across all level regions

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
	// Distance-LOD split distance + level baked into the last draw-list build; a
	// change in either (without the camera moving) must force a rebuild too, since
	// the coarse flag depends on the coarse-parent grouping (which depends on N).
	float lastCoarseSplitDist = -1.0f;
	float lastCoarseSplitDist2 = -1.0f;
	int lastCoarseLodLevelForList = -1;

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
