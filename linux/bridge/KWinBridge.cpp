#include "KWinBridge.h"
#include <algorithm>
#include <cstring>
#include <poll.h>
#include <fmt/format.h>
#include <wayland-client.h>
#include "Logger.h"
#include "plasma-window-management-client-protocol.h"
#include "zkde-screencast-unstable-v1-client-protocol.h"

namespace Magpie {

// The newest versions this code has been written against. Version 5 of the
// screen cast protocol is the last that announces the stream by the id of its
// PipeWire node, which PipeWireFrameSource connects to.
static constexpr uint32_t WINDOW_MANAGEMENT_VERSION = 18;
static constexpr uint32_t SCREENCAST_VERSION = 5;
// Window ids and the list of all windows exist from this version on
static constexpr uint32_t MIN_WINDOW_MANAGEMENT_VERSION = 13;

struct KWinBridge::Connection {
	struct Window {
		WindowInfo info;
		org_kde_plasma_window* proxy = nullptr;
		// The first values of all properties have arrived
		bool isComplete = false;
		bool isUnmapped = false;
	};

	~Connection() {
		for (const std::unique_ptr<Window>& window : windows) {
			org_kde_plasma_window_destroy(window->proxy);
		}
		if (windowManagement) {
			org_kde_plasma_window_management_destroy(windowManagement);
		}
		if (screencast) {
			zkde_screencast_unstable_v1_destroy(screencast);
		}
		if (registry) {
			wl_registry_destroy(registry);
		}
		if (display) {
			wl_display_disconnect(display);
		}
	}

	// Processes the events that have arrived without waiting for more
	bool PumpEvents() noexcept {
		while (wl_display_prepare_read(display) != 0) {
			wl_display_dispatch_pending(display);
		}
		wl_display_flush(display);

		pollfd fd{ wl_display_get_fd(display), POLLIN, 0 };
		if (poll(&fd, 1, 0) > 0) {
			wl_display_read_events(display);
		} else {
			wl_display_cancel_read(display);
		}
		return wl_display_dispatch_pending(display) >= 0;
	}

	static const org_kde_plasma_window_listener WINDOW_LISTENER;
	static const org_kde_plasma_window_management_listener WINDOW_MANAGEMENT_LISTENER;

	wl_display* display = nullptr;
	wl_registry* registry = nullptr;
	org_kde_plasma_window_management* windowManagement = nullptr;
	uint32_t windowManagementVersion = 0;
	zkde_screencast_unstable_v1* screencast = nullptr;
	uint32_t screencastVersion = 0;

