// magpie-engine: the scaling engine as a service of the session. It waits
// for a global shortcut or a call on D-Bus and then scales the active window
// to fullscreen, with the scaling mode of the default profile.
//
// Linux counterpart of ScalingService and ScalingRuntime (src/Magpie and
// src/Magpie.Core) without the user interface. One thread does everything:
// while a window is scaled it runs the scaling session, which wakes for
// D-Bus as well; otherwise it waits for D-Bus.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <poll.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <systemd/sd-bus.h>
#include "KWinBridge.h"
#include "Logger.h"
#include "Paths.h"
#include "PortalShortcuts.h"
#include "ProfileOptions.h"
#include "ScalingSession.h"
#include "SettingsJson.h"
#include "SettingsStore.h"
#include "Shortcut.h"

using namespace Magpie;

static void PrintUsage() {
	std::puts(
		"Usage: magpie-engine [options]\n"
		"\n"
		"Runs until it is ended with Ctrl+C or SIGTERM. The shortcut for scaling is\n"
		"registered with the desktop, which asks for confirmation the first time and\n"
		"lists it in its shortcut settings. magpie-ctl sends the same commands.\n"
		"\n"
		"Options:\n"
		"  --config FILE      config.json to use (default: Magpie's own)\n"
		"  --mode NAME        scale with this scaling mode and not with that of the\n"
		"                     default profile\n"
		"  --effects-dir DIR  directory of the .hlsl files\n"
		"  --no-shortcuts     do not register a global shortcut\n"
		"  --no-cursor-speed  never change the speed of the pointer devices, whatever\n"
		"                     the profile says\n"
		"  --validate         use the Vulkan validation layers\n"
		"  -v                 print the log");
}

namespace {

enum class State {
	Idle,
	Scaling,
	// The source is minimized or is changing its size; scaling goes on when
	// it is shown again and has kept its size for a moment
	Waiting
};

const char* Name(State state) {
	static constexpr const char* NAMES[]{ "idle", "scaling", "waiting" };
	return NAMES[(size_t)state];
}

enum class Command {
	None,
	// What the shortcut does: stop if a window is scaled, otherwise scale
	// the active window
	Toggle,
	ScaleWindow,
	Stop
};

struct Engine {
	// From the command line
	std::filesystem::path effectsDir = MAGPIE_DEFAULT_EFFECTS_DIR;
	const char* configFile = nullptr;
	const char* modeName = nullptr;
	bool validation = false;
	bool allowCursorSpeed = true;

	KWinBridge bridge;
	sd_bus* bus = nullptr;
	std::unique_ptr<ScalingSession> session;
	State state = State::Idle;
	// The window that is scaled or waited for
	std::string windowId;
	// While waiting: the window as last seen, and since when it is like that
	WindowInfo waitingInfo;
	std::chrono::steady_clock::time_point stableSince;

	// Set by D-Bus calls and shortcuts, carried out by the main loop
	Command command = Command::None;
	std::string commandArgument;

	void SetState(State newState) {
		if (state == newState) {
			return;
		}
		state = newState;
		Logger::Get().Info(std::string("State: ") + Name(state));
		sd_bus_emit_signal(bus, MAGPIE_OBJECT_PATH, MAGPIE_APP_ID, "StateChanged", "s", Name(state));
		sd_bus_emit_properties_changed(bus, MAGPIE_OBJECT_PATH, MAGPIE_APP_ID, "State", nullptr);
	}

	// The options of the default profile. Read anew for every start, so that
	// a change of the configuration applies without a restart.
	bool LoadOptions(ScalingOptions& options) {
		Settings settings;
		if (configFile) {
			std::ifstream file(configFile, std::ios::binary);
			std::ostringstream content;
			content << file.rdbuf();
			if (!file || !ReadSettingsJson(content.str(), settings)) {
				Logger::Get().Error(std::string("Cannot read ") + configFile);
				return false;
			}
		} else if (SettingsStore::Load(settings) == SettingsStore::LoadResult::Failed) {
			Logger::Get().Error("Cannot read " + SettingsStore::ConfigPath().string());
			return false;
		}

		Profile profile = settings.defaultProfile;
		if (modeName) {
			profile.scalingMode = -1;
			for (size_t i = 0; i < settings.scalingModes.size(); ++i) {
				if (settings.scalingModes[i].name == modeName) {
					profile.scalingMode = (int)i;
				}
			}
		}

		if (BuildScalingOptions(settings, profile, false, options) != ScalingError::NoError) {
			Logger::Get().Error("The scaling mode does not exist or has no effects");
			return false;
		}
		if (!allowCursorSpeed) {
			options.IsAdjustCursorSpeed(false);
		}
		return true;
	}

