// One scaled window, from start to stop, with the virtual-output strategy: the
// source window moves to a virtual output of its own size, that output is
// captured, and the scaled picture is shown where the user looks: on one or
// more monitors (fullscreen scaling) or in a window (windowed scaling). The
// pointer lives on the virtual output while the user works in the picture, so
// clicks reach the source at the right place, and the keyboard stays with the
// source because the picture never keeps the focus.
//
// A second way, the nested session, serves where the desktop has no back end
// or the program is started for it: the program runs in a compositor of
// Magpie's own (NestedCompositor), the whole output of that compositor is the
// source, and the picture is in an ordinary window, fullscreen or not, which
// has the keyboard and the pointer and passes them on.
//
// Linux counterpart of ScalingWindow, SrcTracker and the capture logic of
// CursorManager in src/Magpie.Core.
#pragma once
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <vulkan/vulkan.h>
#include "Bridge.h"
#include "CursorDrawer.h"
#include "EffectChain.h"
#include "FrameSource.h"
#include "Geometry.h"
#include "OverlayDrawer.h"
#include "ScalingOptions.h"
#include "ScalingSurface.h"
#include "SessionInhibitor.h"
#include "StepTimer.h"
#include "Swapchain.h"
#include "VulkanDevice.h"

namespace Magpie {

class NestedCompositor;

class ScalingSession {
public:
	// Why Step returned false
	enum class EndReason {
		None,
		// The source window is gone
		SourceClosed,
		SourceMinimized,
		// The source changed its size or its state (maximized, fullscreen);
		// scaling can start again with the new size
		SourceResized,
		// The source is no longer where it was put
		SourceMoved,
		// Another window on a monitor of the scaled picture became active
		FocusLost,
		// The user closed the window of windowed scaling
		WindowClosed,
		// The user pressed the stop button of the toolbar
		UserStopped,
		// The user asked for the other way of scaling with the toolbar; the
		// caller starts it
		ModeSwitchRequested,
		CaptureFailed,
		Failed
	};

	struct Config {
		// Directory of the .hlsl files
		std::filesystem::path effectsDir;
		// Where the changes to the desktop are noted while they last, so that
		// they can be undone after a crash
		std::filesystem::path stateFile;
		VkPresentModeKHR presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
		bool validation = false;
		// Under which the desktop knows the program
		std::string appId;
		// The file with the icons of the toolbar; without it the buttons
		// carry text
		std::filesystem::path iconFontPath;
		// Tells the user something in passing: that a screenshot was saved,
		// what state the toolbar has now. Counterpart of the showToast
		// callback of the Windows options. It is called from the thread
		// that writes screenshots as well. Without it the messages only go
		// to the log.
		std::function<void(const std::string& text)> showMessage;
		// The width the picture of the last windowed scaling had, to start
		// with it again; zero: the size comes from the options
		int32_t lastWindowedWidth = 0;
		// Nested session: the name of the socket of the desktop's
		// compositor, on which the picture is shown; empty for the
		// compositor of the session
		std::string hostDisplay;
	};

	struct Statistics {
		uint64_t scaledFrames = 0;
		uint64_t presentedFrames = 0;
		// How often the pointer was sent to the source and away from it
		uint32_t pointerCaptures = 0;
		uint32_t pointerReleases = 0;
	};

	ScalingSession() = default;
	ScalingSession(const ScalingSession&) = delete;
	ScalingSession(ScalingSession&&) = delete;

	// Stops if it is running
	~ScalingSession();

	// What the virtual output is planned for
	struct PlanRequest {
		// The frame of the window before it is moved
		Rect frameRect;
		// The size the window will have on the virtual output
		Size sourceSize;
		// The window is maximized or fullscreen and takes the size of its
		// output, so the output gets the size of the source and no strip
		bool fillsOutput = false;
		bool isWindowedMode = false;
		bool is3DGameMode = false;
		MultiMonitorUsage multiMonitorUsage = MultiMonitorUsage::Closest;
	};

	// Where the virtual output goes and what it looks like
	struct VirtualOutputPlan {
		// The monitors that show the scaled picture, first the one the window
		// is on, and the other real ones. In windowed mode the first list has
		// the monitor of the window only.
		std::vector<OutputInfo> monitors;
		std::vector<OutputInfo> otherMonitors;
		// What `monitors` cover together, in desktop coordinates
		Rect rendererRect;
		// The highest refresh rate of `monitors`, in thousandths of a hertz
		int32_t refreshRate = 0;
		// The sides on which the pointer can leave the picture: those with a
		// neighbouring monitor, all for a window, none in 3D game mode
		bool exitLeft = false, exitTop = false, exitRight = false, exitBottom = false;
		// Width of the strip around the source through which the pointer
		// leaves; zero on sides without an exit, and on all sides for a
		// source that fills its output
		int32_t ringLeft = 0, ringTop = 0, ringRight = 0, ringBottom = 0;
		// Columns on the right that only make the width one the desktop
		// accepts for a mode of its own
		int32_t padRight = 0;
		Size size;
		Point position;
		// The source window on the virtual output, relative to that output
		Rect sourceRect;
		// Where the pointer acts on the source and is left alone. This is
		// the source, without its outermost pixels on the sides with an exit
		// if it fills the output: there they serve as the strip.
		Rect pointerRect;
	};

