// magpie-ctl: sends a command to the running magpie-engine, for scripts and
// for binding keys without the desktop's shortcut portal. Linux counterpart
// of the window messages described in
// "docs/Interact with Magpie programally.md".
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <systemd/sd-bus.h>

static void PrintUsage() {
	std::puts(
		"Usage: magpie-ctl COMMAND\n"
		"\n"
		"Commands:\n"
		"  scale            what the shortcut does: scale the active window to\n"
		"                   fullscreen, or stop if a window is scaled\n"
		"  scale QUERY      scale this window: a window id, an app ID or a part of a\n"
		"                   title, as magpie-viewer --list-windows prints them\n"
		"  window [QUERY]   the same into a window that can be moved and resized\n"
		"  stop             stop scaling\n"
		"  screenshot       save the scaled picture to the screenshots directory\n"
		"  status           print \"idle\", \"scaling\" or \"waiting\"\n"
		"\n"
		"Exit status: 0 on success, 1 if magpie-engine is not running or refused.");
}

int main(int argc, char* argv[]) {
	if (argc < 2 || argc > 3) {
		PrintUsage();
		return 1;
	}
	const std::string_view command = argv[1];
	if (command == "-h" || command == "--help") {
		PrintUsage();
		return 0;
	}

	sd_bus* bus = nullptr;
	if (sd_bus_open_user(&bus) < 0) {
		std::fprintf(stderr, "Cannot connect to the session bus.\n");
		return 1;
	}

	sd_bus_error error = SD_BUS_ERROR_NULL;
	int result;
	if (command == "scale" && argc == 2) {
		result = sd_bus_call_method(bus, MAGPIE_APP_ID, MAGPIE_OBJECT_PATH, MAGPIE_APP_ID, "Scale", &error, nullptr, "");
	} else if (command == "window" && argc == 2) {
		result = sd_bus_call_method(bus, MAGPIE_APP_ID, MAGPIE_OBJECT_PATH, MAGPIE_APP_ID, "ScaleWindowed",
			&error, nullptr, "");
	} else if (command == "scale" || command == "window") {
		result = sd_bus_call_method(bus, MAGPIE_APP_ID, MAGPIE_OBJECT_PATH, MAGPIE_APP_ID, "ScaleWindow",
			&error, nullptr, "sb", argv[2], (int)(command == "window"));
	} else if (command == "screenshot" && argc == 2) {
		result = sd_bus_call_method(bus, MAGPIE_APP_ID, MAGPIE_OBJECT_PATH, MAGPIE_APP_ID, "TakeScreenshot",
			&error, nullptr, "");
	} else if (command == "stop" && argc == 2) {
		result = sd_bus_call_method(bus, MAGPIE_APP_ID, MAGPIE_OBJECT_PATH, MAGPIE_APP_ID, "Stop", &error, nullptr, "");
	} else if (command == "status" && argc == 2) {
		char* state = nullptr;
		result = sd_bus_get_property_string(bus, MAGPIE_APP_ID, MAGPIE_OBJECT_PATH, MAGPIE_APP_ID, "State", &error, &state);
		if (result >= 0) {
			std::puts(state);
			std::free(state);
		}
	} else {
		PrintUsage();
		return 1;
	}

	if (result < 0) {
		std::fprintf(stderr, "%s\n", sd_bus_error_has_name(&error, SD_BUS_ERROR_SERVICE_UNKNOWN) ||
			sd_bus_error_has_name(&error, SD_BUS_ERROR_NAME_HAS_NO_OWNER)
			? "magpie-engine is not running." : (error.message ? error.message : std::strerror(-result)));
	}
	sd_bus_error_free(&error);
	sd_bus_flush_close_unref(bus);
	return result < 0 ? 1 : 0;
}
