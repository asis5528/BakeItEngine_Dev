#pragma once
#include "World/Scene.h"
#include "Core/Pipelines/ComputePipeline.h"
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
	Mesh* findEditableMesh();
};
