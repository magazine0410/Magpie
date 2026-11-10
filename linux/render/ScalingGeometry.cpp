#include "ScalingGeometry.h"
#include <algorithm>
#include <cassert>
#include <cmath>

namespace Magpie {

bool ScalingGeometry::CropSourceRect(Rect& srcRect, const Cropping& cropping) noexcept {
	const Rect cropped{
		(int32_t)std::lround(srcRect.left + cropping.Left),
		(int32_t)std::lround(srcRect.top + cropping.Top),
		(int32_t)std::lround(srcRect.right - cropping.Right),
		(int32_t)std::lround(srcRect.bottom - cropping.Bottom)
	};

	if (cropped.Width() < MIN_SRC_SIZE || cropped.Height() < MIN_SRC_SIZE) {
		return false;
	}

	srcRect = cropped;
	return true;
}

Rect ScalingGeometry::CalcDestRect(Size rendererSize, Size outputSize, OutputAlignment alignment) noexcept {
	using enum OutputAlignment;

	Rect destRect;

	if (alignment == LeftTop || alignment == Left || alignment == LeftBottom) {
		destRect.left = 0;
	} else if (alignment == Top || alignment == Center || alignment == Bottom) {
		destRect.left = (rendererSize.width - outputSize.width) / 2;
	} else {
		destRect.left = rendererSize.width - outputSize.width;
	}
	destRect.right = destRect.left + outputSize.width;

	if (alignment == LeftTop || alignment == Top || alignment == RightTop) {
		destRect.top = 0;
	} else if (alignment == Left || alignment == Center || alignment == Right) {
		destRect.top = (rendererSize.height - outputSize.height) / 2;
	} else {
		destRect.top = rendererSize.height - outputSize.height;
	}
	destRect.bottom = destRect.top + outputSize.height;

	return destRect;
}

Point ScalingGeometry::SrcToScaling(
	Point pt,
	const Rect& srcRect,
	const Rect& destRect,
	const Rect& rendererRect,
	bool skipBorder
) noexcept {
	Point result;

	if (pt.x >= srcRect.right) {
		result.x = (skipBorder ? rendererRect.right : destRect.right) + pt.x - srcRect.right;
	} else if (pt.x < srcRect.left) {
		result.x = (skipBorder ? rendererRect.left : destRect.left) + pt.x - srcRect.left;
	} else {
		const double pos = double(pt.x - srcRect.left) / (srcRect.right - srcRect.left - 1);
		result.x = (int32_t)std::lround(pos * (destRect.right - destRect.left - 1)) + destRect.left;
	}

	if (pt.y >= srcRect.bottom) {
		result.y = (skipBorder ? rendererRect.bottom : destRect.bottom) + pt.y - srcRect.bottom;
	} else if (pt.y < srcRect.top) {
		result.y = (skipBorder ? rendererRect.top : destRect.top) + pt.y - srcRect.top;
	} else {
		const double pos = double(pt.y - srcRect.top) / (srcRect.bottom - srcRect.top - 1);
		result.y = (int32_t)std::lround(pos * (destRect.bottom - destRect.top - 1)) + destRect.top;
	}

	return result;
}

Point ScalingGeometry::ScalingToSrc(Point pt, const Rect& srcRect, const Rect& destRect) noexcept {
	Point result{ srcRect.left, srcRect.top };

	if (pt.x >= destRect.right) {
		result.x += srcRect.Width() + pt.x - destRect.right;
	} else if (pt.x < destRect.left) {
		result.x += pt.x - destRect.left;
	} else {
		const double pos = double(pt.x - destRect.left) / (destRect.Width() - 1);
		result.x += (int32_t)std::lround(pos * (srcRect.Width() - 1));
	}

	if (pt.y >= destRect.bottom) {
		result.y += srcRect.Height() + pt.y - destRect.bottom;
	} else if (pt.y < destRect.top) {
		result.y += pt.y - destRect.top;
	} else {
		const double pos = double(pt.y - destRect.top) / (destRect.Height() - 1);
		result.y += (int32_t)std::lround(pos * (srcRect.Height() - 1));
	}

	return result;
}

bool ScalingGeometry::CalcWindowedRendererSize(
	int32_t& width,
	int32_t& height,
	Size srcSize,
	Size minSize,
	Size maxSize
) noexcept {
	assert((width == 0) != (height == 0));

	const float srcAspectRatio = (float)srcSize.height / (float)srcSize.width;

	if (width != 0) {
		height = (int32_t)std::lround((float)width * srcAspectRatio);
	} else {
		width = (int32_t)std::lround((float)height / srcAspectRatio);
	}

	// Not smaller than the minimum
	if (width > height) {
		if (height < minSize.height) {
			height = minSize.height;
			width = (int32_t)std::lround((float)height / srcAspectRatio);
		}
	} else {
		if (width < minSize.width) {
			width = minSize.width;
			height = (int32_t)std::lround((float)width * srcAspectRatio);
		}
	}

	if (width > maxSize.width || height > maxSize.height) {
		// The largest width if the height fits then, otherwise the largest height
		const int32_t testHeight = (int32_t)std::lround((float)maxSize.width * srcAspectRatio);
		if (testHeight <= maxSize.height) {
			width = maxSize.width;
			height = testHeight;
		} else {
			height = maxSize.height;
			width = (int32_t)std::lround((float)maxSize.height / srcAspectRatio);
		}

		if (width > height ? height < minSize.height : width < minSize.width) {
			// The source is too large
			return false;
		}
	}

	return true;
}

bool ScalingGeometry::CalcInitialWindowedRendererSize(
	Size& size,
	Size srcSize,
	Size monitorSize,
	float initialScaleFactor,
	int32_t lastWidth,
	Size minSize,
	Size maxSize
) noexcept {
	size = {};
	if (lastWidth != 0) {
		// The size of the last windowed scaling
		size.width = lastWidth;
	} else if (initialScaleFactor < 1.0f) {
		// Three quarters of the monitor, and at least a third larger than the source
		const float srcAspectRatio = (float)srcSize.height / (float)srcSize.width;
		if ((float)monitorSize.height / (float)monitorSize.width > srcAspectRatio) {
			size.width = std::max(monitorSize.width * 3 / 4, srcSize.width * 4 / 3);
		} else {
			size.height = std::max(monitorSize.height * 3 / 4, srcSize.height * 4 / 3);
		}
	} else {
		size.width = (int32_t)std::lround((float)srcSize.width * initialScaleFactor);
	}

	return CalcWindowedRendererSize(size.width, size.height, srcSize, minSize, maxSize);
}

Size ScalingGeometry::CalcResizedWindowedRendererSize(
	Size proposed,
	bool isWidthChanged,
	bool isHeightChanged,
	Size srcSize,
	Size minSize,
	Size maxSize
) noexcept {
	Size result{};
	if (isWidthChanged && isHeightChanged) {
		// The direction that limits a picture of the source's shape
		const float srcAspectRatio = (float)srcSize.height / (float)srcSize.width;
		if ((float)proposed.height / (float)proposed.width > srcAspectRatio) {
			result.width = proposed.width;
		} else {
			result.height = proposed.height;
		}
	} else if (isHeightChanged) {
		result.height = proposed.height;
	} else {
		result.width = proposed.width;
	}

	if (!CalcWindowedRendererSize(result.width, result.height, srcSize, minSize, maxSize)) {
		// Cannot happen for a source that could be scaled at all
		return minSize;
	}
	return result;
}

}
