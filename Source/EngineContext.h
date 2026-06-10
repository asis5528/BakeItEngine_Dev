#pragma once

class VulkanBase;
class Scene;
class Renderer;
class Camera;
struct Input;

// Engine-root wiring handed to subsystems instead of individual
// (VulkanBase*, Scene*) pairs. Owned by main.cpp and filled during init;
// everything referenced here outlives every subsystem. New features take
// EngineContext& in their constructor and pick what they need.
struct EngineContext
{
	VulkanBase* base = nullptr;
	Scene* scene = nullptr;       // world data
	Renderer* renderer = nullptr; // render resources + frame recording
	Camera* camera = nullptr;
	const Input* input = nullptr;
};
