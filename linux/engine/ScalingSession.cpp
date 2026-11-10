#include "ScalingSession.h"
#include <algorithm>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <utility>
#include <poll.h>
#include <unistd.h>
#include <fmt/format.h>
#include "KWinBridge.h"
#include "Logger.h"
#include "ProcessInfo.h"
#include "ScalingGeometry.h"
#include "Screenshot.h"

using namespace std::chrono;

namespace Magpie {

// The virtual output is put this far below the real ones. KWin sends the
// pointer to the output nearest to where a motion ends, so the distance has
// to be more than twice what a single motion can cover; then the pointer can
// neither leave the virtual output nor wander onto it.
static constexpr int32_t VIRTUAL_OUTPUT_GAP = 16384;
// X11 clients, which most games are, cannot handle coordinates above this
static constexpr int32_t MAX_COORDINATE = 32767;
static constexpr const char* STATE_FILE_HEADER = "magpie-scaling-state 1";
// What a new virtual output has, in thousandths of a hertz, with some margin
static constexpr int32_t DEFAULT_REFRESH_RATE = 60'500;

static bool Contains(const Rect& rect, Point pt) noexcept {
	return pt.x >= rect.left && pt.x < rect.right && pt.y >= rect.top && pt.y < rect.bottom;
}

static Point ClampTo(Point pt, const Rect& rect) noexcept {
	return { std::clamp(pt.x, rect.left, rect.right - 1), std::clamp(pt.y, rect.top, rect.bottom - 1) };
}

static bool Overlaps(const Rect& a, const Rect& b) noexcept {
	return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
}

static Rect Union(const Rect& a, const Rect& b) noexcept {
	return {
		std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom)
	};
}

ScalingSession::~ScalingSession() {
	Stop();
}

void ScalingSession::_WriteStateFile() const noexcept {
	std::error_code ec;
	std::filesystem::create_directories(_config.stateFile.parent_path(), ec);

	const Rect& frame = _original.frameRect;
	std::ofstream file(_config.stateFile, std::ios::trunc);
	file << STATE_FILE_HEADER << '\n'
		<< "window " << _windowId << '\n'
		<< "frame " << frame.left << ' ' << frame.top << ' ' << frame.right << ' ' << frame.bottom << '\n'
		<< "noBorder " << (_original.hasNoBorder ? 1 : 0) << '\n';
	if (_isSourceFilling) {
		// Its place is that of a maximized or fullscreen window on this monitor
		file << "output " << _monitors[0].name << '\n';
	}
	for (const KWinPointerSpeed::Device& device : _pointerDevices) {
		file << "pointer " << device.sysName << ' ' << device.acceleration << '\n';
	}
	if (!file) {
		Logger::Get().Warn(fmt::format("The state file {} could not be written", _config.stateFile.string()));
	}
}

void ScalingSession::RecoverAfterCrash(KWinBridge& bridge, const std::filesystem::path& stateFile) noexcept {
	std::ifstream file(stateFile);
	if (!file) {
		return;
	}

	std::string header, windowId, output;
	Rect frame;
	int noBorder = 0;
	bool hasWindow = false, hasFrame = false;
	std::vector<KWinPointerSpeed::Device> pointerDevices;

	std::getline(file, header);
	if (header == STATE_FILE_HEADER) {
		std::string line;
		while (std::getline(file, line)) {
			std::istringstream stream(line);
			std::string key;
			stream >> key;
			if (key == "window") {
				hasWindow = (bool)(stream >> windowId);
			} else if (key == "frame") {
				hasFrame = (bool)(stream >> frame.left >> frame.top >> frame.right >> frame.bottom);
			} else if (key == "noBorder") {
				stream >> noBorder;
			} else if (key == "output") {
				stream >> output;
			} else if (key == "pointer") {
				KWinPointerSpeed::Device device;
				if (stream >> device.sysName >> device.acceleration) {
					pointerDevices.push_back(std::move(device));
				}
			}
		}
	}
	file.close();

	if (hasWindow && hasFrame) {
		bridge.ListWindows();
		WindowInfo info;
		if (bridge.GetWindow(windowId, info)) {
			Logger::Get().Warn("Scaling did not end properly last time; putting the window back");
			bridge.PlaceWindow(windowId, {
				.frameRect = output.empty() ? frame : Rect{},
				.noBorder = noBorder != 0,
				.output = output
			});
		}
	}
	if (!pointerDevices.empty()) {
		// Whether they were changed when the program ended is not known;
		// setting them again does no harm
		Logger::Get().Warn("Scaling did not end properly last time; restoring the pointer speed");
		KWinPointerSpeed::Apply(pointerDevices);
	}

	std::error_code ec;
	std::filesystem::remove(stateFile, ec);
}

