#pragma once
#include <vector>
#include "Core/Vulkan/VulkanBase.h"
#include "Core/Vulkan/Framebuffer.h"
#include "Render/ScenePipelines.h"
#include "Render/PostProcess/PostProcessingManager.h"

class Scene;

// Owns the render resources and frame recording: offscreen framebuffer,
// object pipelines, the post-process chain, the scene descriptor pool and
// the command-buffer re-record (renderPassDirty). Scene keeps the world
// data and forwards its legacy render methods here until EngineContext
// replaces the Scene bus.
class Renderer
{
public:
	VulkanBase* base;
	std::vector<Framebuffer*> framebuffers;   // [0] = offscreen scene pass
	ScenePipelines* scenePipelines = nullptr;
	std::vector<GraphicsPipeline*> pipelines; // mirror of scenePipelines->pipelines
	PostProcessingManager* postprocessingManager = nullptr;
	VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
	VkSampler standardSampler = VK_NULL_HANDLE;
	bool renderPassDirty = true; // set by features after instance/mesh changes

	// Descriptor-set headroom baked into the scene pool so objects spawned
	// mid-session (SdfEditor::spawnChunk) allocate from the LIVE pool via
	// createObjectDescriptor - no pool destroy, no device-idle wait, safe
	// during a brush drag. Sized for SdfEditor::kMaxChunks plus slack.
	static const int kSpawnDescriptorHeadroom = 40;

	Renderer(VulkanBase* base);
	void init(Scene* scene);
	void recordCommandBuffers(Scene& scene); // re-records all command buffers
	void createSceneDescriptor(Scene& scene);
	// Allocate + write the descriptor set(s) of ONE object from the existing
	// scene pool (uses the headroom above). For objects added after
	// createSceneDescriptor ran; pair with renderPassDirty = true.
	void createObjectDescriptor(Scene& scene, int objectIndex);
	void cleanDescriptors();
	void update();             // per-frame post-process update
	void recreate(Scene& scene); // viewport resize path
	void clean();              // render-resource shutdown
};
