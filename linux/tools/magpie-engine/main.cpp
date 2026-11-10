// magpie-engine: the scaling engine as a service of the session. It waits
// for a global shortcut or a call on D-Bus and then scales the active window
// to fullscreen or into a window, with the options of the default profile.
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
		"Runs until it is ended with Ctrl+C or SIGTERM. The shortcuts for scaling,\n"
		"windowed scaling and screenshots are registered with the desktop, which asks\n"
		"for confirmation the first time and lists them in its shortcut settings.\n"
		"magpie-ctl sends the same commands.\n"
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
	// What the shortcuts do: scale the active window to fullscreen or into
	// a window; stop if a window is scaled that way already, and change the
	// way if it is scaled the other way. As ScalingWindow::ToggleScaling.
	Toggle,
	ToggleWindowed,
	ScaleWindow,
	Stop,
	Screenshot
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
	// The window that is scaled or waited for, and how it is scaled
	std::string windowId;
	bool isWindowedMode = false;
	// The width the user gave the window of windowed scaling; the next one
	// starts with it, as on Windows while the program runs
	int32_t lastWindowedWidth = 0;
	// What the properties on D-Bus last said
	bool announcedSourceActive = false;
	Rect announcedSourceRect{};
	Rect announcedDestinationRect{};
	// While waiting: the window as last seen, and since when it is like that
	WindowInfo waitingInfo;
	std::chrono::steady_clock::time_point stableSince;

	// Set by D-Bus calls and shortcuts, carried out by the main loop
	Command command = Command::None;
	std::string commandArgument;
	bool commandWindowed = false;

	void SetState(State newState) {
		if (state == newState) {
			return;
		}
		state = newState;
		Logger::Get().Info(std::string("State: ") + Name(state));
		sd_bus_emit_signal(bus, MAGPIE_OBJECT_PATH, MAGPIE_APP_ID, "StateChanged", "s", Name(state));
		sd_bus_emit_properties_changed(bus, MAGPIE_OBJECT_PATH, MAGPIE_APP_ID,
			"State", "Windowed", "SourceWindow", nullptr);
	}

	// Tells listeners on D-Bus what changed while scaling: whether the
	// source is the active window, and where the picture is. Counterpart of
	// the events 0 to 2 of the MagpieScalingChanged message.
	void AnnounceChanges() {
		const bool sourceActive = session && session->IsSourceActive();
		const Rect sourceRect = session ? session->SourceRect() : Rect{};
		const Rect destinationRect = session ? session->DestinationRect() : Rect{};
		if (sourceActive != announcedSourceActive) {
			announcedSourceActive = sourceActive;
			sd_bus_emit_properties_changed(bus, MAGPIE_OBJECT_PATH, MAGPIE_APP_ID, "SourceActive", nullptr);
		}
		if (sourceRect != announcedSourceRect || destinationRect != announcedDestinationRect) {
			announcedSourceRect = sourceRect;
			announcedDestinationRect = destinationRect;
			sd_bus_emit_properties_changed(bus, MAGPIE_OBJECT_PATH, MAGPIE_APP_ID,
				"SourceRect", "DestinationRect", nullptr);
		}
	}

	// The options of the default profile. Read anew for every start, so that
	// a change of the configuration applies without a restart.
	bool LoadOptions(bool windowed, ScalingOptions& options) {
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

		const ScalingError error = BuildScalingOptions(settings, profile, windowed, options);
		if (error == ScalingError::Windowed3DGameMode) {
			Logger::Get().Error("3D game mode cannot be combined with windowed scaling");
			return false;
		}
		if (error != ScalingError::NoError) {
			Logger::Get().Error("The scaling mode does not exist or has no effects");
			return false;
		}
		if (!allowCursorSpeed) {
			options.IsAdjustCursorSpeed(false);
		}
		return true;
	}

	void EndSession() {
		if (!session) {
			return;
		}
		session->Stop();
		if (session->IsWindowedMode() && session->WindowedWidth() > 0) {
			lastWindowedWidth = session->WindowedWidth();
		}
		session.reset();
	}

	bool StartScaling(const std::string& id, bool windowed) {
		// Not StopScaling: the state goes from what it is to "scaling"
		// without "idle" in between
		EndSession();

		ScalingOptions options;
		if (!LoadOptions(windowed, options)) {
			SetState(State::Idle);
			return false;
		}

		session = std::make_unique<ScalingSession>();
		const ScalingError error = session->Start(bridge, id, options, {
			.effectsDir = effectsDir,
			.stateFile = Paths::StateDir() / "scaling-state",
			.validation = validation,
			.appId = MAGPIE_APP_ID,
			.lastWindowedWidth = lastWindowedWidth
		});
		if (error != ScalingError::NoError) {
			Logger::Get().Error("Scaling did not start (error " + std::to_string((int)error) + ")");
			session.reset();
			SetState(State::Idle);
			return false;
		}

		windowId = id;
		isWindowedMode = windowed;
		if (state == State::Scaling) {
			// From one way of scaling to the other
			sd_bus_emit_properties_changed(bus, MAGPIE_OBJECT_PATH, MAGPIE_APP_ID,
				"Windowed", "SourceWindow", nullptr);
		}
		SetState(State::Scaling);
		return true;
	}

	void StopScaling() {
		EndSession();
		SetState(State::Idle);
	}

	// The session cannot go on. Port of how ScalingRuntime restarts after
	// the source was "repositioned".
	void OnSessionEnded() {
		const ScalingSession::EndReason reason = session->GetEndReason();
		Logger::Get().Info("The scaling session ended (reason " + std::to_string((int)reason) + ")");
		EndSession();

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
			StartScaling(windowId, isWindowedMode);
		}
	}

	void Execute() {
		const Command current = command;
		command = Command::None;

		if (current == Command::Stop) {
			StopScaling();
		} else if (current == Command::Screenshot) {
			if (!session || !session->TakeScreenshot()) {
				Logger::Get().Warn("No screenshot: no window is scaled");
			}
		} else if (current == Command::Toggle || current == Command::ToggleWindowed) {
			const bool windowed = current == Command::ToggleWindowed;
			if (state == State::Scaling) {
				if (session->IsWindowedMode() == windowed || !session->IsSourceActive()) {
					StopScaling();
				} else {
					// The other way of scaling for the same window
					StartScaling(std::string(windowId), windowed);
				}
				return;
			}

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
				StartScaling(active, windowed);
			}
		} else if (current == Command::ScaleWindow) {
			const std::vector<WindowInfo> windows = bridge.ListWindows();
			const std::vector<const WindowInfo*> found = FindWindows(windows, commandArgument);
			if (found.size() == 1) {
				StartScaling(found[0]->id, commandWindowed);
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

int OnScaleWindowed(sd_bus_message* message, void* userData, sd_bus_error*) {
	((Engine*)userData)->command = Command::ToggleWindowed;
	return sd_bus_reply_method_return(message, "");
}

int OnScaleWindow(sd_bus_message* message, void* userData, sd_bus_error*) {
	Engine& engine = *(Engine*)userData;
	const char* query = nullptr;
	int windowed = 0;
	const int result = sd_bus_message_read(message, "sb", &query, &windowed);
	if (result < 0) {
		return result;
	}
	engine.command = Command::ScaleWindow;
	engine.commandArgument = query;
	engine.commandWindowed = windowed != 0;
	return sd_bus_reply_method_return(message, "");
}

int OnTakeScreenshot(sd_bus_message* message, void* userData, sd_bus_error*) {
	((Engine*)userData)->command = Command::Screenshot;
	return sd_bus_reply_method_return(message, "");
}

int OnStop(sd_bus_message* message, void* userData, sd_bus_error*) {
	((Engine*)userData)->command = Command::Stop;
	return sd_bus_reply_method_return(message, "");
}

int GetState(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userData, sd_bus_error*) {
	return sd_bus_message_append(reply, "s", Name(((Engine*)userData)->state));
}

int GetWindowed(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userData, sd_bus_error*) {
	const Engine& engine = *(Engine*)userData;
	return sd_bus_message_append(reply, "b", (int)(engine.state != State::Idle && engine.isWindowedMode));
}

int GetSourceWindow(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userData, sd_bus_error*) {
	const Engine& engine = *(Engine*)userData;
	return sd_bus_message_append(reply, "s", engine.state != State::Idle ? engine.windowId.c_str() : "");
}

int GetSourceActive(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userData, sd_bus_error*) {
	const Engine& engine = *(Engine*)userData;
	return sd_bus_message_append(reply, "b", (int)(engine.session && engine.session->IsSourceActive()));
}

int AppendRect(sd_bus_message* reply, const Rect& rect) {
	return sd_bus_message_append(reply, "(iiii)", rect.left, rect.top, rect.right, rect.bottom);
}

int GetSourceRect(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userData, sd_bus_error*) {
	const Engine& engine = *(Engine*)userData;
	return AppendRect(reply, engine.session ? engine.session->SourceRect() : Rect{});
}

int GetDestinationRect(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userData, sd_bus_error*) {
	const Engine& engine = *(Engine*)userData;
	return AppendRect(reply, engine.session ? engine.session->DestinationRect() : Rect{});
}

// The counterpart of the MagpieScalingChanged message and of the window
// properties that "docs/Interact with Magpie programally.md" describes
const sd_bus_vtable VTABLE[]{
	SD_BUS_VTABLE_START(0),
	// The actions of the shortcuts
	SD_BUS_METHOD("Scale", "", "", OnScale, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("ScaleWindowed", "", "", OnScaleWindowed, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("TakeScreenshot", "", "", OnTakeScreenshot, SD_BUS_VTABLE_UNPRIVILEGED),
	// A window id, an app ID or a part of a title; and whether into a window
	SD_BUS_METHOD("ScaleWindow", "sb", "", OnScaleWindow, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Stop", "", "", OnStop, SD_BUS_VTABLE_UNPRIVILEGED),
	// "idle", "scaling" or "waiting"
	SD_BUS_PROPERTY("State", "s", GetState, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	// The window that is scaled or waited for, as the desktop identifies
	// it, and whether it is scaled into a window
	SD_BUS_PROPERTY("SourceWindow", "s", GetSourceWindow, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("Windowed", "b", GetWindowed, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	// The source is the active window
	SD_BUS_PROPERTY("SourceActive", "b", GetSourceActive, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	// Left, top, right and bottom in desktop coordinates: the part of the
	// source that is scaled, where it is while it is scaled, and its picture
	SD_BUS_PROPERTY("SourceRect", "(iiii)", GetSourceRect, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("DestinationRect", "(iiii)", GetDestinationRect, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
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
		const PortalShortcuts::Shortcut wanted[]{
			{
				"scale",
				"Scale the active window to fullscreen, or stop scaling",
				ShortcutToPortalTrigger(settings.shortcuts[(size_t)ShortcutAction::Scale])
			},
			{
				"scale-windowed",
				"Scale the active window into a window, or stop scaling",
				ShortcutToPortalTrigger(settings.shortcuts[(size_t)ShortcutAction::WindowedModeScale])
			},
			{
				"screenshot",
				"Save a screenshot of the scaled picture",
				ShortcutToPortalTrigger(settings.shortcuts[(size_t)ShortcutAction::TakeScreenshot])
			}
		};
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
			} else if (id == "scale-windowed") {
				engine.command = Command::ToggleWindowed;
			} else if (id == "screenshot") {
				engine.command = Command::Screenshot;
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
			engine.AnnounceChanges();
		}
		sd_bus_flush(engine.bus);
	}

	engine.StopScaling();
	sd_bus_flush(engine.bus);
	return 0;
}
