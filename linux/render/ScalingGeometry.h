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

	// Where a point of the source, such as the cursor, appears in the scaled
	// picture. Counterpart of SrcToScaling in src/Magpie.Core/CursorManager.cpp.
	// `pt` and `srcRect` are in one coordinate system; `destRect`,
	// `rendererRect` and the result in another. Inside the source the first
	// pixel maps to the first and the last to the last, so the factor differs
	// a little from that of the picture. Outside it the distance to the
	// source is kept as it is; with `skipBorder` it is counted from the edge
	// of the renderer area and not of the destination, which jumps over
	// black bars.
	static Point SrcToScaling(
		Point pt,
		const Rect& srcRect,
		const Rect& destRect,
		const Rect& rendererRect,
		bool skipBorder
	) noexcept;

	// The size of the picture of windowed scaling. One of `width` and
	// `height` is the wanted value and the other is zero; on return both are
	// set, with the aspect ratio of the source. The picture is at least
	// `minSize` in the direction in which it is smaller and at most `maxSize`
	// in both. Returns false if the two limits cannot both be kept, which
	// means that the source is too large. Port of
	// ScalingWindow::_CalcWindowedScalingWindowSize, without the frame of the
	// window, which the desktop draws here.
	static bool CalcWindowedRendererSize(
		int32_t& width,
		int32_t& height,
		Size srcSize,
		Size minSize,
		Size maxSize
	) noexcept;

	// The size the picture of windowed scaling starts with. `lastWidth` is
	// the width of the last windowed scaling, or zero. Without it a scale
	// factor of one or more gives that multiple of the source, and a smaller
	// one three quarters of the monitor but at least four thirds of the
	// source, as ScalingWindow::_StartImpl does.
	static bool CalcInitialWindowedRendererSize(
		Size& size,
		Size srcSize,
		Size monitorSize,
		float initialScaleFactor,
		int32_t lastWidth,
		Size minSize,
		Size maxSize
	) noexcept;

	// The size for a window that the user resizes, from the size the desktop
	// proposes: the direction in which the proposal differs more from the
	// current size is followed and the other is computed.
	static Size CalcResizedWindowedRendererSize(
		Size proposed,
		Size current,
		Size srcSize,
		Size minSize,
		Size maxSize
	) noexcept;

	// The reverse: which point of the source a point of the scaled picture
	// shows. Counterpart of ScalingToSrc in CursorManager.cpp, with rounding
	// to the nearest pixel. Outside the destination the distance is kept.
	static Point ScalingToSrc(Point pt, const Rect& srcRect, const Rect& destRect) noexcept;
};

}
