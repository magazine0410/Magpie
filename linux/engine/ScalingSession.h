// One scaled window, from start to stop, with the strategy for KWin: the
// source window moves to a virtual output of its own size, that output is
// captured, and the scaled picture is shown where the user looks: on one or
// more monitors (fullscreen scaling) or in a window (windowed scaling). The
// pointer lives on the virtual output while the user works in the picture, so
// clicks reach the source at the right place, and the keyboard stays with the
// source because the picture never keeps the focus.
//
// Linux counterpart of ScalingWindow, SrcTracker and the capture logic of
// CursorManager in src/Magpie.Core.
#pragma once
#include <chrono>
#include <filesystem>
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
#include "Geometry.h"
#include "KWinPointerSpeed.h"
#include "PipeWireFrameSource.h"
#include "ScalingOptions.h"
#include "ScalingSurface.h"
#include "SessionInhibitor.h"
#include "StepTimer.h"
#include "Swapchain.h"
#include "VulkanDevice.h"

namespace Magpie {

class KWinBridge;

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
		// The width the picture of the last windowed scaling had, to start
		// with it again; zero: the size comes from the options
		int32_t lastWindowedWidth = 0;
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
		// Columns on the right that only make the width a multiple of 8
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
	// the virtual output.
	static bool PlanVirtualOutput(
		const PlanRequest& request,
		std::span<const OutputInfo> outputs,
		VirtualOutputPlan& plan
	) noexcept;

	// Undoes what a session that did not end properly left behind: puts the
	// window noted in the state file back. Call at program start.
	static void RecoverAfterCrash(KWinBridge& bridge, const std::filesystem::path& stateFile) noexcept;

	// Moves the window to a virtual output and shows the first scaled frame.
	// On failure everything is put back and the reason has been logged.
	ScalingError Start(
		KWinBridge& bridge,
		const std::string& windowId,
		const ScalingOptions& options,
		const Config& config
	) noexcept;

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
		// Where the surface is, in desktop coordinates
		Rect rect;
	};

	bool _PrepareDesktop(ScalingError& error) noexcept;

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

	void _WriteStateFile() const noexcept;

	// The device is declared first, so that it is destroyed last
	VulkanDevice _device;

	KWinBridge* _bridge = nullptr;
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
	// Columns on the right that only make the width a multiple of 8
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
	std::unique_ptr<PipeWireFrameSource> _frameSource;
	std::vector<View> _views;
	// The area the picture is laid out in, in desktop coordinates: what the
	// monitors cover together, or the content of the window
	Rect _rendererRect;
	std::unique_ptr<EffectChain> _chain;
	std::unique_ptr<CursorDrawer> _cursorDrawer;
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
	std::vector<KWinPointerSpeed::Device> _pointerDevices;
	bool _isPointerSlowed = false;

	// Writes the last screenshot to its file
	std::jthread _screenshotThread;

	StepTimerStatus _stepStatus = StepTimerStatus::WaitingForNewFrame;
	bool _hasNewFrame = false;
	bool _isWindowMoved = false;
	bool _isSourceActive = false;
	bool _isRunning = false;
	EndReason _endReason = EndReason::None;
	Statistics _statistics;
};

}