	// Returns false if the window is on no monitor or there is no room for
	// the virtual output. `widthStep`: see DesktopBridge::Traits.
	static bool PlanVirtualOutput(
		const PlanRequest& request,
		std::span<const OutputInfo> outputs,
		VirtualOutputPlan& plan,
		int32_t widthStep = 8
	) noexcept;

	// Undoes what a session that did not end properly left behind: puts the
	// window noted in the state file back. Call at program start.
	static void RecoverAfterCrash(DesktopBridge& bridge, const std::filesystem::path& stateFile) noexcept;

	// Moves the window to a virtual output and shows the first scaled frame.
	// On failure everything is put back and the reason has been logged.
	ScalingError Start(
		DesktopBridge& bridge,
		const std::string& windowId,
		const ScalingOptions& options,
		const Config& config
	) noexcept;

	// Shows what a nested session's compositor has on its output: scaled, in
	// a window on the desktop that covers a monitor, or in an ordinary one in
	// windowed mode. Keyboard and pointer go from that window to the
	// compositor. The compositor has to stay alive as long as the session;
	// stopping the session leaves it running.
	ScalingError StartNested(
		NestedCompositor& nested,
		const ScalingOptions& options,
		const Config& config
	) noexcept;

	bool IsNested() const noexcept {
		return _nested != nullptr;
	}

	// Waits until there is something to do, at most until one of `wakeFds`
	// becomes readable, and does it: a new frame, the pointer, a change of
	// the source. Returns false when scaling cannot go on; GetEndReason says
	// why. The caller then calls Stop.
	bool Step(std::span<const int> wakeFds = {}) noexcept;

	// Puts the source window and the pointer back and frees everything
	void Stop() noexcept;

	// Saves what an effect of the chain drew last, without cursor, as a PNG
	// file in the screenshots directory of the options. `effectIndex` counts
	// the active effects; without it the last one is taken, which is the
	// scaled picture. The file is written in the background and the outcome
	// goes to the log. Returns false if no picture could be taken.
	bool TakeScreenshot(std::optional<uint32_t> effectIndex = std::nullopt) noexcept;

	// What the toolbar shortcut does: from off to always shown to shown when
	// the pointer is near, and to off again. Port of
	// Renderer::SwitchToolbarState.
	void SwitchToolbarState() noexcept;

	// With the places the user gave the windows of the overlay; to be saved
	// when scaling has ended, as the Windows version does
	const OverlayOptions& GetOverlayOptions() const noexcept {
		return _options.overlayOptions;
	}

	bool IsRunning() const noexcept {
		return _isRunning;
	}

	bool IsWindowedMode() const noexcept {
		return _options.IsWindowedMode();
	}

	// The source is the active window
	bool IsSourceActive() const noexcept {
		return _isSourceActive;
	}

	EndReason GetEndReason() const noexcept {
		return _endReason;
	}

	const std::string& WindowId() const noexcept {
		return _windowId;
	}

	const Statistics& GetStatistics() const noexcept {
		return _statistics;
	}

	// The width of the picture of windowed scaling as the user left it, for
	// Config::lastWindowedWidth of the next session; zero if not known. Valid
	// after Stop as well.
	int32_t WindowedWidth() const noexcept {
		return _windowedWidth;
	}

	// The part of the source that is scaled, where it is while it is scaled,
	// and where its picture is, both in desktop coordinates. Counterparts of
	// the window properties Magpie.SrcLeft and so on, and Magpie.DestLeft and
	// so on, of the Windows version.
	Rect SourceRect() const noexcept;
	Rect DestinationRect() const noexcept;

	// For example "640x480 on Virtual-Magpie (200.00 Hz) to 1440x1080 on DP-6"
	std::string Describe() const noexcept;

private:
	// Width of the strip around the source on the virtual output through
	// which the pointer leaves
	static constexpr int32_t RING = 1;

	// One surface with the scaled picture or a part of it
	struct View {
		std::unique_ptr<ScalingSurface> surface;
		std::unique_ptr<Swapchain> swapchain;
		// Where the surface is, in the same pixels as _rendererRect
		Rect rect;
	};

