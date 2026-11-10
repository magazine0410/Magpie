// Rectangle calculations that do not need a graphics device.
#pragma once
#include "Geometry.h"
#include "ScalingOptions.h"

namespace Magpie {

struct ScalingGeometry {
	// A source smaller than this in either direction is not scaled
	static constexpr int32_t MIN_SRC_SIZE = 64;

	// Removes the cropped edges from the source rectangle, as the end of
	// SrcTracker::_CalcSrcRect does. Returns false, and leaves the rectangle
	// unchanged, if less than MIN_SRC_SIZE would remain.
	static bool CropSourceRect(Rect& srcRect, const Cropping& cropping) noexcept;

	// Where the output of the effects goes inside the renderer area, relative
	// to the top left corner of that area. Counterpart of
	// Renderer::_UpdateDestRect.
	static Rect CalcDestRect(Size rendererSize, Size outputSize, OutputAlignment alignment) noexcept;
};

}
