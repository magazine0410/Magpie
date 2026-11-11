#include "SwayBridge.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <fmt/format.h>
#include "Logger.h"
#include "SwayIpc.h"
#include "WlrInput.h"

using namespace std::chrono;

namespace Magpie {

// What a key binding of Magpie runs: a command that does nothing. Sway
// reports every binding that was run, with its command, and that is how the
// key press arrives here.
static constexpr std::string_view BINDING_COMMAND = "nop magpie:";
// How long Sway is given to carry out what it was told
static constexpr milliseconds SETTLE_TIMEOUT{ 1000 };
static constexpr const char* RESTORE_MARK = "_magpie_restore";

struct SwayBridge::Shared {
	~Shared() {
		if (wakeFd >= 0) {
			close(wakeFd);
		}
	}

	// Events that arrive while a reply is awaited are not announced by the
	// socket again; this descriptor stands in for them
	void NoteQueuedEvents() noexcept {
		if (ipc.HasQueuedEvents() && wakeFd >= 0) {
			eventfd_write(wakeFd, 1);
		}
	}

	bool Request(SwayIpc::Message type, std::string_view payload, std::string& reply) noexcept {
		const bool result = ipc.Request(type, payload, reply);
		NoteQueuedEvents();
		return result;
	}

	bool Command(std::string_view commands, bool quiet = false) noexcept {
		const bool result = ipc.Command(commands, quiet);
		NoteQueuedEvents();
		return result;
	}

