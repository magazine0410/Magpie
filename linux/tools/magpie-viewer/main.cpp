// magpie-viewer: shows a live, scaled copy of another window in a window of
// its own. Input is not passed on to the source; this is the mode that works
// with nothing but a screen cast. The source is captured through KWin without
// a dialog, or through the desktop portal, which asks the user.
//
// The loop follows Renderer::_BackendThreadProc and Renderer::Render: effects
// run when the source delivers a new frame, and the frame is presented again
// without them when only the cursor has moved.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <poll.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <vulkan/vulkan.h>
#include "Bridge.h"
#include "CursorDrawer.h"
#include "EffectChain.h"
#include "KWinBridge.h"
#include "Logger.h"
#include "Paths.h"
#include "PipeWireFrameSource.h"
#include "PortalCapture.h"
#include "ScalingGeometry.h"
#include "StepTimer.h"
#include "Swapchain.h"
#include "ToolOptions.h"
#include "VulkanDevice.h"
#include "WaylandWindow.h"

using namespace Magpie;
using namespace Magpie::ToolOptions;
using namespace std::chrono;

static void PrintUsage() {
	std::puts(
		"Usage: magpie-viewer [options]\n"
		"       magpie-viewer --list-windows | --capabilities\n"
		"\n"
		"Shows a live, scaled copy of a window. Input is not passed on to the source.\n"
		"\n"
		"Source:\n"
		"  --window QUERY     capture this window through KWin, without a dialog. QUERY\n"
		"                     is a window id, an app ID or a part of a title, as\n"
		"                     --list-windows prints them.\n"
		"  --portal           choose the source in the dialog of the desktop portal. This\n"
		"                     is the default when --window is not given. The choice is\n"
		"                     remembered and taken again without the dialog if the portal\n"
		"                     can restore it.\n"
		"  --pick             with the portal: show the dialog in any case\n"
		"  --crop L,T,R,B     remove this many pixels from the edges of the source\n"
		"  --no-dmabuf        receive the frames in shared memory\n"
		"  --no-damage        do not skip the frames for which the stream reports no\n"
		"                     change; with --duplicates always the report then tells\n"
		"                     whether the stream was right about them\n"
		"  --duplicates MODE  compare frames to skip equal ones: always, dynamic\n"
		"                     (default) or never\n"
		"\n"
		"Effects:\n"
		"  --effect SPEC      add an effect; may be given several times. SPEC is\n"
		"                     NAME[,KEY=VALUE]... as for magpie-scale-image. Without\n"
		"                     --effect and --mode: Bilinear,type=fit\n"
		"  --mode NAME        use the effects of scaling mode NAME from the configuration\n"
		"  --config FILE      config.json to read scaling modes from\n"
		"  --effects-dir DIR  directory of the .hlsl files\n"
		"\n"
		"Window:\n"
		"  --window-size WxH  initial size (default 1280x720)\n"
		"  --fullscreen       ask for a fullscreen window\n"
		"  --align POS        where the picture goes if it is smaller than the window:\n"
		"                     lefttop, top, righttop, left, center (default), right,\n"
		"                     leftbottom, bottom or rightbottom\n"
		"\n"
		"Cursor:\n"
		"  --cursor-scale F   size of the cursor relative to its own (default 1); 0\n"
		"                     scales it like the source\n"
		"  --cursor-bilinear  interpolate the scaled cursor\n"
		"  --no-cursor        do not draw the cursor\n"
		"\n"
		"Rendering:\n"
		"  --present-mode M   mailbox (default), fifo or immediate\n"
		"  --max-fps N        frame rate limit\n"
		"  --min-fps N        frames are repeated to keep this rate\n"
		"  --gpu N            use graphics card N of magpie-scale-image --list-gpus\n"
		"  --validate         use the Vulkan validation layers\n"
		"\n"
		"Run:\n"
		"  --seconds N        end after N seconds (default: when the window is closed or\n"
		"                     the source ends)\n"
		"  --stats            print a line of statistics every second\n"
		"  -v                 print the log");
}

