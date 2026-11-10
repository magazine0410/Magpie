#include "ScalingService.h"
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <fmt/format.h>
#include <systemd/sd-bus.h>
#include "DesktopNotification.h"
#include "Logger.h"
#include "Paths.h"
#include "ProcessInfo.h"
#include "ProfileOptions.h"
#include "ProfileRules.h"
#include "Shortcut.h"

namespace Magpie {

const char* ScalingService::StateName(State state) noexcept {
	static constexpr const char* NAMES[]{ "idle", "scaling", "waiting" };
	return NAMES[(size_t)state];
}

// The handlers of the D-Bus interface. Calls that change something are put
// in the queue of tasks and carried out by the main loop, not inside the
// processing of the bus.
struct ScalingServiceDBus {
	static ScalingService& Service(void* userData) noexcept {
		return *(ScalingService*)userData;
	}

	static int OnScale(sd_bus_message* message, void* userData, sd_bus_error*) {
		Service(userData).ToggleScaling(false);
		return sd_bus_reply_method_return(message, "");
	}

	static int OnScaleWindowed(sd_bus_message* message, void* userData, sd_bus_error*) {
		Service(userData).ToggleScaling(true);
		return sd_bus_reply_method_return(message, "");
	}

	static int OnScaleWindow(sd_bus_message* message, void* userData, sd_bus_error*) {
		ScalingService& service = Service(userData);
		const char* query = nullptr;
		int windowed = 0;
		const int result = sd_bus_message_read(message, "sb", &query, &windowed);
		if (result < 0) {
			return result;
		}
		service._Post([&service, query = std::string(query), windowed = windowed != 0] {
			service._ScaleWindow(query, windowed);
		});
		return sd_bus_reply_method_return(message, "");
	}

	static int OnSwitchToolbar(sd_bus_message* message, void* userData, sd_bus_error*) {
		ScalingService& service = Service(userData);
		service._Post([&service] {
			if (service._session) {
				service._session->SwitchToolbarState();
			}
		});
		return sd_bus_reply_method_return(message, "");
	}

	static int OnTakeScreenshot(sd_bus_message* message, void* userData, sd_bus_error*) {
		ScalingService& service = Service(userData);
		service._Post([&service] {
			if (!service._session || !service._session->TakeScreenshot()) {
				Logger::Get().Warn("No screenshot: no window is scaled");
			}
		});
		return sd_bus_reply_method_return(message, "");
	}

	static int OnStop(sd_bus_message* message, void* userData, sd_bus_error*) {
		Service(userData).StopScaling();
		return sd_bus_reply_method_return(message, "");
	}

	static int OnShowWindow(sd_bus_message* message, void* userData, sd_bus_error*) {
		ScalingService& service = Service(userData);
		if (service._config.showWindowRequested) {
			service._config.showWindowRequested();
		}
		return sd_bus_reply_method_return(message, "");
	}

	static int GetState(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userData, sd_bus_error*) {
		return sd_bus_message_append(reply, "s", ScalingService::StateName(Service(userData)._state));
	}

	static int GetWindowed(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userData, sd_bus_error*) {
		const ScalingService& service = Service(userData);
		return sd_bus_message_append(reply, "b",
			(int)(service._state != ScalingService::State::Idle && service._isWindowedMode));
	}

	static int GetSourceWindow(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userData, sd_bus_error*) {
		const ScalingService& service = Service(userData);
		return sd_bus_message_append(reply, "s",
			service._state != ScalingService::State::Idle ? service._windowId.c_str() : "");
	}

	static int GetSourceActive(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userData, sd_bus_error*) {
		const ScalingService& service = Service(userData);
		return sd_bus_message_append(reply, "b", (int)(service._session && service._session->IsSourceActive()));
	}

	static int AppendRect(sd_bus_message* reply, const Rect& rect) {
		return sd_bus_message_append(reply, "(iiii)", rect.left, rect.top, rect.right, rect.bottom);
	}

	static int GetSourceRect(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userData, sd_bus_error*) {
		const ScalingService& service = Service(userData);
		return AppendRect(reply, service._session ? service._session->SourceRect() : Rect{});
	}