ScalingError ScalingSession::Start(
	KWinBridge& bridge,
	const std::string& windowId,
	const ScalingOptions& options,
	const Config& config
) noexcept {
	_bridge = &bridge;
	_options = options;
	_config = config;
	_windowId = windowId;
	// Debug mode of the Windows version also turns on the debug layer
	_config.validation = config.validation || options.IsDebugMode();

	if (bridge.WindowManagementVersion() == 0 || bridge.ScreencastVersion() == 0 || !bridge.HasFakeInput()) {
		Logger::Get().Error("The desktop lacks an interface that scaling needs "
			"(window list, virtual outputs or pointer movement)");
		return ScalingError::ScalingFailedGeneral;
	}
	if (options.effects.empty()) {
		return ScalingError::InvalidScalingMode;
	}
	if (options.IsWindowedMode() && options.Is3DGameMode()) {
		return ScalingError::Windowed3DGameMode;
	}
	_options.Prepare();

	ScalingError error = ScalingError::ScalingFailedGeneral;
	if (!_PrepareDesktop(error)) {
		Stop();
		return error;
	}

	// From here on every failure is one of initialisation
	const auto fail = [&](ScalingError result) {
		Stop();
		return result;
	};

	if (!_device.Initialize({ options.graphicsCardId, _config.validation, true })) {
		return fail(ScalingError::ScalingFailedGeneral);
	}

	error = ScalingError::ScalingFailedGeneral;
	if (!_CreateViews(error)) {
		return fail(error);
	}

	// The strip around the source is not part of the picture
	Cropping cropping = _options.cropping;
	cropping.Left += (float)_ringLeft;
	cropping.Top += (float)_ringTop;
	cropping.Right += (float)(_ringRight + _padRight);
	cropping.Bottom += (float)_ringBottom;

	_frameSource = std::make_unique<PipeWireFrameSource>();
	if (!_frameSource->Initialize(_device, {
		.pipeWireFd = _capture->PipeWireFd(),
		.nodeId = _capture->NodeId(),
		.fixedSize = { _virtualOutputRect.Width(), _virtualOutputRect.Height() },
		.cropping = cropping,
		.duplicateFrameDetectionMode = _options.duplicateFrameDetectionMode,
		.isStatisticsForDynamicDetectionEnabled = _options.IsStatisticsForDynamicDetectionEnabled()
	}) || !_WaitForFirstFrame()) {
		return fail(ScalingError::CaptureFailed);
	}

	_chain = std::make_unique<EffectChain>();
	if (!_chain->Initialize(_device, _options, { config.effectsDir, _RendererSize() }, _frameSource->Output())) {
		return fail(ScalingError::ScalingFailedGeneral);
	}
	_chainInput = _frameSource->Output();

	_cursorDrawer = std::make_unique<CursorDrawer>();
	if (!_cursorDrawer->Initialize(_device, _views[0].swapchain->Format())) {
		return fail(ScalingError::ScalingFailedGeneral);
	}

	// Toolbar and profiler. 3D game mode has neither, as on Windows, and a
	// desktop without a usable font is no reason not to scale.
	if (!_options.Is3DGameMode()) {
		_overlay = std::make_unique<OverlayDrawer>();
		if (_overlay->Initialize(_device, _views[0].swapchain->Format(), {
			.iconFontPath = config.iconFontPath,
			.captureMethod = "Virtual output (PipeWire)",
			.isWindowedMode = _options.IsWindowedMode(),
			.isDeveloperMode = _options.IsDeveloperMode(),
			.showDynamicDetectionStatistics = _options.IsStatisticsForDynamicDetectionEnabled() &&
				_options.duplicateFrameDetectionMode == DuplicateFrameDetectionMode::Dynamic
		}, _options.overlayOptions, _chain->ActiveEffectDescs())) {
			_overlay->SetToolbarState(_options.IsWindowedMode() ?
				_options.windowedInitialToolbarState : _options.fullscreenInitialToolbarState);
		} else {
			Logger::Get().Warn("The toolbar is not available");
			_overlay.reset();
		}
	}

	// In benchmark mode every pass through the loop renders, whether the
	// source has a new frame or not, as on Windows
	_stepTimer.Initialize(_options.IsBenchmarkMode() ? std::numeric_limits<float>::max() : _options.minFrameRate,
		_options.maxFrameRate);
	_cursorAutoHide.SetDelay(_options.autoHideCursorDelay);

	_wakeFds = { _frameSource->EventFd(), _bridge->EventFd(), _capture->EventFd() };
	for (const View& view : _views) {
		_wakeFds.push_back(view.surface->EventFd());
	}

	// "Keep screen on" and "simulate exclusive fullscreen" of the Windows
	// version. A desktop that cannot do them is no reason not to scale.
	if (_options.IsKeepScreenOn() || _options.IsSimulateExclusiveFullscreen()) {
		_inhibitor = std::make_unique<SessionInhibitor>();
		if (_options.IsKeepScreenOn()) {
			_inhibitor->KeepScreenOn("A window is scaled");
		}
		if (_options.IsSimulateExclusiveFullscreen()) {
			_inhibitor->SuppressNotifications(config.appId, "A window is scaled to fullscreen");
		}
	}

	_isRunning = true;
	_isSourceActive = true;

	if (_options.IsWindowedMode()) {
		// The window appears with its first frame, and only then can it be
		// given its place
		if (!_Render(true) || !_PlaceOwnWindow()) {
			return fail(ScalingError::ScalingFailedGeneral);
		}
	} else {
		// The first frame is there already
		_hasNewFrame = true;
	}

	if (_options.Is3DGameMode()) {
		// The pointer belongs to the source from the start, wherever it is,
		// as CursorManager does in this mode
		_bridge->MovePointer({
			_virtualOutputRect.left + (_pointerRect.left + _pointerRect.right) / 2,
			_virtualOutputRect.top + (_pointerRect.top + _pointerRect.bottom) / 2
		});
		++_statistics.pointerCaptures;
	}

	Logger::Get().Info("Scaling " + Describe());
	return ScalingError::NoError;
}

// Chooses the monitors and the place of the virtual output, creates it, and
// moves the source window there
bool ScalingSession::_PrepareDesktop(ScalingError& error) noexcept {
	_bridge->ListWindows();
	if (!_bridge->GetWindow(_windowId, _original) || _original.isMinimized) {
		Logger::Get().Error("The window to scale does not exist or is minimized");
		error = ScalingError::InvalidSourceWindow;
		return false;
	}

	_isSourceFilling = _original.isFullscreen || _original.isMaximized;
	if (_isSourceFilling && _options.IsWindowedMode()) {
		Logger::Get().Error("A maximized or fullscreen window cannot be scaled in windowed mode");
		error = ScalingError::BannedInWindowedMode;
		return false;
	}
	if (_isSourceFilling && !_options.IsAllowScalingMaximized()) {
		Logger::Get().Error("The window is maximized or fullscreen, and scaling such windows is not allowed");
		error = ScalingError::Maximized;
		return false;
	}

	// "Capture title bar": the decoration that the desktop draws stays and
	// is part of the picture. Without it the window loses the decoration
	// while it is scaled.
	const bool keepsBorder = _options.IsCaptureTitleBar() && !_original.hasNoBorder && !_original.isFullscreen;
	const Rect& kept = keepsBorder ? _original.frameRect : _original.clientRect;
	const Size sourceSize{ kept.Width(), kept.Height() };
	Logger::Get().Info(fmt::format("The window has a frame of {}x{} and a content of {}x{}",
		_original.frameRect.Width(), _original.frameRect.Height(), sourceSize.width, sourceSize.height));
	if (sourceSize.width < ScalingGeometry::MIN_SRC_SIZE || sourceSize.height < ScalingGeometry::MIN_SRC_SIZE) {
		Logger::Get().Error("The window is too small to scale");
		error = ScalingError::InvalidSourceWindow;
		return false;
	}
	Rect cropped{ 0, 0, sourceSize.width, sourceSize.height };
	if (!ScalingGeometry::CropSourceRect(cropped, _options.cropping)) {
		error = ScalingError::InvalidCropping;
		return false;
	}
	_srcSize = { cropped.Width(), cropped.Height() };

	VirtualOutputPlan plan;
	if (!PlanVirtualOutput({
		.frameRect = _original.frameRect,
		.sourceSize = sourceSize,
		.fillsOutput = _isSourceFilling,
		.isWindowedMode = _options.IsWindowedMode(),
		.is3DGameMode = _options.Is3DGameMode(),
		.multiMonitorUsage = _options.multiMonitorUsage
	}, _bridge->ListOutputs(), plan)) {
		Logger::Get().Error("The window is not on a monitor, or the monitors leave no room for the virtual output");
		error = ScalingError::InvalidSourceWindow;
		return false;
	}
	_monitors = plan.monitors;
	_otherMonitors = plan.otherMonitors;
	_refreshRate = plan.refreshRate;
	_rendererRect = plan.rendererRect;
	_ringLeft = plan.ringLeft;
	_ringTop = plan.ringTop;
	_ringRight = plan.ringRight;
	_ringBottom = plan.ringBottom;
	_padRight = plan.padRight;
	_sourceRect = plan.sourceRect;
	_pointerRect = plan.pointerRect;
	const Size outputSize = plan.size;
	const Point position = plan.position;

	// Not in debug mode, as on Windows
	if (_options.IsAdjustCursorSpeed() && !_options.IsDebugMode()) {
		_pointerDevices = KWinPointerSpeed::ReadDevices();
	}

	// Noted before the desktop is changed, so that a crash can be undone
	_WriteStateFile();

	_capture = _bridge->CreateVirtualOutput("Magpie", outputSize, _virtualOutputName);
	if (!_capture) {
		return false;
	}
	_virtualOutputRect = { position.x, position.y, position.x + outputSize.width, position.y + outputSize.height };
	// The refresh rate of the monitor, so that the source is not held to the
	// 60 Hz a new virtual output has
	if (!_bridge->ConfigureOutput(_virtualOutputName, outputSize, position, _refreshRate)) {
		return false;
	}

	// A fullscreen window shows no decoration whatever this says
	_sourceNoBorder = _original.isFullscreen ? _original.hasNoBorder : !keepsBorder;
	_isWindowMoved = true;
	if (!_bridge->PlaceWindow(_windowId, {
		.frameRect = {
			position.x + _sourceRect.left, position.y + _sourceRect.top,
			position.x + _sourceRect.right, position.y + _sourceRect.bottom
		},
		.noBorder = _sourceNoBorder,
		.activate = true,
		// A maximized or fullscreen window takes the size of the output by itself
		.output = _isSourceFilling ? _virtualOutputName : std::string()
	})) {
		Logger::Get().Error("The window could not be moved to the virtual output");
		error = ScalingError::InvalidSourceWindow;
		return false;
	}

	return true;
}

