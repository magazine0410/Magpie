// What the engine asks of the desktop it runs on. The scaling code never
// talks to a compositor or a portal itself; it uses the types below, and a
// back end per desktop provides them (KWinBridge, SwayBridge, PortalCapture).
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "Geometry.h"

namespace Magpie {

// The capabilities C1 to C9 of section 3 of LINUX_PORT_PLAN.md
enum class Capability {
	WindowInfo,			// C1: list windows with geometry, focus, state and owner
	WindowCapture,		// C2: capture a chosen window without a prompt
	Overlay,			// C3: show output above the source without taking input
	CursorInfo,			// C4: cursor position and image
	CursorControl,		// C5: hide, move and confine the cursor, change its speed
	WindowControl,		// C6: move, resize and restack other windows
	GlobalShortcuts,	// C7
	TouchInput,			// C8: remap touch input
	SessionServices,	// C9: keep the screen on, suppress notifications and the like
	COUNT
};

// Which capabilities this desktop provides and by which means
struct CapabilityReport {
	struct Entry {
		// In order of preference; empty if nothing provides the capability
		std::vector<std::string> providers;
		std::string note;
	};

	static const char* Name(Capability capability) noexcept;

	Entry& operator[](Capability capability) noexcept {
		return entries[(size_t)capability];
	}

	const Entry& operator[](Capability capability) const noexcept {
		return entries[(size_t)capability];
	}

	std::array<Entry, (size_t)Capability::COUNT> entries;
};

// Looks at the running desktop. Opens and closes its own connections.
CapabilityReport ProbeCapabilities() noexcept;

struct WindowInfo {
	// What the back end identifies the window by
	std::string id;
	std::string title;
	std::string appId;
	// Instance part of the window class of an X11 client
	std::string resourceName;
	uint32_t pid = 0;
	// With decoration, in desktop coordinates
	Rect frameRect;
	// The part the program draws; equal to frameRect if the back end does
	// not report it
	Rect clientRect;
	bool isActive = false;
	bool isMinimized = false;
	bool isMaximized = false;
	bool isFullscreen = false;
	// The desktop draws no title bar and border around it
	bool hasNoBorder = false;
	// What the back end needs besides frameRect and hasNoBorder to give the
	// window its place back, in a form only it understands: one line of
	// text, empty if there is nothing. It goes into WindowPlacement::restore.
	std::string restoreData;
};

struct OutputInfo {
	std::string name;
	// In desktop coordinates
	Rect rect;
	// In thousandths of a hertz
	int32_t refreshRate = 0;
	// No monitor shows it
	bool isVirtual = false;
	// How many of the output's pixels one unit of desktop coordinates is:
	// 1.5 for a monitor set to 150 %
	double scale = 1.0;
};

// Where DesktopBridge::PlaceWindow puts a window
struct WindowPlacement {
	// With decoration, in desktop coordinates
	Rect frameRect;
	bool noBorder = false;
	// Also make it the active window
	bool activate = false;
	// False: only the decoration and the activation are changed, for a
	// window whose place the desktop decides (maximized, fullscreen)
	bool setGeometry = true;
	// Not empty: the window is sent to this output and keeps its state,
	// for a maximized or fullscreen window. frameRect is then where it
	// is expected to end up, or empty if that is not known.
	std::string output;
	// Not empty: the window gets back what WindowInfo::restoreData described
	std::string restore;
};

// What else can be set on a window. What has no value is left alone.
struct WindowProperties {
	// The desktop keeps the window above the ordinary ones
	std::optional<bool> keepAbove;
	std::optional<bool> minimized;
	// Not in the task bar, the window switcher and the pager
	std::optional<bool> skipTaskbar;
	// Put above the other windows once
	bool raise = false;
};

// A pointer device whose speed can be set
struct PointerDevice {
	// What the back end knows the device by; without spaces
	std::string id;
	// The acceleration setting of libinput, from -1 to 1
	double acceleration = 0.0;

	// The setting that makes a device `scale` times slower than `acceleration`
	// does. With the flat profile the speed is 1 + acceleration, so this is
	// exact; with the adaptive profile, where the setting shapes a curve, it
	// only goes in the right direction. The result stays within -1 to 1.
	static double Slowed(double acceleration, double scale) noexcept;
};

// The windows that `query` describes: the one with this id; otherwise those
// with this app ID or resource name (letter case is ignored); otherwise
// those with a title that contains it.
std::vector<const WindowInfo*> FindWindows(std::span<const WindowInfo> windows, std::string_view query) noexcept;

// A running screen cast: where its frames come from, and how to learn that
// it has ended. Destroying the object ends the screen cast, and removes the
// virtual output if it was made together with one.
class CaptureSession {
public:
	enum class Kind {
		// A PipeWire stream: PipeWireFd and NodeId say where
		PipeWire,
		// An output of a Wayland compositor that is copied with the protocol
		// ext-image-copy-capture: WaylandDisplay and OutputName say which
		ImageCopy
	};

	virtual ~CaptureSession() = default;

	virtual Kind GetKind() const noexcept {
		return Kind::PipeWire;
	}

	// The connection to the PipeWire daemon that has the node, or -1 for the
	// session's own daemon. It stays owned by this object.
	virtual int PipeWireFd() const noexcept {
		return -1;
	}

	virtual uint32_t NodeId() const noexcept {
		return 0;
	}

	// The name of the compositor's socket, as in WAYLAND_DISPLAY; empty for
	// the one of the session
	virtual std::string WaylandDisplay() const noexcept {
		return {};
	}

	virtual std::string OutputName() const noexcept {
		return {};
	}

	// Readable when Dispatch has something to do
	virtual int EventFd() const noexcept = 0;

