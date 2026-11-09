// Linux counterpart of src/Magpie/Shortcut.h and the ShortcutAction
// enumeration of the Windows user interface.
#pragma once
#include <cstdint>

namespace Magpie {

enum class ShortcutAction {
	Scale,
	WindowedModeScale,
	Toolbar,
	TakeScreenshot,
	COUNT_OR_NONE
};

struct Shortcut {
	bool operator==(const Shortcut&) const noexcept = default;

	// Windows virtual-key code, 0 for none. Kept in this form because
	// config.json stores it this way; the mapping to Linux key symbols belongs
	// to the code that registers the shortcuts.
	uint8_t code = 0;

	bool win = false;
	bool ctrl = false;
	bool alt = false;
	bool shift = false;
};

}
