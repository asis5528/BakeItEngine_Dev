#pragma once
#include "../Graphics/VulkanBase.h"
#include "../Object.h"
#include "../quad.h"
#include "../SceneObjects.h"
#include "../Material.h"
#include "ScenePipelines.h"
#include "../PostProcessing/PostProcessingManager.h"
#include "../PGEResourceTypes.h"
#include <array>


class Scene
{
	
public:
	static constexpr int MaxFloorBlobShadows = 64;
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
	 glm::mat4 projNoJitter[2];
	 glm::mat4 view[2];
	 glm::vec2 taaJitter = glm::vec2(0.0f);
	 float camFovDegrees = 45.0f;
	 float camNearPlane = 0.001f;
	float camFarPlane = 100.0f;
	glm::vec3 camPos;
	float offset = 2.;
	bool slowZoom = false;
	bool dispatch = false;
	 bool ButtonPressed = false;
	 bool LeftButtonDown = false;
	 bool MiddleButtonDown = false;
	 bool RightButtonDown = false;
	 bool ShiftDown = false;
	 bool cameraMoveMode = false; // toggled by middle mouse press
	 bool WKeyDown = false;
	 bool SKeyDown = false;
	 bool AKeyDown = false;
	 bool DKeyDown = false;
	 bool LeftArrowDown = false;
	 bool RightArrowDown = false;
	 glm::vec4 brushData = glm::vec4(0.0f, 0.0f, 0.2f, 0.01f);
	 glm::vec4 brushColor = glm::vec4(1.0f, 0.35f, 0.75f, 1.0f);
	 bool brushUseSphere = false;
	 int brushShapeMode = 0; // 0 cube, 1 sphere, 2 plane
	 float brushRadius = 0.01f;
	 float brushZAxis = 0.2f;
	 float brushAction = 1.0f; // 1.0 add (LMB), 0.0 remove (RMB)
	 bool emissiveMode = false; // toggled with I
	 int dirtyBlockCount = 0;
	 int sdfAtlasUsedBricks = 0;
	 int sdfAtlasCapacityBricks = 0;
	 int sdfAtlasTailBricks = 0;
	 int sdfAtlasFreeBricks = 0;
	 float sdfAtlasUsage = 0.0f;
	 std::array<glm::vec4, MaxFloorBlobShadows> floorBlobShadows{};
	 std::array<glm::vec4, MaxFloorBlobShadows> floorBlobBounceColors{};
	 int floorBlobShadowCount = 0;
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

