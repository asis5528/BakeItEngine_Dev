#pragma once
#include "World/Scene.h"
#include "Core/Pipelines/ComputePipeline.h"
#include "Features/Sdf/SdfBackend.h"
#include "Features/Sdf/SdfChunk.h"
#include "Features/Sdf/SdfVolume.h"
#include "Features/Sdf/Edit/BrushState.h"
#include "Features/Sdf/Edit/BrushInputSource.h"
#include "Features/Sdf/Edit/SdfDrawList.h"
#include "EngineContext.h"
#include <glm/glm.hpp>
#include <string>
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

	VulkanBase* base;
	Scene* scene;

	// Chunks: each a self-contained block grid + field tables + draw list
	// (SdfChunk.h), all sharing the one tile pool. chunks[0] is the primary
	// editable chunk today; stroke targeting / spawn-anywhere arrive with the
	// multi-chunk stages. Reserved to kMaxChunks so growth never reallocates
	// (references to a chunk stay valid across a stroke).
	static const int kMaxChunks = 32;
	std::vector<SdfChunk> chunks;

	// Shared fine-tile allocator - ONE for all chunks, like the coarse pyramid's
	// shared counter. A block's allocated base indexes the one tile pool, so a
	// block can migrate between chunks by table surgery alone (split/weld).
	// [0] tail counter + free-stack pop cursor, [1] free stack count,
	// [2] free stack. The brush compute binds these; the per-chunk sbo[2]/[6]/[7]
	// slots are retained only as descriptor filler (sbo[2] = graphics binding 6).
	std::vector<SBO> tileAllocSBO;

	// Allocatable (stable) free-stack entries for the next stamp: the kernel
	// may only pop entries that existed before its dispatch (same-dispatch
	// reuse races); update() compacts the stack and refreshes this snapshot.
	// Allocator-wide (shared), not per-chunk.
	int freeStackStableCount = 0;

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

	// Draw-anywhere, step 1: the world is a LAZY CHUNK-GRID CANVAS. Space is
	// tiled by 1-unit cells; drawing into empty air spawns the cell's chunk
	// GRID-ALIGNED (never brush-centered), and every stamp dispatches into
	// all cells its footprint overlaps - the field stays seamless across
	// cell boundaries by construction (both sides evaluate the same analytic
	// brush). Known gap until step 2 (chunkGrid resolve in the render
	// shaders): the raymarcher can't sample across a cell seam, so strokes
	// straddling a boundary can show a shading crease along the seam plane.
	bool airDrawSpawnEnabled = true;
	// The chunk owning grid cell `cell` (active, unmoved, not physical), or
	// -1. ensureCellChunk spawns (or recycles) the cell's chunk on demand.
	int chunkForCell(const glm::ivec3& cell);
	int ensureCellChunk(const glm::ivec3& cell);
	// Is c part of the grid canvas (grid-aligned, identity model, not
	// physical)? Strokes targeting a NON-canvas chunk (a fallen piece, a
	// moved chunk) stamp ONLY that object - spilling into the overlapping
	// grid cells would write an invisible coincident duplicate.
	bool isCanvasChunk(const SdfChunk& c);
	// Chunks stamped by the current stroke: ALL of them get the release
	// connectivity pass (a stroke may span several canvas cells).
	std::vector<int> strokeTouchedChunks;

	// Stroke targeting: which chunk the current/last stroke edits (picked by
	// ray at stroke start), and which chunk submitted the last stamp (its
	// fence must be signaled before the shared free-stack is compacted -
	// same-dispatch reuse race, now across chunks).
	int editableChunkIndex = 0;
	int lastStampChunkIndex = -1;
	// Brush depth mode for the CURRENT stroke. Normal strokes measure
	// BrushDistanceFromCamera from the chunk's entry face (feels right when
	// pointing at a big chunk from outside). The stroke that SPAWNED its
	// chunk measures it from the camera instead: the chunk is centered at
	// camera + D, so entry-relative depth would overshoot past the far
	// boundary whenever D > half a chunk - the "spheres cut in half at the
	// bounds" artifact.
	bool strokeAbsoluteBrushDepth = false;
	// Spawn-stroke chunk chaining: previous frame's brush point (world), for
	// the stroke direction that places the next chained chunk ahead.
	glm::vec3 lastSpawnStrokeBrushWS = glm::vec3(0.0f);
	// Does pWS (world) lie inside the chunk's bounds, shrunk by margin?
	bool chunkContainsPoint(const SdfChunk& c, const glm::vec3& pWS, float margin);
	// Nearest chunk whose bounds the world-space ray hits (each tested in its
	// own object space); -1 when the ray misses everything (spawn cue).
	int pickChunkIndexByRay(const glm::vec3& rayOriginWS, const glm::vec3& rayDirWS);
	// First material-block hit along the world ray in chunk c (3D-DDA over the
	// chunk's cached CPU block table); ray-t of the hit, or < 0 on miss.
	float raycastChunkMaterial(SdfChunk& c, const glm::vec3& rayOriginWS, const glm::vec3& rayDirWS);
	// Lazily refresh chunk c's CPU block-table mirror (only when a writer
	// flagged it AND the chunk's stamps are idle). False = no usable mirror.
	bool refreshChunkCpuTable(SdfChunk& c);
	// Wake every physical chunk (edits/splits/welds may have removed a
	// support: pieces re-settle, and re-sleep in one frame if still held up).
	void wakePhysicalChunks();

	// --- Blender-style brush placement (GUI-tunable).
	// Surface stick: while hovering, the brush rides ON the material surface
	// under the cursor (nearest hit across all chunks) instead of at a manual
	// camera distance; the depth locks for the length of a stroke (classic
	// sculpt) so carving toward the camera can't drag the brush along. W/S
	// then nudges surfaceBias (push into / pull off the surface); the manual
	// camera-distance depth remains the fallback when aiming at empty air.
	bool surfaceStickEnabled = true;
	float surfaceBias = 0.0f;
	// R-rotate operator (Blender style: R -> X/Y/Z -> typed degrees -> Enter
	// applies, Esc cancels). While active, strokes are swallowed (typing must
	// not paint). brushShapeRot accumulates the applied rotations; the GUI
	// shows it and can reset it.
	bool rotateModalActive = false;
	int rotateModalAxis = 1;                    // 0/1/2 = world X/Y/Z
	std::string rotateModalNumber;              // typed degrees (overrides the mouse)
	float rotateModalMouseDeg = 0.0f;           // interactive angle from cursor motion
	glm::mat3 brushShapeRot = glm::mat3(1.0f);  // accumulated shape rotation

	// --- Object states & physics (BAKEIT_PHYSICS_RULES.md - the reference).
	// Sketch chunks hang where drawn; the "P" commit promotes them to
	// physical (one-way). Linear-only integration in updateChunkPhysics(),
	// run at the top of update() every frame. Chunks are ~1 world unit across.
	float physicsGravity = 9.8f;      // downward accel, world units / s^2 (1 unit ~ 1 m: earth gravity)
	float physicsGroundY = -0.5f;     // ground plane height (chunk 0's bottom face)
	float physicsRestitution = 0.2f;  // bounce energy kept on ground impact
	float lastPhysicsTime = -1.0f;    // previous update() time, for dt
	// "P" commit: promote every sketch chunk to physical, immediately.
	// Already-physical chunks are untouched (drains the unassigned queue).
	void commitPhysics();
	// Retire a spawned chunk's slot (never chunk 0): recycle its tiles into
	// the shared free stack, empty its table, stop its draws, mark inactive.
	// spawnChunk() reuses retired slots - the chunk vector is a fixed budget
	// (kMaxChunks, reserved so growth never reallocates), and without reuse a
	// long session of cuts exhausts it ("chunk cap reached").
	void retireChunkSlot(int chunkIndex);
	// Promote one chunk: measure its content AABB (block-table readback,
	// release-path - a discrete user action) and hand it to the integrator.
	void makeChunkPhysical(int chunkIndex);
	void updateChunkPhysics(float time);
	// The connectivity rule (rules doc §1): a chunk = ONE connected piece of
	// material. Runs on every stroke release: flood-fills the edited chunk's
	// block table; one island stays, every other island moves into a fresh
	// chunk by table surgery. strokeWasCut says whether the stroke erased:
	// only then do split children inherit the parent's physical state (a cut
	// piece falls instantly) - islands created by DRAWING are always born
	// sketch and wait for the next "P", and the island keeping the parent's
	// chunk is the one overlapping the pre-stroke content (so a freshly drawn
	// blob can never steal a physical object's state). Also refreshes content
	// bounds and wakes a physical chunk so it re-settles on its new shape.
	void splitChunkIslands(int chunkIndex, bool strokeWasCut);
	// Did the current stroke erase at any point? Sampled per frame while the
	// pointer is down; consumed by the release split (see strokeWasCut).
	bool strokeHadErase = false;
	// Occupied-content AABB scan (block-table readback -> contentMin/MaxOS).
	// False when the chunk holds no content.
	bool computeChunkContentBounds(SdfChunk& c);

	// Stroke gesture state. A stroke lasts as long as the button is held;
	// dispatches happen when the cursor has moved far enough since the last
	// executed dispatch (deltas accumulate across skipped frames).
	bool wasBrushPointerDown = false;
	bool strokeNeedsInitialDispatch = false;
	glm::dvec2 previousBrushCursorPos = glm::dvec2(0.0);

	SdfEditor(EngineContext& ctx);
	~SdfEditor();
	void update(float time);
	// Spawn a new chunk at the given translation (object space): per-chunk
	// tables + brush pipeline, a cloned proxy-cube mesh/object on the brick
	// pipeline, and a draw list. debugStamp adds a centered sphere so the
	// chunk is visible without a stroke. rebuildScene = true re-enters the
	// new object into the live scene (descriptor rebuild + command-buffer
	// re-record, the model-drop sequence) - pass false only during init,
	// before Scene::createSceneDescriptor runs. Needs the tile-pool material
	// wired first (the cloned proxy inherits its matID).
	SdfChunk& spawnChunk(const glm::vec3& translationWS, bool debugStamp, bool rebuildScene);
	// Per-chunk GPU state: the 8-slot field-table SBO set (empty tables) and
	// the chunk's brush ComputePipeline/UBO bound to the shared pool+allocator.
	void createChunkResources(SdfChunk& chunk);
	// Weld (smooth-union) chunk src's field into chunk dst, then absorb src:
	// its tiles return to the shared pool and its tables empty. Synchronous
	// (a discrete user action on the release path, never the drag path).
	// The blend width is the current brush smoothness. Parts of src outside
	// dst's domain are lost - v1 assumes src was moved inside/onto dst.
	void weldChunkInto(int srcIndex, int dstIndex);
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
