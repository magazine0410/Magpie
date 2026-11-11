#include "KWinPointerSpeed.h"
#include <algorithm>
#include <cstring>
#include <fmt/format.h>
#include <systemd/sd-bus.h>
#include "Logger.h"

namespace Magpie {

static constexpr const char* KWIN = "org.kde.KWin";
static constexpr const char* DEVICES_PATH = "/org/kde/KWin/InputDevice";
static constexpr const char* DEVICE_INTERFACE = "org.kde.KWin.InputDevice";

static std::string DevicePath(const std::string& sysName) {
	return fmt::format("{}/{}", DEVICES_PATH, sysName);
}

static bool ReadBool(sd_bus* bus, const std::string& path, const char* property) noexcept {
	int value = 0;
	return sd_bus_get_property_trivial(bus, KWIN, path.c_str(), DEVICE_INTERFACE, property, nullptr, 'b', &value) >= 0 &&
		value != 0;
}

std::vector<PointerDevice> KWinPointerSpeed::ReadDevices() noexcept {
	std::vector<PointerDevice> result;

	sd_bus* bus = nullptr;
	if (sd_bus_open_user(&bus) < 0) {
		return result;
	}

	char** names = nullptr;
	if (sd_bus_get_property_strv(bus, KWIN, DEVICES_PATH, "org.kde.KWin.InputDeviceManager",
		"devicesSysNames", nullptr, &names) >= 0 && names) {
		for (char** name = names; *name; ++name) {
			const std::string path = DevicePath(*name);
			double acceleration = 0.0;
			if (ReadBool(bus, path, "pointer") && ReadBool(bus, path, "supportsPointerAcceleration") &&
				sd_bus_get_property_trivial(bus, KWIN, path.c_str(), DEVICE_INTERFACE,
					"pointerAcceleration", nullptr, 'd', &acceleration) >= 0) {
				result.push_back({ *name, acceleration });
			}
			free(*name);
		}
		free(names);
	}

	sd_bus_flush_close_unref(bus);
	return result;
}

void KWinPointerSpeed::Apply(std::span<const PointerDevice> devices) noexcept {
	if (devices.empty()) {
		return;
	}

	sd_bus* bus = nullptr;
	if (sd_bus_open_user(&bus) < 0) {
		return;
	}

	for (const PointerDevice& device : devices) {
		sd_bus_error error = SD_BUS_ERROR_NULL;
		if (sd_bus_set_property(bus, KWIN, DevicePath(device.id).c_str(), DEVICE_INTERFACE,
			"pointerAcceleration", &error, "d", device.acceleration) < 0) {
			// For example a device that was unplugged
			Logger::Get().Warn(fmt::format("The pointer speed of {} could not be set: {}",
				device.id, error.message ? error.message : "unknown error"));
		}
		sd_bus_error_free(&error);
	}

	sd_bus_flush_close_unref(bus);
}

}
