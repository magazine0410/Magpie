// The scaling engine as a service: it decides which window is scaled with
// which profile, runs the scaling session, registers the global shortcuts
// and answers on D-Bus. Linux counterpart of ScalingService (src/Magpie) and
// ScalingRuntime (src/Magpie.Core).
//
// One thread does everything: the one that calls Initialize and Run. While a
// window is scaled it runs the scaling session, which wakes for D-Bus and
// for commands as well. The functions marked as callable from any thread
// hand their work to that thread.
#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "KWinBridge.h"
#include "PortalShortcuts.h"
#include "ScalingSession.h"
#include "Settings.h"

struct sd_bus;

namespace Magpie {

class ScalingService {
public:
	enum class State {
		Idle,
		Scaling,
		// The source is minimized or is changing its size; scaling goes on
		// when it is shown again and has kept its size for a moment
		Waiting
	};

	// "idle", "scaling" or "waiting", as the D-Bus property has it
	static const char* StateName(State state) noexcept;

	// A window with what profiles recognise it by
	struct Window {
		WindowInfo info;
		// Profile::classNameRule and Profile::pathRule; see ProfileRules.h
		std::string className;
		std::string path;
	};

	using BoundShortcuts = std::vector<std::pair<std::string, std::string>>;

	// The ids of the shortcuts
	static constexpr const char* SHORTCUT_SCALE = "scale";
	static constexpr const char* SHORTCUT_SCALE_WINDOWED = "scale-windowed";
	static constexpr const char* SHORTCUT_TOOLBAR = "toolbar";
	static constexpr const char* SHORTCUT_SCREENSHOT = "screenshot";

	struct Config {
		// Directory of the .hlsl files
		std::filesystem::path effectsDir;
		// The file with the icons of the toolbar
		std::filesystem::path iconFontPath;
		// Names the program to the desktop and on D-Bus
		std::string appId;
		std::string objectPath;
		bool useShortcuts = true;
		// Use the Vulkan validation layers
		bool validation = false;
		// Readable when the service is to end, for example a signalfd; -1
		// for none
		int quitFd = -1;

		// The functions below are called on the service's thread.

		// The settings in effect. Asked before every start of scaling and
		// when another window becomes active, so a change applies without a
		// restart. Null if there are none; the reason has been logged then.
		std::function<std::shared_ptr<const Settings>()> settings;
		std::function<void(State state, bool isWindowedMode)> stateChanged;
		// For every shortcut the desktop bound: its id and the key as text.
		// Called once after Initialize and when the user changes a key.
		std::function<void(const BoundShortcuts& shortcuts)> shortcutsChanged;
		// The user moved a window of the overlay; counterpart of the save
		// callback of the Windows options
		std::function<void(const std::map<std::string, OverlayWindowOption>& windows)> overlayOptionsChanged;
		// Another start of the program asks for the main window
		std::function<void()> showWindowRequested;
	};

	ScalingService() noexcept;
	ScalingService(const ScalingService&) = delete;
	ScalingService(ScalingService&&) = delete;
	~ScalingService();

	// Connects to the desktop, puts a window back that a crash left on a
	// virtual output, registers the shortcuts and the D-Bus name. The call
	// waits while the desktop shows its dialog for new shortcuts. Returns
	// false with a message for the user in `error` if the service cannot
	// run, for example because it runs already.
	bool Initialize(Config config, std::string& error) noexcept;

	// Returns after Quit, or when Config::quitFd becomes readable, or when
	// the connection to the desktop is lost. Scaling is stopped then.
	void Run() noexcept;

	// From here on: callable from any thread, also before Run.

	void Quit() noexcept;

	// What the shortcuts do: scale the active window to fullscreen or into
	// a window; stop if a window is scaled that way already, and change the
	// way if it is scaled the other way. As ScalingWindow::ToggleScaling.
	void ToggleScaling(bool windowedMode) noexcept;

	// Scales the active window unless a window is scaled already: what the
	// end of the countdown does. As ScalingService::_ScaleForegroundWindow.
	void ScaleActiveWindow(bool windowedMode) noexcept;

	void StopScaling() noexcept;

	// Looks at the active window again for auto-scale, although it has been
	// looked at: after a change of the profiles. As
	// ScalingService::CheckForeground.
	void CheckForeground() noexcept;

	// Opens the desktop's dialog for the keys of the shortcuts
	void ConfigureShortcuts() noexcept;

	// The windows that can be scaled, without the program's own. `reply` is
	// called on the service's thread.
	void ListWindows(std::function<void(std::vector<Window> windows)> reply) noexcept;

private:
	friend struct ScalingServiceDBus;

	void _Post(std::function<void()> task) noexcept;
	void _RunTasks() noexcept;

	void _SetState(State state) noexcept;
	void _AnnounceChanges() noexcept;
	void _UpdateShortcuts() noexcept;

	Window _Identify(const WindowInfo& info) const noexcept;
	bool _IsOwnOrForbidden(const WindowInfo& info) const noexcept;

	bool _StartScaling(const std::string& windowId, bool windowedMode) noexcept;
	void _EndSession() noexcept;
	void _StopScaling(bool byUser) noexcept;
	void _OnSessionEnded() noexcept;
	void _CheckWaiting() noexcept;
	void _Toggle(bool windowedMode) noexcept;
	void _ScaleWindow(const std::string& query, bool windowedMode) noexcept;
	void _CheckForeground() noexcept;

	Config _config;
	KWinBridge _bridge;
	sd_bus* _bus = nullptr;
	PortalShortcuts _shortcuts;
	std::unique_ptr<ScalingSession> _session;
	State _state = State::Idle;

	// The window that is scaled or waited for, its process, and how it is
	// scaled
	std::string _windowId;
	uint32_t _windowPid = 0;
	bool _isWindowedMode = false;
	// The window that auto-scale has looked at, so that it is not started
	// again right after the user stopped it. As _hwndChecked on Windows.
	std::string _checkedWindow;
	// The width the user gave the window of windowed scaling; the next one
	// starts with it, as on Windows while the program runs
	int32_t _lastWindowedWidth = 0;
	// The keys the desktop bound to the actions, for the tool tips of the
	// toolbar
	std::string _scaleShortcut, _windowedScaleShortcut, _screenshotShortcut;
	// What the properties on D-Bus last said
	bool _announcedSourceActive = false;
	Rect _announcedSourceRect{};
	Rect _announcedDestinationRect{};
	// While waiting: the window as last seen, and since when it is like that
	WindowInfo _waitingInfo;
	std::chrono::steady_clock::time_point _stableSince;

	// What other threads and the handlers of D-Bus calls want done
	std::mutex _tasksMutex;
	std::vector<std::function<void()>> _tasks;
	int _taskFd = -1;
	std::atomic<bool> _isQuitRequested = false;
};

}
