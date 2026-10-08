#include "DesktopBrushInput.h"
#include "Platform/Desktop/Input.h"
#include "World/Camera/Camera.h"
#include "Core/Vulkan/VulkanBase.h"

BrushInput DesktopBrushInput::poll()
{
	BrushInput b;
	const Input& in = *ctx.input;

	b.interactionAllowed = !in.cameraMoveMode && !in.uiWantsMouse;
	b.drawDown = in.leftButtonDown;
	b.eraseDown = in.rightButtonDown;
	b.recolorModifier = in.shiftDown;
	b.snapModifier = in.ctrlDown;
	b.shapeTogglePressed = in.uPressed;
	b.emissiveTogglePressed = in.iPressed;
	b.physicsCommitPressed = in.pPressed;
	b.rotateTogglePressed = in.rPressed;
	b.typedChars = in.typedThisFrame;
	b.confirmPressed = in.enterPressed;
	b.cancelPressed = in.escPressed;
	b.rotateSteps = in.arrowSteps;
	b.fineModifier = in.altDown;

	const float fine = in.altDown ? 0.1f : 1.0f;
	b.radiusDelta = ((in.aDown ? 1.0f : 0.0f) - (in.dDown ? 1.0f : 0.0f)) * fine;
	b.smoothnessDelta = ((in.qDown ? 1.0f : 0.0f) - (in.eDown ? 1.0f : 0.0f)) * fine;
	b.distanceDelta = ((in.wDown ? 1.0f : 0.0f) - (in.sDown ? 1.0f : 0.0f)) * fine;

	b.strokeCursor = in.mousePos;

	// Cursor ray through the camera (same NDC convention as the indicator
	// overlay in PostPassFS).
	const VkExtent2D extent = ctx.base->presentExtent();
	const float msx = float(in.mousePos.x) / float(extent.width);
	const float msy = float(in.mousePos.y) / float(extent.height);
	const glm::mat4 invProj = glm::inverse(ctx.camera->proj[0]);
	const glm::mat4 invView = glm::inverse(ctx.camera->view[0]);
	glm::vec4 rayView = invProj * glm::vec4(msx * 2.0f - 1.0f, 1.0f - msy * 2.0f, -1.0f, 1.0f);
	rayView = glm::vec4(rayView.x, rayView.y, -1.0f, 0.0f);
	b.rayDir = glm::normalize(glm::vec3(invView * rayView));
	b.rayOrigin = ctx.camera->position;

	return b;
}
