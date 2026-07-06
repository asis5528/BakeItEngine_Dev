#pragma once
#include "Platform/Platform.h"
#ifdef BAKEIT_PLATFORM_DESKTOP
#ifndef GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>
#endif
#include <glm/glm.hpp>
#include <string>

// Engine-side input state.
//
// GLFW provides exactly one callback per event type per window, so ONLY
// DesktopInput registers input callbacks (key/mouse-button/scroll), and it
// must be installed BEFORE ImGui_ImplGlfw init so ImGui chain-calls these.
// Never call glfwSetKeyCallback / glfwSetMouseButtonCallback /
// glfwSetScrollCallback anywhere else.
//
// Consumers read the Input struct (wired as Scene::input for now; moves into
// EngineContext when Scene stops being the shared-state bus).
struct Input
{
	// mouse
	bool leftButtonDown = false;
	bool rightButtonDown = false;
	bool middleButtonDown = false;
	bool anyButtonPressed = false;
	glm::dvec2 mousePos = glm::dvec2(0.0);   // window pixels
	glm::dvec2 mouseDelta = glm::dvec2(0.0); // window pixels, this frame
	double scrollDelta = 0.0;                // this frame
	// modifiers
	bool shiftDown = false;
	bool altDown = false;
	bool ctrlDown = false;
	// held keys
	bool wDown = false, aDown = false, sDown = false, dDown = false;
	bool qDown = false, eDown = false;
	// Left/right arrow steps this frame (right = +1, left = -1, OS key-repeat
	// included): the R-rotate modal's angle stepper.
	int arrowSteps = 0;
	// pressed-this-frame edges
	bool uPressed = false;
	bool iPressed = false;
	bool pPressed = false;
	bool rPressed = false;
	bool enterPressed = false;
	bool escPressed = false;
	// Characters typed this frame for modal numeric entry (the R-rotate
	// operator): digits, '.', '-', and the axis letters x/y/z.
	std::string typedThisFrame;
	// interaction mode, toggled by middle mouse press
	bool cameraMoveMode = false;
};

#ifdef BAKEIT_PLATFORM_DESKTOP
class DesktopInput
{
public:
	Input state;

	// Register GLFW callbacks. Call once after window creation, before
	// ImGui_ImplGlfw init.
	void install(GLFWwindow* window);

	// Publish per-frame state (mouse delta, edge flags, scroll). Call after
	// glfwPollEvents, before any consumer runs.
	void update();

private:
	GLFWwindow* m_window = nullptr;
	double m_pendingScroll = 0.0;
	bool m_pendingU = false;
	bool m_pendingI = false;
	bool m_pendingP = false;
	bool m_pendingR = false;
	bool m_pendingEnter = false;
	bool m_pendingEsc = false;
	int m_pendingArrowSteps = 0;
	std::string m_pendingTyped;
	bool m_hasPrevMousePos = false;
	glm::dvec2 m_prevMousePos = glm::dvec2(0.0);

	static void keyCallback(GLFWwindow* win, int key, int scancode, int action, int mods);
	static void mouseButtonCallback(GLFWwindow* win, int button, int action, int mods);
	static void scrollCallback(GLFWwindow* win, double xoffset, double yoffset);
};
#endif // BAKEIT_PLATFORM_DESKTOP
