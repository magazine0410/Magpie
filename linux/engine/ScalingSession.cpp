#include "ScalingSession.h"
#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <poll.h>
#include <fmt/format.h>
#include "KWinBridge.h"
#include "Logger.h"
#include "ScalingGeometry.h"

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

	std::string header, windowId;
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
			bridge.PlaceWindow(windowId, { frame, noBorder != 0, false });
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

	if (bridge.WindowManagementVersion() == 0 || bridge.ScreencastVersion() == 0 || !bridge.HasFakeInput()) {
		Logger::Get().Error("The desktop lacks an interface that scaling needs "
			"(window list, virtual outputs or pointer movement)");
		return ScalingError::ScalingFailedGeneral;
	}
	if (options.effects.empty()) {
		return ScalingError::InvalidScalingMode;
	}

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

	if (!_device.Initialize({ options.graphicsCardId, config.validation, true })) {
		return fail(ScalingError::ScalingFailedGeneral);
	}

	_surface = std::make_unique<LayerSurface>();
	if (!_surface->Create(_output.name)) {
		return fail(ScalingError::ScalingFailedGeneral);
	}

	VkSurfaceKHR surface = VK_NULL_HANDLE;
	const VkWaylandSurfaceCreateInfoKHR surfaceInfo{
		.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
		.display = _surface->Display(),
		.surface = _surface->Surface()
	};
	if (vkCreateWaylandSurfaceKHR(_device.Instance(), &surfaceInfo, nullptr, &surface) != VK_SUCCESS) {
		Logger::Get().Error("vkCreateWaylandSurfaceKHR failed");
		return fail(ScalingError::ScalingFailedGeneral);
	}

	_swapchain = std::make_unique<Swapchain>();
	if (!_swapchain->Initialize(_device, surface, _surface->GetSize(), config.presentMode)) {
		return fail(ScalingError::ScalingFailedGeneral);
	}

	// The strip around the source is not part of the picture
	Cropping cropping = options.cropping;
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
		.duplicateFrameDetectionMode = options.duplicateFrameDetectionMode,
		.isStatisticsForDynamicDetectionEnabled = options.IsStatisticsForDynamicDetectionEnabled()
	}) || !_WaitForFirstFrame()) {
		return fail(ScalingError::CaptureFailed);
	}

	_chain = std::make_unique<EffectChain>();
	if (!_chain->Initialize(_device, options, { config.effectsDir, _swapchain->GetSize() }, _frameSource->Output())) {
		return fail(ScalingError::ScalingFailedGeneral);
	}
	_chainInput = _frameSource->Output();

	_cursorDrawer = std::make_unique<CursorDrawer>();
	if (!_cursorDrawer->Initialize(_device, _swapchain->Format())) {
		return fail(ScalingError::ScalingFailedGeneral);
	}

	_stepTimer.Initialize(options.minFrameRate, options.maxFrameRate);
	_wakeFds = { _surface->EventFd(), _frameSource->EventFd(), _bridge->EventFd(), _capture->EventFd() };

	// "Keep screen on" and "simulate exclusive fullscreen" of the Windows
	// version. A desktop that cannot do them is no reason not to scale.
	if (options.IsKeepScreenOn() || options.IsSimulateExclusiveFullscreen()) {
		_inhibitor = std::make_unique<SessionInhibitor>();
		if (options.IsKeepScreenOn()) {
			_inhibitor->KeepScreenOn("A window is scaled");
		}
		if (options.IsSimulateExclusiveFullscreen()) {
			_inhibitor->SuppressNotifications(config.appId, "A window is scaled to fullscreen");
		}
	}

	// The first frame is there already
	_hasNewFrame = true;
	_isRunning = true;
	Logger::Get().Info("Scaling " + Describe());
	return ScalingError::NoError;
}

