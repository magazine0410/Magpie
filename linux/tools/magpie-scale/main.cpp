// magpie-scale: scales one window to fullscreen until it is told to stop.
// It runs a single scaling session in the foreground, which is what the
// engine service does when a hotkey is pressed, and serves to try and to
// test fullscreen scaling without that service.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <spdlog/sinks/stdout_sinks.h>
#include "Bridge.h"
#include "Logger.h"
#include "Paths.h"
#include "ScalingSession.h"
#include "ToolOptions.h"

using namespace Magpie;
using namespace Magpie::ToolOptions;

static void PrintUsage() {
	std::puts(
		"Usage: magpie-scale (--window QUERY | --active) [options]\n"
		"\n"
		"Scales a window to the whole monitor it is on, or into a larger window.\n"
		"While it is scaled the window is on a virtual monitor; mouse and keyboard\n"
		"act on it through the scaled picture. Ctrl+C in the terminal, or SIGTERM,\n"
		"ends scaling and puts the window back.\n"
		"\n"
		"Source:\n"
		"  --window QUERY     a window id, an app ID or a part of a title, as\n"
		"                     magpie-viewer --list-windows prints them\n"
		"  --active           the window that is active when scaling starts\n"
		"  --delay N          wait N seconds before starting, to give time to\n"
		"                     activate the window\n"
		"  --crop L,T,R,B     remove this many pixels from the edges of the source\n"
		"  --title-bar        keep the title bar that the desktop draws; it is part\n"
		"                     of the picture then\n"
		"  --allow-maximized  also scale a maximized or fullscreen window\n"
		"\n"
		"Where the picture goes:\n"
		"  --windowed         into a window that can be moved and resized\n"
		"  --window-scale F   the size that window starts with, as a multiple of the\n"
		"                     source (default: three quarters of the monitor)\n"
		"  --monitors WHICH   closest (default): the monitor the window is on;\n"
		"                     intersected: all monitors the window touches; all\n"
		"  --3d               3D game mode: the pointer stays with the source\n"
		"  --no-topmost       do not keep the picture above everything else\n"
		"  --toolbar STATE    off, show or auto (default): the toolbar at the top of\n"
		"                     the picture is shown when the pointer is near\n"
		"  --developer        developer mode: more in the profiler\n"
		"\n"
		"Effects:\n"
		"  --effect SPEC      add an effect; may be given several times. Without\n"
		"                     --effect and --mode: Bilinear,type=fit\n"
		"  --mode NAME        use the effects of scaling mode NAME from the configuration\n"
		"  --config FILE      config.json to read scaling modes from\n"
		"  --effects-dir DIR  directory of the .hlsl files\n"
		"\n"
		"Picture and cursor:\n"
		"  --align POS        lefttop, top, righttop, left, center (default), right,\n"
		"                     leftbottom, bottom or rightbottom\n"
		"  --cursor-scale F   size of the cursor relative to its own (default 1); 0\n"
		"                     scales it like the source\n"
		"  --cursor-bilinear  interpolate the scaled cursor\n"
		"  --hide-cursor N    hide the cursor after N seconds without movement\n"
		"  --adjust-cursor-speed  slow the pointer down by the scale of the picture\n"
		"                     while it is on the picture. This changes the speed\n"
		"                     setting of the pointer devices and restores it afterwards.\n"
		"\n"
		"Rendering:\n"
		"  --present-mode M   mailbox (default), fifo or immediate\n"
		"  --max-fps N        frame rate limit\n"
		"  --min-fps N        frames are repeated to keep this rate\n"
		"  --benchmark        render without waiting for new frames of the source\n"
		"  --validate         use the Vulkan validation layers\n"
		"\n"
		"Desktop:\n"
		"  --keep-screen-on   ask the desktop not to blank the screen while scaling\n"
		"  --quiet            ask the desktop to hold back notifications while scaling\n"
		"\n"
		"Screenshots:\n"
		"  --screenshot-at N  save the scaled picture N seconds after the start\n"
		"  --screenshots-dir DIR  where to (default: Screenshots in the pictures\n"
		"                     directory)\n"
		"  --screenshot-name TEMPLATE  file name template (default: %PN:20%)\n"
		"\n"
		"Run:\n"
		"  --seconds N        end after N seconds\n"
		"  -v                 print the log");
}