bool ScalingSession::PlanVirtualOutput(
	const PlanRequest& request,
	std::span<const OutputInfo> outputs,
	VirtualOutputPlan& plan
) noexcept {
	plan = {};

	// The monitor the window is on, by its centre
	const Rect& frameRect = request.frameRect;
	const Point centre{ (frameRect.left + frameRect.right) / 2, (frameRect.top + frameRect.bottom) / 2 };
	const OutputInfo* home = nullptr;
	Rect bounds{ INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN };
	for (const OutputInfo& output : outputs) {
		if (output.name.starts_with("Virtual-")) {
			continue;
		}
		bounds = Union(bounds, output.rect);
		if (!home && Contains(output.rect, centre)) {
			home = &output;
		}
	}
	if (!home) {
		return false;
	}

	// The monitors that show the picture, as
	// ScalingWindow::_CalcFullscreenRendererRect chooses them
	plan.monitors.push_back(*home);
	plan.rendererRect = home->rect;
	plan.refreshRate = home->refreshRate;
	for (const OutputInfo& output : outputs) {
		if (output.name.starts_with("Virtual-") || &output == home) {
			continue;
		}

		bool isUsed = false;
		if (!request.isWindowedMode) {
			if (request.multiMonitorUsage == MultiMonitorUsage::All) {
				isUsed = true;
			} else if (request.multiMonitorUsage == MultiMonitorUsage::Intersected) {
				// A maximized window is on one monitor only
				isUsed = !request.fillsOutput && Overlaps(output.rect, frameRect);
			}
		}

		if (isUsed) {
			plan.monitors.push_back(output);
			plan.rendererRect = Union(plan.rendererRect, output.rect);
			plan.refreshRate = std::max(plan.refreshRate, output.refreshRate);
		} else {
			plan.otherMonitors.push_back(output);
		}
	}

	if (request.isWindowedMode) {
		// Around a window the desktop goes on in every direction
		plan.exitLeft = plan.exitTop = plan.exitRight = plan.exitBottom = true;
	} else if (!request.is3DGameMode) {
		// The sides on which another monitor lies next to the picture; the
		// pointer leaves through them. Without a neighbour the edge of the
		// virtual output keeps the pointer in. In 3D game mode the pointer
		// never leaves.
		const Rect& area = plan.rendererRect;
		for (const OutputInfo& other : plan.otherMonitors) {
			const bool overlapsVertically = other.rect.top < area.bottom && other.rect.bottom > area.top;
			const bool overlapsHorizontally = other.rect.left < area.right && other.rect.right > area.left;
			plan.exitLeft = plan.exitLeft || (overlapsVertically && other.rect.right <= area.left);
			plan.exitRight = plan.exitRight || (overlapsVertically && other.rect.left >= area.right);
			plan.exitTop = plan.exitTop || (overlapsHorizontally && other.rect.bottom <= area.top);
			plan.exitBottom = plan.exitBottom || (overlapsHorizontally && other.rect.top >= area.bottom);
		}
	}

	// A mode with another refresh rate than 60 Hz has to be computed, and the
	// formula for that only yields widths that are multiples of 8
	const bool needsCustomMode = plan.refreshRate > DEFAULT_REFRESH_RATE;
	const Size sourceSize = request.sourceSize;

	if (request.fillsOutput) {
		// The window takes the size of the output, so there is no room for a
		// strip around it; its outermost pixels serve as the strip. A width
		// that is rounded up makes the window that much wider.
		plan.size = sourceSize;
		if (needsCustomMode) {
			plan.size.width += (8 - plan.size.width % 8) % 8;
		}
		plan.sourceRect = { 0, 0, plan.size.width, plan.size.height };
		plan.pointerRect = {
			plan.exitLeft ? RING : 0, plan.exitTop ? RING : 0,
			plan.size.width - (plan.exitRight ? RING : 0), plan.size.height - (plan.exitBottom ? RING : 0)
		};
	} else {
		// A strip on every side with an exit
		plan.ringLeft = plan.exitLeft ? RING : 0;
		plan.ringTop = plan.exitTop ? RING : 0;
		plan.ringRight = plan.exitRight ? RING : 0;
		plan.ringBottom = plan.exitBottom ? RING : 0;
		plan.size = {
			sourceSize.width + plan.ringLeft + plan.ringRight,
			sourceSize.height + plan.ringTop + plan.ringBottom
		};
		// The columns added on the right are cut off like the strips
		if (needsCustomMode) {
			plan.padRight = (8 - plan.size.width % 8) % 8;
			plan.size.width += plan.padRight;
		}
		plan.sourceRect = {
			plan.ringLeft, plan.ringTop, plan.ringLeft + sourceSize.width, plan.ringTop + sourceSize.height
		};
		plan.pointerRect = plan.sourceRect;
	}

	plan.position = { bounds.left, bounds.bottom + VIRTUAL_OUTPUT_GAP };
	plan.position.y = std::min(plan.position.y, MAX_COORDINATE - plan.size.height);
	return plan.position.y - bounds.bottom >= VIRTUAL_OUTPUT_GAP / 2 &&
		plan.position.x + plan.size.width <= MAX_COORDINATE;
}

bool ScalingSession::_AddView(std::unique_ptr<ScalingSurface> surface, const Rect& rect) noexcept {
	VkSurfaceKHR vkSurface = VK_NULL_HANDLE;
	const VkWaylandSurfaceCreateInfoKHR surfaceInfo{
		.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
		.display = surface->Display(),
		.surface = surface->Surface()
	};
	if (vkCreateWaylandSurfaceKHR(_device.Instance(), &surfaceInfo, nullptr, &vkSurface) != VK_SUCCESS) {
		Logger::Get().Error("vkCreateWaylandSurfaceKHR failed");
		return false;
	}

	auto swapchain = std::make_unique<Swapchain>();
	if (!swapchain->Initialize(_device, vkSurface, surface->GetSize(), _config.presentMode)) {
		return false;
	}
	if (!_views.empty() && swapchain->Format() != _views[0].swapchain->Format()) {
		Logger::Get().Error("The monitors take pictures in different formats, which is not supported");
		return false;
	}

	_views.push_back({ std::move(surface), std::move(swapchain), rect });
	return true;
}

