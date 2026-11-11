// For the tests that need a desktop: a Sway that the test starts itself, with
// outputs that no screen shows, and a small window to put in it. Nothing of
// it reaches the desktop the tests are started from: the Sway has a runtime
// directory of its own, uses no input device and no monitor.
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>
#include "Geometry.h"
#include "SwayIpc.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace SwayTest {

using namespace Magpie;
using namespace std::chrono_literals;

// Whether there is a program "sway" to start
inline bool IsSwayInstalled() {
	const char* path = std::getenv("PATH");
	if (!path) {
		return false;
	}
	std::string_view rest = path;
	while (!rest.empty()) {
		const size_t end = rest.find(':');
		const std::filesystem::path candidate = std::filesystem::path(rest.substr(0, end)) / "sway";
		if (access(candidate.c_str(), X_OK) == 0) {
			return true;
		}
		rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
	}
	return false;
}

// A Sway with one output of 1920x1080 at 120 Hz, called HEADLESS-1
class HeadlessSway {
public:
	HeadlessSway() {
		std::string pattern = (std::filesystem::temp_directory_path() / "magpie-sway-XXXXXX").string();
		if (!mkdtemp(pattern.data())) {
			return;
		}
		_dir = pattern;
		_socketPath = (_dir / "sway.sock").string();

		// Xwayland would open a socket in /tmp
		std::ofstream(_dir / "config") << "xwayland disable\n";

		_pid = fork();
		if (_pid == 0) {
			// Not left behind if the test program dies
			prctl(PR_SET_PDEATHSIG, SIGKILL);
			setenv("XDG_RUNTIME_DIR", _dir.c_str(), 1);
			setenv("SWAYSOCK", _socketPath.c_str(), 1);
			setenv("WLR_BACKENDS", "headless", 1);
			setenv("WLR_LIBINPUT_NO_DEVICES", "1", 1);
			unsetenv("WAYLAND_DISPLAY");
			unsetenv("DISPLAY");
			// MAGPIE_TEST_SWAY_LOG names a file that gets Sway's detailed log
			// and stays; otherwise the few lines Sway prints go away with
			// its directory
			const char* debugLog = std::getenv("MAGPIE_TEST_SWAY_LOG");
			const int log = open(debugLog ? debugLog : (_dir / "sway.log").c_str(),
				O_WRONLY | O_CREAT | O_TRUNC, 0600);
			if (log >= 0) {
				dup2(log, STDOUT_FILENO);
				dup2(log, STDERR_FILENO);
			}
			if (debugLog) {
				execlp("sway", "sway", "-d", "-c", (_dir / "config").c_str(), (char*)nullptr);
			}
			execlp("sway", "sway", "-c", (_dir / "config").c_str(), (char*)nullptr);
			_exit(127);
		}
		if (_pid < 0) {
			return;
		}

		// Until its two sockets are there
		const auto deadline = std::chrono::steady_clock::now() + 10s;
		while (std::chrono::steady_clock::now() < deadline) {
			if (_waylandDisplay.empty()) {
				std::error_code ec;
				for (const auto& entry : std::filesystem::directory_iterator(_dir, ec)) {
					const std::string name = entry.path().filename().string();
					if (name.starts_with("wayland-") && !name.ends_with(".lock")) {
						// An absolute path is taken in place of a name
						_waylandDisplay = entry.path().string();
					}
				}
			}
			if (!_waylandDisplay.empty() && _ipc.Connect(_socketPath)) {
				break;
			}
			int status;
			if (waitpid(_pid, &status, WNOHANG) == _pid) {
				_pid = -1;
				return;
			}
			std::this_thread::sleep_for(20ms);
		}
		_isRunning = _ipc.IsConnected() && Command("output HEADLESS-1 mode --custom 1920x1080@120Hz position 0 0");
	}

	HeadlessSway(const HeadlessSway&) = delete;

