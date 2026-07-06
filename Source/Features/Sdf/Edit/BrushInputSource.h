#pragma once
#include <glm/glm.hpp>
#include <string>

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
	// Hold to snap the brush center to the voxel-block grid (desktop: Ctrl,
	// Blender-style) - aligned cuts and stamps land on exact block lines.
	bool snapModifier = false;

	// Edge-triggered toggles.
	bool shapeTogglePressed = false;
	bool emissiveTogglePressed = false;
	// "R": enter the Blender-style rotate operator (R -> X/Y/Z axis ->
	// typed degrees -> Enter applies, Esc cancels).
	bool rotateTogglePressed = false;
	// Modal numeric entry for the rotate operator: characters typed this
	// frame (digits, '.', '-', axis letters x/y/z) + confirm/cancel edges.
	// Ignored outside a modal. Desktop: keyboard; Quest: none yet.
	std::string typedChars;
	bool confirmPressed = false;
	bool cancelPressed = false;
	// Left/right arrow steps this frame (right = +1, includes key repeat):
	// the rotate modal's angle stepper.
	int rotateSteps = 0;
	// Fine-adjust modifier held (desktop: Alt) - smaller rotate steps.
	bool fineModifier = false;
	// "P" commit: promote every sketch chunk to physical
	// (BAKEIT_PHYSICS_RULES.md §2). Quest wiring pending - defaults false.
	bool physicsCommitPressed = false;

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