	SwayIpc ipc;
	WlrInput input;
	std::string waylandDisplay;
	// The outputs this program created
	std::vector<std::string> ownOutputs;
	int wakeFd = -1;
};

class SwayBridge::Capture final : public CaptureSession {
public:
	Capture(std::shared_ptr<Shared> shared, std::string outputName, bool isOwnOutput) noexcept :
		_shared(std::move(shared)), _outputName(std::move(outputName)), _isOwnOutput(isOwnOutput),
		_eventFd(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {}

	~Capture() override {
		if (_isOwnOutput) {
			// Sway moves what is still on the output to another one
			_shared->Command(fmt::format("output {} unplug", _outputName), true);
			std::erase(_shared->ownOutputs, _outputName);
		}
		if (_eventFd >= 0) {
			close(_eventFd);
		}
	}

	Kind GetKind() const noexcept override {
		return Kind::ImageCopy;
	}

	std::string WaylandDisplay() const noexcept override {
		return _shared->waylandDisplay;
	}

	std::string OutputName() const noexcept override {
		return _outputName;
	}

	// Never readable: the frame source learns of the end of the output itself
	int EventFd() const noexcept override {
		return _eventFd;
	}

	bool Dispatch() noexcept override {
		return true;
	}

private:
	std::shared_ptr<Shared> _shared;
	std::string _outputName;
	bool _isOwnOutput;
	int _eventFd;
};

SwayKeys SwayKeys::FromTrigger(const std::string& trigger) noexcept {
	SwayKeys keys;
	if (trigger.empty()) {
		return keys;
	}

	size_t start = 0;
	while (true) {
		const size_t end = trigger.find('+', start);
		const std::string part = trigger.substr(start, end == std::string::npos ? std::string::npos : end - start);
		if (end == std::string::npos) {
			// The key itself: a key symbol of XKB, which Sway takes as it is
			keys.binding += part;
			keys.display += part.size() == 1 ? std::string(1, (char)std::toupper((unsigned char)part[0])) : part;
			return keys;
		}

		if (part == "CTRL") {
			keys.binding += "Ctrl+";
			keys.display += "Ctrl+";
		} else if (part == "ALT") {
			keys.binding += "Mod1+";
			keys.display += "Alt+";
		} else if (part == "SHIFT") {
			keys.binding += "Shift+";
			keys.display += "Shift+";
		} else if (part == "LOGO") {
			keys.binding += "Mod4+";
			keys.display += "Super+";
		}
		start = end + 1;
	}
}

SwayBridge::SwayBridge() noexcept : _shared(std::make_shared<Shared>()) {}

SwayBridge::~SwayBridge() {
	for (const Binding& binding : _bindings) {
		_shared->Command("unbindsym " + binding.keys, true);
	}
	if (_epollFd >= 0) {
		close(_epollFd);
	}
	if (_timerFd >= 0) {
		close(_timerFd);
	}
}

bool SwayBridge::IsSessionCompositor() noexcept {
	const char* socket = std::getenv("SWAYSOCK");
	return socket && *socket;
}

bool SwayBridge::Initialize(const Options& options) noexcept {
	std::string socketPath = options.socketPath;
	if (socketPath.empty()) {
		if (const char* value = std::getenv("SWAYSOCK")) {
			socketPath = value;
		}
	}
	if (!_shared->ipc.Connect(socketPath)) {
		if (!socketPath.empty()) {
			Logger::Get().Error("Cannot connect to Sway's control socket " + socketPath);
		}
		return false;
	}
	if (!_shared->ipc.Subscribe(R"(["window","workspace","output","binding","shutdown"])")) {
		Logger::Get().Error("Sway does not report changes");
		return false;
	}

	_shared->waylandDisplay = options.waylandDisplay;
	// Without it the windows can still be listed; MissingForScaling says
	// what cannot be done
	_shared->input.Connect(options.waylandDisplay);

	_shared->wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	_timerFd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	_epollFd = epoll_create1(EPOLL_CLOEXEC);
	if (_shared->wakeFd < 0 || _timerFd < 0 || _epollFd < 0) {
		return false;
	}
	for (int fd : { _shared->ipc.Fd(), _shared->wakeFd, _timerFd }) {
		epoll_event event{ .events = EPOLLIN, .data = { .fd = fd } };
		epoll_ctl(_epollFd, EPOLL_CTL_ADD, fd, &event);
	}

	return _RefreshOutputs() && _RefreshTree();
}

DesktopBridge::Traits SwayBridge::GetTraits() const noexcept {
	return {
		.customModeWidthStep = 1,
		// With Sway's default setting the pointer goes to a window on
		// another output that is made the active one
		.activationMovesPointer = true,
		.opensWindowsOnActiveOutput = true,
		.tilesNewWindows = true,
		.canMinimize = false,
		.captureMethod = "Virtual output (image copy)"
	};
}

std::string SwayBridge::MissingForScaling() const noexcept {
	const WlrInput& input = _shared->input;
	std::string missing;
	const auto add = [&](bool isThere, const char* what) {
		if (!isThere) {
			missing += missing.empty() ? "" : ", ";
			missing += what;
		}
	};
	add(input.HasPointer(), "pointer movement (wlr-virtual-pointer)");
	add(input.HasInterface("ext_image_copy_capture_manager_v1") &&
		input.HasInterface("ext_output_image_capture_source_manager_v1"),
		"capturing an output (ext-image-copy-capture, which Sway has from version 1.11)");
	add(input.HasInterface("zwlr_layer_shell_v1"), "the layer shell");
	return missing;
}

std::string SwayBridge::WaylandDisplay() const noexcept {
	return _shared->waylandDisplay;
}

int SwayBridge::EventFd() const noexcept {
	return _epollFd;
}

bool SwayBridge::Dispatch() noexcept {
	SwayIpc& ipc = _shared->ipc;
	eventfd_t queued;
	eventfd_read(_shared->wakeFd, &queued);
	if (!ipc.ReadEvents()) {
		return false;
	}

	bool refreshTree = false, refreshOutputs = false;
	for (const SwayIpc::Event& event : ipc.TakeEvents()) {
		switch (event.type) {
		case SwayIpc::EVENT_WINDOW:
			refreshTree = true;
			break;
		case SwayIpc::EVENT_WORKSPACE:
			refreshTree = true;
			if (SwayJson::IsReloadEvent(event.payload)) {
				// Reading the configuration again has removed the bindings
				_Bind();
			}
			break;
		case SwayIpc::EVENT_OUTPUT:
			refreshTree = refreshOutputs = true;
			break;
		case SwayIpc::EVENT_BINDING:
			if (std::string id = SwayJson::ParseBindingEvent(event.payload, BINDING_COMMAND); !id.empty()) {
				_activated.push_back(std::move(id));
			}
			break;
		case SwayIpc::EVENT_SHUTDOWN:
			return false;
		}
	}

	uint64_t expirations;
	if (read(_timerFd, &expirations, sizeof(expirations)) == sizeof(expirations)) {
		// Sway does not report a window that changes its size or is dragged
		refreshTree = true;
	}

	if ((refreshOutputs && !_RefreshOutputs()) || (refreshTree && !_RefreshTree())) {
		return false;
	}
	_shared->input.Flush();
	return ipc.IsConnected();
}

bool SwayBridge::_RefreshTree() noexcept {
	std::string reply;
	std::vector<SwayWindow> windows;
	if (!_shared->Request(SwayIpc::Message::GetTree, {}, reply) || !SwayJson::ParseTree(reply, windows)) {
		return false;
	}
	_windows = std::move(windows);
	return true;
}

bool SwayBridge::_RefreshOutputs() noexcept {
	std::string reply;
	std::vector<OutputInfo> outputs;
	if (!_shared->Request(SwayIpc::Message::GetOutputs, {}, reply) || !SwayJson::ParseOutputs(reply, outputs)) {
		return false;
	}
	for (OutputInfo& output : outputs) {
		output.isVirtual = std::ranges::find(_shared->ownOutputs, output.name) != _shared->ownOutputs.end();
	}
	_outputs = std::move(outputs);
	return true;
}

const SwayWindow* SwayBridge::_Find(const std::string& windowId) const noexcept {
	const auto it = std::ranges::find_if(_windows, [&](const SwayWindow& window) {
		return window.info.id == windowId;
	});
	return it != _windows.end() ? &*it : nullptr;
}

std::vector<WindowInfo> SwayBridge::ListWindows() noexcept {
	_RefreshTree();
	std::vector<WindowInfo> result;
	result.reserve(_windows.size());
	for (const SwayWindow& window : _windows) {
		result.push_back(window.info);
	}
	return result;
}

bool SwayBridge::GetWindow(const std::string& windowId, WindowInfo& info) const noexcept {
	const SwayWindow* window = _Find(windowId);
	if (!window) {
		return false;
	}
	info = window->info;
	return true;
}

std::string SwayBridge::ActiveWindowId() const noexcept {
	for (const SwayWindow& window : _windows) {
		if (window.info.isActive) {
			return window.info.id;
		}
	}
	return {};
}

std::vector<OutputInfo> SwayBridge::ListOutputs() noexcept {
	_RefreshOutputs();
	return _outputs;
}

std::unique_ptr<CaptureSession> SwayBridge::CreateVirtualOutput(
	const std::string&,
	Size,
	std::string& outputName
) noexcept {
	std::vector<std::string> before;
	for (const OutputInfo& output : ListOutputs()) {
		before.push_back(output.name);
	}

	if (!_shared->Command("create_output")) {
		Logger::Get().Error("Sway did not create an output");
		return nullptr;
	}

	// The command does not say what the output is called
	outputName.clear();
	const auto deadline = steady_clock::now() + SETTLE_TIMEOUT;
	while (outputName.empty()) {
		for (const OutputInfo& output : ListOutputs()) {
			if (std::ranges::find(before, output.name) == before.end()) {
				outputName = output.name;
			}
		}
		if (outputName.empty()) {
			if (steady_clock::now() > deadline) {
				Logger::Get().Error("The output that Sway created did not appear");
				return nullptr;
			}
			usleep(5'000);
		}
	}

	_shared->ownOutputs.push_back(outputName);
	_RefreshOutputs();
	return std::make_unique<Capture>(_shared, outputName, true);
}

std::unique_ptr<CaptureSession> SwayBridge::CaptureOutput(const std::string& outputName) noexcept {
	return std::make_unique<Capture>(_shared, outputName, false);
}

bool SwayBridge::ConfigureOutput(
	const std::string& outputName,
	Size size,
	Point position,
	int32_t refreshRate
) noexcept {
	// Sway arranges outputs that were given no position next to those that
	// were, so an output placed far away would take the monitors with it.
	// They are told to stay where they are, which lasts until Sway reads its
	// configuration again.
	std::string command;
	for (const OutputInfo& output : ListOutputs()) {
		if (output.name != outputName) {
			command += fmt::format("output {} position {} {}; ", output.name, output.rect.left, output.rect.top);
		}
	}
	command += fmt::format("output {} mode --custom {}x{}@{:.3f}Hz position {} {} scale 1",
		outputName, size.width, size.height, refreshRate / 1000.0, position.x, position.y);
	if (!_shared->Command(command)) {
		return false;
	}

	const Rect expected{ position.x, position.y, position.x + size.width, position.y + size.height };
	const auto deadline = steady_clock::now() + SETTLE_TIMEOUT;
	while (true) {
		for (const OutputInfo& output : ListOutputs()) {
			if (output.name == outputName && output.rect == expected) {
				return true;
			}
		}
		if (steady_clock::now() > deadline) {
			Logger::Get().Error(fmt::format("The output {} did not take the size {}x{} at {},{}",
				outputName, size.width, size.height, position.x, position.y));
			return false;
		}
		usleep(5'000);
	}
}

void SwayBridge::RemoveStaleVirtualOutput(const std::string& outputName) noexcept {
	// Only an output without a screen that lies far below all others, as the
	// scaling session places its own. After a restart of Sway the name may
	// belong to an output the user made.
	if (!outputName.starts_with("HEADLESS-")) {
		return;
	}
	const std::vector<OutputInfo> outputs = ListOutputs();
	const auto stale = std::ranges::find_if(outputs, [&](const OutputInfo& output) {
		return output.name == outputName;
	});
	if (stale == outputs.end()) {
		return;
	}
	static constexpr int32_t MIN_DISTANCE = 4096;
	if (outputs.size() < 2) {
		return;
	}
	for (const OutputInfo& other : outputs) {
		if (&other != &*stale && stale->rect.top - other.rect.bottom < MIN_DISTANCE) {
			return;
		}
	}
	Logger::Get().Warn("Removing the output " + outputName + ", which the last scaling left behind");
	_shared->Command(fmt::format("output {} unplug", outputName), true);
}

// Sway takes names with spaces in quotes
static std::string Quoted(std::string_view text) noexcept {
	std::string result = "\"";
	for (char c : text) {
		if (c == '"' || c == '\\') {
			result += '\\';
		}
		result += c;
	}
	return result + '"';
}

bool SwayBridge::_Restore(
	const SwayWindow& window,
	const WindowPlacement& placement,
	const SwayRestore& restore
) noexcept {
	const std::string target = fmt::format("[con_id={}] ", window.info.id);
	const auto exists = [&](int64_t id) {
		return id != 0 && _Find(std::to_string(id)) != nullptr;
	};

	// A fullscreen window keeps its state; only where it is changes
	const bool hasGeometry = placement.setGeometry && placement.output.empty() &&
		placement.frameRect.Width() > 0 && placement.frameRect.Height() > 0;
	const Rect& frame = placement.frameRect;

	if (restore.isFloating) {
		if (!window.isFloating) {
			_shared->Command(target + "floating enable", true);
		}
	} else {
		if (window.isFloating) {
			_shared->Command(target + "floating disable", true);
		}
		// Back to its place in the tiling: after the container that was
		// before it, or before the one that was after it
		if (exists(restore.previousSibling)) {
			_shared->Command(fmt::format("[con_id={}] mark --add {}", restore.previousSibling, RESTORE_MARK), true);
			_shared->Command(fmt::format("{}move container to mark {}", target, RESTORE_MARK), true);
			_shared->Command(fmt::format("unmark {}", RESTORE_MARK), true);
		} else if (exists(restore.nextSibling)) {
			_shared->Command(fmt::format("[con_id={}] mark --add {}", restore.nextSibling, RESTORE_MARK), true);
			_shared->Command(fmt::format("{}move container to mark {}", target, RESTORE_MARK), true);
			_shared->Command(fmt::format("unmark {}", RESTORE_MARK), true);
			_shared->Command(fmt::format("{}swap container with con_id {}", target, restore.nextSibling), true);
		} else if (!restore.workspace.empty()) {
			_shared->Command(fmt::format("{}move container to workspace {}", target, Quoted(restore.workspace)), true);
		}
	}

	// After the change between floating and tiled, because Sway gives a
	// program that draws its own decoration another border with it. A
	// window that was listed with a decoration of its own never lost it.
	if (restore.border != "csd") {
		_shared->Command(restore.border == "none" ? target + "border none" :
			fmt::format("{}border {} {}", target, restore.border, restore.borderWidth), true);
	}

	if (restore.isFloating) {
		if (hasGeometry) {
			_shared->Command(fmt::format("{}resize set width {} px height {} px, move absolute position {} px {} px",
				target, frame.Width(), frame.Height(), frame.left, frame.top));
		} else if (!restore.workspace.empty() && window.workspace != restore.workspace) {
			_shared->Command(fmt::format("{}move container to workspace {}", target, Quoted(restore.workspace)), true);
		}
	} else if (hasGeometry) {
		// As near to its old share of the workspace as the tiling allows
		_shared->Command(fmt::format("{}resize set width {} px height {} px",
			target, frame.Width(), frame.Height()), true);
	}

	if (placement.activate) {
		_shared->Command(target + "focus", true);
	}
	_RefreshTree();
	return _Find(window.info.id) != nullptr;
}

bool SwayBridge::PlaceWindow(const std::string& windowId, const WindowPlacement& placement) noexcept {
	_RefreshTree();
	const SwayWindow* found = _Find(windowId);
	if (!found) {
		return false;
	}
	const SwayWindow window = *found;

	if (SwayRestore restore; !placement.restore.empty() && SwayJson::ParseRestore(placement.restore, restore)) {
		return _Restore(window, placement, restore);
	}

	const std::string target = fmt::format("[con_id={}] ", windowId);
	std::vector<std::string> commands;
	// Where the container is expected afterwards; empty if nothing is checked
	Rect expected{};

	if (!placement.output.empty()) {
		// A fullscreen window takes the size of the output by itself
		commands.push_back("move container to output " + placement.output);
	} else {
		const bool removeBorder = placement.noBorder && window.border != "none" && window.border != "csd";
		if (removeBorder) {
			commands.push_back("border none");
		}
		if (placement.setGeometry) {
			const Rect& frame = placement.frameRect;
			if (!window.isFloating) {
				commands.insert(commands.begin(), "floating enable");
			}
			commands.push_back(fmt::format("resize set width {} px height {} px", frame.Width(), frame.Height()));
			commands.push_back(fmt::format("move absolute position {} px {} px", frame.left, frame.top));
			expected = frame;
		}
	}
	if (placement.activate) {
		commands.push_back("focus");
	}
	if (commands.empty()) {
		return true;
	}

	std::string command = target;
	for (size_t i = 0; i < commands.size(); ++i) {
		command += (i == 0 ? "" : ", ") + commands[i];
	}
	if (!_shared->Command(command)) {
		// One of them was refused; what matters is where the window is now
		Logger::Get().Warn("Sway did not carry out every command for the window");
	}

	const auto deadline = steady_clock::now() + SETTLE_TIMEOUT;
	while (true) {
		_RefreshTree();
		const SwayWindow* now = _Find(windowId);
		if (!now) {
			return false;
		}
		if (expected.Width() <= 0 || now->info.frameRect == expected) {
			return true;
		}
		if (steady_clock::now() > deadline) {
			const Rect& frame = now->info.frameRect;
			Logger::Get().Error(fmt::format("The window is at {},{} with {}x{} and not at {},{} with {}x{}",
				frame.left, frame.top, frame.Width(), frame.Height(),
				expected.left, expected.top, expected.Width(), expected.Height()));
			return false;
		}
		usleep(5'000);
	}
}

bool SwayBridge::SetWindowProperties(const std::string& windowId, const WindowProperties& properties) noexcept {
	if (!_Find(windowId)) {
		return false;
	}
	// Floating windows are above the tiled ones, and the active one is on
	// top; there is nothing to set for that
	return !properties.minimized.has_value();
}

bool SwayBridge::MovePointer(Point position) noexcept {
	if (_outputs.empty() && !_RefreshOutputs()) {
		return false;
	}
	if (_outputs.empty() || !_shared->input.HasPointer()) {
		return false;
	}

	// Positions of a virtual pointer refer to everything the outputs cover
	Rect all = _outputs[0].rect;
	for (const OutputInfo& output : _outputs) {
		all = {
			std::min(all.left, output.rect.left), std::min(all.top, output.rect.top),
			std::max(all.right, output.rect.right), std::max(all.bottom, output.rect.bottom)
		};
	}
	_shared->input.MoveAbsolute({ position.x - all.left, position.y - all.top }, { all.Width(), all.Height() });
	return _shared->input.Flush();
}

std::vector<PointerDevice> SwayBridge::ReadPointerDevices() noexcept {
	std::string reply;
	std::vector<PointerDevice> devices;
	if (_shared->Request(SwayIpc::Message::GetInputs, {}, reply)) {
		SwayJson::ParsePointerDevices(reply, devices);
	}
	return devices;
}

void SwayBridge::ApplyPointerSpeed(std::span<const PointerDevice> devices) noexcept {
	for (const PointerDevice& device : devices) {
		// Unlike on KWin this lasts only until Sway reads its configuration again
		_shared->Command(fmt::format("input {} pointer_accel {:.4f}", Quoted(device.id), device.acceleration));
	}
}

void SwayBridge::SetWatching(bool isWatching) noexcept {
	itimerspec spec{};
	if (isWatching) {
		spec.it_interval.tv_nsec = spec.it_value.tv_nsec = 250'000'000;
	}
	timerfd_settime(_timerFd, 0, &spec, nullptr);
}

void SwayBridge::_Bind() noexcept {
	for (const Binding& binding : _bindings) {
		// --no-repeat: once per press, however long the keys are held
		_shared->Command(fmt::format("bindsym --no-warn --no-repeat {} {}{}", binding.keys, BINDING_COMMAND, binding.id));
	}
}

std::vector<std::pair<std::string, std::string>> SwayBridge::BindShortcuts(
	std::span<const ShortcutRequest> shortcuts
) noexcept {
	std::vector<std::pair<std::string, std::string>> bound;
	for (const ShortcutRequest& shortcut : shortcuts) {
		const SwayKeys keys = SwayKeys::FromTrigger(shortcut.trigger);
		if (keys.binding.empty()) {
			continue;
		}
		_bindings.push_back({ shortcut.id, keys.binding });
		bound.emplace_back(shortcut.id, keys.display);
	}
	_Bind();
	return bound;
}

std::vector<std::string> SwayBridge::TakeActivatedShortcuts() noexcept {
	return std::exchange(_activated, {});
}

SwayIpc& SwayBridge::Ipc() noexcept {
	return _shared->ipc;
}

WlrInput& SwayBridge::Input() noexcept {
	return _shared->input;
}

}