	std::vector<std::unique_ptr<Window>> windows;
};

///////////////////////////////////////////////////////////////////////////////
// Windows

const org_kde_plasma_window_listener KWinBridge::Connection::WINDOW_LISTENER{
	.title_changed = [](void* data, org_kde_plasma_window*, const char* title) {
		((Window*)data)->info.title = title;
	},
	.app_id_changed = [](void* data, org_kde_plasma_window*, const char* appId) {
		((Window*)data)->info.appId = appId;
	},
	.state_changed = [](void* data, org_kde_plasma_window*, uint32_t state) {
		WindowInfo& info = ((Window*)data)->info;
		info.isActive = state & ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_ACTIVE;
		info.isMinimized = state & ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_MINIMIZED;
		info.isFullscreen = state & ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_FULLSCREEN;
	},
	.virtual_desktop_changed = [](void*, org_kde_plasma_window*, int32_t) {},
	.themed_icon_name_changed = [](void*, org_kde_plasma_window*, const char*) {},
	.unmapped = [](void* data, org_kde_plasma_window*) {
		((Window*)data)->isUnmapped = true;
	},
	.initial_state = [](void* data, org_kde_plasma_window*) {
		((Window*)data)->isComplete = true;
	},
	.parent_window = [](void*, org_kde_plasma_window*, org_kde_plasma_window*) {},
	.geometry = [](void* data, org_kde_plasma_window*, int32_t x, int32_t y, uint32_t width, uint32_t height) {
		((Window*)data)->info.frameRect = { x, y, x + (int32_t)width, y + (int32_t)height };
	},
	.icon_changed = [](void*, org_kde_plasma_window*) {},
	.pid_changed = [](void* data, org_kde_plasma_window*, uint32_t pid) {
		((Window*)data)->info.pid = pid;
	},
	.virtual_desktop_entered = [](void*, org_kde_plasma_window*, const char*) {},
	.virtual_desktop_left = [](void*, org_kde_plasma_window*, const char*) {},
	.application_menu = [](void*, org_kde_plasma_window*, const char*, const char*) {},
	.activity_entered = [](void*, org_kde_plasma_window*, const char*) {},
	.activity_left = [](void*, org_kde_plasma_window*, const char*) {},
	.resource_name_changed = [](void* data, org_kde_plasma_window*, const char* resourceName) {
		((Window*)data)->info.resourceName = resourceName;
	},
	.client_geometry = [](void*, org_kde_plasma_window*, int32_t, int32_t, uint32_t, uint32_t) {},
	.mapped = [](void*, org_kde_plasma_window*) {}
};

const org_kde_plasma_window_management_listener KWinBridge::Connection::WINDOW_MANAGEMENT_LISTENER{
	.show_desktop_changed = [](void*, org_kde_plasma_window_management*, uint32_t) {},
	.window = [](void*, org_kde_plasma_window_management*, uint32_t) {},
	.stacking_order_changed = [](void*, org_kde_plasma_window_management*, wl_array*) {},
	.stacking_order_uuid_changed = [](void*, org_kde_plasma_window_management*, const char*) {},
	// Sent for every existing window after binding, and for every new one
	.window_with_uuid = [](void* data, org_kde_plasma_window_management* management, uint32_t, const char* uuid) {
		Connection& connection = *(Connection*)data;

		auto window = std::make_unique<Window>();
		window->info.id = uuid;
		window->proxy = org_kde_plasma_window_management_get_window_by_uuid(management, uuid);
		org_kde_plasma_window_add_listener(window->proxy, &WINDOW_LISTENER, window.get());
		connection.windows.push_back(std::move(window));
	},
	.stacking_order_changed_2 = [](void*, org_kde_plasma_window_management*) {}
};

///////////////////////////////////////////////////////////////////////////////
// Screen cast

class KWinBridge::Session final : public CaptureSession {
public:
	Session(std::shared_ptr<Connection> connection, zkde_screencast_stream_unstable_v1* stream) noexcept :
		_connection(std::move(connection)), _stream(stream) {
		zkde_screencast_stream_unstable_v1_add_listener(_stream, &LISTENER, this);
	}

	~Session() override {
		zkde_screencast_stream_unstable_v1_close(_stream);
		wl_display_flush(_connection->display);
	}

	// Waits for KWin's answer to the request
	bool WaitUntilCreated() noexcept {
		while (_nodeId == 0 && !_isClosed) {
			if (wl_display_roundtrip(_connection->display) < 0) {
				Logger::Get().Error("The Wayland connection was lost while waiting for the screen cast");
				return false;
			}
		}
		return _nodeId != 0;
	}

	int PipeWireFd() const noexcept override {
		return -1;
	}

	uint32_t NodeId() const noexcept override {
		return _nodeId;
	}

	int EventFd() const noexcept override {
		return wl_display_get_fd(_connection->display);
	}

	bool Dispatch() noexcept override {
		if (!_connection->PumpEvents()) {
			Logger::Get().Error("The Wayland connection of the KWin back end is lost");
			_isClosed = true;
		}
		return !_isClosed;
	}

private:
	// Declared before the listener, whose functions use them
	std::shared_ptr<Connection> _connection;
	zkde_screencast_stream_unstable_v1* _stream;
	uint32_t _nodeId = 0;
	bool _isClosed = false;

