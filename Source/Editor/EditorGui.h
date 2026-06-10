#pragma once
#include "World/Scene.h"
#include "Features/Volumetric/SdfVolume.h"
#include "Features/Volumetric/Edit/BrushState.h"
class Renderer;
class EditorGui
{
public:
	Scene* m_scene;
	VulkanBase* m_base;
	Renderer* renderer = nullptr;         // wired in main.cpp
	const SdfVolume* sdfVolume = nullptr; // wired in main.cpp once Pottery exists
	BrushState* brush = nullptr;          // wired in main.cpp once Pottery exists
	int frameID = 0;
	std::vector<VkDescriptorSet> ds;
	VkSampler texturesamp;
	EditorGui(Scene *scene, VulkanBase* base);
	void createdescriptor(VulkanTexture& tex);
	void run();

	void tree(Node& node);
	void setupGui();
	void setupStyle();
};