	bool StartScaling(const std::string& id) {
		// Not StopScaling: the state goes from what it is to "scaling"
		// without "idle" in between
		if (session) {
			session->Stop();
			session.reset();
		}

		ScalingOptions options;
		if (!LoadOptions(options)) {
			SetState(State::Idle);
			return false;
		}

		session = std::make_unique<ScalingSession>();
		const ScalingError error = session->Start(bridge, id, options, {
			.effectsDir = effectsDir,
			.stateFile = Paths::StateDir() / "scaling-state",
			.validation = validation,
			.appId = MAGPIE_APP_ID
		});
		if (error != ScalingError::NoError) {
			Logger::Get().Error("Scaling did not start (error " + std::to_string((int)error) + ")");
			session.reset();
			SetState(State::Idle);
			return false;
		}

		windowId = id;
		SetState(State::Scaling);
		return true;
	}

	void StopScaling() {
		if (session) {
			session->Stop();
			session.reset();
		}
		SetState(State::Idle);
	}

	// The session cannot go on. Port of how ScalingRuntime restarts after
	// the source was "repositioned".
	void OnSessionEnded() {
		const ScalingSession::EndReason reason = session->GetEndReason();
		Logger::Get().Info("The scaling session ended (reason " + std::to_string((int)reason) + ")");
		session->Stop();
		session.reset();

		if (reason == ScalingSession::EndReason::SourceResized ||
			reason == ScalingSession::EndReason::SourceMinimized) {
			// Scaled again once the window is shown and its size has settled
			waitingInfo = {};
			stableSince = std::chrono::steady_clock::now();
			SetState(State::Waiting);
		} else {
			SetState(State::Idle);
		}
	}

	void CheckWaiting() {
		// As long as the window has kept its size, as IsSrcRepositioning
		// waits for the end of a move or resize on Windows
		static constexpr std::chrono::milliseconds SETTLE_TIME{ 200 };

		WindowInfo info;
		if (!bridge.GetWindow(windowId, info)) {
			SetState(State::Idle);
			return;
		}

		const auto now = std::chrono::steady_clock::now();
		if (info.isMinimized || info.frameRect != waitingInfo.frameRect || info.clientRect != waitingInfo.clientRect) {
			waitingInfo = info;
			stableSince = now;
		} else if (now - stableSince >= SETTLE_TIME) {
			StartScaling(windowId);
		}
	}

