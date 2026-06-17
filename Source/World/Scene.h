#pragma once
#include "Core/Vulkan/VulkanBase.h"
#include "World/Object.h"
#include "World/Material.h"
#include "Core/Resources/ResourceTypes.h"
#include "Platform/Desktop/Input.h"
#include "World/Camera/Camera.h"
#include <array>

class Renderer;
struct BrushState;
struct SdfVolume;

// World state: what exists in the scene. Render resources live in
// Render/Renderer; the render methods below are legacy forwarders kept
// until EngineContext replaces the Scene bus.
class Scene
{

public:
	std::vector<GraphicsPipeline*> pipelines; // mirror of Renderer::pipelines (set by Renderer::init)
	std::vector<ResourceTypes::Texture> ptextures;

	std::vector<std::vector<SBO> >SBOs;
	std::vector<Material> materials;
	std::vector<Mesh> meshes;
	std::vector<Animation> animations;
	std::vector<Object> objects;
	int selectedID;
	Node rootNode;
	VulkanTexture computeTexture;
	VulkanBase* base;
	bool dispatch = false;
	const Input* input = nullptr; // routed input, owned by Platform (wired in main.cpp)
	Camera* camera = nullptr;     // world camera, owned by the engine root (wired in main.cpp)
	Renderer* renderer = nullptr; // render resources + recording (wired in main.cpp)
	const BrushState* brush = nullptr; // brush mirror for the indicator overlay, owned by SdfEditor (wired in main.cpp)
	// Block-wireframe debug view (brick cage edges in SdfBrickFS). Desktop:
	// EditorGui checkbox; Quest: held left grip.
	bool showBlockWireframe = false;
	// Deferred-shading test (desktop A/B toggle): when on, SdfBrickFS writes a
	// thin G-buffer (albedo + hit grid position) and the post pass samples the
	// SDF field for the normal + lighting. Default OFF - it turned out slower
	// because the post pass cold-misses the 103MB atlas (the brick shader's
	// atlas access is cache-coherent; the post pass's is not). Forced off on
	// Quest (no G-buffer attachments).
	bool deferredLighting = false;
	// Coarse-LOD test (desktop): 0 = off (fine), else N where a coarse block =
	// N x N x N fine blocks (2/4/8). Renders one big cube per active coarse block
	// marching the full-res field. Step 1 of the LOD system; chosen in the GUI.
	int coarseLodLevel = 0;
	// Fine SSBO-vs-3D-texture fetch test (desktop): when on, SdfBrickFS samples
	// the dense 513^3 fine texture (hardware trilinear) instead of the SSBO brick
	// atlas. Same field, so flipping it isolates the fetch cost. ubo2.color.w.
	bool fineTexFetch = false;
	// Sculptable-bounds cage (blue box marking the block region). The Quest build
	// always shows it; on desktop it's this toggle (default on for parity). The
	// post pass draws it from sdfVolume->boundsMinWS/MaxWS when set.
	bool showChunkBounds = true;
	// SDF volume mirror (stats for the on-device HUD digits, world bounds for
	// the boundary cage); owned by SdfEditor, wired by the Quest entry point.
	// Desktop shows stats in ImGui instead and leaves it null.
	const SdfVolume* sdfVolume = nullptr;
	float fps = 0.0;
	Scene(VulkanBase *vbase);
	void init();
	void update(float time);
	void renderPass();
	void createSceneDescriptor();
	void cleanUniforms();
	void cleanDescriptors();
	void cleanTextures();
	void cleanMeshes();
	void cleanQuads();
	void clean();
	void recreate();


};
