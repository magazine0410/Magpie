// magpie-ctl: sends a command to the running Magpie (the application or the
// service without a window, magpie-engine), for scripts and
// for binding keys without the desktop's shortcut portal. Linux counterpart
// of the window messages described in
// "docs/Interact with Magpie programally.md".
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <unistd.h>
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
		"  stop             stop scaling; a nested session ends with its program\n"
		"  launch [--windowed] [--profile NAME] [--size WxH] -- PROGRAM [ARGUMENT...]\n"
		"                   start a program in a nested session and scale it: it\n"
		"                   runs in a compositor of Magpie's own, which works on\n"
		"                   every desktop. --size is what the session offers the\n"
		"                   program and what the picture is scaled from; without\n"
		"                   it and without --profile it is 1280x720\n"
		"  screenshot       save the scaled picture to the screenshots directory\n"
		"  toolbar          the toolbar on the scaled picture: off, always shown,\n"
		"                   shown when the pointer is near\n"
		"  status           print \"idle\", \"scaling\" or \"waiting\"\n"
		"  nested           print \"yes\" if what is scaled is a nested session\n"
		"\n"
		"Exit status: 0 on success, 1 if Magpie is not running or refused.");
}

// "launch": the options up to "--", then the program and its arguments
static int Launch(sd_bus* bus, int argc, char* argv[], sd_bus_error* error) {
	const char* profile = "";
	int windowed = 0, width = 0, height = 0;
	int i = 2;
	for (; i < argc && std::string_view(argv[i]) != "--"; ++i) {
		const std::string_view option = argv[i];
		if (option == "--windowed") {
			windowed = 1;
		} else if (option == "--profile" && i + 1 < argc) {
			profile = argv[++i];
		} else if (option == "--size" && i + 1 < argc) {
			if (std::sscanf(argv[++i], "%dx%d", &width, &height) != 2 || width <= 0 || height <= 0) {
				std::fprintf(stderr, "--size takes WIDTHxHEIGHT\n");
				return -EINVAL;
			}
		} else {
			PrintUsage();
			return -EINVAL;
		}
	}
	// After the "--"
	++i;
	if (i >= argc) {
		PrintUsage();
		return -EINVAL;
	}

	// The program starts where this command was given
	char directory[4096];
	if (!getcwd(directory, sizeof(directory))) {
		directory[0] = 0;
	}

	sd_bus_message* call = nullptr;
	int result = sd_bus_message_new_method_call(bus, &call, MAGPIE_APP_ID, MAGPIE_OBJECT_PATH, MAGPIE_APP_ID, "Launch");
	if (result >= 0) {
		result = sd_bus_message_append_strv(call, argv + i);
	}
	if (result >= 0) {
		result = sd_bus_message_append(call, "ssbii", directory, profile, windowed, width, height);
	}
	if (result >= 0) {
		result = sd_bus_call(bus, call, 0, error, nullptr);
	}
	sd_bus_message_unref(call);
	return result;
}

int main(int argc, char* argv[]) {
	if (argc < 2) {
		PrintUsage();
		return 1;
	}
	const std::string_view command = argv[1];
	if (command == "-h" || command == "--help") {
		PrintUsage();
		return 0;
	}
	if (argc > 3 && command != "launch") {
		PrintUsage();
		return 1;
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
	} else if (command == "toolbar" && argc == 2) {
		result = sd_bus_call_method(bus, MAGPIE_APP_ID, MAGPIE_OBJECT_PATH, MAGPIE_APP_ID, "SwitchToolbar",
			&error, nullptr, "");
	} else if (command == "screenshot" && argc == 2) {
		result = sd_bus_call_method(bus, MAGPIE_APP_ID, MAGPIE_OBJECT_PATH, MAGPIE_APP_ID, "TakeScreenshot",
			&error, nullptr, "");
	} else if (command == "stop" && argc == 2) {
		result = sd_bus_call_method(bus, MAGPIE_APP_ID, MAGPIE_OBJECT_PATH, MAGPIE_APP_ID, "Stop", &error, nullptr, "");
	} else if (command == "launch") {
		result = Launch(bus, argc, argv, &error);
		if (result == -EINVAL) {
			sd_bus_flush_close_unref(bus);
			return 1;
		}
	} else if (command == "nested" && argc == 2) {
		int nested = 0;
		result = sd_bus_get_property_trivial(bus, MAGPIE_APP_ID, MAGPIE_OBJECT_PATH, MAGPIE_APP_ID, "Nested",
			&error, 'b', &nested);
		if (result >= 0) {
			std::puts(nested ? "yes" : "no");
		}
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
			? "Magpie is not running." : (error.message ? error.message : std::strerror(-result)));
	}
	sd_bus_error_free(&error);
	sd_bus_flush_close_unref(bus);
	return result < 0 ? 1 : 0;
}
