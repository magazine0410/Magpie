// The speed of the pointer devices, through the properties KWin publishes for
// every input device on D-Bus. Linux counterpart of SPI_SETMOUSESPEED in
// CursorManager::_AdjustCursorSpeed.
//
// KWin writes a changed value to the user's input settings, so a change that
// is meant to last only while a window is scaled has to be undone, also
// after a crash; ScalingSession notes the original values in its state file.
#pragma once
#include <span>
#include <vector>
#include "Bridge.h"

namespace Magpie {

struct KWinPointerSpeed {
	// The pointer devices whose speed can be set, with their current values.
	// The id is the name KWin knows the device by, for example "event7".
	static std::vector<PointerDevice> ReadDevices() noexcept;

	// Gives every listed device its value
	static void Apply(std::span<const PointerDevice> devices) noexcept;
};

}
