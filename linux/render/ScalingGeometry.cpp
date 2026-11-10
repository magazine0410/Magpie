#include "ScalingGeometry.h"
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

}
