#include "Input.h"
#include <cstdio>

// Single window for now; the Vulkan app owns the GLFW user pointer, so the
// input router uses its own instance pointer instead.
static DesktopInput* g_input = nullptr;

void DesktopInput::install(GLFWwindow* window)
{
	m_window = window;
	g_input = this;
	glfwSetKeyCallback(window, keyCallback);
	glfwSetMouseButtonCallback(window, mouseButtonCallback);
	glfwSetScrollCallback(window, scrollCallback);
}

void DesktopInput::update()
{
	double x = 0.0, y = 0.0;
	glfwGetCursorPos(m_window, &x, &y);
	state.mousePos = glm::dvec2(x, y);
	if (!m_hasPrevMousePos) {
		m_prevMousePos = state.mousePos;
		m_hasPrevMousePos = true;
	}
	state.mouseDelta = state.mousePos - m_prevMousePos;
	m_prevMousePos = state.mousePos;

	state.scrollDelta = m_pendingScroll;
	m_pendingScroll = 0.0;
	state.uPressed = m_pendingU;
	state.iPressed = m_pendingI;
	state.pPressed = m_pendingP;
	state.rPressed = m_pendingR;
	state.enterPressed = m_pendingEnter;
	state.escPressed = m_pendingEsc;
	state.arrowSteps = m_pendingArrowSteps;
	m_pendingArrowSteps = 0;
	state.typedThisFrame = m_pendingTyped;
	m_pendingU = false;
	m_pendingI = false;
	m_pendingP = false;
	m_pendingR = false;
	m_pendingEnter = false;
	m_pendingEsc = false;
	m_pendingTyped.clear();
}

void DesktopInput::keyCallback(GLFWwindow* win, int key, int scancode, int action, int mods)
{
	if (!g_input) return;
	Input& s = g_input->state;
	const bool down = (action != GLFW_RELEASE);

	if (key == GLFW_KEY_LEFT_SHIFT || key == GLFW_KEY_RIGHT_SHIFT) s.shiftDown = down;
	if (key == GLFW_KEY_LEFT_ALT) s.altDown = down;
	if (key == GLFW_KEY_LEFT_CONTROL || key == GLFW_KEY_RIGHT_CONTROL) s.ctrlDown = down;
	if (key == GLFW_KEY_W) s.wDown = down;
	if (key == GLFW_KEY_A) s.aDown = down;
	if (key == GLFW_KEY_S) s.sDown = down;
	if (key == GLFW_KEY_D) s.dDown = down;
	if (key == GLFW_KEY_Q) s.qDown = down;
	if (key == GLFW_KEY_E) s.eDown = down;
	// Arrow steps with OS key-repeat: tap = one step, hold = auto-repeat.
	if (key == GLFW_KEY_LEFT && (action == GLFW_PRESS || action == GLFW_REPEAT)) g_input->m_pendingArrowSteps -= 1;
	if (key == GLFW_KEY_RIGHT && (action == GLFW_PRESS || action == GLFW_REPEAT)) g_input->m_pendingArrowSteps += 1;
	if (key == GLFW_KEY_U && action == GLFW_PRESS) g_input->m_pendingU = true;
	if (key == GLFW_KEY_I && action == GLFW_PRESS) g_input->m_pendingI = true;
	if (key == GLFW_KEY_P && action == GLFW_PRESS) g_input->m_pendingP = true;
	if (key == GLFW_KEY_R && action == GLFW_PRESS) g_input->m_pendingR = true;
	if (action == GLFW_PRESS) {
		// Modal numeric entry (R-rotate): digits, sign, decimal point, axis
		// letters. Consumers ignore the string outside a modal.
		if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) g_input->m_pendingTyped += static_cast<char>('0' + (key - GLFW_KEY_0));
		if (key >= GLFW_KEY_KP_0 && key <= GLFW_KEY_KP_9) g_input->m_pendingTyped += static_cast<char>('0' + (key - GLFW_KEY_KP_0));
		if (key == GLFW_KEY_MINUS || key == GLFW_KEY_KP_SUBTRACT) g_input->m_pendingTyped += '-';
		if (key == GLFW_KEY_PERIOD || key == GLFW_KEY_KP_DECIMAL) g_input->m_pendingTyped += '.';
		if (key == GLFW_KEY_X) g_input->m_pendingTyped += 'x';
		if (key == GLFW_KEY_Y) g_input->m_pendingTyped += 'y';
		if (key == GLFW_KEY_Z) g_input->m_pendingTyped += 'z';
		if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) g_input->m_pendingEnter = true;
		if (key == GLFW_KEY_ESCAPE) g_input->m_pendingEsc = true;
	}
}

void DesktopInput::mouseButtonCallback(GLFWwindow* win, int button, int action, int mods)
{
	if (!g_input) return;
	Input& s = g_input->state;

	s.anyButtonPressed = (action == GLFW_PRESS);
	s.shiftDown = ((mods & GLFW_MOD_SHIFT) != 0);

	if (button == GLFW_MOUSE_BUTTON_LEFT)
		s.leftButtonDown = (action == GLFW_PRESS);
	else if (button == GLFW_MOUSE_BUTTON_RIGHT)
		s.rightButtonDown = (action == GLFW_PRESS);
	else if (button == GLFW_MOUSE_BUTTON_MIDDLE && action == GLFW_PRESS) {
		s.cameraMoveMode = !s.cameraMoveMode;
		s.middleButtonDown = s.cameraMoveMode;
	}
}

void DesktopInput::scrollCallback(GLFWwindow* win, double xoffset, double yoffset)
{
	if (!g_input) return;
	g_input->m_pendingScroll += yoffset;
}
