// Integer sizes and rectangles, in place of the Win32 SIZE and RECT.
#pragma once
#include <cstdint>

namespace Magpie {

struct Size {
	bool operator==(const Size&) const noexcept = default;

	int32_t width = 0;
	int32_t height = 0;
};

// right and bottom are exclusive, as in a Win32 RECT
struct Rect {
	bool operator==(const Rect&) const noexcept = default;

	int32_t Width() const noexcept {
		return right - left;
	}

	int32_t Height() const noexcept {
		return bottom - top;
	}

	int32_t left = 0;
	int32_t top = 0;
	int32_t right = 0;
	int32_t bottom = 0;
};

}
