#pragma once
#include "Features/Sdf/Edit/BrushInputSource.h"
#include "EngineContext.h"

// Desktop brush input: cursor ray through the camera, mouse buttons for
// draw/erase, keyboard for parameter adjust (A/D radius, Q/E smoothness,
// W/S distance, alt = fine, shift = recolor, U/I toggles).
class DesktopBrushInput : public BrushInputSource
{
public:
	explicit DesktopBrushInput(EngineContext& ctx) : ctx(ctx) {}
	BrushInput poll() override;

private:
	EngineContext& ctx;
};
