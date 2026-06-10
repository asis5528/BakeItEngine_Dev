#pragma once
#include "Core/Vulkan/VulkanBase.h"
#include "World/Object.h"
#include "Render/PostProcess/Quad.h"
#include "World/Material.h"
#include "ScenePipelines.h"
#include "Render/PostProcess/PostProcessingManager.h"
#include "Core/Resources/PGEResourceTypes.h"
#include "Platform/Desktop/Input.h"
#include "World/Camera/Camera.h"
#include <array>


class Scene
{
	
public:
	std::vector<GraphicsPipeline*> pipelines;
	//std::vector<VulkanTexture> textures;
	std::vector<PGEResourceTypes::Texture> ptextures;
	
	std::vector<std::vector<SBO> >SBOs;
	std::vector<Material> materials;
	std::vector<Mesh> meshes;
	std::vector<Animation> animations;
	std::vector<Object> objects;
	int selectedID;
	std::vector<Quad*> quads;
	Node rootNode;
	std::vector<Framebuffer*> framebuffers;
	VkDescriptorPool descriptorPool;
	VulkanTexture computeTexture;
	VkSampler standardSampler;
	PostProcessingManager* postprocessingManager;
	VulkanBase* base;
	ScenePipelines *scenePipelines;
	bool dispatch = false;
	const Input* input = nullptr; // routed input, owned by Platform (wired in main.cpp)
	Camera* camera = nullptr;     // world camera, owned by the engine root (wired in main.cpp)
	 bool renderPassDirty = true;
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

