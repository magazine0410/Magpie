// What the engine asks of the desktop it runs on. The scaling code never
// talks to a compositor or a portal itself; it uses the types below, and a
// back end per desktop provides them (KWinBridge, PortalCapture).
//
// Only the capabilities that have an implementation have an interface here:
// finding windows and capturing them. The others get theirs together with
// their first back end.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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
	bool isActive = false;
	bool isMinimized = false;
	bool isFullscreen = false;
};

// The windows that `query` describes: the one with this id; otherwise those
// with this app ID or resource name (letter case is ignored); otherwise
// those with a title that contains it.
std::vector<const WindowInfo*> FindWindows(std::span<const WindowInfo> windows, std::string_view query) noexcept;

// A running screen cast: where its PipeWire stream is, and how to learn that
// it has ended. Destroying the object ends the screen cast.
class CaptureSession {
public:
	virtual ~CaptureSession() = default;

	// The connection to the PipeWire daemon that has the node, or -1 for the
	// session's own daemon. It stays owned by this object.
	virtual int PipeWireFd() const noexcept = 0;

	virtual uint32_t NodeId() const noexcept = 0;

	// Readable when Dispatch has something to do
	virtual int EventFd() const noexcept = 0;

	// Processes what the desktop has sent. Returns false once the screen cast
	// has ended, for example because the source was closed or the user
	// stopped it.
	virtual bool Dispatch() noexcept = 0;
};

}
