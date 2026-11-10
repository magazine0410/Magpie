// magpie-engine: the scaling service without the settings window. It waits
// for a global shortcut or a call on D-Bus and then scales the active window
// to fullscreen or into a window, with the profile that applies to it; it
// also scales windows whose profile says so when they become active.
//
// The service itself is ScalingService (linux/engine), which the application
// runs as well. This program reads config.json anew whenever the service
// asks for the settings, so a change of the file applies without a restart.
#include <csignal>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/signalfd.h>
#include <spdlog/sinks/stdout_sinks.h>
#include "Logger.h"
#include "ScalingService.h"
#include "SettingsJson.h"
#include "SettingsStore.h"

using namespace Magpie;

static void PrintUsage() {
	std::puts(
		"Usage: magpie-engine [options]\n"
		"\n"
		"Runs until it is ended with Ctrl+C or SIGTERM. The shortcuts for scaling,\n"
		"windowed scaling, the toolbar and screenshots are registered with the desktop, which asks\n"
		"for confirmation the first time and lists them in its shortcut settings.\n"
		"magpie-ctl sends the same commands.\n"
		"\n"
		"Options:\n"
		"  --config FILE      config.json to use (default: Magpie's own)\n"
		"  --mode NAME        scale with this scaling mode and not with that of the\n"
		"                     profile\n"
		"  --effects-dir DIR  directory of the .hlsl files\n"
		"  --no-shortcuts     do not register a global shortcut\n"
		"  --no-cursor-speed  never change the speed of the pointer devices, whatever\n"
		"                     the profile says\n"
		"  --validate         use the Vulkan validation layers\n"
		"  -v                 print the log");
}

int main(int argc, char* argv[]) {
	// The signals that end the program arrive through a descriptor, so that
	// the service can wait for them together with everything else. They are
	// blocked before anything starts a thread, because a thread without the
	// block would take them and end the program without clean-up.
	sigset_t signals;
	sigemptyset(&signals);
	sigaddset(&signals, SIGINT);
	sigaddset(&signals, SIGTERM);
	sigprocmask(SIG_BLOCK, &signals, nullptr);

	ScalingService::Config config{
		.effectsDir = MAGPIE_DEFAULT_EFFECTS_DIR,
		.iconFontPath = MAGPIE_DEFAULT_ICON_FONT,
		.appId = MAGPIE_APP_ID,
		.objectPath = MAGPIE_OBJECT_PATH,
		.quitFd = signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC)
	};
	const char* configFile = nullptr;
	const char* modeName = nullptr;
	bool allowCursorSpeed = true;
	bool verbose = false;

	for (int i = 1; i < argc; ++i) {
		const std::string_view arg = argv[i];
		const auto value = [&]() -> const char* {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "%s needs a value.\n", argv[i]);
				std::exit(1);
			}
			return argv[++i];
		};

		if (arg == "--config") {
			configFile = value();
		} else if (arg == "--mode") {
			modeName = value();
		} else if (arg == "--effects-dir") {
			config.effectsDir = value();
		} else if (arg == "--no-shortcuts") {
			config.useShortcuts = false;
		} else if (arg == "--no-cursor-speed") {
			allowCursorSpeed = false;
		} else if (arg == "--validate") {
			config.validation = true;
		} else if (arg == "-v") {
			verbose = true;
		} else {
			PrintUsage();
			return arg == "-h" || arg == "--help" ? 0 : 1;
		}
	}

	Logger::Get().InitializeWithSink(
		verbose ? spdlog::level::info : spdlog::level::warn,
		std::make_shared<spdlog::sinks::stderr_sink_mt>());

	config.settings = [=]() -> std::shared_ptr<const Settings> {
		const auto settings = std::make_shared<Settings>();
		if (configFile) {
			std::ifstream file(configFile, std::ios::binary);
			std::ostringstream content;
			content << file.rdbuf();
			if (!file || !ReadSettingsJson(content.str(), *settings)) {
				Logger::Get().Error(std::string("Cannot read ") + configFile);
				return nullptr;
			}
		} else if (SettingsStore::Load(*settings) == SettingsStore::LoadResult::Failed) {
			Logger::Get().Error("Cannot read " + SettingsStore::ConfigPath().string());
			return nullptr;
		}

		// What the command line overrides, in every profile
		const auto adjust = [&](Profile& profile) {
			if (modeName) {
				profile.scalingMode = -1;
				for (size_t i = 0; i < settings->scalingModes.size(); ++i) {
					if (settings->scalingModes[i].name == modeName) {
						profile.scalingMode = (int)i;
					}
				}
			}
			if (!allowCursorSpeed) {
				profile.IsAdjustCursorSpeed(false);
			}
		};
		adjust(settings->defaultProfile);
		for (Profile& profile : settings->profiles) {
			adjust(profile);
		}
		return settings;
	};

	config.shortcutsChanged = [](const ScalingService::BoundShortcuts& shortcuts) {
		for (const auto& [id, trigger] : shortcuts) {
			std::printf("Shortcut \"%s\": %s\n", id.c_str(), trigger.empty() ? "no key assigned" : trigger.c_str());
		}
		std::fflush(stdout);
	};

	// Keeps the places that the user gave the windows of the overlay, as the
	// Windows version does when scaling ends. Nothing is written with a
	// configuration file from the command line, and none is created: that
	// is for the settings application to do.
	if (!configFile) {
		config.overlayOptionsChanged = [](const std::map<std::string, OverlayWindowOption>& windows) {
			Settings settings;
			if (SettingsStore::Load(settings) != SettingsStore::LoadResult::Loaded ||
				settings.overlayWindowOptions == windows) {
				return;
			}
			settings.overlayWindowOptions = windows;
			if (!SettingsStore::Save(settings)) {
				Logger::Get().Warn("The places of the overlay windows could not be saved");
			}
		};
	}

	ScalingService service;
	std::string error;
	if (!service.Initialize(std::move(config), error)) {
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}

	std::printf("magpie-engine is ready.\n");
	std::fflush(stdout);

	service.Run();
	return 0;
}