	bool _PrepareDesktop(ScalingError& error) noexcept;

	// What Start and StartNested have in common, once the source is where
	// it is captured: the renderer, the surfaces, the first frame
	ScalingError _StartScaling() noexcept;

	// Nested session: follows the program's windows and the size of its
	// output. False when scaling has to end; sets _endReason.
	bool _TrackNested() noexcept;

	// Nested session: takes the output's size as the size of the source
	bool _SetNestedSourceSize(Size size) noexcept;

	// Nested session: passes keyboard and pointer on to the compositor
	void _ForwardInput(const Rect& destRect) noexcept;

	// Nested session: lets go of every key and button that is held there
	void _ReleaseNestedInput() noexcept;

	bool _CreateViews(ScalingError& error) noexcept;

	bool _AddView(std::unique_ptr<ScalingSurface> surface, const Rect& rect) noexcept;

	bool _WaitForFirstFrame() noexcept;

	// Windowed mode: gives the window of the picture its place and its
	// properties, once it has appeared
	bool _PlaceOwnWindow() noexcept;

	// Windowed mode: follows the window of the picture. False when scaling
	// has to end; sets _endReason.
	bool _TrackOwnWindow() noexcept;

	// False when scaling has to end; sets _endReason
	bool _TrackSource() noexcept;

	// Adapts the swap chains and the effects to the sizes of the surfaces.
	// Returns false on failure; `resized` says whether anything changed.
	bool _UpdateSizes(bool& resized) noexcept;

	void _UpdatePointer(const Rect& destRect) noexcept;

	// Passes the pointer on to the overlay, lays the overlay out and carries
	// out what its buttons ask for. False when scaling has to end; sets
	// _endReason. `needsRedraw`: the overlay looks different than in the
	// last frame.
	bool _UpdateOverlay(const Rect& destRect, bool& needsRedraw) noexcept;

	// The cursor on the source, as the capture reports it. A pointer that is
	// on a surface with the picture is not on the source, whatever the
	// capture says: Sway does not report that the cursor has left an output
	// when it goes to a surface that hides it.
	CaptureCursorState _SourceCursor() const noexcept;

	// Sends the pointer from the source to where its picture is on the desktop
	void _ReleasePointerToPicture(const Rect& destRect) noexcept;

	// Slows the pointer devices down by the scale of the picture while the
	// pointer acts on the source, and gives them their own speed otherwise
	void _SetPointerSlowed(bool slowed, const Rect& destRect) noexcept;

	bool _Render(bool runEffects) noexcept;

	Size _RendererSize() const noexcept {
		return { _rendererRect.Width(), _rendererRect.Height() };
	}

	// Where the picture is in the renderer area, relative to that area
	Rect _DestRect() const noexcept;

	// Between the pixels of the picture, in which _rendererRect and the
	// views are, and desktop coordinates, in which the desktop counts. They
	// are the same unless the monitor has a scale factor.
	Point _ToDesktop(Point picture) const noexcept;
	Point _ToPicture(Point desktop) const noexcept;

	void _WriteStateFile() const noexcept;

	// Writes the answers of the desktop about shown frames to the latency log
	void _WriteLatencyLog() noexcept;

	// The device is declared first, so that it is destroyed last
	VulkanDevice _device;

	// The desktop; in a nested session the compositor of that session
	DesktopBridge* _bridge = nullptr;
	DesktopBridge::Traits _traits;
	NestedCompositor* _nested = nullptr;
	ScalingOptions _options;
	Config _config;

	std::string _windowId;
	// The window before it was moved
	WindowInfo _original;
	// The monitors that show the picture (in windowed mode: the one the
	// window was on), and the other real ones
	std::vector<OutputInfo> _monitors;
	std::vector<OutputInfo> _otherMonitors;
	// The highest refresh rate of _monitors
	int32_t _refreshRate = 0;

	std::string _virtualOutputName;
	Rect _virtualOutputRect;
	// The strip on each side of the virtual output; zero where the pointer
	// cannot leave
	int32_t _ringLeft = 0, _ringTop = 0, _ringRight = 0, _ringBottom = 0;
	// Columns on the right that only make the width one the desktop accepts
	int32_t _padRight = 0;
	// The source window on the virtual output, relative to that output
	Rect _sourceRect;
	// Where the pointer is left alone, see VirtualOutputPlan
	Rect _pointerRect;
	// The part of the source that is scaled: without the cropped edges
	Size _srcSize;
	// The source was maximized or fullscreen and is so on the virtual output
	bool _isSourceFilling = false;
	// The decoration the source has while it is scaled
	bool _sourceNoBorder = true;