bool ScalingSession::_CreateViews(ScalingError& error) noexcept {
	if (!_options.IsWindowedMode()) {
		// One surface per monitor; together they show one picture
		for (const OutputInfo& monitor : _monitors) {
			auto surface = std::make_unique<ScalingSurface>();
			if (!surface->CreateLayer(monitor.name, !_options.IsTopmostDisabled()) ||
				!_AddView(std::move(surface), monitor.rect)) {
				return false;
			}
		}
		return true;
	}

	// A window with the aspect ratio of the source. On Windows it has to be
	// larger than the source, which lies below it there; here the source is
	// elsewhere, and the picture only may not be smaller than the source.
	Rect bounds = _monitors[0].rect;
	for (const OutputInfo& monitor : _otherMonitors) {
		bounds = Union(bounds, monitor.rect);
	}
	_minWindowedSize = _srcSize;
	_maxWindowedSize = { bounds.Width(), bounds.Height() };

	const Rect& monitor = _monitors[0].rect;
	Size size;
	if (!ScalingGeometry::CalcInitialWindowedRendererSize(size, _srcSize, { monitor.Width(), monitor.Height() },
		_options.initialWindowedScaleFactor, _config.lastWindowedWidth, _minWindowedSize, _maxWindowedSize)) {
		Logger::Get().Error("The window is too large for windowed scaling");
		error = ScalingError::InvalidSourceWindow;
		return false;
	}

	auto surface = std::make_unique<ScalingSurface>();
	if (!surface->CreateWindow(_original.title.empty() ? "Magpie" : _original.title, _config.appId, size,
		_minWindowedSize, [this](Size proposed, bool isWidthChanged, bool isHeightChanged) {
			return ScalingGeometry::CalcResizedWindowedRendererSize(
				proposed, isWidthChanged, isHeightChanged, _srcSize, _minWindowedSize, _maxWindowedSize);
		})) {
		return false;
	}

	// Where the window is, is known once it has appeared; until then it is
	// taken to be where the source was
	const Point centre{
		(_original.frameRect.left + _original.frameRect.right) / 2,
		(_original.frameRect.top + _original.frameRect.bottom) / 2
	};
	_rendererRect = {
		centre.x - size.width / 2, centre.y - size.height / 2,
		centre.x - size.width / 2 + size.width, centre.y - size.height / 2 + size.height
	};
	_windowedWidth = size.width;
	return _AddView(std::move(surface), _rendererRect);
}

