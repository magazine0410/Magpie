// Linux counterpart of src/Magpie/Shortcut.h and the ShortcutAction
// enumeration of the Windows user interface.
#pragma once
#include <cstdint>
#include <string>

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

// The shortcut in the form the desktop portal takes as a preferred trigger,
// for example "ALT+SHIFT+a". Empty for no shortcut and for keys other than
// letters, digits and F1 to F24, which the desktop then lets the user choose.
inline std::string ShortcutToPortalTrigger(const Shortcut& shortcut) {
	std::string key;
	if (shortcut.code >= 'A' && shortcut.code <= 'Z') {
		// Key symbols of letters are the lower-case letters
		key = std::string(1, char(shortcut.code - 'A' + 'a'));
	} else if (shortcut.code >= '0' && shortcut.code <= '9') {
		key = std::string(1, char(shortcut.code));
	} else if (shortcut.code >= 0x70 && shortcut.code <= 0x87) {
		// VK_F1 to VK_F24
		key = "F" + std::to_string(shortcut.code - 0x70 + 1);
	} else {
		return {};
	}

	std::string result;
	if (shortcut.ctrl) {
		result += "CTRL+";
	}
	if (shortcut.alt) {
		result += "ALT+";
	}
	if (shortcut.shift) {
		result += "SHIFT+";
	}
	if (shortcut.win) {
		result += "LOGO+";
	}
	return result + key;
}

}
