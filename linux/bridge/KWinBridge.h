// Back end for KWin on Wayland. Uses two of KWin's own protocols, which
// KWin 6.8 offers to ordinary programs (see linux/spikes/RESULTS.md):
// org_kde_plasma_window_management for the list of windows and
// zkde_screencast_unstable_v1 for a screen cast without a dialog.
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "Bridge.h"

namespace Magpie {

class KWinBridge {
public:
	KWinBridge() noexcept;
	KWinBridge(const KWinBridge&) = delete;
	KWinBridge(KWinBridge&&) = delete;
	~KWinBridge();

	// Opens a Wayland connection of its own. Returns false if there is no
	// compositor or it offers neither interface.
	bool Initialize() noexcept;

	// The version in use, or 0 if the interface is not offered
	uint32_t WindowManagementVersion() const noexcept;
	uint32_t ScreencastVersion() const noexcept;

	// The windows as they are now
	std::vector<WindowInfo> ListWindows() noexcept;

	// Starts a screen cast of the window, with the cursor as metadata of the
	// stream. Returns null if KWin refuses.
	std::unique_ptr<CaptureSession> CaptureWindow(const std::string& windowId) noexcept;

private:
	struct Connection;
	class Session;

	// Shared with the capture sessions, which may outlive this object
	std::shared_ptr<Connection> _connection;
};

}
