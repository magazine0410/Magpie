// The speed of the pointer devices, through the properties KWin publishes for
// every input device on D-Bus. Linux counterpart of SPI_SETMOUSESPEED in
// CursorManager::_AdjustCursorSpeed.
//
// KWin writes a changed value to the user's input settings, so a change that
// is meant to last only while a window is scaled has to be undone, also
// after a crash; ScalingSession notes the original values in its state file.
#pragma once
#include <span>
#include <string>
#include <vector>

namespace Magpie {

struct KWinPointerSpeed {
	struct Device {
		// The name KWin knows the device by, for example "event7"
		std::string sysName;
		// The acceleration setting of libinput, from -1 to 1
		double acceleration = 0.0;
	};

	// The pointer devices whose speed can be set, with their current values
	static std::vector<Device> ReadDevices() noexcept;

	// Gives every listed device its value
	static void Apply(std::span<const Device> devices) noexcept;

	// The setting that makes a device `scale` times slower than `acceleration`
	// does. With the flat profile the speed is 1 + acceleration, so this is
	// exact; with the adaptive profile, where the setting shapes a curve, it
	// only goes in the right direction. The result stays within -1 to 1.
	static double Slowed(double acceleration, double scale) noexcept;
};

}
