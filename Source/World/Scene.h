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
struct SdfVolumeStats;

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
	// Atlas stats mirror for the on-device HUD (post pass digits); owned by
	// SdfEditor, wired by the Quest entry point. Desktop shows these in ImGui
	// instead and leaves it null.
	const SdfVolumeStats* sdfStats = nullptr;
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