	~HeadlessSway() {
		if (_pid > 0) {
			kill(_pid, SIGTERM);
			int status;
			waitpid(_pid, &status, 0);
		}
		if (_dir.empty()) {
			return;
		}
		// Only what is known to be there, and only files: nothing is followed
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(_dir, ec)) {
			if (!entry.is_directory(ec)) {
				std::filesystem::remove(entry.path(), ec);
			}
		}
		std::filesystem::remove(_dir, ec);
	}

	bool IsRunning() const {
		return _isRunning;
	}

	const std::string& SocketPath() const {
		return _socketPath;
	}

	const std::string& WaylandDisplay() const {
		return _waylandDisplay;
	}

	const std::filesystem::path& Dir() const {
		return _dir;
	}

	// As "swaymsg" would send it
	bool Command(const std::string& command) {
		return _ipc.Command(command);
	}

	// The answer to a question such as "get_tree", as JSON
	std::string Query(SwayIpc::Message type) {
		std::string reply;
		_ipc.Request(type, {}, reply);
		return reply;
	}

private:
	std::filesystem::path _dir;
	std::string _socketPath;
	std::string _waylandDisplay;
	SwayIpc _ipc;
	pid_t _pid = -1;
	bool _isRunning = false;
};

// What a test window is opened with
struct TestWindowOptions {
	// The name of the compositor's socket
	std::string display;
	Size size{ 640, 480 };
	std::string title = "Magpie test window";
	std::string appId = "magpie.test";
	// The window says that it cannot be resized, which makes Sway float it
	bool isFixedSize = true;
	// The window gives the pointer no image
	bool hidesCursor = false;
	// The window asks the desktop for a title bar, as most programs do.
	// Without this Sway takes it for a program that draws its own.
	bool wantsDecoration = true;
};

struct TestPointerEvent {
	enum class Type { Enter, Leave, Motion, Button };
	Type type = Type::Motion;
	// Relative to the window
	double x = 0, y = 0;
	// As in linux/input-event-codes.h
	uint32_t button = 0;
	bool isPressed = false;
};

struct TestKeyEvent {
	uint32_t key = 0;
	bool isPressed = false;
};

// The test window as a process of its own (the program magpie_test_window),
// which ends with this object
class TestWindowProcess {
public:
	TestWindowProcess(const std::string& program, const std::string& display, std::vector<std::string> arguments) {
		_pid = fork();
		if (_pid == 0) {
			prctl(PR_SET_PDEATHSIG, SIGKILL);
			std::vector<const char*> argv{ program.c_str(), "--display", display.c_str() };
			for (const std::string& argument : arguments) {
				argv.push_back(argument.c_str());
			}
			argv.push_back(nullptr);
			const int null = open("/dev/null", O_WRONLY);
			if (null >= 0) {
				dup2(null, STDOUT_FILENO);
			}
			execv(program.c_str(), (char* const*)argv.data());
			_exit(127);
		}
	}

	TestWindowProcess(const TestWindowProcess&) = delete;

	~TestWindowProcess() {
		if (_pid > 0) {
			kill(_pid, SIGTERM);
			int status;
			waitpid(_pid, &status, 0);
		}
	}

	pid_t Pid() const {
		return _pid;
	}

private:
	pid_t _pid = -1;
};

// A window with a picture that tests can recognise: four quarters in red,
// green, blue and yellow. It notes what the pointer and the keyboard do on
// it. Everything of it runs on a thread of its own.
class TestWindow {
public:
	using Options = TestWindowOptions;
	using PointerEvent = TestPointerEvent;
	using KeyEvent = TestKeyEvent;

	// The image the window gives the pointer: a square of this size in one
	// colour, with the hot spot off its corner
	static constexpr int32_t CURSOR_SIZE = 16;
	static constexpr Point CURSOR_HOT_SPOT{ 3, 5 };
	static constexpr uint8_t CURSOR_RGBA[4]{ 255, 0, 255, 255 };

