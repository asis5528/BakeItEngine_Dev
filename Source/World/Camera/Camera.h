#pragma once
#include <glm/glm.hpp>
#include "Platform/Desktop/Input.h"

// World camera. Multiview: index 0 = mono/left eye, index 1 = right eye (VR).
// Owned by the engine root (main.cpp); Scene and features reach it via
// Scene::camera until EngineContext replaces the Scene bus.
class Camera
{
public:
	glm::mat4 proj[2];
	glm::mat4 view[2];
	// Optional combined-eye culling override (VR): a single frustum that
	// contains both eye frustums, fed to GPU frustum culling (SdfEditor's
	// draw-list build). Left zeroed on desktop - culling then uses
	// proj[0]*view[0]. The VR platform layer refreshes it every frame.
	glm::mat4 cullProj = glm::mat4(0.0f);
	glm::mat4 cullView = glm::mat4(0.0f);
	glm::vec3 position = glm::vec3(0.0f);
	float fovDegrees = 45.0f;
	float nearPlane = 0.001f;
	float farPlane = 100.0f;
	glm::vec2 viewportResolution = glm::vec2(512.0f);

	// Non-VR free-fly update: rebuilds proj[0] from the viewport aspect and
	// drives view[0]/position with WASD + mouse look (while cameraMoveMode)
	// and arrow-key roll. Window size is used to normalize the mouse position.
	void update(const Input& in, float dt, float windowWidth, float windowHeight);

private:
	// free-fly state
	glm::vec3 m_pos = glm::vec3(0.0f);
	float m_yaw = -90.0f;
	float m_pitch = 0.0f;
	float m_roll = 0.0f;
	// previous normalized mouse position for look deltas
	float m_msxOld = 0.0f;
	float m_msyOld = 0.0f;
	// per-axis movement accumulators (reset when the key releases)
	float m_forwardKey = 0.0f;
	float m_backwardKey = 0.0f;
	float m_rightKey = 0.0f;
	float m_leftKey = 0.0f;
};
