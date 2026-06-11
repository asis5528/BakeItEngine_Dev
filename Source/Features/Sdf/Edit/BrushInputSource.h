#pragma once
#include <glm/glm.hpp>

// Platform-free brush input contract (see Platform/Platform.h).
//
// SdfEditor consumes the brush as "a ray + intents" so the same edit logic
// runs everywhere; what produces the ray is the platform's business:
//   desktop - cursor ray through the camera (Platform/Desktop/DesktopBrushInput)
//   quest   - controller aim pose + trigger/buttons via OpenXR actions
struct BrushInput
{
	// Pointing ray, world space.
	glm::vec3 rayOrigin = glm::vec3(0.0f);
	glm::vec3 rayDir = glm::vec3(0.0f, 0.0f, -1.0f);

	// False while the user is navigating (desktop: camera move mode) - the
	// brush neither paints nor adjusts parameters then.
	bool interactionAllowed = false;

	// Stroke intents. drawDown adds, eraseDown removes; holding either is a
	// stroke gesture. recolorModifier turns a draw into recolor-only.
	bool drawDown = false;
	bool eraseDown = false;
	bool recolorModifier = false;

	// Edge-triggered toggles.
	bool shapeTogglePressed = false;
	bool emissiveTogglePressed = false;

	// Parameter intents in [-1, 1] per frame (platform applies its own
	// fine-adjust modifier); SdfEditor owns the step sizes and clamps.
	float radiusDelta = 0.0f;
	float smoothnessDelta = 0.0f;
	float distanceDelta = 0.0f;

	// Monotonic cursor for stroke stamp spacing: stamps fire when this moved
	// far enough since the last stamp. Desktop: mouse position in pixels;
	// quest: a controller-motion equivalent at comparable scale.
	glm::dvec2 strokeCursor = glm::dvec2(0.0);
};

struct BrushInputSource
{
	virtual ~BrushInputSource() = default;
	virtual BrushInput poll() = 0;
};