///////////////////////////////////////////////////////////////////////////////
// Sources

static int ListWindows() {
	KWinBridge kwin;
	if (!kwin.Initialize() || kwin.WindowManagementVersion() == 0) {
		std::fprintf(stderr, "This desktop does not offer KWin's window list.\n");
		return 1;
	}

	for (const WindowInfo& window : kwin.ListWindows()) {
		std::printf("%s  %dx%d%+d%+d  pid %u  app \"%s\"%s%s%s  \"%s\"\n",
			window.id.c_str(), window.frameRect.Width(), window.frameRect.Height(),
			window.frameRect.left, window.frameRect.top, window.pid,
			window.appId.empty() ? window.resourceName.c_str() : window.appId.c_str(),
			window.isActive ? "  active" : "", window.isMinimized ? "  minimized" : "",
			window.isFullscreen ? "  fullscreen" : "", window.title.c_str());
	}
	return 0;
}

static int PrintCapabilities() {
	const CapabilityReport report = ProbeCapabilities();
	for (uint32_t i = 0; i < (uint32_t)Capability::COUNT; ++i) {
		const CapabilityReport::Entry& entry = report.entries[i];
		std::printf("%s:%s\n", CapabilityReport::Name((Capability)i), entry.providers.empty() ? " not available" : "");
		for (const std::string& provider : entry.providers) {
			std::printf("    %s\n", provider.c_str());
		}
		if (!entry.note.empty()) {
			std::printf("    (%s)\n", entry.note.c_str());
		}
	}
	return 0;
}

static std::unique_ptr<CaptureSession> CaptureThroughKWin(const std::string& query) {
	KWinBridge kwin;
	if (!kwin.Initialize() || kwin.ScreencastVersion() == 0) {
		std::fprintf(stderr, "This desktop does not offer KWin's screen cast. Use --portal.\n");
		return nullptr;
	}

	const std::vector<WindowInfo> windows = kwin.ListWindows();
	const std::vector<const WindowInfo*> found = FindWindows(windows, query);
	if (found.empty()) {
		std::fprintf(stderr, "No window matches \"%s\". --list-windows prints the windows.\n", query.c_str());
		return nullptr;
	}
	if (found.size() > 1) {
		std::fprintf(stderr, "\"%s\" matches %zu windows; use the id of one:\n", query.c_str(), found.size());
		for (const WindowInfo* window : found) {
			std::fprintf(stderr, "  %s  \"%s\"\n", window->id.c_str(), window->title.c_str());
		}
		return nullptr;
	}

	std::printf("Source:          \"%s\" through KWin\n", found[0]->title.c_str());
	return kwin.CaptureWindow(found[0]->id);
}

// The portal's restore token is kept between runs, so that the dialog need
// not be answered every time
static std::filesystem::path RestoreTokenPath() {
	return Paths::StateDir() / "portal-restore-token";
}

static std::unique_ptr<CaptureSession> CaptureThroughPortal(bool pick) {
	PortalCapture::Request request;
	if (!pick) {
		std::ifstream file(RestoreTokenPath());
		std::getline(file, request.restoreToken);
	}

	std::printf("Asking the desktop portal for a source%s ...\n",
		request.restoreToken.empty() ? "; choose one in its dialog" : "");
	std::fflush(stdout);

	PortalCapture::Result result = PortalCapture::Start(request);
	if (!result.session) {
		return nullptr;
	}

	// A token is valid once; the portal hands out the next one
	std::error_code ec;
	if (result.restoreToken.empty()) {
		std::filesystem::remove(RestoreTokenPath(), ec);
	} else {
		std::filesystem::create_directories(Paths::StateDir(), ec);
		std::ofstream(RestoreTokenPath()) << result.restoreToken << '\n';
	}

	std::printf("Source:          a %s through the portal%s\n",
		result.sourceType == PortalCapture::Window ? "window" :
		result.sourceType == PortalCapture::Monitor ? "monitor" : "source",
		result.isCursorEmbedded ? ", with the cursor painted into the picture" : "");
	return std::move(result.session);
}

