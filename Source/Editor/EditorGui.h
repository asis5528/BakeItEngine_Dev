#pragma once
#include "World/Scene.h"
#include "Features/Sdf/SdfVolume.h"
#include "Features/Sdf/Edit/BrushState.h"
#include "Platform/Desktop/QuestDeploy.h"
#include "EngineContext.h"
class Renderer;
class SdfEditor;
class EditorGui
{
public:
	QuestDeploy questDeploy; // one-click APK build + install ("Quest" panel)
	Scene* m_scene;
	VulkanBase* m_base;
	Renderer* renderer = nullptr;         // wired in main.cpp
	const SdfVolume* sdfVolume = nullptr; // wired in main.cpp once SdfEditor exists
	BrushState* brush = nullptr;          // wired in main.cpp once SdfEditor exists
	SdfEditor* sdfEditor = nullptr;       // wired in main.cpp: chunks panel (move/weld)
	int frameID = 0;
	std::vector<VkDescriptorSet> ds;
	VkSampler texturesamp;
	EditorGui(EngineContext& ctx);
	void createdescriptor(VulkanTexture& tex);
	// Re-point the viewport image descriptor (ds[0]) at the current
	// post-process output. Reuses the existing set - allocating a fresh one
	// per refresh (ImGui_ImplVulkan_AddTexture) leaks it: this backend has no
	// RemoveTexture and the pool eventually runs dry. Call with the device
	// idle (resize / swapchain recreate paths already are).
	void refreshViewportTexture();
	void run();

	void tree(Node& node);
	void setupGui();
	void setupStyle();
};

