// Reads what Sway answers on its control socket: the tree of windows, the
// outputs, the input devices and the events of key bindings.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "Bridge.h"

namespace Magpie {

// A window as Sway's tree has it. `info.id` is the number of its container.
struct SwayWindow {
	WindowInfo info;
	// The output and the workspace it is on; a window in the scratchpad that
	// is not shown has neither and counts as minimized
	std::string output;
	std::string workspace;
	// Not part of the tiling
	bool isFloating = false;
	// "normal" (title bar and border), "pixel" (border), "none", or "csd"
	// if the program draws its decoration itself
	std::string border;
	int32_t borderWidth = 0;
	// The height of the title bar, which is the top of the frame; zero
	// without one
	int32_t titleBarHeight = 0;
	// The containers before and after it in its parent; zero if there is
	// none. They say where a tiled window was.
	int64_t previousSibling = 0;
	int64_t nextSibling = 0;
};

// What it takes to put a window back where it was before it was scaled;
// the content of WindowInfo::restoreData
struct SwayRestore {
	bool isFloating = false;
	std::string border;
	int32_t borderWidth = 0;
	int64_t previousSibling = 0;
	int64_t nextSibling = 0;
	std::string workspace;
};

namespace SwayJson {

// The answer to "get_tree". Every window gets its restore data.
bool ParseTree(std::string_view json, std::vector<SwayWindow>& windows) noexcept;

// The answer to "get_outputs"; only the outputs that are switched on
bool ParseOutputs(std::string_view json, std::vector<OutputInfo>& outputs) noexcept;

// The answer to "get_inputs": the devices that move the pointer and have a
// speed setting
bool ParsePointerDevices(std::string_view json, std::vector<PointerDevice>& devices) noexcept;

// One line of text, see SwayRestore
std::string FormatRestore(const SwayWindow& window) noexcept;

bool ParseRestore(std::string_view text, SwayRestore& restore) noexcept;

// What follows `prefix` in the command of a key binding that was run, or
// nothing if the event is about another binding
std::string ParseBindingEvent(std::string_view json, std::string_view prefix) noexcept;

// Whether an event of the kind "workspace" says that Sway has read its
// configuration again, which removes the bindings made at run time
bool IsReloadEvent(std::string_view json) noexcept;

}

}