	// Keeping the screen on and notifications away, if the options ask for it
	std::unique_ptr<SessionInhibitor> _inhibitor;
	std::unique_ptr<CaptureSession> _capture;
	std::unique_ptr<FrameSource> _frameSource;
	std::vector<View> _views;
	// The area the picture is laid out in: what the monitors cover together,
	// or the content of the window. In pixels of the picture, which are
	// desktop coordinates times the scale factor of the monitor.
	Rect _rendererRect;
	// The scale factor of the monitors that show the picture
	double _pictureScale = 1.0;
	std::unique_ptr<EffectChain> _chain;
	std::unique_ptr<CursorDrawer> _cursorDrawer;
	// Toolbar and profiler; null in 3D game mode, which has neither
	std::unique_ptr<OverlayDrawer> _overlay;
	// The pointer as the overlay last saw it
	std::optional<Point> _overlayMousePos;
	// Buttons that are down on the overlay
	bool _overlayButtons[2]{};
	float _overlayWheel = 0.0f;
	bool _isOverlayRedrawForced = false;
	std::chrono::steady_clock::time_point _lastOverlayReleaseTime;
	StepTimer _stepTimer;
	std::vector<int> _wakeFds;

	// Windowed mode: the window of the picture as the desktop knows it, and
	// the limits of its size
	std::string _ownWindowId;
	Size _minWindowedSize;
	Size _maxWindowedSize;
	int32_t _windowedWidth = 0;
	bool _isOwnWindowKeptAbove = false;

	// What the chain was last sized for
	std::shared_ptr<Texture> _chainInput;
	// The cursor as it is to be drawn, and as it was last drawn
	CaptureCursorState _cursor;
	CaptureCursorState _drawnCursor;
	CursorAutoHide _cursorAutoHide;
	uint32_t _loadedCursorImage = 0;
	// The pointer position for which the pointer was last moved, so that one
	// position is acted on once
	Point _handledRingPos{ -1, -1 };
	std::chrono::steady_clock::time_point _lastCaptureTime;
	// After the pointer was sent away because another window became active,
	// it is not taken back before it has moved again
	bool _isFocusLossHandled = false;
	bool _isCaptureBlocked = false;
	std::chrono::steady_clock::time_point _captureBlockSettleTime;
	uint32_t _captureBlockMotionCount = 0;

	// The pointer devices with the speed they had before scaling; empty if
	// the speed is not to be adjusted
	std::vector<PointerDevice> _pointerDevices;
	bool _isPointerSlowed = false;

	// What the pointer did on the surfaces since the last step
	std::vector<ScalingSurface::PointerEvent> _pointerEvents;

	// Nested session: the keys and buttons that are down in the compositor
	std::vector<uint32_t> _pressedKeys;
	std::vector<uint32_t> _pressedButtons;
	// The pointer is locked on the picture and its movement is passed on
	// as it is, for a program that hides the cursor
	bool _isRelativeMode = false;
	// Since when the program shows no cursor
	std::optional<std::chrono::steady_clock::time_point> _cursorHiddenSince;
	std::chrono::steady_clock::time_point _startTime;
	// Since when the compositor has no window, after it had one
	std::optional<std::chrono::steady_clock::time_point> _noWindowSince;
	bool _hadNestedWindow = false;
	// A window of a fixed size that the output is to take the size of, and
	// since when it has that size
	Size _fitSize;
	std::chrono::steady_clock::time_point _fitSince;

	// Writes the last screenshot to its file
	std::jthread _screenshotThread;

	// The latency log, for measuring: with the environment variable
	// MAGPIE_LATENCY_LOG naming a file, every scaled frame gets a line in it
	// with the times of its way through Magpie, in nanoseconds of the steady
	// clock, and the colour of the source's pixel at 8,8, by which a test
	// program numbers its frames (linux/tests/LatencySourceMain.cpp).
	struct LatencyRecord {
		uint64_t tag = 0;
		uint32_t frameId = 0;
		// The frame arrived from the capture, rendering began, the frame was
		// handed to the desktop
		int64_t arrival = 0;
		int64_t renderStart = 0;
		int64_t queued = 0;
	};
	std::FILE* _latencyLog = nullptr;
	std::vector<LatencyRecord> _latencyRecords;
	uint64_t _nextLatencyTag = 1;

	StepTimerStatus _stepStatus = StepTimerStatus::WaitingForNewFrame;
	bool _hasNewFrame = false;
	bool _isWindowMoved = false;
	bool _isSourceActive = false;
	bool _isRunning = false;
	EndReason _endReason = EndReason::None;
	Statistics _statistics;
};

}