	// Processes what the desktop has sent. Returns false once the screen cast
	// has ended, for example because the source was closed or the user
	// stopped it.
	virtual bool Dispatch() noexcept = 0;
};

// A global shortcut that a back end is asked to bind
struct ShortcutRequest {
	// Names the action; comes back when the keys are pressed
	std::string id;
	// The keys as ShortcutToPortalTrigger gives them, for example
	// "ALT+SHIFT+a"; empty for none
	std::string trigger;
};

// A desktop on which windows can be scaled with the virtual-output strategy:
// the source window moves to an output that no monitor shows, that output is
// captured, and the pointer is sent there and back. One back end per
// compositor implements this.
class DesktopBridge {
public:
	// Where the desktops differ in ways the scaling session has to know
	struct Traits {
		// A virtual output that gets a refresh rate of its own has a width
		// that is a multiple of this
		int32_t customModeWidthStep = 1;
		// Making a window the active one puts the pointer on it. The session
		// then makes the source active only when the pointer goes there anyway.
		bool activationMovesPointer = false;
		// A window that opens while the source is the active window opens on
		// the virtual output, whatever program it belongs to. The session
		// sends it to the monitor unless it is of the source's program.
		bool opensWindowsOnActiveOutput = false;
		// The desktop lays new windows out side by side unless they say
		// that they cannot be resized. The window of windowed scaling then
		// starts as one of a fixed size, so that it gets the size it asks for.
		bool tilesNewWindows = false;
		// The desktop can minimize a window and tells when one is minimized
		bool canMinimize = true;
		// What the toolbar and the log call the way of capturing
		const char* captureMethod = "";
	};

	virtual ~DesktopBridge() = default;

	// For the log and for messages, for example "KWin"
	virtual const char* Name() const noexcept = 0;

	virtual Traits GetTraits() const noexcept = 0;

	// Empty if the desktop offers everything that scaling needs; otherwise
	// what it lacks, as a text for the log
	virtual std::string MissingForScaling() const noexcept = 0;

	// The name of the socket of the Wayland compositor that this back end
	// talks to, for the surfaces of the scaled picture; empty for the
	// compositor of the session
	virtual std::string WaylandDisplay() const noexcept {
		return {};
	}

	// Readable when Dispatch has something to do
	virtual int EventFd() const noexcept = 0;

	// Processes what the desktop has sent, so that GetWindow and
	// ActiveWindowId are current. Returns false when the connection is lost.
	virtual bool Dispatch() noexcept = 0;

	// The windows as they are now
	virtual std::vector<WindowInfo> ListWindows() noexcept = 0;

	// The window as of the last Dispatch; false if it does not exist (any more)
	virtual bool GetWindow(const std::string& windowId, WindowInfo& info) const noexcept = 0;

	// As of the last Dispatch; empty if no window is active
	virtual std::string ActiveWindowId() const noexcept = 0;

	// The outputs as they are now, virtual ones included
	virtual std::vector<OutputInfo> ListOutputs() noexcept = 0;

	// Creates an output that no monitor shows, and a screen cast of it with
	// the cursor as metadata. The output exists as long as the session; on
	// return `outputName` is its name. The consumer of the screen cast must
	// ask for exactly `size`.
	virtual std::unique_ptr<CaptureSession> CreateVirtualOutput(
		const std::string& name, Size size, std::string& outputName) noexcept = 0;

	// Moves an output and gives it the mode of this size with this refresh
	// rate (in thousandths of a hertz). Returns false if the output does not
	// end up with this size at this place.
	virtual bool ConfigureOutput(
		const std::string& outputName, Size size, Point position, int32_t refreshRate) noexcept = 0;

	// Removes a virtual output that a session which did not end properly
	// left behind, if the desktop keeps such outputs
	virtual void RemoveStaleVirtualOutput(const std::string& /*outputName*/) noexcept {}

	// Puts a window at an exact place, on whatever output that is, and waits
	// until the desktop reports it there. Returns false if that does not happen.
	virtual bool PlaceWindow(const std::string& windowId, const WindowPlacement& placement) noexcept = 0;

	// Does not wait for the change to be reported
	virtual bool SetWindowProperties(const std::string& windowId, const WindowProperties& properties) noexcept = 0;

	// Moves the pointer to a position in desktop coordinates
	virtual bool MovePointer(Point position) noexcept = 0;

	// The pointer devices whose speed can be set, with their current values
	virtual std::vector<PointerDevice> ReadPointerDevices() noexcept = 0;

	// Gives every listed device its value
	virtual void ApplyPointerSpeed(std::span<const PointerDevice> devices) noexcept = 0;

	// While this is on, changes of a window's place and size reach GetWindow
	// within a quarter of a second, also on a desktop that does not report
	// them by itself. The scaling session turns it on while it runs.
	virtual void SetWatching(bool /*isWatching*/) noexcept {}

	// Global shortcuts of the back end's own, for a desktop whose portal has
	// none. Without them the caller uses the portal.
	virtual bool HasOwnShortcuts() const noexcept {
		return false;
	}

	// Binds the keys. For every shortcut that was bound, the result has its
	// id and the keys as a text for the user.
	virtual std::vector<std::pair<std::string, std::string>> BindShortcuts(
		std::span<const ShortcutRequest> /*shortcuts*/) noexcept {
		return {};
	}

	// The ids of the shortcuts that were pressed since the last call
	virtual std::vector<std::string> TakeActivatedShortcuts() noexcept {
		return {};
	}

	// The back end for the desktop the program runs on; null with the reason
	// in `error` if there is none
	static std::unique_ptr<DesktopBridge> Create(std::string& error) noexcept;
};

}
