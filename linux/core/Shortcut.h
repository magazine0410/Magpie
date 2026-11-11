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

// Whether Magpie can bind the shortcut itself on a desktop that leaves that
// to programs: a letter, a digit or F1 to F24 with at least one modifier,
// which is the rule of the Windows version for the keys this port knows.
inline bool IsBindableShortcut(const Shortcut& shortcut) {
	return (shortcut.ctrl || shortcut.alt || shortcut.shift || shortcut.win) &&
		!ShortcutToPortalTrigger(shortcut).empty();
}

// The shortcut as a text for the user, for example "Alt+Shift+A"; empty for
// keys that ShortcutToPortalTrigger does not know
inline std::string ShortcutToText(const Shortcut& shortcut) {
	const std::string trigger = ShortcutToPortalTrigger(shortcut);
	if (trigger.empty()) {
		return {};
	}
	std::string key = trigger.substr(trigger.rfind('+') == std::string::npos ? 0 : trigger.rfind('+') + 1);
	if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z') {
		key[0] = char(key[0] - 'a' + 'A');
	}
	return std::string(shortcut.ctrl ? "Ctrl+" : "") + (shortcut.alt ? "Alt+" : "") +
		(shortcut.shift ? "Shift+" : "") + (shortcut.win ? "Super+" : "") + key;
}

}