// Chooses the monitor and the place of the virtual output, creates it, and
// moves the source window there
bool ScalingSession::_PrepareDesktop(ScalingError& error) noexcept {
	_bridge->ListWindows();
	if (!_bridge->GetWindow(_windowId, _original) || _original.isMinimized) {
		Logger::Get().Error("The window to scale does not exist or is minimized");
		error = ScalingError::InvalidSourceWindow;
		return false;
	}
	if (_original.isFullscreen || _original.isMaximized) {
		// The Windows version scales such a window if a setting allows it. Here
		// it would need a virtual output smaller than the window wants to be.
		Logger::Get().Error("The window is maximized or fullscreen; scaling such windows is not supported yet");
		error = ScalingError::Maximized;
		return false;
	}

	const Size sourceSize{ _original.clientRect.Width(), _original.clientRect.Height() };
	Logger::Get().Info(fmt::format("The window has a frame of {}x{} and a content of {}x{}",
		_original.frameRect.Width(), _original.frameRect.Height(), sourceSize.width, sourceSize.height));
	if (sourceSize.width < ScalingGeometry::MIN_SRC_SIZE || sourceSize.height < ScalingGeometry::MIN_SRC_SIZE) {
		Logger::Get().Error("The window is too small to scale");
		error = ScalingError::InvalidSourceWindow;
		return false;
	}
	if (Rect cropped{ 0, 0, sourceSize.width, sourceSize.height };
		!ScalingGeometry::CropSourceRect(cropped, _options.cropping)) {
		error = ScalingError::InvalidCropping;
		return false;
	}

	VirtualOutputPlan plan;
	if (!PlanVirtualOutput(_original.frameRect, sourceSize, _bridge->ListOutputs(), plan)) {
		Logger::Get().Error("The window is not on a monitor, or the monitors leave no room for the virtual output");
		error = ScalingError::InvalidSourceWindow;
		return false;
	}
	_output = plan.monitor;
	_otherOutputs = plan.otherMonitors;
	_ringLeft = plan.ringLeft;
	_ringTop = plan.ringTop;
	_ringRight = plan.ringRight;
	_ringBottom = plan.ringBottom;
	_padRight = plan.padRight;
	const Size outputSize = plan.size;
	const Point position = plan.position;

	if (_options.IsAdjustCursorSpeed()) {
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
	if (!_bridge->ConfigureOutput(_virtualOutputName, outputSize, position, _output.refreshRate)) {
		return false;
	}

	_sourceRect = { _ringLeft, _ringTop, _ringLeft + sourceSize.width, _ringTop + sourceSize.height };
	_isWindowMoved = true;
	if (!_bridge->PlaceWindow(_windowId, {
		.frameRect = {
			position.x + _sourceRect.left, position.y + _sourceRect.top,
			position.x + _sourceRect.right, position.y + _sourceRect.bottom
		},
		.noBorder = true,
		.activate = true
	})) {
		Logger::Get().Error("The window could not be moved to the virtual output");
		error = ScalingError::InvalidSourceWindow;
		return false;
	}

	return true;
}

bool ScalingSession::PlanVirtualOutput(
	const Rect& frameRect,
	Size sourceSize,
	std::span<const OutputInfo> outputs,
	VirtualOutputPlan& plan
) noexcept {
	plan = {};

	// The monitor the window is on, by its centre
	const Point centre{ (frameRect.left + frameRect.right) / 2, (frameRect.top + frameRect.bottom) / 2 };
	Rect bounds{ INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN };
	bool found = false;
	for (const OutputInfo& output : outputs) {
		if (output.name.starts_with("Virtual-")) {
			continue;
		}

		bounds = {
			std::min(bounds.left, output.rect.left), std::min(bounds.top, output.rect.top),
			std::max(bounds.right, output.rect.right), std::max(bounds.bottom, output.rect.bottom)
		};
		if (!found && Contains(output.rect, centre)) {
			plan.monitor = output;
			found = true;
		} else {
			plan.otherMonitors.push_back(output);
		}
	}
	if (!found) {
		return false;
	}

	// A strip on every side where another monitor lies next to this one; the
	// pointer leaves through it. Without a neighbour the edge of the virtual
	// output keeps the pointer in.
	const Rect& monitor = plan.monitor.rect;
	for (const OutputInfo& other : plan.otherMonitors) {
		const bool overlapsVertically = other.rect.top < monitor.bottom && other.rect.bottom > monitor.top;
		const bool overlapsHorizontally = other.rect.left < monitor.right && other.rect.right > monitor.left;
		if (overlapsVertically && other.rect.right <= monitor.left) {
			plan.ringLeft = RING;
		}
		if (overlapsVertically && other.rect.left >= monitor.right) {
			plan.ringRight = RING;
		}
		if (overlapsHorizontally && other.rect.bottom <= monitor.top) {
			plan.ringTop = RING;
		}
		if (overlapsHorizontally && other.rect.top >= monitor.bottom) {
			plan.ringBottom = RING;
		}
	}

	plan.size = {
		sourceSize.width + plan.ringLeft + plan.ringRight,
		sourceSize.height + plan.ringTop + plan.ringBottom
	};
	// A mode with another refresh rate than 60 Hz has to be computed, and the
	// formula for that only yields widths that are multiples of 8. The
	// columns added on the right are cut off like the strips.
	if (plan.monitor.refreshRate > DEFAULT_REFRESH_RATE) {
		plan.padRight = (8 - plan.size.width % 8) % 8;
		plan.size.width += plan.padRight;
	}

	plan.position = { bounds.left, bounds.bottom + VIRTUAL_OUTPUT_GAP };
	plan.position.y = std::min(plan.position.y, MAX_COORDINATE - plan.size.height);
	return plan.position.y - bounds.bottom >= VIRTUAL_OUTPUT_GAP / 2 &&
		plan.position.x + plan.size.width <= MAX_COORDINATE;
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
	return ScalingGeometry::CalcDestRect(_swapchain->GetSize(),
		{ (int32_t)output.width, (int32_t)output.height }, _options.outputAlignment);
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

	if (!_surface->PumpEvents() || _surface->IsClosed() || !_bridge->Dispatch()) {
		_endReason = EndReason::Failed;
		return false;
	}
	if (!_capture->Dispatch() || !_frameSource->Dispatch()) {
		_endReason = EndReason::CaptureFailed;
		return false;
	}
	if (!_TrackSource()) {
		return false;
	}

	// The frame source, unless the frame rate limit says to wait. The cursor
	// is not held back by the limit.
	bool runEffects = false;
	if (_stepStatus != StepTimerStatus::WaitingForFPSLimiter) {
		const FrameSourceState state = _hasNewFrame ? FrameSourceState::NewFrame : _frameSource->Update();
		_hasNewFrame = false;

		if (state == FrameSourceState::Error) {
			_endReason = EndReason::CaptureFailed;
			return false;
		}
		runEffects = state == FrameSourceState::NewFrame || _stepStatus == StepTimerStatus::ForceNewFrame;
	}

	_UpdatePointer(_DestRect());

	const CaptureCursorState& cursor = _frameSource->CursorState();
	const bool cursorChanged = cursor.isVisible != _drawnCursor.isVisible ||
		(cursor.isVisible && (cursor.pos != _drawnCursor.pos || cursor.imageSerial != _drawnCursor.imageSerial));
	if (!runEffects && !cursorChanged) {
		return true;
	}

	if (!_Render(runEffects)) {
		_endReason = EndReason::Failed;
		return false;
	}
	return true;
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
	if (info.isFullscreen || info.isMaximized ||
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

	// Another window became active. On Windows the scaled picture then goes
	// behind that window; a layer surface cannot, so scaling ends if the
	// window is on the monitor the picture covers. Windows on the virtual
	// output are dialogs and menus of the source and part of the picture.
	const std::string active = _bridge->ActiveWindowId();
	WindowInfo activeInfo;
	if (!active.empty() && active != _windowId && _bridge->GetWindow(active, activeInfo)) {
		// The tolerance of ScalingWindow::_CheckForegroundFor3DGameMode
		static constexpr int32_t TOLERANCE = 8;
		const Rect& frame = activeInfo.frameRect;
		const Rect& monitor = _output.rect;
		const int32_t overlapWidth = std::min(frame.right, monitor.right) - std::max(frame.left, monitor.left);
		const int32_t overlapHeight = std::min(frame.bottom, monitor.bottom) - std::max(frame.top, monitor.top);
		if (overlapWidth >= TOLERANCE && overlapHeight >= TOLERANCE) {
			_endReason = EndReason::FocusLost;
			return false;
		}
	}

	return true;
}

// Port of the capture logic of CursorManager for a pointer that is either on
// the virtual output (captured: it acts on the source, and its picture is
// drawn scaled) or on a real monitor (released)
void ScalingSession::_UpdatePointer(const Rect& destRect) noexcept {
	const CaptureCursorState& cursor = _frameSource->CursorState();
	const Size rendererSize = _swapchain->GetSize();
	const Rect rendererRect{ 0, 0, rendererSize.width, rendererSize.height };
	const Point origin{ _virtualOutputRect.left, _virtualOutputRect.top };

	if (cursor.isVisible) {
		// Captured. Inside the source nothing is to be done.
		_SetPointerSlowed(true, destRect);
		if (Contains(_sourceRect, cursor.pos)) {
			_handledRingPos = { -1, -1 };
			return;
		}
		// In the strip around the source: the user pushes against an edge
		// that has a neighbouring monitor
		if (cursor.pos == _handledRingPos) {
			return;
		}
		_handledRingPos = cursor.pos;

		// Where that is on the monitors: just outside the one with the
		// picture, with black bars skipped, as _StopCapture does
		const Point scaled = ScalingGeometry::SrcToScaling(
			cursor.pos, _frameSource->SrcRect(), destRect, rendererRect, true);
		const Point target{ _output.rect.left + scaled.x, _output.rect.top + scaled.y };
		for (const OutputInfo& other : _otherOutputs) {
			if (Contains(other.rect, target)) {
				_SetPointerSlowed(false, destRect);
				_bridge->MovePointer(target);
				++_statistics.pointerReleases;
				return;
			}
		}

		// No monitor there: the pointer stays with the source
		const Point back = ClampTo(cursor.pos, _sourceRect);
		_bridge->MovePointer({ origin.x + back.x, origin.y + back.y });
		return;
	}

	_handledRingPos = { -1, -1 };

	// Released, or the source hides the cursor. If the pointer is on the
	// scaled picture, it goes to the point of the source that it shows there,
	// as _StartCapture does.
	if (_surface->IsPointerInside()) {
		// Not again before the move has had time to take effect
		const auto now = steady_clock::now();
		if (now - _lastCaptureTime < 50ms) {
			return;
		}
		_lastCaptureTime = now;

		const Rect& srcRect = _frameSource->SrcRect();
		const Point onPicture = ClampTo(_surface->PointerPos(), destRect);
		const Point onSource = ClampTo(ScalingGeometry::ScalingToSrc(onPicture, srcRect, destRect), srcRect);
		_SetPointerSlowed(true, destRect);
		_bridge->MovePointer({ origin.x + onSource.x, origin.y + onSource.y });
		++_statistics.pointerCaptures;
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
			if (!_chain->Resize(_swapchain->GetSize(), _frameSource->Output())) {
				return false;
			}
			_chainInput = _frameSource->Output();
		}
		_stepTimer.PrepareForRender();
	}

	const CaptureCursorState cursor = _frameSource->CursorState();
	if (cursor.isVisible && cursor.imageSerial != _loadedCursorImage) {
		const CaptureCursorImage& image = _frameSource->CursorImage();
		if (!_cursorDrawer->SetImage(image.pixels, image.size, image.hotSpot)) {
			return false;
		}
		_loadedCursorImage = cursor.imageSerial;
	}

	const auto draw = [&](VkCommandBuffer cmd, const Swapchain::Frame& frame) {
		if (runEffects) {
			_chain->Draw(cmd, _stepTimer.GetFrameCount());
		}

		const Size rendererSize = _swapchain->GetSize();
		const Rect destRect = _DestRect();
		Swapchain::Compose(cmd, *_chain->Output(), frame.image, rendererSize, destRect);

		if (cursor.isVisible) {
			const Rect& srcRect = _frameSource->SrcRect();
			const Rect rendererRect{ 0, 0, rendererSize.width, rendererSize.height };
			const Point cursorPos = ScalingGeometry::SrcToScaling(cursor.pos, srcRect, destRect, rendererRect, false);
			_cursorDrawer->Draw(cmd, { frame.image, frame.view, rendererSize, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL },
				cursorPos, { srcRect.Width(), srcRect.Height() }, destRect, _options);
		}
	};

	Swapchain::FrameResult result = _swapchain->PresentFrame(draw);
	if (result == Swapchain::FrameResult::NeedsResize) {
		// The surface changed under the frame; once more with the new size
		if (!_swapchain->Resize(_surface->GetSize()) ||
			!_chain->Resize(_swapchain->GetSize(), _frameSource->Output())) {
			return false;
		}
		runEffects = true;
		result = _swapchain->PresentFrame(draw);
	}
	if (result != Swapchain::FrameResult::Presented) {
		return false;
	}

	_drawnCursor = cursor;
	++_statistics.presentedFrames;
	if (runEffects) {
		++_statistics.scaledFrames;
	}
	return true;
}

void ScalingSession::Stop() noexcept {
	if (!_bridge) {
		return;
	}

	// The pointer goes to where the user sees it, as the destructor of
	// CursorManager does
	if (_isRunning && _frameSource->CursorState().isVisible) {
		const Size rendererSize = _swapchain->GetSize();
		const Rect rendererRect{ 0, 0, rendererSize.width, rendererSize.height };
		const Point scaled = ClampTo(ScalingGeometry::SrcToScaling(_frameSource->CursorState().pos,
			_frameSource->SrcRect(), _DestRect(), rendererRect, false), rendererRect);
		_bridge->MovePointer({ _output.rect.left + scaled.x, _output.rect.top + scaled.y });
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
			// The place it had, with the size it has now: the window may
			// have been resized while it was scaled. It has no border there,
			// so its frame is its content.
			Rect frame = _original.frameRect;
			frame.right += info.frameRect.Width() - _sourceRect.Width();
			frame.bottom += info.frameRect.Height() - _sourceRect.Height();
			Logger::Get().Info(fmt::format("Putting the window back: it is {}x{}, and goes to {},{} with {}x{}",
				info.frameRect.Width(), info.frameRect.Height(), frame.left, frame.top, frame.Width(), frame.Height()));
			const KWinBridge::Placement placement{
				.frameRect = frame,
				.noBorder = _original.hasNoBorder,
				.activate = info.isActive,
				.setGeometry = !info.isFullscreen && !info.isMaximized
			};
			_bridge->PlaceWindow(_windowId, placement);
			if (placement.setGeometry && frame != _original.frameRect) {
				placeAgain = placement;
			}
		}
		_isWindowMoved = false;
	}

	// In this order: everything that draws, the surface, the capture
	_cursorDrawer.reset();
	_chain.reset();
	_chainInput.reset();
	_swapchain.reset();
	_surface.reset();
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
			"the pointer went to the source {} times and to another monitor {} times",
			_statistics.scaledFrames, _statistics.presentedFrames,
			_statistics.pointerCaptures, _statistics.pointerReleases));
	}
	_isRunning = false;
	_bridge = nullptr;
}

std::string ScalingSession::Describe() const noexcept {
	if (!_frameSource || !_chain) {
		return "nothing";
	}

	const Rect& srcRect = _frameSource->SrcRect();
	const Texture& output = *_chain->Output();
	return fmt::format("{}x{} on {} ({:.2f} Hz) to {}x{} on {}; stream: {}",
		srcRect.Width(), srcRect.Height(), _virtualOutputName, _output.refreshRate / 1000.0,
		output.width, output.height, _output.name, _frameSource->DescribeFormat());
}

}
