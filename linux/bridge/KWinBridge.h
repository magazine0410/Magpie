// Back end for KWin on Wayland. It uses interfaces that KWin 6.8 offers to
// ordinary programs (see linux/spikes/RESULTS.md and the Phase 5 notes of the
// plan):
// - org_kde_plasma_window_management: the windows and their state
// - zkde_screencast_unstable_v1: screen casts without a dialog, and virtual
//   outputs
// - org_kde_kwin_fake_input: moving the pointer
// - KWin's scripting interface on D-Bus: placing a window exactly
// - the program kscreen-doctor: position and refresh rate of a virtual output
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "Bridge.h"

namespace Magpie {

class KWinBridge {
public:
	// Where a window is put by PlaceWindow
	struct Placement {
		// With decoration, in desktop coordinates
		Rect frameRect;
		bool noBorder = false;
		// Also make it the active window
		bool activate = false;
	};

	KWinBridge() noexcept;
	KWinBridge(const KWinBridge&) = delete;
	KWinBridge(KWinBridge&&) = delete;
	~KWinBridge();

	// Opens a Wayland connection of its own. Returns false if there is no
	// compositor or it offers neither the window list nor the screen cast.
	bool Initialize() noexcept;

	// The version in use, or 0 if the interface is not offered
	uint32_t WindowManagementVersion() const noexcept;
	uint32_t ScreencastVersion() const noexcept;
	bool HasFakeInput() const noexcept;

	// Readable when Dispatch has something to do
	int EventFd() const noexcept;

	// Processes what KWin has sent, so that GetWindow and ActiveWindowId are
	// current. Returns false when the connection is lost.
	bool Dispatch() noexcept;

	// The windows as they are now
	std::vector<WindowInfo> ListWindows() noexcept;

	// The window as of the last Dispatch; false if it does not exist (any more)
	bool GetWindow(const std::string& windowId, WindowInfo& info) const noexcept;

	// As of the last Dispatch; empty if no window is active
	std::string ActiveWindowId() const noexcept;

	// The outputs as they are now, virtual ones included
	std::vector<OutputInfo> ListOutputs() noexcept;

	// Starts a screen cast of the window, with the cursor as metadata of the
	// stream. Returns null if KWin refuses.
	std::unique_ptr<CaptureSession> CaptureWindow(const std::string& windowId) noexcept;

	// Creates an output that no monitor shows, and a screen cast of it with
	// the cursor as metadata. The output exists as long as the session; on
	// return `outputName` is its name. KWin takes the output's size from the
	// stream's negotiation, so the consumer must ask for exactly `size`.
	std::unique_ptr<CaptureSession> CreateVirtualOutput(
		const std::string& name, Size size, std::string& outputName) noexcept;

	// Moves an output and gives it a mode with this refresh rate (in
	// thousandths of a hertz). A new virtual output has 60 Hz.
	bool ConfigureOutput(const std::string& outputName, Size size, Point position, int32_t refreshRate) noexcept;

	// Puts a window at an exact place, on whatever output that is, and waits
	// until KWin reports it there. Returns false if that does not happen.
	bool PlaceWindow(const std::string& windowId, const Placement& placement) noexcept;

	// Moves the pointer to a position in desktop coordinates
	bool MovePointer(Point position) noexcept;

private:
	struct Connection;
	class Session;

	bool _RunScript(const std::string& script) noexcept;

	// Shared with the capture sessions, which may outlive this object
	std::shared_ptr<Connection> _connection;
};

}