	static constexpr zkde_screencast_stream_unstable_v1_listener LISTENER{
		.closed = [](void* data, zkde_screencast_stream_unstable_v1*) {
			Logger::Get().Info("KWin has closed the screen cast");
			((Session*)data)->_isClosed = true;
		},
		.created = [](void* data, zkde_screencast_stream_unstable_v1*, uint32_t node) {
			((Session*)data)->_nodeId = node;
		},
		.failed = [](void* data, zkde_screencast_stream_unstable_v1*, const char* error) {
			Logger::Get().Error(fmt::format("KWin refused the screen cast: {}", error));
			((Session*)data)->_isClosed = true;
		},
		.serial = [](void*, zkde_screencast_stream_unstable_v1*, uint32_t, uint32_t) {}
	};

};

///////////////////////////////////////////////////////////////////////////////

KWinBridge::KWinBridge() noexcept = default;

KWinBridge::~KWinBridge() = default;

bool KWinBridge::Initialize() noexcept {
	auto connection = std::make_shared<Connection>();
	connection->display = wl_display_connect(nullptr);
	if (!connection->display) {
		Logger::Get().Info("There is no Wayland compositor to connect to");
		return false;
	}

	static const wl_registry_listener REGISTRY_LISTENER{
		.global = [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
			Connection& connection = *(Connection*)data;
			if (std::strcmp(interface, org_kde_plasma_window_management_interface.name) == 0) {
				if (version >= MIN_WINDOW_MANAGEMENT_VERSION) {
					connection.windowManagementVersion = std::min(version, WINDOW_MANAGEMENT_VERSION);
					connection.windowManagement = (org_kde_plasma_window_management*)wl_registry_bind(registry, name,
						&org_kde_plasma_window_management_interface, connection.windowManagementVersion);
					org_kde_plasma_window_management_add_listener(
						connection.windowManagement, &Connection::WINDOW_MANAGEMENT_LISTENER, &connection);
				}
			} else if (std::strcmp(interface, zkde_screencast_unstable_v1_interface.name) == 0) {
				connection.screencastVersion = std::min(version, SCREENCAST_VERSION);
				connection.screencast = (zkde_screencast_unstable_v1*)wl_registry_bind(registry, name,
					&zkde_screencast_unstable_v1_interface, connection.screencastVersion);
			}
		},
		.global_remove = [](void*, wl_registry*, uint32_t) {}
	};

	connection->registry = wl_display_get_registry(connection->display);
	wl_registry_add_listener(connection->registry, &REGISTRY_LISTENER, connection.get());
	if (wl_display_roundtrip(connection->display) < 0) {
		Logger::Get().Error("The Wayland connection failed");
		return false;
	}

	if (!connection->windowManagement && !connection->screencast) {
		Logger::Get().Info("The compositor offers neither of KWin's interfaces");
		return false;
	}

	Logger::Get().Info(fmt::format("KWin back end: window management version {}, screen cast version {}",
		connection->windowManagementVersion, connection->screencastVersion));

	_connection = std::move(connection);
	return true;
}

uint32_t KWinBridge::WindowManagementVersion() const noexcept {
	return _connection ? _connection->windowManagementVersion : 0;
}

uint32_t KWinBridge::ScreencastVersion() const noexcept {
	return _connection ? _connection->screencastVersion : 0;
}

std::vector<WindowInfo> KWinBridge::ListWindows() noexcept {
	std::vector<WindowInfo> result;
	if (!_connection || !_connection->windowManagement) {
		return result;
	}

	// The first round trip brings the windows that are new, the second their
	// properties
	if (wl_display_roundtrip(_connection->display) < 0 || wl_display_roundtrip(_connection->display) < 0) {
		Logger::Get().Error("The Wayland connection of the KWin back end is lost");
		return result;
	}

	std::erase_if(_connection->windows, [](const std::unique_ptr<Connection::Window>& window) {
		if (window->isUnmapped) {
			org_kde_plasma_window_destroy(window->proxy);
		}
		return window->isUnmapped;
	});

	for (const std::unique_ptr<Connection::Window>& window : _connection->windows) {
		if (window->isComplete) {
			result.push_back(window->info);
		}
	}
	return result;
}

std::unique_ptr<CaptureSession> KWinBridge::CaptureWindow(const std::string& windowId) noexcept {
	if (!_connection || !_connection->screencast) {
		Logger::Get().Error("KWin's screen cast interface is not available");
		return nullptr;
	}

	auto session = std::make_unique<Session>(_connection, zkde_screencast_unstable_v1_stream_window(
		_connection->screencast, windowId.c_str(), ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_METADATA));
	if (!session->WaitUntilCreated()) {
		return nullptr;
	}

	Logger::Get().Info(fmt::format("KWin casts window {} as PipeWire node {}", windowId, session->NodeId()));
	return session;
}

}