	// The colour at a place of a window of this size, as red, green, blue
	static std::array<uint8_t, 3> ColorAt(Size size, int32_t x, int32_t y) {
		const bool isRight = x >= size.width / 2;
		const bool isBottom = y >= size.height / 2;
		if (!isBottom) {
			return isRight ? std::array<uint8_t, 3>{ 30, 200, 30 } : std::array<uint8_t, 3>{ 200, 30, 30 };
		}
		return isRight ? std::array<uint8_t, 3>{ 200, 200, 30 } : std::array<uint8_t, 3>{ 30, 30, 200 };
	}

	explicit TestWindow(Options options) : _options(std::move(options)), _size(_options.size) {
		_wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		_thread = std::thread([this] { _Run(); });
	}

	TestWindow(const TestWindow&) = delete;

	~TestWindow() {
		_isQuitRequested = true;
		eventfd_write(_wakeFd, 1);
		_thread.join();
		close(_wakeFd);
	}

	// Until the compositor has taken the first picture
	bool WaitUntilShown(std::chrono::milliseconds timeout = 5s) const {
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (!_isShown && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(5ms);
		}
		return _isShown;
	}

	// Changes a few pixels in the top left corner, so that the compositor
	// has a new frame
	void Touch() {
		_isTouchRequested = true;
		eventfd_write(_wakeFd, 1);
	}

	Size GetSize() const {
		const std::scoped_lock lock(_mutex);
		return _size;
	}

	std::vector<PointerEvent> TakePointerEvents() {
		const std::scoped_lock lock(_mutex);
		return std::exchange(_pointerEvents, {});
	}

	std::vector<KeyEvent> TakeKeyEvents() {
		const std::scoped_lock lock(_mutex);
		return std::exchange(_keyEvents, {});
	}

	// Where the pointer is on the window; nothing if it is elsewhere
	std::optional<Point> PointerPos() const {
		const std::scoped_lock lock(_mutex);
		return _pointerPos;
	}

	bool HasKeyboardFocus() const {
		return _hasKeyboardFocus;
	}

	// The desktop asked the window to close, or the connection is gone
	bool IsClosed() const {
		return _isQuitRequested;
	}

private:
	struct Buffer {
		wl_buffer* buffer = nullptr;
		uint32_t* pixels = nullptr;
		size_t bytes = 0;
		Size size;
	};

	Options _options;
	std::thread _thread;
	int _wakeFd = -1;
	std::atomic<bool> _isQuitRequested = false;
	std::atomic<bool> _isTouchRequested = false;
	std::atomic<bool> _isShown = false;
	std::atomic<bool> _hasKeyboardFocus = false;

	mutable std::mutex _mutex;
	Size _size;
	std::vector<PointerEvent> _pointerEvents;
	std::vector<KeyEvent> _keyEvents;
	std::optional<Point> _pointerPos;

	// Only used on the thread
	wl_display* _display = nullptr;
	wl_compositor* _compositor = nullptr;
	wl_shm* _shm = nullptr;
	xdg_wm_base* _wmBase = nullptr;
	zxdg_decoration_manager_v1* _decorationManager = nullptr;
	wl_seat* _seat = nullptr;
	wl_pointer* _pointer = nullptr;
	wl_keyboard* _keyboard = nullptr;
	wl_surface* _surface = nullptr;
	xdg_surface* _xdgSurface = nullptr;
	xdg_toplevel* _toplevel = nullptr;
	wl_surface* _cursorSurface = nullptr;
	Buffer _picture;
	Buffer _cursorImage;
	bool _isConfigured = false;
	bool _needsDraw = false;
	uint32_t _touchCount = 0;