	static int GetDestinationRect(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* userData, sd_bus_error*) {
		const ScalingService& service = Service(userData);
		return AppendRect(reply, service._session ? service._session->DestinationRect() : Rect{});
	}

	// The counterpart of the MagpieScalingChanged message and of the window
	// properties that "docs/Interact with Magpie programally.md" describes
	static inline const sd_bus_vtable VTABLE[]{
		SD_BUS_VTABLE_START(0),
		// The actions of the shortcuts
		SD_BUS_METHOD("Scale", "", "", OnScale, SD_BUS_VTABLE_UNPRIVILEGED),
		SD_BUS_METHOD("ScaleWindowed", "", "", OnScaleWindowed, SD_BUS_VTABLE_UNPRIVILEGED),
		SD_BUS_METHOD("TakeScreenshot", "", "", OnTakeScreenshot, SD_BUS_VTABLE_UNPRIVILEGED),
		// The toolbar: off, always shown, shown when the pointer is near
		SD_BUS_METHOD("SwitchToolbar", "", "", OnSwitchToolbar, SD_BUS_VTABLE_UNPRIVILEGED),
		// A window id, an app ID or a part of a title; and whether into a window
		SD_BUS_METHOD("ScaleWindow", "sb", "", OnScaleWindow, SD_BUS_VTABLE_UNPRIVILEGED),
		SD_BUS_METHOD("Stop", "", "", OnStop, SD_BUS_VTABLE_UNPRIVILEGED),
		// Shows the main window of the application; does nothing in a
		// service without one
		SD_BUS_METHOD("ShowWindow", "", "", OnShowWindow, SD_BUS_VTABLE_UNPRIVILEGED),
		// "idle", "scaling" or "waiting"
		SD_BUS_PROPERTY("State", "s", GetState, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
		// The window that is scaled or waited for, as the desktop identifies
		// it, and whether it is scaled into a window
		SD_BUS_PROPERTY("SourceWindow", "s", GetSourceWindow, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
		SD_BUS_PROPERTY("Windowed", "b", GetWindowed, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
		// The source is the active window
		SD_BUS_PROPERTY("SourceActive", "b", GetSourceActive, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
		// Left, top, right and bottom in desktop coordinates: the part of the
		// source that is scaled, where it is while it is scaled, and its picture
		SD_BUS_PROPERTY("SourceRect", "(iiii)", GetSourceRect, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
		SD_BUS_PROPERTY("DestinationRect", "(iiii)", GetDestinationRect, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
		SD_BUS_SIGNAL("StateChanged", "s", 0),
		SD_BUS_VTABLE_END
	};
};

ScalingService::ScalingService() noexcept : _taskFd(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {}

ScalingService::~ScalingService() {
	_EndSession();
	if (_bus) {
		sd_bus_flush_close_unref(_bus);
	}
	if (_taskFd >= 0) {
		close(_taskFd);
	}
}

bool ScalingService::Initialize(Config config, std::string& error) noexcept {
	_config = std::move(config);

	if (!_bridge.Initialize()) {
		error = "This desktop does not offer KWin's interfaces.";
		return false;
	}
	ScalingSession::RecoverAfterCrash(_bridge, Paths::StateDir() / "scaling-state");

	if (sd_bus_open_user(&_bus) < 0) {
		error = "Cannot connect to the session bus.";
		return false;
	}

	// The name comes before the shortcuts, so that a second start fails
	// without having asked the desktop for anything. Calls that arrive from
	// now on only put tasks in the queue, which Run carries out.
	if (sd_bus_add_object_vtable(_bus, nullptr, _config.objectPath.c_str(), _config.appId.c_str(),
		ScalingServiceDBus::VTABLE, this) < 0 ||
		sd_bus_request_name(_bus, _config.appId.c_str(), 0) < 0) {
		error = "The service cannot be registered; it seems to run already.";
		return false;
	}

	if (_config.useShortcuts) {
		const std::shared_ptr<const Settings> settings = _config.settings ? _config.settings() : nullptr;
		const auto trigger = [&](ShortcutAction action) {
			return settings ? ShortcutToPortalTrigger(settings->shortcuts[(size_t)action]) : std::string();
		};
		const PortalShortcuts::Shortcut wanted[]{
			{
				SHORTCUT_SCALE,
				"Scale the active window to fullscreen, or stop scaling",
				trigger(ShortcutAction::Scale)
			},
			{
				SHORTCUT_SCALE_WINDOWED,
				"Scale the active window into a window, or stop scaling",
				trigger(ShortcutAction::WindowedModeScale)
			},
			{
				SHORTCUT_TOOLBAR,
				"Show or hide the toolbar on the scaled picture",
				trigger(ShortcutAction::Toolbar)
			},
			{
				SHORTCUT_SCREENSHOT,
				"Save a screenshot of the scaled picture",
				trigger(ShortcutAction::TakeScreenshot)
			}
		};
		if (!_shortcuts.Initialize(_bus, _config.appId, wanted)) {
			Logger::Get().Warn("No global shortcut is registered");
		}
	}
	_UpdateShortcuts();

	return true;
}

void ScalingService::_UpdateShortcuts() noexcept {
	_scaleShortcut.clear();
	_windowedScaleShortcut.clear();
	_screenshotShortcut.clear();
	for (const auto& [id, trigger] : _shortcuts.Bound()) {
		if (id == SHORTCUT_SCALE) {
			_scaleShortcut = trigger;
		} else if (id == SHORTCUT_SCALE_WINDOWED) {
			_windowedScaleShortcut = trigger;
		} else if (id == SHORTCUT_SCREENSHOT) {
			_screenshotShortcut = trigger;
		}
	}

	if (_config.shortcutsChanged) {
		_config.shortcutsChanged(_shortcuts.Bound());
	}
}

void ScalingService::_Post(std::function<void()> task) noexcept {
	{
		const std::scoped_lock lock(_tasksMutex);
		_tasks.push_back(std::move(task));
	}
	const uint64_t one = 1;
	[[maybe_unused]] const ssize_t written = write(_taskFd, &one, sizeof(one));
}

void ScalingService::_RunTasks() noexcept {
	uint64_t count;
	[[maybe_unused]] const ssize_t got = read(_taskFd, &count, sizeof(count));

	std::vector<std::function<void()>> tasks;
	{
		const std::scoped_lock lock(_tasksMutex);
		tasks.swap(_tasks);
	}
	for (const std::function<void()>& task : tasks) {
		task();
	}
}

void ScalingService::Quit() noexcept {
	_isQuitRequested = true;
	_Post([] {});
}

void ScalingService::ToggleScaling(bool windowedMode) noexcept {
	_Post([this, windowedMode] { _Toggle(windowedMode); });
}

void ScalingService::ScaleActiveWindow(bool windowedMode) noexcept {
	_Post([this, windowedMode] {
		if (_state != State::Scaling) {
			_Toggle(windowedMode);
		}
	});
}

void ScalingService::StopScaling() noexcept {
	_Post([this] { _StopScaling(true); });
}

void ScalingService::CheckForeground() noexcept {
	_Post([this] {
		_checkedWindow.clear();
		_CheckForeground();
	});
}

void ScalingService::ConfigureShortcuts() noexcept {
	_Post([this] { _shortcuts.Configure(); });
}

void ScalingService::ListWindows(std::function<void(std::vector<Window> windows)> reply) noexcept {
	_Post([this, reply = std::move(reply)] {
		std::vector<Window> windows;
		for (const WindowInfo& info : _bridge.ListWindows()) {
			if (!_IsOwnOrForbidden(info)) {
				windows.push_back(_Identify(info));
			}
		}
		reply(std::move(windows));
	});
}

void ScalingService::_SetState(State state) noexcept {
	if (_state == state) {
		return;
	}
	_state = state;
	Logger::Get().Info(std::string("State: ") + StateName(state));
	sd_bus_emit_signal(_bus, _config.objectPath.c_str(), _config.appId.c_str(), "StateChanged", "s", StateName(state));
	sd_bus_emit_properties_changed(_bus, _config.objectPath.c_str(), _config.appId.c_str(),
		"State", "Windowed", "SourceWindow", nullptr);
	if (_config.stateChanged) {
		_config.stateChanged(state, state != State::Idle && _isWindowedMode);
	}
}

// Tells listeners on D-Bus what changed while scaling: whether the source is
// the active window, and where the picture is. Counterpart of the events 0
// to 2 of the MagpieScalingChanged message.
void ScalingService::_AnnounceChanges() noexcept {
	const bool sourceActive = _session && _session->IsSourceActive();
	const Rect sourceRect = _session ? _session->SourceRect() : Rect{};
	const Rect destinationRect = _session ? _session->DestinationRect() : Rect{};
	if (sourceActive != _announcedSourceActive) {
		_announcedSourceActive = sourceActive;
		sd_bus_emit_properties_changed(_bus, _config.objectPath.c_str(), _config.appId.c_str(),
			"SourceActive", nullptr);
	}
	if (sourceRect != _announcedSourceRect || destinationRect != _announcedDestinationRect) {
		_announcedSourceRect = sourceRect;
		_announcedDestinationRect = destinationRect;
		sd_bus_emit_properties_changed(_bus, _config.objectPath.c_str(), _config.appId.c_str(),
			"SourceRect", "DestinationRect", nullptr);
	}
}

ScalingService::Window ScalingService::_Identify(const WindowInfo& info) const noexcept {
	return {
		.info = info,
		// A Wayland program has an app ID; for an X11 program KWin reports
		// its window class as the app ID, or the name of a .desktop file it
		// found for it
		.className = info.appId.empty() ? info.resourceName : info.appId,
		.path = ProcessInfo::GetPath(info.pid)
	};
}

// The windows that are never scaled: the program's own (the scaled picture
// in a window, the settings window) and those of the desktop and the
// panels. Counterpart of WindowHelper::IsForbiddenSystemWindow.
bool ScalingService::_IsOwnOrForbidden(const WindowInfo& info) const noexcept {
	return info.pid == (uint32_t)getpid() || info.appId == "org.kde.plasmashell";
}

void ScalingService::_EndSession() noexcept {
	if (!_session) {
		return;
	}
	_session->Stop();
	if (_session->IsWindowedMode() && _session->WindowedWidth() > 0) {
		_lastWindowedWidth = _session->WindowedWidth();
	}
	if (_config.overlayOptionsChanged) {
		_config.overlayOptionsChanged(_session->GetOverlayOptions().windows);
	}
	_session.reset();
}

bool ScalingService::_StartScaling(const std::string& windowId, bool windowedMode) noexcept {
	// Not _StopScaling: the state goes from what it is to "scaling" without
	// "idle" in between
	_EndSession();

	const auto fail = [&](const std::string& message) {
		if (!message.empty()) {
			Logger::Get().Error(message);
			DesktopNotification::Show(_config.appId, "Magpie", message);
		}
		_SetState(State::Idle);
		return false;
	};

	WindowInfo info;
	if (!_bridge.GetWindow(windowId, info) || _IsOwnOrForbidden(info)) {
		return fail({});
	}

	// The settings stay alive as long as the profile is used
	const std::shared_ptr<const Settings> settings = _config.settings ? _config.settings() : nullptr;
	if (!settings) {
		return fail("Scaling did not start: the settings cannot be read.");
	}
	const Window window = _Identify(info);
	const Profile& profile = *FindProfileForWindow(*settings, window.className, window.path, false);

	ScalingOptions options;
	ScalingError error = BuildScalingOptions(*settings, profile, windowedMode, options);
	if (error == ScalingError::NoError) {
		options.overlayOptions.scaleShortcut = _scaleShortcut;
		options.overlayOptions.windowedModeScaleShortcut = _windowedScaleShortcut;
		options.overlayOptions.takeScreenshotShortcut = _screenshotShortcut;

		_session = std::make_unique<ScalingSession>();
		error = _session->Start(_bridge, windowId, options, {
			.effectsDir = _config.effectsDir,
			.stateFile = Paths::StateDir() / "scaling-state",
			.validation = _config.validation,
			.appId = _config.appId,
			.iconFontPath = _config.iconFontPath,
			.showMessage = [appId = _config.appId](const std::string& text) {
				DesktopNotification::Show(appId, "Magpie", text);
			},
			.lastWindowedWidth = _lastWindowedWidth
		});
	}
	if (error != ScalingError::NoError) {
		// Counterpart of the showError callback of the Windows options
		_session.reset();
		return fail(std::string("Scaling did not start: ") + ScalingErrorMessage(error) + ".");
	}

	const bool wasScaling = _state == State::Scaling;
	_windowId = windowId;
	_windowPid = info.pid;
	_isWindowedMode = windowedMode;
	if (wasScaling) {
		// From one way of scaling to the other, or to another window
		sd_bus_emit_properties_changed(_bus, _config.objectPath.c_str(), _config.appId.c_str(),
			"Windowed", "SourceWindow", nullptr);
		if (_config.stateChanged) {
			_config.stateChanged(State::Scaling, windowedMode);
		}
	}
	_SetState(State::Scaling);
	return true;
}

void ScalingService::_StopScaling(bool byUser) noexcept {
	_EndSession();

	// A window whose scaling the user stopped is not scaled again at once
	// by auto-scale; one that only lost the focus is, when it gets it back.
	// As ScalingService::_ScalingRuntime_StateChanged on Windows, which
	// tells the two apart by the foreground window.
	if (_state != State::Idle && (byUser || _bridge.ActiveWindowId() == _windowId)) {
		_checkedWindow = _windowId;
	}
	_windowId.clear();
	_windowPid = 0;
	_SetState(State::Idle);
}

// The session cannot go on. Port of how ScalingRuntime restarts after the
// source was "repositioned".
void ScalingService::_OnSessionEnded() noexcept {
	const ScalingSession::EndReason reason = _session->GetEndReason();
	Logger::Get().Info("The scaling session ended (reason " + std::to_string((int)reason) + ")");
	if (reason == ScalingSession::EndReason::ModeSwitchRequested) {
		// The button of the toolbar: the same window the other way
		_StartScaling(std::string(_windowId), !_isWindowedMode);
		return;
	}

	if (reason == ScalingSession::EndReason::SourceResized ||
		reason == ScalingSession::EndReason::SourceMinimized) {
		// Scaled again once the window is shown and its size has settled
		_EndSession();
		_waitingInfo = {};
		_stableSince = std::chrono::steady_clock::now();
		_SetState(State::Waiting);
	} else {
		_StopScaling(reason == ScalingSession::EndReason::UserStopped ||
			reason == ScalingSession::EndReason::WindowClosed);
	}
}

void ScalingService::_CheckWaiting() noexcept {
	// As long as the window has kept its size, as IsSrcRepositioning waits
	// for the end of a move or resize on Windows
	static constexpr std::chrono::milliseconds SETTLE_TIME{ 200 };

	WindowInfo info;
	if (!_bridge.GetWindow(_windowId, info)) {
		_StopScaling(false);
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	if (info.isMinimized || info.frameRect != _waitingInfo.frameRect || info.clientRect != _waitingInfo.clientRect) {
		_waitingInfo = info;
		_stableSince = now;
	} else if (now - _stableSince >= SETTLE_TIME) {
		_StartScaling(std::string(_windowId), _isWindowedMode);
	}
}

void ScalingService::_Toggle(bool windowedMode) noexcept {
	if (_state == State::Scaling) {
		if (_session->IsWindowedMode() == windowedMode || !_session->IsSourceActive()) {
			_StopScaling(true);
		} else {
			// The other way of scaling for the same window
			_StartScaling(std::string(_windowId), windowedMode);
		}
		return;
	}

	// Also ends a wait for a minimized source
	_bridge.ListWindows();
	const std::string active = _bridge.ActiveWindowId();
	if (active.empty()) {
		Logger::Get().Warn("No window is active");
		_StopScaling(true);
	} else {
		_StartScaling(active, windowedMode);
	}
}

void ScalingService::_ScaleWindow(const std::string& query, bool windowedMode) noexcept {
	const std::vector<WindowInfo> windows = _bridge.ListWindows();
	const std::vector<const WindowInfo*> found = FindWindows(windows, query);
	if (found.size() == 1) {
		_StartScaling(found[0]->id, windowedMode);
	} else {
		Logger::Get().Error(fmt::format("\"{}\" matches {} windows", query, found.size()));
	}
}

// Auto-scale: a window that becomes active and has a profile that says so is
// scaled. Port of ScalingService::_CheckForegroundTimer_Tick, which looks at
// the foreground window every 50 ms; here the desktop reports the change.
void ScalingService::_CheckForeground() noexcept {
	const std::string active = _bridge.ActiveWindowId();
	if (active.empty() || active == _checkedWindow) {
		return;
	}

	WindowInfo info;
	if (!_bridge.GetWindow(active, info)) {
		return;
	}

	if (!(_state != State::Idle && active == _windowId) && !_IsOwnOrForbidden(info)) {
		const std::shared_ptr<const Settings> settings = _config.settings ? _config.settings() : nullptr;
		if (!settings) {
			return;
		}

		const Window window = _Identify(info);
		const Profile* profile = FindProfileForWindow(*settings, window.className, window.path, true);
		// While a window is scaled, another window of its program is not
		// scaled in its place: it is most likely a dialog of it. Windows
		// looks at the owner of the window, which the desktop does not tell
		// here.
		if (profile && !(_state != State::Idle && info.pid != 0 && info.pid == _windowPid)) {
			// A window that is not shown yet is looked at again when it changes
			if (info.isMinimized || info.frameRect.right <= info.frameRect.left) {
				return;
			}

			// Auto-scale can end the scaling of another window
			_checkedWindow = active;
			_StartScaling(active, profile->autoScale == AutoScale::Windowed);
			return;
		}
	}

	_checkedWindow = active;
}

void ScalingService::Run() noexcept {
	const int busFd = sd_bus_get_fd(_bus);

	while (!_isQuitRequested) {
		if (_session) {
			const int wakeFds[3]{ busFd, _taskFd, _config.quitFd };
			if (!_session->Step(std::span(wakeFds, _config.quitFd >= 0 ? 3 : 2))) {
				_OnSessionEnded();
			}
		} else {
			pollfd fds[4]{
				{ busFd, POLLIN, 0 },
				{ _taskFd, POLLIN, 0 },
				{ _bridge.EventFd(), POLLIN, 0 },
				{ _config.quitFd, POLLIN, 0 }
			};
			poll(fds, _config.quitFd >= 0 ? 4 : 3, _state == State::Waiting ? 20 : -1);
			if (!_bridge.Dispatch()) {
				Logger::Get().Error("The connection to the desktop is lost");
				break;
			}
		}

		while (sd_bus_process(_bus, nullptr) > 0) {
		}

		if (_config.quitFd >= 0) {
			pollfd quit{ _config.quitFd, POLLIN, 0 };
			if (poll(&quit, 1, 0) > 0) {
				break;
			}
		}

		for (const std::string& id : _shortcuts.TakeActivated()) {
			if (id == SHORTCUT_SCALE) {
				_Toggle(false);
			} else if (id == SHORTCUT_SCALE_WINDOWED) {
				_Toggle(true);
			} else if (id == SHORTCUT_SCREENSHOT) {
				if (!_session || !_session->TakeScreenshot()) {
					Logger::Get().Warn("No screenshot: no window is scaled");
				}
			} else if (id == SHORTCUT_TOOLBAR) {
				if (_session) {
					_session->SwitchToolbarState();
				}
			}
		}
		if (_shortcuts.TakeChanged()) {
			_UpdateShortcuts();
		}

		_RunTasks();
		if (_isQuitRequested) {
			break;
		}

		if (_state == State::Waiting) {
			_CheckWaiting();
		}
		_CheckForeground();
		_AnnounceChanges();
		sd_bus_flush(_bus);
	}

	_StopScaling(true);
	sd_bus_flush(_bus);
}

}
