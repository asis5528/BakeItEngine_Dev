#pragma once
#include "World/Scene.h"
#include "Features/Sdf/SdfVolume.h"
#include "Features/Sdf/Edit/BrushState.h"
#include "Platform/Desktop/QuestDeploy.h"
#include "EngineContext.h"
class Renderer;
class EditorGui
{
public:
	QuestDeploy questDeploy; // one-click APK build + install ("Quest" panel)
	Scene* m_scene;
	VulkanBase* m_base;
	Renderer* renderer = nullptr;         // wired in main.cpp
	const SdfVolume* sdfVolume = nullptr; // wired in main.cpp once SdfEditor exists
	BrushState* brush = nullptr;          // wired in main.cpp once SdfEditor exists
	int frameID = 0;
	std::vector<VkDescriptorSet> ds;
	VkSampler texturesamp;
	EditorGui(EngineContext& ctx);
	void createdescriptor(VulkanTexture& tex);
	void run();

	void tree(Node& node);
	void setupGui();
	void setupStyle();
};

