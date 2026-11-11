// Back end for Sway. It uses what Sway offers to ordinary programs:
// - its control socket (the IPC protocol of i3): the tree of windows, the
//   outputs, commands that move windows and create outputs without a screen,
//   the speed of the pointer devices, key bindings
// - wlr-virtual-pointer: moving the pointer
// - ext-image-copy-capture: the picture and the cursor of an output, which
//   ImageCopyFrameSource reads
//
// The same class drives the Sway that Magpie starts for a nested session.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "Bridge.h"
#include "SwayJson.h"

namespace Magpie {

class SwayIpc;
class WlrInput;

// The keys of a shortcut for Sway's "bindsym" and as a text for the user,
// from the form "ALT+SHIFT+a" that ShortcutToPortalTrigger gives. Both are
// empty for an empty trigger.
struct SwayKeys {
	std::string binding;
	std::string display;

	static SwayKeys FromTrigger(const std::string& trigger) noexcept;
};

class SwayBridge final : public DesktopBridge {
public:
	struct Options {
		// Sway's control socket; empty: the one the variable SWAYSOCK names
		std::string socketPath;
		// The name of Sway's Wayland socket; empty: the one of the session
		std::string waylandDisplay;
	};

	SwayBridge() noexcept;
	SwayBridge(const SwayBridge&) = delete;
	SwayBridge(SwayBridge&&) = delete;
	~SwayBridge() override;

	// The session's compositor is Sway
	static bool IsSessionCompositor() noexcept;

	bool Initialize(const Options& options) noexcept;
	bool Initialize() noexcept {
		return Initialize({});
	}

	const char* Name() const noexcept override {
		return "Sway";
	}

	Traits GetTraits() const noexcept override;

	std::string MissingForScaling() const noexcept override;

	std::string WaylandDisplay() const noexcept override;

	int EventFd() const noexcept override;

	bool Dispatch() noexcept override;

	std::vector<WindowInfo> ListWindows() noexcept override;

	bool GetWindow(const std::string& windowId, WindowInfo& info) const noexcept override;

	std::string ActiveWindowId() const noexcept override;

	std::vector<OutputInfo> ListOutputs() noexcept override;

	// An output of Sway's headless back end. `name` is not used: Sway names
	// such outputs itself (HEADLESS-1 and so on).
	std::unique_ptr<CaptureSession> CreateVirtualOutput(
		const std::string& name, Size size, std::string& outputName) noexcept override;

	bool ConfigureOutput(
		const std::string& outputName, Size size, Point position, int32_t refreshRate) noexcept override;

	void RemoveStaleVirtualOutput(const std::string& outputName) noexcept override;

	// A window that is put at a place is taken out of the tiling for that.
	// With WindowPlacement::restore it goes back to where it was in it.
	bool PlaceWindow(const std::string& windowId, const WindowPlacement& placement) noexcept override;

	// Sway keeps floating windows above the others by itself and has no task
	// bar of its own; minimizing is not possible.
	bool SetWindowProperties(const std::string& windowId, const WindowProperties& properties) noexcept override;

	bool MovePointer(Point position) noexcept override;

	std::vector<PointerDevice> ReadPointerDevices() noexcept override;

	void ApplyPointerSpeed(std::span<const PointerDevice> devices) noexcept override;

	void SetWatching(bool isWatching) noexcept override;

	// Key bindings that are made at run time and removed at the end. They
	// replace a binding of the same keys in Sway's configuration until Sway
	// reads that again.
	bool HasOwnShortcuts() const noexcept override {
		return true;
	}

	std::vector<std::pair<std::string, std::string>> BindShortcuts(
		std::span<const ShortcutRequest> shortcuts) noexcept override;

	std::vector<std::string> TakeActivatedShortcuts() noexcept override;

	// For the nested session, which drives a Sway of its own:

	// A screen cast of an output that exists already and stays
	std::unique_ptr<CaptureSession> CaptureOutput(const std::string& outputName) noexcept;

	// The windows as of the last Dispatch or ListWindows
	const std::vector<SwayWindow>& Windows() const noexcept {
		return _windows;
	}

	SwayIpc& Ipc() noexcept;

	WlrInput& Input() noexcept;

private:
	struct Shared;
	class Capture;

	bool _RefreshTree() noexcept;
	bool _RefreshOutputs() noexcept;
	const SwayWindow* _Find(const std::string& windowId) const noexcept;
	bool _Restore(const SwayWindow& window, const WindowPlacement& placement, const SwayRestore& restore) noexcept;
	void _Bind() noexcept;

	// Shared with the capture sessions, which may outlive this object
	std::shared_ptr<Shared> _shared;
	std::vector<SwayWindow> _windows;
	std::vector<OutputInfo> _outputs;
	int _epollFd = -1;
	int _timerFd = -1;

	struct Binding {
		std::string id;
		std::string keys;
	};
	std::vector<Binding> _bindings;
	std::vector<std::string> _activated;
};

}