	void Execute() {
		const Command current = command;
		command = Command::None;

		if (current == Command::Stop || (current == Command::Toggle && state == State::Scaling)) {
			StopScaling();
		} else if (current == Command::Toggle) {
			// Also ends a wait for a minimized source
			bridge.ListWindows();
			const std::string active = bridge.ActiveWindowId();
			WindowInfo info;
			if (active.empty() || !bridge.GetWindow(active, info)) {
				Logger::Get().Warn("No window is active");
				SetState(State::Idle);
			} else if (info.appId == "org.kde.plasmashell") {
				// The desktop and the panels; the counterpart of
				// WindowHelper::IsForbiddenSystemWindow
				SetState(State::Idle);
			} else {
				StartScaling(active);
			}
		} else if (current == Command::ScaleWindow) {
			const std::vector<WindowInfo> windows = bridge.ListWindows();
			const std::vector<const WindowInfo*> found = FindWindows(windows, commandArgument);
			if (found.size() == 1) {
				StartScaling(found[0]->id);
			} else {
				Logger::Get().Error(fmt::format("\"{}\" matches {} windows", commandArgument, found.size()));
			}
		}
	}
};

int OnScale(sd_bus_message* message, void* userData, sd_bus_error*) {
	((Engine*)userData)->command = Command::Toggle;
	return sd_bus_reply_method_return(message, "");
}

int OnScaleWindow(sd_bus_message* message, void* userData, sd_bus_error*) {
	Engine& engine = *(Engine*)userData;
	const char* query = nullptr;
	const int result = sd_bus_message_read(message, "s", &query);
	if (result < 0) {
		return result;
	}
	engine.command = Command::ScaleWindow;
	engine.commandArgument = query;
	return sd_bus_reply_method_return(message, "");
}

int OnStop(sd_bus_message* message, void* userData, sd_bus_error*) {
	((Engine*)userData)->command = Command::Stop;
	return sd_bus_reply_method_return(message, "");
}

int GetState(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userData, sd_bus_error*) {
	return sd_bus_message_append(reply, "s", Name(((Engine*)userData)->state));
}

const sd_bus_vtable VTABLE[]{
	SD_BUS_VTABLE_START(0),
	// The action of the shortcut
	SD_BUS_METHOD("Scale", "", "", OnScale, SD_BUS_VTABLE_UNPRIVILEGED),
	// A window id, an app ID or a part of a title
	SD_BUS_METHOD("ScaleWindow", "s", "", OnScaleWindow, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Stop", "", "", OnStop, SD_BUS_VTABLE_UNPRIVILEGED),
	// "idle", "scaling" or "waiting"
	SD_BUS_PROPERTY("State", "s", GetState, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_SIGNAL("StateChanged", "s", 0),
	SD_BUS_VTABLE_END
};

}

int main(int argc, char* argv[]) {
	// The signals that end the program arrive through a descriptor, so that
	// the main loop can wait for them together with everything else. They
	// are blocked before anything starts a thread, because a thread without
	// the block would take them and end the program without clean-up.
	sigset_t signals;
	sigemptyset(&signals);
	sigaddset(&signals, SIGINT);
	sigaddset(&signals, SIGTERM);
	sigprocmask(SIG_BLOCK, &signals, nullptr);
	const int signalFd = signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);

	Engine engine;
	bool useShortcuts = true;
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
			engine.configFile = value();
		} else if (arg == "--mode") {
			engine.modeName = value();
		} else if (arg == "--effects-dir") {
			engine.effectsDir = value();
		} else if (arg == "--no-shortcuts") {
			useShortcuts = false;
		} else if (arg == "--no-cursor-speed") {
			engine.allowCursorSpeed = false;
		} else if (arg == "--validate") {
			engine.validation = true;
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

	if (!engine.bridge.Initialize()) {
		std::fprintf(stderr, "This desktop does not offer KWin's interfaces.\n");
		return 1;
	}
	ScalingSession::RecoverAfterCrash(engine.bridge, Paths::StateDir() / "scaling-state");

	if (sd_bus_open_user(&engine.bus) < 0) {
		std::fprintf(stderr, "Cannot connect to the session bus.\n");
		return 1;
	}

	// The shortcuts come first: the portal wants to know the program before
	// its first other call
	PortalShortcuts shortcuts;
	if (useShortcuts) {
		Settings settings;
		SettingsStore::Load(settings);
		const PortalShortcuts::Shortcut wanted[]{ {
			"scale",
			"Scale the active window to fullscreen, or stop scaling",
			ShortcutToPortalTrigger(settings.shortcuts[(size_t)ShortcutAction::Scale])
		} };
		if (shortcuts.Initialize(engine.bus, MAGPIE_APP_ID, wanted)) {
			for (const auto& [id, trigger] : shortcuts.Bound()) {
				std::printf("Shortcut \"%s\": %s\n", id.c_str(), trigger.empty() ? "no key assigned" : trigger.c_str());
			}
		} else {
			std::fprintf(stderr, "No global shortcut is registered; magpie-ctl still works.\n");
		}
	}

	if (sd_bus_add_object_vtable(engine.bus, nullptr, MAGPIE_OBJECT_PATH, MAGPIE_APP_ID, VTABLE, &engine) < 0 ||
		sd_bus_request_name(engine.bus, MAGPIE_APP_ID, 0) < 0) {
		std::fprintf(stderr, "The service cannot be registered; is magpie-engine running already?\n");
		return 1;
	}

	std::printf("magpie-engine is ready.\n");
	std::fflush(stdout);

	const int busFd = sd_bus_get_fd(engine.bus);
	bool quit = false;
	while (!quit) {
		if (engine.session) {
			const int wakeFds[2]{ busFd, signalFd };
			if (!engine.session->Step(wakeFds)) {
				engine.OnSessionEnded();
			}
		} else {
			pollfd fds[3]{ { busFd, POLLIN, 0 }, { signalFd, POLLIN, 0 }, { engine.bridge.EventFd(), POLLIN, 0 } };
			poll(fds, 3, engine.state == State::Waiting ? 20 : -1);
			if (!engine.bridge.Dispatch()) {
				break;
			}
		}

		while (sd_bus_process(engine.bus, nullptr) > 0) {
		}
		for (const std::string& id : shortcuts.TakeActivated()) {
			if (id == "scale") {
				engine.command = Command::Toggle;
			}
		}

		signalfd_siginfo info;
		if (read(signalFd, &info, sizeof(info)) == sizeof(info)) {
			quit = true;
		}

		if (!quit) {
			engine.Execute();
			if (engine.state == State::Waiting) {
				engine.CheckWaiting();
			}
		}
		sd_bus_flush(engine.bus);
	}

	engine.StopScaling();
	sd_bus_flush(engine.bus);
	return 0;
}