static const char* Describe(ScalingSession::EndReason reason) {
	using enum ScalingSession::EndReason;
	switch (reason) {
	case SourceClosed: return "the source window was closed";
	case SourceMinimized: return "the source window was minimized";
	case SourceResized: return "the source window changed its size";
	case SourceMoved: return "the source window was moved";
	case FocusLost: return "another window on the same monitor became active";
	case WindowClosed: return "the window with the picture was closed";
	case UserStopped: return "the stop button of the toolbar was pressed";
	case ModeSwitchRequested: return "the toolbar asked for the other way of scaling, which this tool does not do";
	case CaptureFailed: return "capturing failed";
	case Failed: return "of an error";
	default: return "it was asked to";
	}
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

	ScalingOptions options;
	ScalingSession::Config config;
	config.effectsDir = MAGPIE_DEFAULT_EFFECTS_DIR;
	config.stateFile = Paths::StateDir() / "scaling-state";
	// Unlike in a profile, where it is on by default
	options.IsAdjustCursorSpeed(false);
	config.appId = MAGPIE_APP_ID;
	config.iconFontPath = MAGPIE_DEFAULT_ICON_FONT;
	config.showMessage = [](const std::string& text) {
		std::printf("%s\n", text.c_str());
		std::fflush(stdout);
	};

	const char* configFile = nullptr;
	const char* modeName = nullptr;
	std::string windowQuery;
	bool useActive = false;
	float delay = 0.0f;
	float seconds = 0.0f;
	float screenshotAt = 0.0f;
	bool verbose = false;
	options.screenshotsDir = Paths::ScreenshotsDir({});

	for (int i = 1; i < argc; ++i) {
		const std::string_view arg = argv[i];

		const auto value = [&]() -> const char* {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "%s needs a value.\n", argv[i]);
				std::exit(1);
			}
			return argv[++i];
		};
		const auto invalid = [&](const char* text) {
			std::fprintf(stderr, "Invalid value for %.*s: %s\n", (int)arg.size(), arg.data(), text);
			std::exit(1);
		};
		const auto number = [&](float min) {
			const char* text = value();
			float result = 0.0f;
			if (!ParseFloat(text, result) || result < min) {
				invalid(text);
			}
			return result;
		};

		if (arg == "--window") {
			windowQuery = value();
		} else if (arg == "--active") {
			useActive = true;
		} else if (arg == "--delay") {
			delay = number(0.0f);
		} else if (arg == "--crop") {
			const char* text = value();
			if (!ParseCropping(text, options.cropping)) {
				invalid(text);
			}
		} else if (arg == "--windowed") {
			options.IsWindowedMode(true);
		} else if (arg == "--window-scale") {
			options.initialWindowedScaleFactor = number(1.0f);
		} else if (arg == "--monitors") {
			const std::string_view which = value();
			if (which == "closest") {
				options.multiMonitorUsage = MultiMonitorUsage::Closest;
			} else if (which == "intersected") {
				options.multiMonitorUsage = MultiMonitorUsage::Intersected;
			} else if (which == "all") {
				options.multiMonitorUsage = MultiMonitorUsage::All;
			} else {
				invalid(which.data());
			}
		} else if (arg == "--3d") {
			options.Is3DGameMode(true);
		} else if (arg == "--no-topmost") {
			options.IsTopmostDisabled(true);
		} else if (arg == "--toolbar") {
			const std::string_view state = value();
			ToolbarState toolbarState;
			if (state == "off") {
				toolbarState = ToolbarState::Off;
			} else if (state == "show") {
				toolbarState = ToolbarState::AlwaysShow;
			} else if (state == "auto") {
				toolbarState = ToolbarState::AutoHide;
			} else {
				invalid(state.data());
			}
			options.fullscreenInitialToolbarState = options.windowedInitialToolbarState = toolbarState;
		} else if (arg == "--developer") {
			options.IsDeveloperMode(true);
		} else if (arg == "--title-bar") {
			options.IsCaptureTitleBar(true);
		} else if (arg == "--allow-maximized") {
			options.IsAllowScalingMaximized(true);
		} else if (arg == "--hide-cursor") {
			const char* text = value();
			float delay = 0.0f;
			if (!ParseFloat(text, delay) || delay <= 0) {
				invalid(text);
			}
			options.autoHideCursorDelay = delay;
		} else if (arg == "--benchmark") {
			options.IsBenchmarkMode(true);
		} else if (arg == "--effect") {
			const char* spec = value();
			if (!ParseEffectSpec(spec, options.effects.emplace_back())) {
				invalid(spec);
			}
		} else if (arg == "--mode") {
			modeName = value();
		} else if (arg == "--config") {
			configFile = value();
		} else if (arg == "--effects-dir") {
			config.effectsDir = value();
		} else if (arg == "--align") {
			const char* text = value();
			const std::optional<OutputAlignment> alignment = ParseAlignment(text);
			if (!alignment) {
				invalid(text);
			}
			options.outputAlignment = *alignment;
		} else if (arg == "--cursor-scale") {
			options.cursorScaleFactor = number(0.0f);
		} else if (arg == "--cursor-bilinear") {
			options.cursorInterpolationMode = CursorInterpolationMode::Bilinear;
		} else if (arg == "--adjust-cursor-speed") {
			options.IsAdjustCursorSpeed(true);
		} else if (arg == "--present-mode") {
			const std::string_view mode = value();
			if (mode == "fifo") {
				config.presentMode = VK_PRESENT_MODE_FIFO_KHR;
			} else if (mode == "mailbox") {
				config.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
			} else if (mode == "immediate") {
				config.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
			} else {
				invalid(mode.data());
			}
		} else if (arg == "--max-fps") {
			options.maxFrameRate = number(1.0f);
		} else if (arg == "--min-fps") {
			options.minFrameRate = number(0.0f);
		} else if (arg == "--validate") {
			config.validation = true;
		} else if (arg == "--keep-screen-on") {
			options.IsKeepScreenOn(true);
		} else if (arg == "--quiet") {
			options.IsSimulateExclusiveFullscreen(true);
		} else if (arg == "--screenshot-at") {
			screenshotAt = number(0.0f);
		} else if (arg == "--screenshots-dir") {
			options.screenshotsDir = Paths::ScreenshotsDir(value());
		} else if (arg == "--screenshot-name") {
			options.screenshotFilenameTemplate = value();
		} else if (arg == "--seconds") {
			seconds = number(0.0f);
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

	if (windowQuery.empty() == !useActive) {
		std::fprintf(stderr, "Give either --window or --active.\n");
		return 1;
	}
	if (modeName) {
		if (!options.effects.empty()) {
			std::fprintf(stderr, "--mode and --effect cannot be combined.\n");
			return 1;
		}
		if (!LoadMode(configFile, modeName, options.effects)) {
			return 1;
		}
	}
	if (options.effects.empty()) {
		options.effects.push_back({ .name = "Bilinear", .scalingType = ScalingType::Fit });
	}

	std::string bridgeError;
	const std::unique_ptr<DesktopBridge> bridgeOwner = DesktopBridge::Create(bridgeError);
	if (!bridgeOwner) {
		std::fprintf(stderr, "%s\n", bridgeError.c_str());
		return 1;
	}
	DesktopBridge& bridge = *bridgeOwner;
	ScalingSession::RecoverAfterCrash(bridge, config.stateFile);

	if (delay > 0) {
		std::this_thread::sleep_for(std::chrono::duration<float>(delay));
	}

	std::string windowId;
	{
		const std::vector<WindowInfo> windows = bridge.ListWindows();
		if (useActive) {
			windowId = bridge.ActiveWindowId();
			if (windowId.empty()) {
				std::fprintf(stderr, "No window is active.\n");
				return 1;
			}
		} else {
			const std::vector<const WindowInfo*> found = FindWindows(windows, windowQuery);
			if (found.size() != 1) {
				std::fprintf(stderr, "\"%s\" matches %zu windows; magpie-viewer --list-windows prints them.\n",
					windowQuery.c_str(), found.size());
				return 1;
			}
			windowId = found[0]->id;
		}
	}

	ScalingSession session;
	const ScalingError error = session.Start(bridge, windowId, options, config);
	if (error != ScalingError::NoError) {
		std::fprintf(stderr, "Scaling did not start: %s.\n", ScalingErrorMessage(error));
		return 1;
	}
	std::printf("Scaling %s\n", session.Describe().c_str());
	std::fflush(stdout);

	const auto makeTimer = [](float time) {
		const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
		const long long nanoseconds = (long long)(time * 1e9);
		const itimerspec spec{ {}, { nanoseconds / 1'000'000'000, nanoseconds % 1'000'000'000 } };
		timerfd_settime(fd, 0, &spec, nullptr);
		return fd;
	};
	const int timerFd = seconds > 0 ? makeTimer(seconds) : -1;
	const int screenshotFd = screenshotAt > 0 ? makeTimer(screenshotAt) : -1;

	std::vector<int> wake{ signalFd };
	for (const int fd : { timerFd, screenshotFd }) {
		if (fd >= 0) {
			wake.push_back(fd);
		}
	}
	const auto start = std::chrono::steady_clock::now();

	const char* endText = nullptr;
	while (session.Step(wake)) {
		signalfd_siginfo info;
		uint64_t expirations;
		if (read(signalFd, &info, sizeof(info)) == sizeof(info)) {
			endText = "a signal asked for it";
			break;
		}
		if (timerFd >= 0 && read(timerFd, &expirations, sizeof(expirations)) == sizeof(expirations)) {
			endText = "the time is up";
			break;
		}
		if (screenshotFd >= 0 && read(screenshotFd, &expirations, sizeof(expirations)) == sizeof(expirations)) {
			session.TakeScreenshot();
		}
	}
	if (!endText) {
		endText = Describe(session.GetEndReason());
	}

	const double runTime = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	const ScalingSession::Statistics statistics = session.GetStatistics();
	const bool failed = session.GetEndReason() == ScalingSession::EndReason::Failed ||
		session.GetEndReason() == ScalingSession::EndReason::CaptureFailed;
	session.Stop();

	std::printf("Scaling ended after %.1f s because %s.\n", runTime, endText);
	std::printf("Frames:  %llu scaled (%.1f per second), %llu presented (%.1f per second)\n",
		(unsigned long long)statistics.scaledFrames, statistics.scaledFrames / runTime,
		(unsigned long long)statistics.presentedFrames, statistics.presentedFrames / runTime);
	std::printf("Pointer: sent to the source %u times, away from it %u times\n",
		statistics.pointerCaptures, statistics.pointerReleases);
	return failed ? 1 : 0;
}
