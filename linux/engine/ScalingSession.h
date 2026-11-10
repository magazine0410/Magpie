// One scaled window, from start to stop, with the strategy for KWin: the
// source window moves to a virtual output of its own size, that output is
// captured, and the scaled picture covers the monitor the window was on. The
// pointer lives on the virtual output while the user works in the picture, so
// clicks reach the source at the right place, and the keyboard stays with the
// source because the picture's surface never takes focus.
//
// Linux counterpart of ScalingWindow, SrcTracker and the capture logic of
// CursorManager in src/Magpie.Core, for fullscreen scaling.
#pragma once
#include <chrono>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include "Bridge.h"
#include "CursorDrawer.h"
#include "EffectChain.h"
#include "Geometry.h"
#include "KWinPointerSpeed.h"
#include "LayerSurface.h"
#include "PipeWireFrameSource.h"
#include "ScalingOptions.h"
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
		// The source changed its size or went fullscreen; scaling can start
		// again with the new size
		SourceResized,
		// The source is no longer where it was put
		SourceMoved,
		// Another window on the monitor of the scaled picture became active
		FocusLost,
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
	};

	struct Statistics {
		uint64_t scaledFrames = 0;
		uint64_t presentedFrames = 0;
		// How often the pointer was sent to the source and to another monitor
		uint32_t pointerCaptures = 0;
		uint32_t pointerReleases = 0;
	};

	ScalingSession() = default;
	ScalingSession(const ScalingSession&) = delete;
	ScalingSession(ScalingSession&&) = delete;

	// Stops if it is running
	~ScalingSession();

	// Where the virtual output goes and what it looks like
	struct VirtualOutputPlan {
		// The monitor that shows the scaled picture, and the other real ones
		OutputInfo monitor;
		std::vector<OutputInfo> otherMonitors;
		// Width of the strip on each side through which the pointer leaves to
		// another monitor; zero where the monitor has no neighbour
		int32_t ringLeft = 0, ringTop = 0, ringRight = 0, ringBottom = 0;
		// Columns on the right that only make the width a multiple of 8
		int32_t padRight = 0;
		Size size;
		Point position;
	};

	// Decides that for a window with this frame and this size of its
	// content, on a desktop with these outputs. Returns false if the window
	// is on no monitor or there is no room for the virtual output.
	static bool PlanVirtualOutput(
		const Rect& frameRect,
		Size sourceSize,
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

	bool IsRunning() const noexcept {
		return _isRunning;
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

	// For example "640x480 on Virtual-Magpie (200.00 Hz) scaled to 1440x1080 on DP-6"
	std::string Describe() const noexcept;

private:
	// Width of the strip around the source on the virtual output through
	// which the pointer leaves to another monitor
	static constexpr int32_t RING = 1;

	bool _PrepareDesktop(ScalingError& error) noexcept;

	bool _WaitForFirstFrame() noexcept;

	// False when scaling has to end; sets _endReason
	bool _TrackSource() noexcept;

	void _UpdatePointer(const Rect& destRect) noexcept;

	// Slows the pointer devices down by the scale of the picture while the
	// pointer acts on the source, and gives them their own speed otherwise
	void _SetPointerSlowed(bool slowed, const Rect& destRect) noexcept;

	bool _Render(bool runEffects) noexcept;

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
	// The monitor that shows the scaled picture, and the other real ones
	OutputInfo _output;
	std::vector<OutputInfo> _otherOutputs;

	std::string _virtualOutputName;
	Rect _virtualOutputRect;
	// The strip on each side of the virtual output; zero where the monitor
	// has no neighbour to leave to
	int32_t _ringLeft = 0, _ringTop = 0, _ringRight = 0, _ringBottom = 0;
	// Columns on the right that only make the width a multiple of 8
	int32_t _padRight = 0;
	// The source window on the virtual output, relative to that output
	Rect _sourceRect;

	// Keeping the screen on and notifications away, if the options ask for it
	std::unique_ptr<SessionInhibitor> _inhibitor;
	std::unique_ptr<CaptureSession> _capture;
	std::unique_ptr<PipeWireFrameSource> _frameSource;
	std::unique_ptr<LayerSurface> _surface;
	std::unique_ptr<Swapchain> _swapchain;
	std::unique_ptr<EffectChain> _chain;
	std::unique_ptr<CursorDrawer> _cursorDrawer;
	StepTimer _stepTimer;
	std::vector<int> _wakeFds;

	// What the chain was last sized for
	std::shared_ptr<Texture> _chainInput;
	// The cursor as it was last drawn
	CaptureCursorState _drawnCursor;
	uint32_t _loadedCursorImage = 0;
	// The pointer position for which the pointer was last moved, so that one
	// position is acted on once
	Point _handledRingPos{ -1, -1 };
	std::chrono::steady_clock::time_point _lastCaptureTime;

	// The pointer devices with the speed they had before scaling; empty if
	// the speed is not to be adjusted
	std::vector<KWinPointerSpeed::Device> _pointerDevices;
	bool _isPointerSlowed = false;

	StepTimerStatus _stepStatus = StepTimerStatus::WaitingForNewFrame;
	bool _hasNewFrame = false;
	bool _isWindowMoved = false;
	bool _isRunning = false;
	EndReason _endReason = EndReason::None;
	Statistics _statistics;
};

}