///////////////////////////////////////////////////////////////////////////////
// Statistics

struct Timings {
	std::vector<float> values;

	void Add(float value) {
		values.push_back(value);
	}

	// "median 1.23 ms, 99% below 2.34 ms, longest 5.67 ms"
	std::string Describe() {
		if (values.empty()) {
			return "no data";
		}
		std::sort(values.begin(), values.end());
		char text[128];
		std::snprintf(text, sizeof(text), "median %.2f ms, 99%% below %.2f ms, longest %.2f ms",
			values[values.size() / 2], values[std::min(values.size() - 1, size_t(0.99 * values.size()))], values.back());
		return text;
	}
};

static float Milliseconds(steady_clock::duration duration) {
	return duration_cast<std::chrono::duration<float, std::milli>>(duration).count();
}

///////////////////////////////////////////////////////////////////////////////

int main(int argc, char* argv[]) {
	ScalingOptions options;
	std::filesystem::path effectsDir = Paths::EffectsDir(MAGPIE_DEFAULT_EFFECTS_DIR);
	const char* configFile = nullptr;
	const char* modeName = nullptr;
	std::string windowQuery;
	bool usePortal = false;
	bool pick = false;
	bool allowDmabuf = true;
	bool useDamage = true;
	Size windowSize{ 1280, 720 };
	bool fullscreen = false;
	bool drawCursor = true;
	VkPresentModeKHR presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
	int gpu = -1;
	bool validate = false;
	float seconds = 0.0f;
	bool printStats = false;
	bool verbose = false;
	bool listWindows = false;
	bool printCapabilities = false;

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
		} else if (arg == "--portal") {
			usePortal = true;
		} else if (arg == "--pick") {
			pick = true;
		} else if (arg == "--crop") {
			const char* text = value();
			if (!ParseCropping(text, options.cropping)) {
				invalid(text);
			}
		} else if (arg == "--no-dmabuf") {
			allowDmabuf = false;
		} else if (arg == "--no-damage") {
			useDamage = false;
		} else if (arg == "--duplicates") {
			const std::string_view mode = value();
			if (mode == "always") {
				options.duplicateFrameDetectionMode = DuplicateFrameDetectionMode::Always;
			} else if (mode == "dynamic") {
				options.duplicateFrameDetectionMode = DuplicateFrameDetectionMode::Dynamic;
			} else if (mode == "never") {
				options.duplicateFrameDetectionMode = DuplicateFrameDetectionMode::Never;
			} else {
				invalid(mode.data());
			}
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
			effectsDir = value();
		} else if (arg == "--window-size") {
			const char* text = value();
			float width, height;
			if (!ParsePair(text, width, height, false) || width < 1 || height < 1) {
				invalid(text);
			}
			windowSize = { (int32_t)width, (int32_t)height };
		} else if (arg == "--fullscreen") {
			fullscreen = true;
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
		} else if (arg == "--no-cursor") {
			drawCursor = false;
		} else if (arg == "--present-mode") {
			const std::string_view mode = value();
			if (mode == "fifo") {
				presentMode = VK_PRESENT_MODE_FIFO_KHR;
			} else if (mode == "mailbox") {
				presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
			} else if (mode == "immediate") {
				presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
			} else {
				invalid(mode.data());
			}
		} else if (arg == "--max-fps") {
			options.maxFrameRate = number(1.0f);
		} else if (arg == "--min-fps") {
			options.minFrameRate = number(0.0f);
		} else if (arg == "--gpu") {
			const char* text = value();
			if (!ParseInt(text, gpu) || gpu < 0) {
				invalid(text);
			}
		} else if (arg == "--validate") {
			validate = true;
		} else if (arg == "--seconds") {
			seconds = number(0.0f);
		} else if (arg == "--stats") {
			printStats = true;
		} else if (arg == "-v") {
			verbose = true;
		} else if (arg == "--list-windows") {
			listWindows = true;
		} else if (arg == "--capabilities") {
			printCapabilities = true;
		} else {
			PrintUsage();
			return arg == "-h" || arg == "--help" ? 0 : 1;
		}
	}

	Logger::Get().InitializeWithSink(
		verbose ? spdlog::level::info : spdlog::level::warn,
		std::make_shared<spdlog::sinks::stderr_sink_mt>());

	if (listWindows) {
		return ListWindows();
	}
	if (printCapabilities) {
		return PrintCapabilities();
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
	if (options.maxFrameRate && options.minFrameRate > *options.maxFrameRate) {
		std::fprintf(stderr, "--min-fps cannot be above --max-fps.\n");
		return 1;
	}
	if (!windowQuery.empty() && usePortal) {
		std::fprintf(stderr, "--window and --portal cannot be combined.\n");
		return 1;
	}

	// The graphics card with number N. GraphicsCardId needs its IDs as well,
	// which only a device has.
	if (gpu >= 0) {
		VulkanDevice probe;
		if (!probe.Initialize({})) {
			return 1;
		}
		const auto& cards = probe.GraphicsCards();
		if ((size_t)gpu >= cards.size() || !cards[gpu].isUsable) {
			std::fprintf(stderr, "Graphics card %d does not exist or is not usable.\n", gpu);
			return 1;
		}
		options.graphicsCardId = { gpu, cards[gpu].vendorId, cards[gpu].deviceId };
	}

	// Everything that uses the device is declared after it, so that it is
	// destroyed first.
	VulkanDevice device;
	if (!device.Initialize({ options.graphicsCardId, validate, true })) {
		return 1;
	}

	const std::unique_ptr<CaptureSession> session =
		windowQuery.empty() ? CaptureThroughPortal(pick) : CaptureThroughKWin(windowQuery);
	if (!session) {
		return 1;
	}

	PipeWireFrameSource frameSource;
	if (!frameSource.Initialize(device, {
		.pipeWireFd = session->PipeWireFd(),
		.nodeId = session->NodeId(),
		.isDmabufAllowed = allowDmabuf,
		.isDamageUsed = useDamage,
		.cropping = options.cropping,
		.duplicateFrameDetectionMode = options.duplicateFrameDetectionMode
	})) {
		return 1;
	}

	// The first frame tells the size of the source
	{
		const auto deadline = steady_clock::now() + 10s;
		FrameSourceState state = FrameSourceState::Waiting;
		while (state == FrameSourceState::Waiting) {
			if (steady_clock::now() > deadline) {
				std::fprintf(stderr, "The source delivered no frame within 10 seconds.\n");
				return 1;
			}

			pollfd fds[2]{ { frameSource.EventFd(), POLLIN, 0 }, { session->EventFd(), POLLIN, 0 } };
			poll(fds, 2, 100);
			if (!session->Dispatch() || !frameSource.Dispatch()) {
				std::fprintf(stderr, "The screen cast ended before its first frame.\n");
				return 1;
			}
			state = frameSource.Update();
		}
		if (state == FrameSourceState::Error) {
			return 1;
		}
	}

	WaylandWindow window;
	if (!window.Create("Magpie viewer", "magpie-viewer", fullscreen)) {
		return 1;
	}
	if (window.ConfiguredSize().width > 0 && window.ConfiguredSize().height > 0) {
		windowSize = window.ConfiguredSize();
	}

	VkSurfaceKHR surface = VK_NULL_HANDLE;
	const VkWaylandSurfaceCreateInfoKHR surfaceInfo{
		.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
		.display = window.Display(),
		.surface = window.Surface()
	};
	if (vkCreateWaylandSurfaceKHR(device.Instance(), &surfaceInfo, nullptr, &surface) != VK_SUCCESS) {
		std::fprintf(stderr, "vkCreateWaylandSurfaceKHR failed.\n");
		return 1;
	}

	Swapchain swapchain;
	if (!swapchain.Initialize(device, surface, windowSize, presentMode)) {
		return 1;
	}

	EffectChain chain;
	if (!chain.Initialize(device, options, { effectsDir, swapchain.GetSize() }, frameSource.Output())) {
		return 1;
	}
	chain.Profiler().Start(device, chain.PassCount());

	CursorDrawer cursorDrawer;
	if (!cursorDrawer.Initialize(device, swapchain.Format())) {
		return 1;
	}

	StepTimer stepTimer;
	stepTimer.Initialize(options.minFrameRate, options.maxFrameRate);
	const int wakeFds[3]{ window.EventFd(), frameSource.EventFd(), session->EventFd() };
	stepTimer.SetWakeDescriptors(wakeFds);

	std::printf("Graphics card:   %s\n", device.GraphicsCards()[device.GraphicsCardIdx()].name.c_str());
	std::printf("Stream:          %s\n", frameSource.DescribeFormat().c_str());
	std::fflush(stdout);

	// What the chain was last sized for
	std::shared_ptr<Texture> chainInput = frameSource.Output();
	Size chainSize = swapchain.GetSize();
	// The cursor as it was last drawn
	CaptureCursorState drawnCursor;
	uint32_t loadedCursorImage = 0;

	Timings updateTimes, presentTimes, latencies;
	uint64_t renderedFrames = 0, presentedFrames = 0;
	double gpuTimeSum = 0.0;
	uint64_t gpuTimeFrames = 0;
	const char* endReason = "the time is up";

	const auto start = steady_clock::now();
	auto lastStatsTime = start;
	PipeWireFrameSource::Statistics lastStats;
	uint64_t lastRendered = 0, lastPresented = 0;

	// The first frame is there already
	bool hasNewFrame = true;
	StepTimerStatus status = StepTimerStatus::WaitingForNewFrame;
	bool failed = false;

	while (true) {
		if (window.IsClosed()) {
			endReason = "the window was closed";
			break;
		}
		if (seconds > 0 && steady_clock::now() - start >= duration<float>(seconds)) {
			break;
		}

		bool fpsUpdated = false;
		status = stepTimer.WaitForNextFrame(
			!hasNewFrame && status != StepTimerStatus::WaitingForFPSLimiter, fpsUpdated);

		if (!window.PumpEvents()) {
			endReason = "the connection to the compositor is lost";
			failed = true;
			break;
		}
		if (!session->Dispatch() || !frameSource.Dispatch()) {
			endReason = "the source has ended";
			break;
		}

		// The frame source, unless the frame rate limit says to wait. The
		// cursor is not held back by the limit, as on Windows, where the
		// thread that presents is not the one that is limited.
		bool runEffects = false;
		if (status != StepTimerStatus::WaitingForFPSLimiter) {
			const auto updateStart = steady_clock::now();
			const FrameSourceState state = hasNewFrame ? FrameSourceState::NewFrame : frameSource.Update();
			hasNewFrame = false;

			if (state == FrameSourceState::Error) {
				endReason = "capturing failed";
				failed = true;
				break;
			}
			if (state == FrameSourceState::NewFrame) {
				updateTimes.Add(Milliseconds(steady_clock::now() - updateStart));
				runEffects = true;
			} else if (status == StepTimerStatus::ForceNewFrame) {
				runEffects = true;
			}
		}

		// The window was resized
		const Size wantedSize = window.ConfiguredSize().width > 0 && window.ConfiguredSize().height > 0
			? window.ConfiguredSize() : swapchain.GetSize();
		if (wantedSize != swapchain.GetSize()) {
			if (!swapchain.Resize(wantedSize)) {
				failed = true;
				break;
			}
			runEffects = true;
		}

		const CaptureCursorState& cursor = frameSource.CursorState();
		const bool isCursorShown = drawCursor && cursor.isVisible;
		const bool wasCursorShown = drawCursor && drawnCursor.isVisible;
		const bool cursorChanged = isCursorShown != wasCursorShown ||
			(isCursorShown && (cursor.pos != drawnCursor.pos || cursor.imageSerial != drawnCursor.imageSerial));
		if (!runEffects && !cursorChanged) {
			continue;
		}

		if (runEffects) {
			// The chain follows the size of the window and of the source
			if (frameSource.Output() != chainInput || swapchain.GetSize() != chainSize) {
				if (!chain.Resize(swapchain.GetSize(), frameSource.Output())) {
					failed = true;
					break;
				}
				chainInput = frameSource.Output();
				chainSize = swapchain.GetSize();
			}
			stepTimer.PrepareForRender();
		}

		if (isCursorShown && cursor.imageSerial != loadedCursorImage) {
			const CaptureCursorImage& image = frameSource.CursorImage();
			if (!cursorDrawer.SetImage(image.pixels, image.size, image.hotSpot)) {
				failed = true;
				break;
			}
			loadedCursorImage = cursor.imageSerial;
		}

		const auto presentStart = steady_clock::now();
		Swapchain::FrameResult result = Swapchain::FrameResult::NeedsResize;
		// A second attempt if the surface changed under the first
		for (int attempt = 0; attempt < 2 && result == Swapchain::FrameResult::NeedsResize; ++attempt) {
			if (attempt > 0) {
				if (!swapchain.Resize(wantedSize) || !chain.Resize(swapchain.GetSize(), frameSource.Output())) {
					result = Swapchain::FrameResult::Failed;
					break;
				}
				chainSize = swapchain.GetSize();
				runEffects = true;
			}

			result = swapchain.PresentFrame([&](VkCommandBuffer cmd, const Swapchain::Frame& frame) {
				if (runEffects) {
					chain.Draw(cmd, stepTimer.GetFrameCount());
				}

				const Size rendererSize = swapchain.GetSize();
				const Texture& output = *chain.Output();
				const Rect destRect = ScalingGeometry::CalcDestRect(rendererSize,
					{ (int32_t)output.width, (int32_t)output.height }, options.outputAlignment);
				Swapchain::Compose(cmd, output, frame.image, rendererSize, destRect);

				if (isCursorShown) {
					// The stream gives the position in its own picture
					const Rect& srcRect = frameSource.SrcRect();
					const Rect rendererRect{ 0, 0, rendererSize.width, rendererSize.height };
					const Point cursorPos = ScalingGeometry::SrcToScaling(cursor.pos, srcRect, destRect, rendererRect, false);
					cursorDrawer.Draw(cmd, { frame.image, frame.view, rendererSize, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL },
						cursorPos, { srcRect.Width(), srcRect.Height() }, destRect, options);
				}
			});
		}
		if (result != Swapchain::FrameResult::Presented) {
			endReason = "presenting failed";
			failed = true;
			break;
		}

		const auto now = steady_clock::now();
		drawnCursor = cursor;
		++presentedFrames;
		presentTimes.Add(Milliseconds(now - presentStart));

		if (runEffects) {
			// The first frame waited for the window and the effects to be set up
			if (++renderedFrames > 1) {
				latencies.Add(Milliseconds(now - frameSource.LastFrameArrivalTime()));
			}

			chain.Profiler().QueryTimings();
			const SmallVector<float> timings = chain.Profiler().GetTimings();
			if (!timings.empty()) {
				for (float timing : timings) {
					gpuTimeSum += timing;
				}
				++gpuTimeFrames;
			}
		}

		if (printStats && now - lastStatsTime >= 1s) {
			const PipeWireFrameSource::Statistics& stats = frameSource.GetStatistics();
			const double elapsed = duration<double>(now - lastStatsTime).count();
			std::printf("t=%.0f source frames=%.0f/s cursor updates=%.0f/s skipped=%llu duplicates=%llu | "
				"effects=%.0f/s presented=%.0f/s\n",
				duration<double>(now - start).count(),
				(stats.frames - lastStats.frames) / elapsed,
				(stats.cursorUpdates - lastStats.cursorUpdates) / elapsed,
				(unsigned long long)(stats.skippedFrames - lastStats.skippedFrames),
				(unsigned long long)(stats.duplicateFrames - lastStats.duplicateFrames),
				(renderedFrames - lastRendered) / elapsed, (presentedFrames - lastPresented) / elapsed);
			std::fflush(stdout);
			lastStatsTime = now;
			lastStats = stats;
			lastRendered = renderedFrames;
			lastPresented = presentedFrames;
		}
	}

	///////////////////////////////////////////////////////////////////////////
	// Report

	static constexpr const char* MODE_NAMES[] = { "immediate", "mailbox", "fifo", "fifo relaxed" };
	const double runTime = duration<double>(steady_clock::now() - start).count();
	const PipeWireFrameSource::Statistics& stats = frameSource.GetStatistics();

	std::printf("Ended because %s, after %.1f s.\n", endReason, runTime);
	std::printf("Stream:          %s\n", frameSource.DescribeFormat().c_str());
	std::printf("Window:          %dx%d, present mode %s\n", swapchain.GetSize().width, swapchain.GetSize().height,
		(uint32_t)swapchain.PresentMode() < std::size(MODE_NAMES) ? MODE_NAMES[swapchain.PresentMode()] : "other");
	std::printf("Picture:         %ux%u scaled to %ux%u in %u passes\n", frameSource.Output()->width,
		frameSource.Output()->height, chain.Output()->width, chain.Output()->height, chain.PassCount());
	std::printf("From the source: %llu frames (%.1f per second), %llu replaced by a newer one before use,\n"
		"                 %llu without damage, %llu equal to the one before, %llu updates of the cursor alone\n",
		(unsigned long long)stats.frames, stats.frames / runTime, (unsigned long long)stats.skippedFrames,
		(unsigned long long)stats.undamagedFrames, (unsigned long long)stats.duplicateFrames,
		(unsigned long long)stats.cursorUpdates);
	if (!useDamage && options.duplicateFrameDetectionMode == DuplicateFrameDetectionMode::Always) {
		std::printf("                 %llu of the frames without damage differed from the one before\n",
			(unsigned long long)stats.changedUndamagedFrames);
	}
	std::printf("Shown:           %llu frames scaled (%.1f per second), %llu presented (%.1f per second)\n",
		(unsigned long long)renderedFrames, renderedFrames / runTime,
		(unsigned long long)presentedFrames, presentedFrames / runTime);
	{
		const CaptureCursorState& cursor = frameSource.CursorState();
		const CaptureCursorImage& image = frameSource.CursorImage();
		if (cursor.imageSerial == 0) {
			std::printf("Cursor:          the stream delivered none\n");
		} else {
			std::printf("Cursor:          last at %d,%d of the stream, %s; image %dx%d with the hot spot at %d,%d\n",
				cursor.pos.x, cursor.pos.y, cursor.isVisible ? "visible" : "not over the source",
				image.size.width, image.size.height, image.hotSpot.x, image.hotSpot.y);
		}
	}
	std::printf("Taking a frame:  %s\n", updateTimes.Describe().c_str());
	std::printf("Presenting:      %s\n", presentTimes.Describe().c_str());
	std::printf("Arrival to shown: %s\n", latencies.Describe().c_str());
	if (gpuTimeFrames > 0) {
		std::printf("GPU time:        %.3f ms per scaled frame\n", gpuTimeSum / gpuTimeFrames);
	}
	if (validate) {
		std::printf("Validation:      %u messages\n", device.ValidationMessageCount());
	}

	return failed || device.ValidationMessageCount() > 0 ? 1 : 0;
}
