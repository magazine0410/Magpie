#include "KWinBridge.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fmt/format.h>
#include <rapidjson/document.h>
#include <systemd/sd-bus.h>
#include <wayland-client.h>
#include "KWinPointerSpeed.h"
#include "Logger.h"
#include "fake-input-client-protocol.h"
#include "plasma-window-management-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "zkde-screencast-unstable-v1-client-protocol.h"

extern char** environ;

namespace Magpie {

// The newest versions this code has been written against. Version 5 of the
// screen cast protocol is the last that announces the stream by the id of its
// PipeWire node, which PipeWireFrameSource connects to. Version 19 of the
// window management names the "no border" state.
static constexpr uint32_t WINDOW_MANAGEMENT_VERSION = 19;
static constexpr uint32_t SCREENCAST_VERSION = 5;
static constexpr uint32_t FAKE_INPUT_VERSION = 4;
// Window ids and the list of all windows exist from this version on
static constexpr uint32_t MIN_WINDOW_MANAGEMENT_VERSION = 13;
// Virtual outputs
static constexpr uint32_t MIN_SCREENCAST_VERSION_FOR_VIRTUAL = 2;
// Absolute pointer positions
static constexpr uint32_t MIN_FAKE_INPUT_VERSION = 3;
// The name of an output
static constexpr uint32_t OUTPUT_VERSION = 4;

struct KWinBridge::Connection {
	struct Window {
		WindowInfo info;
		org_kde_plasma_window* proxy = nullptr;
		bool hasClientRect = false;
		// The first values of all properties have arrived
		bool isComplete = false;
		bool isUnmapped = false;
	};

	struct Output {
		// Name and refresh rate; the rectangle and the scale are worked out
		// from what follows (Info)
		OutputInfo info;
		wl_output* proxy = nullptr;
		zxdg_output_v1* xdgOutput = nullptr;
		// The number the registry announced it with
		uint32_t registryName = 0;
		bool isComplete = false;
		// From wl_output: the place, and the size of the mode in pixels
		Point position;
		Size modeSize;
		// From xdg_output: place and size in desktop coordinates; without a
		// scale factor the same as the two above
		Point logicalPosition;
		Size logicalSize;

		OutputInfo Info() const noexcept {
			OutputInfo result = info;
			if (logicalSize.width > 0 && logicalSize.height > 0) {
				result.rect = {
					logicalPosition.x, logicalPosition.y,
					logicalPosition.x + logicalSize.width, logicalPosition.y + logicalSize.height
				};
				// The longer sides, so that a turned output gives the same
				result.scale = (double)std::max(modeSize.width, modeSize.height) /
					std::max(logicalSize.width, logicalSize.height);
			} else {
				result.rect = { position.x, position.y, position.x + modeSize.width, position.y + modeSize.height };
			}
			return result;
		}
	};

	// Asks for the place and size of an output in desktop coordinates
	void AddXdgOutput(Output& output) noexcept {
		if (xdgOutputManager && !output.xdgOutput) {
			output.xdgOutput = zxdg_output_manager_v1_get_xdg_output(xdgOutputManager, output.proxy);
			zxdg_output_v1_add_listener(output.xdgOutput, &XDG_OUTPUT_LISTENER, &output);
		}
	}

	static const zxdg_output_v1_listener XDG_OUTPUT_LISTENER;

