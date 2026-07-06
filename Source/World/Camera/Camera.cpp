#include "Camera.h"
#include <glm/gtc/matrix_transform.hpp>

void Camera::update(const Input& in, float dt, float windowWidth, float windowHeight)
{
	const float viewportWidth = glm::max(viewportResolution.x, 1.0f);
	const float viewportHeight = glm::max(viewportResolution.y, 1.0f);
	proj[0] = glm::perspective(glm::radians(fovDegrees), viewportWidth / viewportHeight, nearPlane, farPlane);

	float msx = in.mousePos.x / windowWidth;
	float msy = in.mousePos.y / windowHeight;
	float msxdelta = msx - m_msxOld;
	float msydelta = msy - m_msyOld;
	m_msxOld = msx;
	m_msyOld = msy;

	float dx = msxdelta * windowWidth * 4.f;
	float dy = -msydelta * windowHeight * 4.f;
	float cameraStep = in.altDown ? 0.0001f : 0.005f;
	m_forwardKey += in.wDown ? cameraStep : 0.0f;
	m_backwardKey += in.sDown ? cameraStep : 0.0f;
	m_forwardKey *= in.wDown ? 1.0f : 0.0f;
	m_backwardKey *= in.sDown ? 1.0f : 0.0f;
	m_rightKey += in.dDown ? cameraStep : 0.0f;
	m_leftKey += in.aDown ? cameraStep : 0.0f;
	m_rightKey *= in.dDown ? 1.0f : 0.0f;
	m_leftKey *= in.aDown ? 1.0f : 0.0f;

	float moveF = m_forwardKey - m_backwardKey; // W/S
	float moveR = m_rightKey - m_leftKey;       // D/A
	float moveU = 0.0f;
	// Camera roll on the arrow keys is gone: the arrows now step the brush
	// rotation in the R-rotate modal (SdfEditor).
	const float rollInput = 0.0f;

	if (!in.cameraMoveMode) {
		return;
	}

	const float mouseDx = in.cameraMoveMode ? dx : 0.0f;
	const float mouseDy = in.cameraMoveMode ? dy : 0.0f;
	const float mouseSens = 0.1f;
	const float moveSpeed = 5.0f;
	const float rollSpeed = 90.0f;

	// --- mouse look ---
	m_yaw += mouseDx * mouseSens;
	m_pitch += mouseDy * mouseSens;
	m_pitch = glm::clamp(m_pitch, -89.0f, 89.0f);
	m_roll += rollInput * rollSpeed * dt;

	// --- forward vector ---
	glm::vec3 forward;
	forward.x = cos(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
	forward.y = sin(glm::radians(m_pitch));
	forward.z = sin(glm::radians(m_yaw)) * cos(glm::radians(m_pitch));
	forward = glm::normalize(forward);

	// --- basis ---
	glm::vec3 worldUp = glm::vec3(0, 1, 0);
	glm::vec3 right = glm::normalize(glm::cross(forward, worldUp));
	glm::vec3 up = glm::normalize(glm::cross(right, forward));

	if (glm::abs(m_roll) > 0.0001f) {
		glm::mat4 rollRot = glm::rotate(glm::mat4(1.0f), glm::radians(m_roll), forward);
		up = glm::normalize(glm::vec3(rollRot * glm::vec4(up, 0.0f)));
		right = glm::normalize(glm::cross(forward, up));
	}

	// --- movement (camera space) ---
	m_pos += forward * moveF * moveSpeed * dt;
	m_pos += right * moveR * moveSpeed * dt;
	m_pos += up * moveU * moveSpeed * dt;

	// --- view matrix ---
	view[0] = glm::lookAt(m_pos, m_pos + forward, up);
	position = m_pos;
}
