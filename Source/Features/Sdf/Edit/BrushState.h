#pragma once
#include <glm/glm.hpp>

// Brush shape catalog - single source of truth shared by the editor UI, the
// stamp UBO, and the indicator. The index is BrushState::shapeMode and the
// BrushShape uniform; it MUST match brushShapeSdf() in the brush shaders
// (SdfBrushCS / SdfBrushTexCS / SdfCoarseBrushCS) and PostPassFS.
constexpr int kBrushShapeCount = 12;
inline const char* BrushShapeName(int mode)
{
	static const char* kNames[kBrushShapeCount] = {
		"Cube", "Sphere", "Plane", "Round Box", "Torus", "Cylinder",
		"Cone", "Capsule", "Octahedron", "Hex Prism", "Ellipsoid", "Pyramid"
	};
	return (mode >= 0 && mode < kBrushShapeCount) ? kNames[mode] : "Cube";
}

// Brush parameters. Owned and written by Pottery each frame; the Editor UI
// reads them for display and writes color. Wired to EditorGui in main.cpp.
struct BrushState
{
	glm::vec4 data = glm::vec4(0.0f, 0.0f, 0.2f, 0.01f); // xyz: negated center WS, w: radius
	glm::vec4 color = glm::vec4(1.0f, 0.35f, 0.75f, 1.0f);
	bool useSphere = false;
	int shapeMode = 0;        // index into BrushShapeName (U cycles)
	float radius = 0.01f;     // A/D
	float smoothness = 0.001f;// Q/E
	float zAxis = 0.2f;       // distance from camera, W/S
	float action = 1.0f;      // 1.0 add (LMB), 0.0 remove (RMB)
	bool emissive = false;    // toggled with I
};