bool ScalingSession::_PlaceOwnWindow() noexcept {
	// The desktop lists the window after its first frame
	WindowInfo own;
	const auto deadline = steady_clock::now() + 2s;
	while (true) {
		bool found = false;
		for (const WindowInfo& window : _bridge->ListWindows()) {
			if (window.pid == (uint32_t)getpid() && window.id != _windowId) {
				own = window;
				found = true;
			}
		}
		if (found) {
			break;
		}
		if (steady_clock::now() > deadline) {
			Logger::Get().Error("The window with the scaled picture did not appear");
			return false;
		}
		if (!_views[0].surface->PumpEvents()) {
			return false;
		}
		usleep(10'000);
	}
	_ownWindowId = own.id;

	// The content gets the centre that the source had, as on Windows, but
	// the window stays on its monitor if it fits there. The title bar wins
	// over the bottom edge.
	const Rect& content = _rendererRect;
	Rect frame{
		content.left - (own.clientRect.left - own.frameRect.left),
		content.top - (own.clientRect.top - own.frameRect.top),
		0, 0
	};
	const Rect& monitor = _monitors[0].rect;
	frame.left = std::max(std::min(frame.left, monitor.right - own.frameRect.Width()), monitor.left);
	frame.top = std::max(std::min(frame.top, monitor.bottom - own.frameRect.Height()), monitor.top);
	frame.right = frame.left + own.frameRect.Width();
	frame.bottom = frame.top + own.frameRect.Height();

	if (!_bridge->PlaceWindow(_ownWindowId, { .frameRect = frame, .noBorder = own.hasNoBorder })) {
		Logger::Get().Warn("The window with the scaled picture could not be put where the source was");
	}
	// The source stands for both in the task bar and the window switcher
	_bridge->SetWindowProperties(_ownWindowId, {
		.keepAbove = !_options.IsTopmostDisabled(),
		.skipTaskbar = true,
		.raise = true
	});
	_isOwnWindowKeptAbove = !_options.IsTopmostDisabled();
	// A new window may have taken the focus; it belongs to the source
	_bridge->PlaceWindow(_windowId, { .noBorder = _sourceNoBorder, .activate = true, .setGeometry = false });

	return _TrackOwnWindow();
}

bool ScalingSession::_TrackOwnWindow() noexcept {
	WindowInfo own;
	if (!_bridge->GetWindow(_ownWindowId, own)) {
		_endReason = EndReason::WindowClosed;
		return false;
	}
	if (own.isMinimized) {
		// Minimizing the picture means minimizing what it shows; scaling
		// goes on when the source is shown again
		_bridge->SetWindowProperties(_windowId, { .minimized = true });
		_endReason = EndReason::SourceMinimized;
		return false;
	}
	if (own.isActive) {
		// The user clicked the title bar. The keyboard belongs to the source.
		_bridge->PlaceWindow(_windowId, { .noBorder = _sourceNoBorder, .activate = true, .setGeometry = false });
	}

	// The place from the desktop, the size from the surface, which is ahead
	// of the desktop while the user resizes
	View& view = _views[0];
	const Size size = view.surface->GetSize();
	view.rect = {
		own.clientRect.left, own.clientRect.top,
		own.clientRect.left + size.width, own.clientRect.top + size.height
	};
	_rendererRect = view.rect;
	return true;
}

bool ScalingSession::_WaitForFirstFrame() noexcept {
	const auto deadline = steady_clock::now() + 5s;
	while (true) {
		pollfd fds[2]{ { _frameSource->EventFd(), POLLIN, 0 }, { _capture->EventFd(), POLLIN, 0 } };
		poll(fds, 2, 50);
		if (!_capture->Dispatch() || !_frameSource->Dispatch()) {
			Logger::Get().Error("The screen cast of the virtual output ended before its first frame");
			return false;
		}

		const FrameSourceState state = _frameSource->Update();
		if (state == FrameSourceState::NewFrame) {
			return true;
		}
		if (state == FrameSourceState::Error) {
			return false;
		}
		if (steady_clock::now() > deadline) {
			Logger::Get().Error("The virtual output delivered no frame within 5 seconds");
			return false;
		}
	}
}

Rect ScalingSession::_DestRect() const noexcept {
	const Texture& output = *_chain->Output();
	return ScalingGeometry::CalcDestRect(_RendererSize(),
		{ (int32_t)output.width, (int32_t)output.height }, _options.outputAlignment);
}

bool ScalingSession::_UpdateSizes(bool& resized) noexcept {
	resized = false;
	for (View& view : _views) {
		const Size size = view.surface->GetSize();
		if (size == Size{ view.rect.Width(), view.rect.Height() } && size == view.swapchain->GetSize()) {
			continue;
		}

		if (!view.swapchain->Resize(size)) {
			return false;
		}
		view.rect.right = view.rect.left + size.width;
		view.rect.bottom = view.rect.top + size.height;
		resized = true;
	}
	if (!resized) {
		return true;
	}

	if (_options.IsWindowedMode()) {
		_rendererRect = _views[0].rect;
		if (!_views[0].surface->IsSizeFixed()) {
			_windowedWidth = _rendererRect.Width();
		}
	} else {
		_rendererRect = _views[0].rect;
		for (const View& view : _views) {
			_rendererRect = Union(_rendererRect, view.rect);
		}
	}

	if (!_chain->Resize(_RendererSize(), _frameSource->Output())) {
		return false;
	}
	_chainInput = _frameSource->Output();
	if (_overlay) {
		// With another size the chain may have another last effect
		_overlay->UpdateAfterActiveEffectsChanged(_chain->ActiveEffectDescs());
	}
	return true;
}

bool ScalingSession::Step(std::span<const int> wakeFds) noexcept {
	if (!_isRunning) {
		return false;
	}

	std::vector<int> fds = _wakeFds;
	fds.insert(fds.end(), wakeFds.begin(), wakeFds.end());
	_stepTimer.SetWakeDescriptors(fds);

	bool fpsUpdated = false;
	_stepStatus = _stepTimer.WaitForNextFrame(
		!_hasNewFrame && _stepStatus != StepTimerStatus::WaitingForFPSLimiter, fpsUpdated);

	for (const View& view : _views) {
		if (!view.surface->PumpEvents()) {
			_endReason = EndReason::Failed;
			return false;
		}
		if (view.surface->IsClosed()) {
			// A window is closed by the user, a layer surface by the desktop
			_endReason = _options.IsWindowedMode() ? EndReason::WindowClosed : EndReason::Failed;
			return false;
		}
	}
	if (!_bridge->Dispatch()) {
		_endReason = EndReason::Failed;
		return false;
	}
	if (!_capture->Dispatch() || !_frameSource->Dispatch()) {
		_endReason = EndReason::CaptureFailed;
		return false;
	}
	if (_options.IsWindowedMode() && !_TrackOwnWindow()) {
		return false;
	}
	if (!_TrackSource()) {
		return false;
	}

	bool resized = false;
	if (!_UpdateSizes(resized)) {
		_endReason = EndReason::Failed;
		return false;
	}

	// The frame source, unless the frame rate limit says to wait. The cursor
	// is not held back by the limit.
	bool runEffects = resized;
	if (_stepStatus != StepTimerStatus::WaitingForFPSLimiter) {
		const FrameSourceState state = _hasNewFrame ? FrameSourceState::NewFrame : _frameSource->Update();
		_hasNewFrame = false;

		if (state == FrameSourceState::Error) {
			_endReason = EndReason::CaptureFailed;
			return false;
		}
		runEffects = runEffects || state == FrameSourceState::NewFrame ||
			_stepStatus == StepTimerStatus::ForceNewFrame;
	}

	const Rect destRect = _DestRect();
	bool overlayNeedsRedraw = false;
	if (!_UpdateOverlay(destRect, overlayNeedsRedraw)) {
		return false;
	}
	_UpdatePointer(destRect);

	// The cursor to draw: that of the stream, unless it has rested for
	// longer than the user allows
	_cursor = _frameSource->CursorState();
	_cursor.isVisible = _cursorAutoHide.Update(
		_cursor.isVisible, _cursor.pos, _cursor.imageSerial, steady_clock::now());
	_stepTimer.SetWakeTime(_cursorAutoHide.HideTime());

	const bool cursorChanged = _cursor.isVisible != _drawnCursor.isVisible ||
		(_cursor.isVisible && (_cursor.pos != _drawnCursor.pos || _cursor.imageSerial != _drawnCursor.imageSerial));
	if (!runEffects && !cursorChanged && !overlayNeedsRedraw) {
		return true;
	}

	if (!_Render(runEffects)) {
		_endReason = EndReason::Failed;
		return false;
	}
	if (runEffects && _chain->Profiler().IsProfiling()) {
		// The frame has been executed; its timings are shown with the next one
		_chain->Profiler().QueryTimings();
	}
	return true;
}

bool ScalingSession::_UpdateOverlay(const Rect& destRect, bool& needsRedraw) noexcept {
	needsRedraw = std::exchange(_isOverlayRedrawForced, false);

	// What the pointer did on the surfaces. It is taken in any case, so that
	// nothing piles up.
	bool hasInput = false;
	SmallVector<std::pair<int, bool>, 4> buttonEvents;
	const auto addButtonEvent = [&](int button, bool isDown) {
		if (_overlayButtons[button] != isDown) {
			_overlayButtons[button] = isDown;
			buttonEvents.emplace_back(button, isDown);
			hasInput = true;
		}
	};
	for (const View& view : _views) {
		for (const ScalingSurface::PointerEvent& event : view.surface->TakePointerEvents()) {
			using Type = ScalingSurface::PointerEvent::Type;
			if ((event.type == Type::ButtonDown || event.type == Type::ButtonUp) && event.button < 2) {
				addButtonEvent(event.button, event.type == Type::ButtonDown);
			} else if (event.type == Type::Scroll) {
				// The desktop reports 15 units per notch, positive downwards
				_overlayWheel -= event.value / 15.0f;
				hasInput = true;
			} else if (event.type == Type::Leave) {
				// A button that is held is released for the overlay
				addButtonEvent(0, false);
				addButtonEvent(1, false);
			}
		}
	}
	if (!_overlay) {
		return true;
	}

	// Where the pointer is on the picture: where the cursor of the source is
	// drawn, or where the pointer itself is if it is on a surface. Only
	// there can it click on the overlay.
	std::optional<Point> mousePos;
	bool canClick = false;
	const CaptureCursorState& cursor = _frameSource->CursorState();
	if (cursor.isVisible) {
		const Size rendererSize = _RendererSize();
		const Point onRenderer = ScalingGeometry::SrcToScaling(cursor.pos, _frameSource->SrcRect(), destRect,
			{ 0, 0, rendererSize.width, rendererSize.height }, false);
		mousePos = Point{ onRenderer.x - destRect.left, onRenderer.y - destRect.top };
	} else {
		for (const View& view : _views) {
			if (view.surface->IsPointerInside()) {
				const Point onSurface = view.surface->PointerPos();
				mousePos = Point{
					view.rect.left - _rendererRect.left + onSurface.x - destRect.left,
					view.rect.top - _rendererRect.top + onSurface.y - destRect.top
				};
				canClick = true;
			}
		}
	}

	const uint32_t fps = _stepTimer.GetFPS();
	needsRedraw = needsRedraw || hasInput || mousePos != _overlayMousePos || _overlay->NeedRedraw(fps, mousePos);
	_overlayMousePos = mousePos;

	SmallVector<float> timings;
	if (_chain->Profiler().IsProfiling()) {
		timings = _chain->Profiler().GetTimings();
	}

	OverlayDrawer::Requests requests;
	_overlay->Update({
		.displaySize = { destRect.Width(), destRect.Height() },
		.mousePos = mousePos,
		.canClick = canClick,
		.buttonEvents = buttonEvents,
		.wheel = std::exchange(_overlayWheel, 0.0f),
		.fps = fps,
		.effectTimings = timings,
		.dynamicDetectionStatistics = _frameSource->GetStatisticsForDynamicDetection()
	}, requests);

	// The effects are timed while the profiler shows the timings
	if (_overlay->IsProfilerVisible() != _chain->Profiler().IsProfiling()) {
		if (_overlay->IsProfilerVisible()) {
			_chain->Profiler().Start(_device, _chain->PassCount());
		} else {
			_chain->Profiler().Stop();
		}
		needsRedraw = true;
	}

	// The arrow of the desktop while the pointer is to stay on a surface
	const bool showArrow = _overlay->WantsMouse() || _isCaptureBlocked;
	for (const View& view : _views) {
		view.surface->SetCursorShown(showArrow);
	}

	if (requests.takeScreenshot) {
		TakeScreenshot(requests.screenshotEffect);
	}
	if (requests.minimizeSource) {
		// Scaling ends with it, and goes on when the source is shown again
		_bridge->SetWindowProperties(_windowId, { .minimized = true });
	}
	if (requests.stopScaling) {
		_endReason = EndReason::UserStopped;
		return false;
	}
	if (requests.switchMode) {
		_endReason = EndReason::ModeSwitchRequested;
		return false;
	}
	return true;
}

void ScalingSession::SwitchToolbarState() noexcept {
	if (!_isRunning) {
		return;
	}
	const auto tell = [&](const std::string& message) {
		Logger::Get().Info(message);
		if (_config.showMessage) {
			_config.showMessage(message);
		}
	};
	if (!_overlay) {
		tell("The toolbar is not supported in 3D game mode.");
		return;
	}

	const ToolbarState newState = (ToolbarState)(((uint32_t)_overlay->GetToolbarState() + 1) % (uint32_t)ToolbarState::COUNT);
	_overlay->SetToolbarState(newState);
	// The new state is told, as on Windows: with the toolbar hidden until
	// the pointer is near, the change cannot be seen otherwise
	static constexpr const char* NAMES[]{ "Off", "Always show", "Auto-hide" };
	tell(std::string("Toolbar: ") + NAMES[(size_t)newState]);
	_isOverlayRedrawForced = true;
}

// Port of what SrcTracker::UpdateState and ScalingWindow::_UpdateSrcState
// watch for, as far as it applies to a source on a virtual output
bool ScalingSession::_TrackSource() noexcept {
	WindowInfo info;
	if (!_bridge->GetWindow(_windowId, info)) {
		_endReason = EndReason::SourceClosed;
		return false;
	}
	if (info.isMinimized) {
		_endReason = EndReason::SourceMinimized;
		return false;
	}

	const Rect expected{
		_virtualOutputRect.left + _sourceRect.left, _virtualOutputRect.top + _sourceRect.top,
		_virtualOutputRect.left + _sourceRect.right, _virtualOutputRect.top + _sourceRect.bottom
	};
	if (info.isFullscreen != _original.isFullscreen || info.isMaximized != _original.isMaximized ||
		info.frameRect.Width() != expected.Width() || info.frameRect.Height() != expected.Height()) {
		Logger::Get().Info(fmt::format("The source is now {}x{}{}{}", info.frameRect.Width(), info.frameRect.Height(),
			info.isFullscreen ? ", fullscreen" : "", info.isMaximized ? ", maximized" : ""));
		_endReason = EndReason::SourceResized;
		return false;
	}
	if (info.frameRect != expected) {
		Logger::Get().Info(fmt::format("The source is now at {},{}", info.frameRect.left, info.frameRect.top));
		_endReason = EndReason::SourceMoved;
		return false;
	}

	const bool wasSourceActive = _isSourceActive;
	_isSourceActive = info.isActive;

	// The window that is active in place of the source, unless it is on the
	// virtual output: those are dialogs and menus of the source and part of
	// the picture.
	WindowInfo other;
	bool isOtherActive = false;
	if (!_isSourceActive) {
		const std::string active = _bridge->ActiveWindowId();
		isOtherActive = !active.empty() && active != _windowId && active != _ownWindowId &&
			_bridge->GetWindow(active, other) && !Overlaps(other.frameRect, _virtualOutputRect);
	}

	if (!_options.IsWindowedMode()) {
		// On Windows the scaled picture then goes behind that window; a
		// layer surface cannot, so scaling ends if the window is on a
		// monitor that the picture covers.
		if (isOtherActive) {
			// The tolerance of ScalingWindow::_CheckForegroundFor3DGameMode
			static constexpr int32_t TOLERANCE = 8;
			const Rect& frame = other.frameRect;
			for (const OutputInfo& monitor : _monitors) {
				const Rect& rect = monitor.rect;
				const int32_t overlapWidth = std::min(frame.right, rect.right) - std::max(frame.left, rect.left);
				const int32_t overlapHeight = std::min(frame.bottom, rect.bottom) - std::max(frame.top, rect.top);
				if (overlapWidth >= TOLERANCE && overlapHeight >= TOLERANCE) {
					_endReason = EndReason::FocusLost;
					return false;
				}
			}
		}
		return true;
	}

	// Windowed mode. While the source is the active window, the window with
	// its picture is kept above the others, as ScalingWindow::_CalcTopmostState
	// has it; it comes to the front with the source in any case.
	const bool keepAbove = _isSourceActive && !_options.IsTopmostDisabled();
	const bool becameActive = _isSourceActive && !wasSourceActive;
	if (keepAbove != _isOwnWindowKeptAbove || becameActive) {
		_isOwnWindowKeptAbove = keepAbove;
		_bridge->SetWindowProperties(_ownWindowId, { .keepAbove = keepAbove, .raise = becameActive });
	}

	// The user went to another window, with the keyboard for example. The
	// pointer follows to where its picture was, so that it can be used
	// there. The desktop may report the source as inactive before it reports
	// the other window as active, so this is done once per loss of focus and
	// not at the moment the source loses it.
	if (_isSourceActive) {
		_isFocusLossHandled = false;
	} else if (isOtherActive && !_isFocusLossHandled) {
		_isFocusLossHandled = true;
		if (_frameSource->CursorState().isVisible) {
			_ReleasePointerToPicture(_DestRect());
			++_statistics.pointerReleases;
			_isCaptureBlocked = true;
			_captureBlockSettleTime = steady_clock::now() + 150ms;
		}
	}

	return true;
}

void ScalingSession::_ReleasePointerToPicture(const Rect& destRect) noexcept {
	const Size rendererSize = _RendererSize();
	const Rect rendererArea{ 0, 0, rendererSize.width, rendererSize.height };
	const Point scaled = ClampTo(ScalingGeometry::SrcToScaling(_frameSource->CursorState().pos,
		_frameSource->SrcRect(), destRect, rendererArea, false), rendererArea);
	_SetPointerSlowed(false, destRect);
	_bridge->MovePointer({ _rendererRect.left + scaled.x, _rendererRect.top + scaled.y });
}

// Port of the capture logic of CursorManager for a pointer that is either on
// the virtual output (captured: it acts on the source, and its picture is
// drawn scaled) or on a real monitor (released)
void ScalingSession::_UpdatePointer(const Rect& destRect) noexcept {
	const CaptureCursorState& cursor = _frameSource->CursorState();
	const Size rendererSize = _RendererSize();
	const Rect rendererArea{ 0, 0, rendererSize.width, rendererSize.height };
	const Point origin{ _virtualOutputRect.left, _virtualOutputRect.top };

	// The pointer is over the toolbar or the profiler, or holds one of their
	// controls. Clicks have to reach them and not the source, so the pointer
	// goes to the surface with the picture and stays there, as CursorManager
	// does when the cursor is on the overlay.
	const bool overlayWantsPointer = _overlay && _overlay->WantsMouse();

	if (cursor.isVisible) {
		if (overlayWantsPointer) {
			const auto now = steady_clock::now();
			if (now - _lastOverlayReleaseTime >= 50ms) {
				_lastOverlayReleaseTime = now;
				_ReleasePointerToPicture(destRect);
				++_statistics.pointerReleases;
			}
			return;
		}

		// Captured. Inside the source nothing is to be done.
		_SetPointerSlowed(true, destRect);
		if (Contains(_pointerRect, cursor.pos)) {
			_handledRingPos = { -1, -1 };
			return;
		}
		// In the strip around the source: the user pushes against an edge
		// behind which the desktop goes on
		if (cursor.pos == _handledRingPos) {
			return;
		}
		_handledRingPos = cursor.pos;

		// Where that is on the desktop: just outside the area with the
		// picture, with black bars skipped, as _StopCapture does. The strip
		// of a source that fills its output is inside the source and counts
		// as the first pixel outside.
		const Rect& srcRect = _frameSource->SrcRect();
		Point outside = cursor.pos;
		if (outside.x < _pointerRect.left) {
			outside.x = std::min(outside.x, srcRect.left - 1);
		} else if (outside.x >= _pointerRect.right) {
			outside.x = std::max(outside.x, srcRect.right);
		}
		if (outside.y < _pointerRect.top) {
			outside.y = std::min(outside.y, srcRect.top - 1);
		} else if (outside.y >= _pointerRect.bottom) {
			outside.y = std::max(outside.y, srcRect.bottom);
		}
		const Point scaled = ScalingGeometry::SrcToScaling(outside, srcRect, destRect, rendererArea, true);
		const Point target{ _rendererRect.left + scaled.x, _rendererRect.top + scaled.y };

		// It has to be on a monitor. Next to a fullscreen picture that is
		// one of the other monitors; next to a window any monitor will do.
		const auto isOnOneOf = [&](const std::vector<OutputInfo>& monitors) {
			return std::ranges::any_of(monitors, [&](const OutputInfo& monitor) {
				return Contains(monitor.rect, target);
			});
		};
		if (isOnOneOf(_otherMonitors) || (_options.IsWindowedMode() && isOnOneOf(_monitors))) {
			_SetPointerSlowed(false, destRect);
			_bridge->MovePointer(target);
			++_statistics.pointerReleases;
			return;
		}

		// No monitor there: the pointer stays with the source
		const Point back = ClampTo(cursor.pos, _pointerRect);
		_bridge->MovePointer({ origin.x + back.x, origin.y + back.y });
		return;
	}

	_handledRingPos = { -1, -1 };

	if (overlayWantsPointer) {
		return;
	}

	// Released, or the source hides the cursor. If the pointer is on the
	// scaled picture, it goes to the point of the source that it shows there,
	// as _StartCapture does.
	for (const View& view : _views) {
		if (!view.surface->IsPointerInside()) {
			continue;
		}

		const auto now = steady_clock::now();
		if (_isCaptureBlocked) {
			// Not before the pointer has moved again, and the move that sent
			// it here does not count
			const uint32_t motionCount = view.surface->PointerMotionCount();
			if (now < _captureBlockSettleTime) {
				_captureBlockMotionCount = motionCount;
				return;
			}
			if (motionCount == _captureBlockMotionCount) {
				return;
			}
			_isCaptureBlocked = false;
		}

		// Not again before the move has had time to take effect
		if (now - _lastCaptureTime < 50ms) {
			return;
		}
		_lastCaptureTime = now;

		const Rect& srcRect = _frameSource->SrcRect();
		const Point onSurface = view.surface->PointerPos();
		const Point onRenderer{
			view.rect.left - _rendererRect.left + onSurface.x,
			view.rect.top - _rendererRect.top + onSurface.y
		};
		const Point onPicture = ClampTo(onRenderer, destRect);
		const Point onSource = ClampTo(
			ClampTo(ScalingGeometry::ScalingToSrc(onPicture, srcRect, destRect), srcRect), _pointerRect);
		_SetPointerSlowed(true, destRect);
		_bridge->MovePointer({ origin.x + onSource.x, origin.y + onSource.y });
		++_statistics.pointerCaptures;
		return;
	}
}

// Port of CursorManager::_AdjustCursorSpeed and _RestoreCursorSpeed
void ScalingSession::_SetPointerSlowed(bool slowed, const Rect& destRect) noexcept {
	if (_pointerDevices.empty() || slowed == _isPointerSlowed) {
		return;
	}
	_isPointerSlowed = slowed;

	if (!slowed) {
		KWinPointerSpeed::Apply(_pointerDevices);
		return;
	}

	// On the virtual output the pointer moves in pixels of the source, which
	// the picture shows enlarged. Slowed by that factor it moves over the
	// picture as fast as over the rest of the desktop.
	const Rect& srcRect = _frameSource->SrcRect();
	const double scale = ((double)destRect.Width() / srcRect.Width() +
		(double)destRect.Height() / srcRect.Height()) / 2;

	std::vector<KWinPointerSpeed::Device> devices = _pointerDevices;
	for (KWinPointerSpeed::Device& device : devices) {
		device.acceleration = KWinPointerSpeed::Slowed(device.acceleration, scale);
	}
	KWinPointerSpeed::Apply(devices);
}

bool ScalingSession::_Render(bool runEffects) noexcept {
	if (runEffects) {
		if (_frameSource->Output() != _chainInput) {
			if (!_chain->Resize(_RendererSize(), _frameSource->Output())) {
				return false;
			}
			_chainInput = _frameSource->Output();
		}
		_stepTimer.PrepareForRender();
	}

	const CaptureCursorState cursor = _cursor;
	if (cursor.isVisible && cursor.imageSerial != _loadedCursorImage) {
		const CaptureCursorImage& image = _frameSource->CursorImage();
		if (!_cursorDrawer->SetImage(image.pixels, image.size, image.hotSpot)) {
			return false;
		}
		_loadedCursorImage = cursor.imageSerial;
	}

	// The effects are drawn once, with the first surface
	bool drawEffects = runEffects;
	for (View& view : _views) {
		const auto draw = [&](VkCommandBuffer cmd, const Swapchain::Frame& frame) {
			if (drawEffects) {
				_chain->Draw(cmd, _stepTimer.GetFrameCount());
			}

			// The renderer area and the picture in it, relative to this surface
			const Size viewSize = view.swapchain->GetSize();
			const Point offset{ _rendererRect.left - view.rect.left, _rendererRect.top - view.rect.top };
			const Rect rendererArea{
				offset.x, offset.y, offset.x + _rendererRect.Width(), offset.y + _rendererRect.Height()
			};
			Rect destRect = _DestRect();
			destRect = {
				destRect.left + offset.x, destRect.top + offset.y,
				destRect.right + offset.x, destRect.bottom + offset.y
			};
			Swapchain::Compose(cmd, *_chain->Output(), frame.image, viewSize, destRect);

			// Overlay and cursor are drawn onto the frame, in this order
			if (_overlay) {
				_overlay->Draw(cmd, frame.image, frame.view, viewSize, { destRect.left, destRect.top });
			}

			if (cursor.isVisible) {
				const Rect& srcRect = _frameSource->SrcRect();
				const Point cursorPos = ScalingGeometry::SrcToScaling(cursor.pos, srcRect, destRect, rendererArea, false);
				_cursorDrawer->Draw(cmd, { frame.image, frame.view, viewSize, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL },
					cursorPos, { srcRect.Width(), srcRect.Height() }, destRect, _options);
			}
		};

		Swapchain::FrameResult result = view.swapchain->PresentFrame(draw);
		if (result == Swapchain::FrameResult::NeedsResize) {
			// The surface changed under the frame; once more with the new size
			bool resized = false;
			if (!_UpdateSizes(resized) || (!resized && !view.swapchain->Resize(view.surface->GetSize()))) {
				return false;
			}
			drawEffects = true;
			runEffects = true;
			result = view.swapchain->PresentFrame(draw);
		}
		if (result != Swapchain::FrameResult::Presented) {
			return false;
		}
		drawEffects = false;
	}

	_drawnCursor = cursor;
	++_statistics.presentedFrames;
	if (runEffects) {
		++_statistics.scaledFrames;
	}
	return true;
}

bool ScalingSession::TakeScreenshot(std::optional<uint32_t> effectIndex) noexcept {
	if (!_isRunning) {
		return false;
	}
	if (effectIndex && *effectIndex >= _chain->ActiveEffectDescs().size()) {
		Logger::Get().Error(fmt::format("There is no effect number {}", *effectIndex));
		return false;
	}

	// Between two steps the GPU is idle and the textures hold the last frame
	const Texture& texture = effectIndex ? *_chain->EffectOutput(*effectIndex) : *_chain->Output();
	ImageData image{ .format = texture.format, .width = texture.width, .height = texture.height };
	if (!_device.ReadTexture(texture, image.pixels)) {
		Logger::Get().Error("Reading the picture for the screenshot failed");
		return false;
	}

	WindowInfo info;
	if (!_bridge->GetWindow(_windowId, info)) {
		info = _original;
	}

	// Encoding takes long enough to be seen as a stutter, so it is done on
	// another thread; one screenshot at a time
	if (_screenshotThread.joinable()) {
		_screenshotThread.join();
	}
	_screenshotThread = std::jthread([image = std::move(image), dir = _options.screenshotsDir,
		filenameTemplate = _options.screenshotFilenameTemplate, title = info.title,
		processName = ProcessInfo::GetName(info.pid), showMessage = _config.showMessage]() mutable {
		std::filesystem::path savedFile;
		const Screenshot::Result result = Screenshot::Save(
			std::move(image), dir, filenameTemplate, title, processName, savedFile);

		// The texts of the Windows version
		std::string message;
		switch (result) {
		case Screenshot::Result::Success:
			message = "Screenshot saved as " + savedFile.filename().string();
			Logger::Get().Info("Screenshot saved as " + savedFile.string());
			break;
		case Screenshot::Result::InvalidDirectory:
			message = "Screenshot failed: Screenshot save directory is invalid.";
			Logger::Get().Error("The screenshot was not saved: the directory \"" + dir.string() + "\" cannot be used");
			break;
		case Screenshot::Result::InvalidFilenameTemplate:
			message = "Screenshot failed: Screenshot filename format is invalid.";
			Logger::Get().Error("The screenshot was not saved: the file name template \"" +
				filenameTemplate + "\" is not valid");
			break;
		default:
			message = "Screenshot failed: An internal error occurred. Please refer to the logs for more details.";
			Logger::Get().Error("The screenshot was not saved");
			break;
		}
		if (showMessage) {
			showMessage(message);
		}
	});
	return true;
}

void ScalingSession::Stop() noexcept {
	if (!_bridge) {
		return;
	}

	if (_screenshotThread.joinable()) {
		_screenshotThread.join();
	}

	// The pointer goes to where the user sees it, as the destructor of
	// CursorManager does
	if (_isRunning && _frameSource->CursorState().isVisible) {
		_ReleasePointerToPicture(_DestRect());
	}

	if (_isPointerSlowed) {
		_isPointerSlowed = false;
		KWinPointerSpeed::Apply(_pointerDevices);
	}

	// Set if the window has another size than before scaling
	std::optional<KWinBridge::Placement> placeAgain;

	// The window goes back while the picture still covers the monitor, and
	// before its output disappears
	if (_isWindowMoved) {
		_bridge->ListWindows();
		WindowInfo info;
		if (_bridge->GetWindow(_windowId, info)) {
			KWinBridge::Placement placement{ .noBorder = _original.hasNoBorder, .activate = info.isActive };
			if (info.isFullscreen || info.isMaximized || _isSourceFilling) {
				// The desktop decides the place of such a window on its
				// monitor. That place is known if the window is in the state
				// it had before scaling.
				placement.output = _monitors[0].name;
				if (info.isFullscreen == _original.isFullscreen && info.isMaximized == _original.isMaximized) {
					placement.frameRect = _original.frameRect;
				}
				Logger::Get().Info("Putting the window back on " + _monitors[0].name);
				_bridge->PlaceWindow(_windowId, placement);
			} else {
				// The place it had, with the size it has now: the window may
				// have been resized while it was scaled
				Rect frame = _original.frameRect;
				frame.right += info.frameRect.Width() - _sourceRect.Width();
				frame.bottom += info.frameRect.Height() - _sourceRect.Height();
				Logger::Get().Info(fmt::format("Putting the window back: it is {}x{}, and goes to {},{} with {}x{}",
					info.frameRect.Width(), info.frameRect.Height(), frame.left, frame.top, frame.Width(), frame.Height()));
				placement.frameRect = frame;
				_bridge->PlaceWindow(_windowId, placement);
				if (frame != _original.frameRect) {
					placeAgain = placement;
				}
			}
		}
		_isWindowMoved = false;
	}

	// In this order: everything that draws, the surfaces, the capture
	_overlay.reset();
	_cursorDrawer.reset();
	_chain.reset();
	_chainInput.reset();
	_views.clear();
	_frameSource.reset();
	// Removes the virtual output
	_capture.reset();
	_inhibitor.reset();

	// When an output goes away, KWin gives the windows the geometry they had
	// before that output existed. That is the old size of a window that was
	// resized while it was scaled, so its new size is set once more.
	if (placeAgain) {
		_bridge->PlaceWindow(_windowId, *placeAgain);
	}

	if (!_config.stateFile.empty()) {
		std::error_code ec;
		std::filesystem::remove(_config.stateFile, ec);
	}

	if (_isRunning) {
		Logger::Get().Info(fmt::format("Scaling ended: {} frames scaled, {} presented; "
			"the pointer went to the source {} times and away from it {} times",
			_statistics.scaledFrames, _statistics.presentedFrames,
			_statistics.pointerCaptures, _statistics.pointerReleases));
	}
	_isRunning = false;
	_bridge = nullptr;
}

Rect ScalingSession::SourceRect() const noexcept {
	if (!_frameSource) {
		return {};
	}
	const Rect& srcRect = _frameSource->SrcRect();
	return {
		_virtualOutputRect.left + srcRect.left, _virtualOutputRect.top + srcRect.top,
		_virtualOutputRect.left + srcRect.right, _virtualOutputRect.top + srcRect.bottom
	};
}

Rect ScalingSession::DestinationRect() const noexcept {
	if (!_chain) {
		return {};
	}
	const Rect destRect = _DestRect();
	return {
		_rendererRect.left + destRect.left, _rendererRect.top + destRect.top,
		_rendererRect.left + destRect.right, _rendererRect.top + destRect.bottom
	};
}

std::string ScalingSession::Describe() const noexcept {
	if (!_frameSource || !_chain) {
		return "nothing";
	}

	std::string where;
	if (_options.IsWindowedMode()) {
		where = fmt::format("in a window of {}x{}", _rendererRect.Width(), _rendererRect.Height());
	} else {
		for (const OutputInfo& monitor : _monitors) {
			where += (where.empty() ? "on " : ", ") + monitor.name;
		}
	}

	const Rect& srcRect = _frameSource->SrcRect();
	const Texture& output = *_chain->Output();
	return fmt::format("{}x{} on {} ({:.2f} Hz) to {}x{} {}; stream: {}",
		srcRect.Width(), srcRect.Height(), _virtualOutputName, _refreshRate / 1000.0,
		output.width, output.height, where, _frameSource->DescribeFormat());
}

}
