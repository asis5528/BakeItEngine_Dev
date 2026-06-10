#pragma once
#include "Core/Vulkan/VulkanBase.h"
#include "World/Object.h"
#include "Render/PostProcess/Quad.h"
#include "World/Material.h"
#include "ScenePipelines.h"
#include "Render/PostProcess/PostProcessingManager.h"
#include "Core/Resources/PGEResourceTypes.h"
#include "Platform/Desktop/Input.h"
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
	glm::vec2 viewport_resolution;
	Node rootNode;
	std::vector<Framebuffer*> framebuffers;
	VkDescriptorPool descriptorPool;
	VulkanTexture computeTexture;
	VkSampler standardSampler;
	PostProcessingManager* postprocessingManager;
	VulkanBase* base;
	ScenePipelines *scenePipelines;
	 glm::mat4 proj[2];
	 glm::mat4 view[2];
	 float camFovDegrees = 45.0f;
	 float camNearPlane = 0.001f;
	float camFarPlane = 100.0f;
	glm::vec3 camPos;
	float offset = 2.;
	bool dispatch = false;
	const Input* input = nullptr; // routed input, owned by Platform (wired in main.cpp)
	 glm::vec4 brushData = glm::vec4(0.0f, 0.0f, 0.2f, 0.01f);
	 glm::vec4 brushColor = glm::vec4(1.0f, 0.35f, 0.75f, 1.0f);
	 bool brushUseSphere = false;
	 int brushShapeMode = 0; // 0 cube, 1 sphere, 2 plane
	 float brushRadius = 0.01f;
	 float brushSmoothness = 0.001f;
	 float brushZAxis = 0.2f;
	 float brushAction = 1.0f; // 1.0 add (LMB), 0.0 remove (RMB)
	 bool emissiveMode = false; // toggled with I
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