	~Connection() {
		for (const std::unique_ptr<Window>& window : windows) {
			org_kde_plasma_window_destroy(window->proxy);
		}
		for (const std::unique_ptr<Output>& output : outputs) {
			if (output->xdgOutput) {
				zxdg_output_v1_destroy(output->xdgOutput);
			}
			wl_output_destroy(output->proxy);
		}
		if (windowManagement) {
			org_kde_plasma_window_management_destroy(windowManagement);
		}
		if (screencast) {
			zkde_screencast_unstable_v1_destroy(screencast);
		}
		if (fakeInput) {
			// The request "destroy" exists only from version 5 on
			wl_proxy_destroy((wl_proxy*)fakeInput);
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
		if (wl_display_dispatch_pending(display) < 0) {
			return false;
		}

		RemoveUnmappedWindows();
		return true;
	}

	void RemoveUnmappedWindows() noexcept {
		std::erase_if(windows, [](const std::unique_ptr<Window>& window) {
			if (window->isUnmapped) {
				org_kde_plasma_window_destroy(window->proxy);
			}
			return window->isUnmapped;
		});
	}

	const Window* FindWindow(const std::string& id) const noexcept {
		for (const std::unique_ptr<Window>& window : windows) {
			if (window->isComplete && window->info.id == id) {
				return window.get();
			}
		}
		return nullptr;
	}

	static const wl_registry_listener REGISTRY_LISTENER;
	static const wl_output_listener OUTPUT_LISTENER;
	static const org_kde_plasma_window_listener WINDOW_LISTENER;
	static const org_kde_plasma_window_management_listener WINDOW_MANAGEMENT_LISTENER;

	wl_display* display = nullptr;
	wl_registry* registry = nullptr;
	org_kde_plasma_window_management* windowManagement = nullptr;
	uint32_t windowManagementVersion = 0;
	zkde_screencast_unstable_v1* screencast = nullptr;
	uint32_t screencastVersion = 0;
	org_kde_kwin_fake_input* fakeInput = nullptr;
	zxdg_output_manager_v1* xdgOutputManager = nullptr;

	std::vector<std::unique_ptr<Window>> windows;
	std::vector<std::unique_ptr<Output>> outputs;
	uint32_t scriptCount = 0;
};

///////////////////////////////////////////////////////////////////////////////
// Outputs

const wl_output_listener KWinBridge::Connection::OUTPUT_LISTENER{
	.geometry = [](void* data, wl_output*, int32_t x, int32_t y, int32_t, int32_t, int32_t,
		const char*, const char*, int32_t) {
		((Output*)data)->position = { x, y };
	},
	.mode = [](void* data, wl_output*, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
		if (flags & WL_OUTPUT_MODE_CURRENT) {
			Output& output = *(Output*)data;
			output.modeSize = { width, height };
			output.info.refreshRate = refresh;
		}
	},
	.done = [](void* data, wl_output*) {
		((Output*)data)->isComplete = true;
	},
	.scale = [](void*, wl_output*, int32_t) {},
	.name = [](void* data, wl_output*, const char* name) {
		((Output*)data)->info.name = name;
	},
	.description = [](void*, wl_output*, const char*) {}
};

const zxdg_output_v1_listener KWinBridge::Connection::XDG_OUTPUT_LISTENER{
	.logical_position = [](void* data, zxdg_output_v1*, int32_t x, int32_t y) {
		((Output*)data)->logicalPosition = { x, y };
	},
	.logical_size = [](void* data, zxdg_output_v1*, int32_t width, int32_t height) {
		((Output*)data)->logicalSize = { width, height };
	},
	.done = [](void*, zxdg_output_v1*) {},
	.name = [](void*, zxdg_output_v1*, const char*) {},
	.description = [](void*, zxdg_output_v1*, const char*) {}
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
		info.isMaximized = state & ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_MAXIMIZED;
		info.isFullscreen = state & ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_FULLSCREEN;
		info.hasNoBorder = state & ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_NO_BORDER;
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
		Window& window = *(Window*)data;
		window.info.frameRect = { x, y, x + (int32_t)width, y + (int32_t)height };
		if (!window.hasClientRect) {
			window.info.clientRect = window.info.frameRect;
		}
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
	.client_geometry = [](void* data, org_kde_plasma_window*, int32_t x, int32_t y, uint32_t width, uint32_t height) {
		Window& window = *(Window*)data;
		window.info.clientRect = { x, y, x + (int32_t)width, y + (int32_t)height };
		window.hasClientRect = true;
	},
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

const wl_registry_listener KWinBridge::Connection::REGISTRY_LISTENER{
	.global = [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
		Connection& connection = *(Connection*)data;
		if (std::strcmp(interface, org_kde_plasma_window_management_interface.name) == 0) {
			if (version >= MIN_WINDOW_MANAGEMENT_VERSION) {
				connection.windowManagementVersion = std::min(version, WINDOW_MANAGEMENT_VERSION);
				connection.windowManagement = (org_kde_plasma_window_management*)wl_registry_bind(registry, name,
					&org_kde_plasma_window_management_interface, connection.windowManagementVersion);
				org_kde_plasma_window_management_add_listener(
					connection.windowManagement, &WINDOW_MANAGEMENT_LISTENER, &connection);
			}
		} else if (std::strcmp(interface, zkde_screencast_unstable_v1_interface.name) == 0) {
			connection.screencastVersion = std::min(version, SCREENCAST_VERSION);
			connection.screencast = (zkde_screencast_unstable_v1*)wl_registry_bind(registry, name,
				&zkde_screencast_unstable_v1_interface, connection.screencastVersion);
		} else if (std::strcmp(interface, org_kde_kwin_fake_input_interface.name) == 0) {
			if (version >= MIN_FAKE_INPUT_VERSION) {
				connection.fakeInput = (org_kde_kwin_fake_input*)wl_registry_bind(registry, name,
					&org_kde_kwin_fake_input_interface, std::min(version, FAKE_INPUT_VERSION));
				org_kde_kwin_fake_input_authenticate(connection.fakeInput, "Magpie",
					"Moves the pointer between the scaled picture and the window it shows");
			}
		} else if (std::strcmp(interface, wl_output_interface.name) == 0 && version >= OUTPUT_VERSION) {
			auto output = std::make_unique<Output>();
			output->registryName = name;
			output->proxy = (wl_output*)wl_registry_bind(registry, name, &wl_output_interface, OUTPUT_VERSION);
			wl_output_add_listener(output->proxy, &OUTPUT_LISTENER, output.get());
			connection.AddXdgOutput(*output);
			connection.outputs.push_back(std::move(output));
		} else if (std::strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
			connection.xdgOutputManager = (zxdg_output_manager_v1*)wl_registry_bind(registry, name,
				&zxdg_output_manager_v1_interface, std::min(version, 3u));
			// For the outputs that were announced before it
			for (const std::unique_ptr<Output>& output : connection.outputs) {
				connection.AddXdgOutput(*output);
			}
		}
	},
	.global_remove = [](void* data, wl_registry*, uint32_t name) {
		Connection& connection = *(Connection*)data;
		std::erase_if(connection.outputs, [&](const std::unique_ptr<Output>& output) {
			if (output->registryName == name) {
				if (output->xdgOutput) {
				zxdg_output_v1_destroy(output->xdgOutput);
			}
			wl_output_destroy(output->proxy);
				return true;
			}
			return false;
		});
	}
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
		// A virtual output goes with its stream; the round trip makes sure
		// KWin has done that before the caller goes on
		wl_display_roundtrip(_connection->display);
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

	connection->registry = wl_display_get_registry(connection->display);
	wl_registry_add_listener(connection->registry, &Connection::REGISTRY_LISTENER, connection.get());
	if (wl_display_roundtrip(connection->display) < 0) {
		Logger::Get().Error("The Wayland connection failed");
		return false;
	}

	if (!connection->windowManagement && !connection->screencast) {
		Logger::Get().Info("The compositor offers neither of KWin's interfaces");
		return false;
	}

	Logger::Get().Info(fmt::format("KWin back end: window management version {}, screen cast version {}, fake input {}",
		connection->windowManagementVersion, connection->screencastVersion, connection->fakeInput ? "yes" : "no"));

	_connection = std::move(connection);
	return true;
}

uint32_t KWinBridge::WindowManagementVersion() const noexcept {
	return _connection ? _connection->windowManagementVersion : 0;
}

uint32_t KWinBridge::ScreencastVersion() const noexcept {
	return _connection ? _connection->screencastVersion : 0;
}

bool KWinBridge::HasFakeInput() const noexcept {
	return _connection && _connection->fakeInput;
}

DesktopBridge::Traits KWinBridge::GetTraits() const noexcept {
	return {
		// KWin computes the mode, and the formula for that only yields
		// widths that are multiples of 8
		.customModeWidthStep = 8,
		.activationMovesPointer = false,
		.opensWindowsOnActiveOutput = false,
		.tilesNewWindows = false,
		.canMinimize = true,
		.captureMethod = "Virtual output (PipeWire)"
	};
}

std::string KWinBridge::MissingForScaling() const noexcept {
	std::string missing;
	const auto add = [&](bool isThere, const char* what) {
		if (!isThere) {
			missing += missing.empty() ? "" : ", ";
			missing += what;
		}
	};
	add(WindowManagementVersion() != 0, "the window list (org_kde_plasma_window_management)");
	add(ScreencastVersion() != 0, "virtual outputs (zkde_screencast_unstable_v1)");
	add(HasFakeInput(), "pointer movement (org_kde_kwin_fake_input)");
	return missing;
}

std::vector<PointerDevice> KWinBridge::ReadPointerDevices() noexcept {
	return KWinPointerSpeed::ReadDevices();
}

void KWinBridge::ApplyPointerSpeed(std::span<const PointerDevice> devices) noexcept {
	KWinPointerSpeed::Apply(devices);
}

int KWinBridge::EventFd() const noexcept {
	return wl_display_get_fd(_connection->display);
}

bool KWinBridge::Dispatch() noexcept {
	if (!_connection->PumpEvents()) {
		Logger::Get().Error("The Wayland connection of the KWin back end is lost");
		return false;
	}
	return true;
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
	_connection->RemoveUnmappedWindows();

	for (const std::unique_ptr<Connection::Window>& window : _connection->windows) {
		if (window->isComplete) {
			result.push_back(window->info);
		}
	}
	return result;
}

bool KWinBridge::GetWindow(const std::string& windowId, WindowInfo& info) const noexcept {
	const Connection::Window* window = _connection->FindWindow(windowId);
	if (!window) {
		return false;
	}
	info = window->info;
	return true;
}

std::string KWinBridge::ActiveWindowId() const noexcept {
	for (const std::unique_ptr<Connection::Window>& window : _connection->windows) {
		if (window->isComplete && window->info.isActive) {
			return window->info.id;
		}
	}
	return {};
}

std::vector<OutputInfo> KWinBridge::ListOutputs() noexcept {
	std::vector<OutputInfo> result;
	if (!_connection) {
		return result;
	}

	// As for the windows: new outputs, then their properties
	if (wl_display_roundtrip(_connection->display) < 0 || wl_display_roundtrip(_connection->display) < 0) {
		Logger::Get().Error("The Wayland connection of the KWin back end is lost");
		return result;
	}

	for (const std::unique_ptr<Connection::Output>& output : _connection->outputs) {
		if (output->isComplete) {
			result.push_back(output->Info());
			// The name KWin gives the outputs of its screen cast protocol:
			// "Virtual-" and what the program called it. A KWin that runs
			// without a screen (kwin_wayland --virtual) numbers its own
			// outputs behind the same word; those are its monitors.
			const std::string& name = output->info.name;
			result.back().isVirtual = name.starts_with("Virtual-") &&
				!std::all_of(name.begin() + 8, name.end(), [](char c) { return c >= '0' && c <= '9'; });
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

std::unique_ptr<CaptureSession> KWinBridge::CreateVirtualOutput(
	const std::string& name,
	Size size,
	std::string& outputName
) noexcept {
	if (!_connection || _connection->screencastVersion < MIN_SCREENCAST_VERSION_FOR_VIRTUAL) {
		Logger::Get().Error("KWin's screen cast interface does not offer virtual outputs");
		return nullptr;
	}

	auto session = std::make_unique<Session>(_connection, zkde_screencast_unstable_v1_stream_virtual_output(
		_connection->screencast, name.c_str(), size.width, size.height, wl_fixed_from_int(1),
		ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_METADATA));
	if (!session->WaitUntilCreated()) {
		return nullptr;
	}

	// KWin names the output after the stream
	outputName = "Virtual-" + name;
	for (int attempt = 0; attempt < 20; ++attempt) {
		for (const OutputInfo& output : ListOutputs()) {
			if (output.name == outputName) {
				Logger::Get().Info(fmt::format("Virtual output {} of {}x{} is PipeWire node {}",
					outputName, size.width, size.height, session->NodeId()));
				return session;
			}
		}
		usleep(10'000);
	}

	Logger::Get().Error(fmt::format("The virtual output {} did not appear", outputName));
	return nullptr;
}

// Runs a program and waits for it. Its standard output goes to `output`, or
// is discarded without one.
static bool RunProgram(const std::vector<std::string>& arguments, std::string* output = nullptr) noexcept {
	std::vector<char*> argv;
	for (const std::string& argument : arguments) {
		argv.push_back(const_cast<char*>(argument.c_str()));
	}
	argv.push_back(nullptr);

	int pipeFds[2]{ -1, -1 };
	if (output && pipe2(pipeFds, O_CLOEXEC) < 0) {
		return false;
	}

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	if (output) {
		posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
	} else {
		posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
	}
	posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

	pid_t pid = 0;
	const int result = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
	posix_spawn_file_actions_destroy(&actions);
	if (output) {
		close(pipeFds[1]);
	}
	if (result != 0) {
		Logger::Get().Error(fmt::format("Starting {} failed: {}", arguments[0], std::strerror(result)));
		if (output) {
			close(pipeFds[0]);
		}
		return false;
	}

	if (output) {
		output->clear();
		char buffer[4096];
		ssize_t count;
		while ((count = read(pipeFds[0], buffer, sizeof(buffer))) > 0 || (count < 0 && errno == EINTR)) {
			if (count > 0) {
				output->append(buffer, (size_t)count);
			}
		}
		close(pipeFds[0]);
	}

	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) {
			return false;
		}
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::vector<KWinOutputModes::Mode> KWinOutputModes::Parse(
	const std::string& json,
	const std::string& outputName
) noexcept {
	std::vector<Mode> result;

	rapidjson::Document document;
	document.Parse(json.c_str(), json.size());
	if (document.HasParseError() || !document.IsObject()) {
		return result;
	}
	const auto outputs = document.FindMember("outputs");
	if (outputs == document.MemberEnd() || !outputs->value.IsArray()) {
		return result;
	}

	for (const auto& output : outputs->value.GetArray()) {
		if (!output.IsObject()) {
			continue;
		}
		const auto name = output.FindMember("name");
		const auto modes = output.FindMember("modes");
		if (name == output.MemberEnd() || !name->value.IsString() || outputName != name->value.GetString() ||
			modes == output.MemberEnd() || !modes->value.IsArray()) {
			continue;
		}

		for (const auto& mode : modes->value.GetArray()) {
			if (!mode.IsObject()) {
				continue;
			}
			const auto id = mode.FindMember("id");
			const auto size = mode.FindMember("size");
			const auto refreshRate = mode.FindMember("refreshRate");
			if (id == mode.MemberEnd() || !id->value.IsString() ||
				size == mode.MemberEnd() || !size->value.IsObject() ||
				refreshRate == mode.MemberEnd() || !refreshRate->value.IsNumber()) {
				continue;
			}
			const auto width = size->value.FindMember("width");
			const auto height = size->value.FindMember("height");
			if (width == size->value.MemberEnd() || !width->value.IsInt() ||
				height == size->value.MemberEnd() || !height->value.IsInt()) {
				continue;
			}
			result.push_back({
				id->value.GetString(), { width->value.GetInt(), height->value.GetInt() }, refreshRate->value.GetDouble()
			});
		}
	}
	return result;
}

const KWinOutputModes::Mode* KWinOutputModes::Choose(
	const std::vector<Mode>& modes,
	Size size,
	int32_t refreshRate
) noexcept {
	const Mode* best = nullptr;
	for (const Mode& mode : modes) {
		if (mode.size != size) {
			continue;
		}
		if (!best || std::abs(mode.refreshRate * 1000 - refreshRate) < std::abs(best->refreshRate * 1000 - refreshRate)) {
			best = &mode;
		}
	}
	return best;
}

bool KWinOutputModes::IsCloseEnough(const Mode& mode, int32_t refreshRate) noexcept {
	// A rate of 200 Hz comes out as 198.99 Hz for some sizes
	return std::abs(mode.refreshRate * 1000 - refreshRate) <= refreshRate * 0.02;
}

bool KWinBridge::ConfigureOutput(
	const std::string& outputName,
	Size size,
	Point position,
	int32_t refreshRate
) noexcept {
	// kscreen-doctor is the command line front end of the library that
	// Plasma's own display settings use.
	static constexpr const char* TOOL = "kscreen-doctor";
	const std::string prefix = "output." + outputName;

	const auto queryModes = [&]() {
		std::string json;
		RunProgram({ TOOL, "-j" }, &json);
		return KWinOutputModes::Parse(json, outputName);
	};

	// The output has the mode of the screen cast's size with 60 Hz, and
	// every mode that was ever made for an output of this name, because
	// KWin keeps those. A mode is made only if none of them will do;
	// otherwise the list KWin keeps would grow with every start.
	std::vector<KWinOutputModes::Mode> modes = queryModes();
	const KWinOutputModes::Mode* mode = KWinOutputModes::Choose(modes, size, refreshRate);
	if (refreshRate > 60'500 && (!mode || !KWinOutputModes::IsCloseEnough(*mode, refreshRate))) {
		RunProgram({ TOOL, fmt::format("{}.addCustomMode.{}.{}.{}.full",
			prefix, size.width, size.height, refreshRate) });
		modes = queryModes();
		mode = KWinOutputModes::Choose(modes, size, refreshRate);
	}
	if (!mode) {
		Logger::Get().Error(fmt::format("The output {} has no mode of {}x{}", outputName, size.width, size.height));
		return false;
	}

	// The mode by its id and not by a name with a rounded refresh rate, and
	// together with the place, so that KWin does not fall back to the mode
	// it remembers for the name in between
	if (!RunProgram({
		TOOL,
		fmt::format("{}.mode.{}", prefix, mode->id),
		fmt::format("{}.position.{},{}", prefix, position.x, position.y)
	})) {
		Logger::Get().Error(fmt::format("Configuring the output {} with kscreen-doctor failed", outputName));
		return false;
	}

	// What KWin made of it
	for (const OutputInfo& output : ListOutputs()) {
		if (output.name == outputName) {
			Logger::Get().Info(fmt::format("Output {}: {}x{} at {},{} with {:.2f} Hz", outputName,
				output.rect.Width(), output.rect.Height(), output.rect.left, output.rect.top,
				output.refreshRate / 1000.0));
			if (output.rect.Width() != size.width || output.rect.Height() != size.height) {
				// The screen cast would show the output squeezed to its own size
				Logger::Get().Error(fmt::format("The output {} did not take the size {}x{}",
					outputName, size.width, size.height));
				return false;
			}
			if (refreshRate > 60'500 && std::abs(output.refreshRate - refreshRate) > refreshRate * 0.02) {
				// Not fatal: the source is then held to this rate
				Logger::Get().Warn(fmt::format("The output {} did not take the refresh rate {:.2f} Hz",
					outputName, refreshRate / 1000.0));
			}
			return output.rect.left == position.x && output.rect.top == position.y;
		}
	}
	return false;
}

// Loads a script into KWin, runs it and removes it again. KWin runs it some
// time after the call; the caller has to watch for its effect.
bool KWinBridge::_RunScript(const std::string& script) noexcept {
	const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
	const std::string name = fmt::format("magpie-{}-{}", getpid(), ++_connection->scriptCount);
	const std::string path = fmt::format("{}/{}.js", runtimeDir && *runtimeDir ? runtimeDir : "/tmp", name);
	{
		std::ofstream file(path);
		file << script;
		if (!file) {
			Logger::Get().Error(fmt::format("Writing {} failed", path));
			return false;
		}
	}

	sd_bus* bus = nullptr;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	int result = sd_bus_open_user(&bus);
	if (result >= 0) {
		result = sd_bus_call_method(bus, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting",
			"loadScript", &error, nullptr, "ss", path.c_str(), name.c_str());
	}
	if (result >= 0) {
		result = sd_bus_call_method(bus, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting",
			"start", &error, nullptr, "");
	}
	if (result < 0) {
		Logger::Get().Error(fmt::format("Running a KWin script failed: {}",
			error.message ? error.message : std::strerror(-result)));
	} else {
		// KWin reads the file and runs the script from its event loop. The
		// round trips give it the turns it needs before the script is removed.
		for (int i = 0; i < 3; ++i) {
			wl_display_roundtrip(_connection->display);
			usleep(5'000);
		}
		sd_bus_call_method(bus, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting",
			"unloadScript", nullptr, nullptr, "s", name.c_str());
	}

	sd_bus_error_free(&error);
	sd_bus_flush_close_unref(bus);
	unlink(path.c_str());
	return result >= 0;
}

bool KWinBridge::PlaceWindow(const std::string& windowId, const WindowPlacement& placement) noexcept {
	const Rect& rect = placement.frameRect;
	const bool toOutput = !placement.output.empty();
	const bool setGeometry = placement.setGeometry && !toOutput;
	// The id is a UUID in braces and the name of an output has no quotation
	// marks; nothing in them needs escaping
	const std::string script = fmt::format(R"(
for (const w of workspace.windowList()) {{
	if (w.internalId.toString() === "{}") {{
		w.noBorder = {};
		if ({}) {{
			w.frameGeometry = {{ x: {}, y: {}, width: {}, height: {} }};
		}}
		for (const screen of workspace.screens) {{
			if (screen.name === "{}") {{
				workspace.sendClientToScreen(w, screen);
			}}
		}}
		if ({}) {{
			workspace.activeWindow = w;
		}}
	}}
}}
)", windowId, placement.noBorder ? "true" : "false", setGeometry ? "true" : "false",
		rect.left, rect.top, rect.Width(), rect.Height(), placement.output, placement.activate ? "true" : "false");

	if (!_RunScript(script)) {
		return false;
	}

	// Wait until the window is reported where it should be
	const bool waitForRect = setGeometry || (toOutput && rect.Width() > 0);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
	while (true) {
		if (wl_display_roundtrip(_connection->display) < 0) {
			return false;
		}

		const Connection::Window* window = _connection->FindWindow(windowId);
		if (!window) {
			Logger::Get().Error("The window to place does not exist");
			return false;
		}
		if ((!waitForRect || window->info.frameRect == rect) &&
			window->info.hasNoBorder == placement.noBorder &&
			(!placement.activate || window->info.isActive)) {
			return true;
		}

		if (std::chrono::steady_clock::now() > deadline) {
			const Rect& actual = window->info.frameRect;
			Logger::Get().Error(fmt::format(
				"The window is at {},{} with {}x{} and not at {},{} with {}x{} (no border: {}, active: {})",
				actual.left, actual.top, actual.Width(), actual.Height(),
				rect.left, rect.top, rect.Width(), rect.Height(), window->info.hasNoBorder, window->info.isActive));
			return false;
		}
		usleep(5'000);
	}
}

bool KWinBridge::SetWindowProperties(const std::string& windowId, const WindowProperties& properties) noexcept {
	std::string body;
	const auto assign = [&](const char* name, const std::optional<bool>& value) {
		if (value) {
			body += fmt::format("\t\tw.{} = {};\n", name, *value ? "true" : "false");
		}
	};
	assign("keepAbove", properties.keepAbove);
	assign("minimized", properties.minimized);
	assign("skipTaskbar", properties.skipTaskbar);
	assign("skipSwitcher", properties.skipTaskbar);
	assign("skipPager", properties.skipTaskbar);
	if (properties.raise) {
		body += "\t\tworkspace.raiseWindow(w);\n";
	}

	return _RunScript(fmt::format(R"(
for (const w of workspace.windowList()) {{
	if (w.internalId.toString() === "{}") {{
{}	}}
}}
)", windowId, body));
}

bool KWinBridge::MovePointer(Point position) noexcept {
	if (!_connection || !_connection->fakeInput) {
		Logger::Get().Error("KWin's fake input interface is not available");
		return false;
	}

	org_kde_kwin_fake_input_pointer_motion_absolute(_connection->fakeInput,
		wl_fixed_from_int(position.x), wl_fixed_from_int(position.y));
	return wl_display_flush(_connection->display) >= 0;
}

}