	Buffer _CreateBuffer(Size size, uint32_t format) {
		Buffer result;
		result.size = size;
		result.bytes = (size_t)size.width * size.height * 4;
		const int fd = memfd_create("magpie-test-window", MFD_CLOEXEC);
		if (fd < 0 || ftruncate(fd, (off_t)result.bytes) != 0) {
			return result;
		}
		result.pixels = (uint32_t*)mmap(nullptr, result.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		wl_shm_pool* pool = wl_shm_create_pool(_shm, fd, (int32_t)result.bytes);
		result.buffer = wl_shm_pool_create_buffer(pool, 0, size.width, size.height, size.width * 4, format);
		wl_shm_pool_destroy(pool);
		close(fd);
		return result;
	}

	void _DestroyBuffer(Buffer& buffer) {
		if (buffer.buffer) {
			wl_buffer_destroy(buffer.buffer);
		}
		if (buffer.pixels) {
			munmap(buffer.pixels, buffer.bytes);
		}
		buffer = {};
	}

	void _Draw() {
		Size size;
		{
			const std::scoped_lock lock(_mutex);
			size = _size;
		}
		if (_picture.size != size) {
			_DestroyBuffer(_picture);
			_picture = _CreateBuffer(size, WL_SHM_FORMAT_XRGB8888);
		}
		for (int32_t y = 0; y < size.height; ++y) {
			for (int32_t x = 0; x < size.width; ++x) {
				const std::array<uint8_t, 3> color = ColorAt(size, x, y);
				_picture.pixels[(size_t)y * size.width + x] =
					0xff000000u | ((uint32_t)color[0] << 16) | ((uint32_t)color[1] << 8) | color[2];
			}
		}
		// A mark that changes with every Touch; it is inside the red quarter,
		// away from the places the tests look at
		const uint32_t mark = 0xff000000u | (200u << 16) | (30u << 8) | (30u + (_touchCount % 2) * 4);
		for (int32_t y = 0; y < 4 && y < size.height; ++y) {
			for (int32_t x = 0; x < 4 && x < size.width; ++x) {
				_picture.pixels[(size_t)y * size.width + x] = mark;
			}
		}

		wl_surface_attach(_surface, _picture.buffer, 0, 0);
		wl_surface_damage_buffer(_surface, 0, 0, size.width, size.height);
		wl_surface_commit(_surface);
	}

	void _Run() {
		_display = wl_display_connect(_options.display.c_str());
		if (!_display) {
			return;
		}
		wl_registry* registry = wl_display_get_registry(_display);
		wl_registry_add_listener(registry, &REGISTRY_LISTENER, this);
		wl_display_roundtrip(_display);
		wl_display_roundtrip(_display);
		if (!_compositor || !_shm || !_wmBase) {
			wl_display_disconnect(_display);
			return;
		}

		_surface = wl_compositor_create_surface(_compositor);
		_xdgSurface = xdg_wm_base_get_xdg_surface(_wmBase, _surface);
		xdg_surface_add_listener(_xdgSurface, &XDG_SURFACE_LISTENER, this);
		_toplevel = xdg_surface_get_toplevel(_xdgSurface);
		xdg_toplevel_add_listener(_toplevel, &TOPLEVEL_LISTENER, this);
		xdg_toplevel_set_title(_toplevel, _options.title.c_str());
		xdg_toplevel_set_app_id(_toplevel, _options.appId.c_str());
		if (_options.isFixedSize) {
			xdg_toplevel_set_min_size(_toplevel, _options.size.width, _options.size.height);
			xdg_toplevel_set_max_size(_toplevel, _options.size.width, _options.size.height);
		}
		if (_options.wantsDecoration && _decorationManager) {
			zxdg_toplevel_decoration_v1* decoration =
				zxdg_decoration_manager_v1_get_toplevel_decoration(_decorationManager, _toplevel);
			zxdg_toplevel_decoration_v1_set_mode(decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		}
		wl_surface_commit(_surface);

		while (!_isQuitRequested) {
			while (wl_display_prepare_read(_display) != 0) {
				wl_display_dispatch_pending(_display);
			}
			wl_display_flush(_display);
			pollfd fds[2]{ { wl_display_get_fd(_display), POLLIN, 0 }, { _wakeFd, POLLIN, 0 } };
			poll(fds, 2, 100);
			if (fds[0].revents & POLLIN) {
				wl_display_read_events(_display);
			} else {
				wl_display_cancel_read(_display);
			}
			if (wl_display_dispatch_pending(_display) < 0) {
				_isQuitRequested = true;
				break;
			}

			eventfd_t value;
			eventfd_read(_wakeFd, &value);
			if (_isConfigured && (_needsDraw || _isTouchRequested.exchange(false))) {
				_needsDraw = false;
				++_touchCount;
				_Draw();
				_isShown = true;
			}
		}

		_DestroyBuffer(_picture);
		_DestroyBuffer(_cursorImage);
		wl_display_disconnect(_display);
	}

	void _SetCursor(uint32_t serial) {
		if (_options.hidesCursor) {
			wl_pointer_set_cursor(_pointer, serial, nullptr, 0, 0);
			return;
		}
		if (!_cursorSurface) {
			_cursorImage = _CreateBuffer({ CURSOR_SIZE, CURSOR_SIZE }, WL_SHM_FORMAT_ARGB8888);
			for (int32_t i = 0; i < CURSOR_SIZE * CURSOR_SIZE; ++i) {
				_cursorImage.pixels[i] = ((uint32_t)CURSOR_RGBA[3] << 24) | ((uint32_t)CURSOR_RGBA[0] << 16) |
					((uint32_t)CURSOR_RGBA[1] << 8) | CURSOR_RGBA[2];
			}
			_cursorSurface = wl_compositor_create_surface(_compositor);
			wl_surface_attach(_cursorSurface, _cursorImage.buffer, 0, 0);
			wl_surface_damage_buffer(_cursorSurface, 0, 0, CURSOR_SIZE, CURSOR_SIZE);
			wl_surface_commit(_cursorSurface);
		}
		wl_pointer_set_cursor(_pointer, serial, _cursorSurface, CURSOR_HOT_SPOT.x, CURSOR_HOT_SPOT.y);
	}

	void _AddPointerEvent(const PointerEvent& event) {
		const std::scoped_lock lock(_mutex);
		_pointerEvents.push_back(event);
	}

	static constexpr wl_pointer_listener POINTER_LISTENER{
		.enter = [](void* data, wl_pointer*, uint32_t serial, wl_surface*, wl_fixed_t x, wl_fixed_t y) {
			TestWindow& that = *(TestWindow*)data;
			that._SetCursor(serial);
			const PointerEvent event{ PointerEvent::Type::Enter, wl_fixed_to_double(x), wl_fixed_to_double(y) };
			{
				const std::scoped_lock lock(that._mutex);
				that._pointerPos = Point{ (int32_t)event.x, (int32_t)event.y };
			}
			that._AddPointerEvent(event);
		},
		.leave = [](void* data, wl_pointer*, uint32_t, wl_surface*) {
			TestWindow& that = *(TestWindow*)data;
			{
				const std::scoped_lock lock(that._mutex);
				that._pointerPos.reset();
			}
			that._AddPointerEvent({ PointerEvent::Type::Leave });
		},
		.motion = [](void* data, wl_pointer*, uint32_t, wl_fixed_t x, wl_fixed_t y) {
			TestWindow& that = *(TestWindow*)data;
			const PointerEvent event{ PointerEvent::Type::Motion, wl_fixed_to_double(x), wl_fixed_to_double(y) };
			{
				const std::scoped_lock lock(that._mutex);
				that._pointerPos = Point{ (int32_t)event.x, (int32_t)event.y };
			}
			that._AddPointerEvent(event);
		},
		.button = [](void* data, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state) {
			TestWindow& that = *(TestWindow*)data;
			PointerEvent event{ PointerEvent::Type::Button };
			{
				const std::scoped_lock lock(that._mutex);
				if (that._pointerPos) {
					event.x = that._pointerPos->x;
					event.y = that._pointerPos->y;
				}
			}
			event.button = button;
			event.isPressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
			that._AddPointerEvent(event);
		},
		.axis = [](void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {},
		.frame = [](void*, wl_pointer*) {},
		.axis_source = [](void*, wl_pointer*, uint32_t) {},
		.axis_stop = [](void*, wl_pointer*, uint32_t, uint32_t) {},
		.axis_discrete = [](void*, wl_pointer*, uint32_t, int32_t) {}
	};

	static constexpr wl_keyboard_listener KEYBOARD_LISTENER{
		.keymap = [](void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); },
		.enter = [](void* data, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {
			((TestWindow*)data)->_hasKeyboardFocus = true;
		},
		.leave = [](void* data, wl_keyboard*, uint32_t, wl_surface*) {
			((TestWindow*)data)->_hasKeyboardFocus = false;
		},
		.key = [](void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
			TestWindow& that = *(TestWindow*)data;
			const std::scoped_lock lock(that._mutex);
			that._keyEvents.push_back({ key, state == WL_KEYBOARD_KEY_STATE_PRESSED });
		},
		.modifiers = [](void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {},
		.repeat_info = [](void*, wl_keyboard*, int32_t, int32_t) {}
	};

	static constexpr wl_seat_listener SEAT_LISTENER{
		.capabilities = [](void* data, wl_seat* seat, uint32_t capabilities) {
			TestWindow& that = *(TestWindow*)data;
			if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !that._pointer) {
				that._pointer = wl_seat_get_pointer(seat);
				wl_pointer_add_listener(that._pointer, &POINTER_LISTENER, &that);
			}
			if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !that._keyboard) {
				that._keyboard = wl_seat_get_keyboard(seat);
				wl_keyboard_add_listener(that._keyboard, &KEYBOARD_LISTENER, &that);
			}
		},
		.name = [](void*, wl_seat*, const char*) {}
	};

	static constexpr xdg_wm_base_listener WM_BASE_LISTENER{
		.ping = [](void*, xdg_wm_base* wmBase, uint32_t serial) { xdg_wm_base_pong(wmBase, serial); }
	};

	static constexpr xdg_toplevel_listener TOPLEVEL_LISTENER{
		.configure = [](void* data, xdg_toplevel*, int32_t width, int32_t height, wl_array*) {
			TestWindow& that = *(TestWindow*)data;
			// A window of a fixed size keeps it whatever is proposed
			if (!that._options.isFixedSize && width > 0 && height > 0) {
				const std::scoped_lock lock(that._mutex);
				that._size = { width, height };
			}
		},
		.close = [](void* data, xdg_toplevel*) { ((TestWindow*)data)->_isQuitRequested = true; },
		.configure_bounds = [](void*, xdg_toplevel*, int32_t, int32_t) {}
	};

	static constexpr xdg_surface_listener XDG_SURFACE_LISTENER{
		.configure = [](void* data, xdg_surface* xdgSurface, uint32_t serial) {
			TestWindow& that = *(TestWindow*)data;
			xdg_surface_ack_configure(xdgSurface, serial);
			that._isConfigured = true;
			that._needsDraw = true;
		}
	};

	static constexpr wl_registry_listener REGISTRY_LISTENER{
		.global = [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
			TestWindow& that = *(TestWindow*)data;
			if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
				that._compositor = (wl_compositor*)wl_registry_bind(registry, name, &wl_compositor_interface, 4);
			} else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
				that._shm = (wl_shm*)wl_registry_bind(registry, name, &wl_shm_interface, 1);
			} else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
				that._wmBase = (xdg_wm_base*)wl_registry_bind(registry, name, &xdg_wm_base_interface, 2);
				xdg_wm_base_add_listener(that._wmBase, &WM_BASE_LISTENER, nullptr);
			} else if (std::strcmp(interface, wl_seat_interface.name) == 0 && !that._seat) {
				that._seat = (wl_seat*)wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u));
				wl_seat_add_listener(that._seat, &SEAT_LISTENER, &that);
			} else if (std::strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
				that._decorationManager = (zxdg_decoration_manager_v1*)wl_registry_bind(
					registry, name, &zxdg_decoration_manager_v1_interface, 1);
			}
		},
		.global_remove = [](void*, wl_registry*, uint32_t) {}
	};

};

}
