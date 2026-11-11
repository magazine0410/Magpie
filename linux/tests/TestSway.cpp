// The back end for Sway: what it reads from Sway's answers, and, with a Sway
// that the test starts itself and that shows nothing, what it does to
// windows, outputs and the pointer, the capture of an output, and a whole
// scaling session. Without a program "sway" the latter tests skip themselves.
#include <fstream>
#include <linux/input-event-codes.h>
#include <sstream>
#include <sys/timerfd.h>
#include <catch2/catch_test_macros.hpp>
#include <xkbcommon/xkbcommon.h>
#include "FrameSource.h"
#include "ImageCopyFrameSource.h"
#include "RenderTestHelpers.h"
#include "ScalingSession.h"
#include "SwayBridge.h"
#include "SwayJson.h"
#include "SwayTestHelpers.h"
#include "WlrInput.h"

using namespace Magpie;
using namespace SwayTest;
using namespace std::chrono;

namespace {

std::string ReadFile(const std::filesystem::path& path) {
	std::ifstream file(path);
	std::stringstream content;
	content << file.rdbuf();
	return content.str();
}

const SwayWindow* FindByTitle(const std::vector<SwayWindow>& windows, std::string_view title) {
	for (const SwayWindow& window : windows) {
		if (window.info.title == title) {
			return &window;
		}
	}
	return nullptr;
}

}

TEST_CASE("A message of Sway's protocol is packed and unpacked", "[sway]") {
	std::string buffer = SwayIpc::Pack(4, "") + SwayIpc::Pack(SwayIpc::EVENT_WINDOW, R"({"change":"focus"})");
	CHECK(buffer.substr(0, 6) == "i3-ipc");
	CHECK(buffer.size() == 14 + 14 + 18);

	uint32_t type = 0;
	std::string payload;
	bool isBroken = true;
	REQUIRE(SwayIpc::Unpack(buffer, type, payload, isBroken));
	CHECK(type == 4);
	CHECK(payload.empty());
	REQUIRE(SwayIpc::Unpack(buffer, type, payload, isBroken));
	CHECK(type == SwayIpc::EVENT_WINDOW);
	CHECK(payload == R"({"change":"focus"})");
	CHECK_FALSE(SwayIpc::Unpack(buffer, type, payload, isBroken));
	CHECK_FALSE(isBroken);

	// A message that has not arrived completely stays where it is
	const std::string whole = SwayIpc::Pack(0, "focus");
	std::string part = whole.substr(0, whole.size() - 2);
	CHECK_FALSE(SwayIpc::Unpack(part, type, payload, isBroken));
	CHECK_FALSE(isBroken);
	CHECK(part.size() == whole.size() - 2);

	std::string garbage = "this is not a message at all";
	CHECK_FALSE(SwayIpc::Unpack(garbage, type, payload, isBroken));
	CHECK(isBroken);
}

TEST_CASE("The windows are read from Sway's tree", "[sway]") {
	// Taken from Sway 1.11: two terminals and an X11 window side by side, and
	// a floating window of a fixed size, all with title bars of 25 pixels
	std::vector<SwayWindow> windows;
	REQUIRE(SwayJson::ParseTree(ReadFile(std::filesystem::path(MAGPIE_TEST_DATA_DIR) / "sway_tree.json"), windows));
	REQUIRE(windows.size() == 4);

	const SwayWindow* left = FindByTitle(windows, "tiled-two");
	const SwayWindow* middle = FindByTitle(windows, "tiled-one");
	const SwayWindow* right = FindByTitle(windows, "x11-window");
	const SwayWindow* floating = FindByTitle(windows, "floating-fixed");
	REQUIRE(left);
	REQUIRE(middle);
	REQUIRE(right);
	REQUIRE(floating);

	// The frame has the title bar, which Sway's rectangle leaves out
	CHECK(left->info.frameRect == Rect{ 0, 0, 427, 720 });
	CHECK(left->info.clientRect == Rect{ 2, 25, 425, 718 });
	CHECK(left->titleBarHeight == 25);
	CHECK(left->border == "normal");
	CHECK_FALSE(left->info.hasNoBorder);
	CHECK_FALSE(left->isFloating);
	CHECK(left->output == "HEADLESS-1");
	CHECK(left->workspace == "1");
	CHECK(left->info.appId == "foot");
	CHECK(left->info.pid != 0);

	// Where a tiled window is among the others
	CHECK(left->previousSibling == 0);
	CHECK(std::to_string(left->nextSibling) == middle->info.id);
	CHECK(std::to_string(middle->previousSibling) == left->info.id);
	CHECK(std::to_string(middle->nextSibling) == right->info.id);
	CHECK(right->nextSibling == 0);

	// An X11 program has no app ID; its window class stands in
	CHECK(right->info.appId == "XTerm");
	CHECK(right->info.resourceName == "xterm");

	CHECK(floating->isFloating);
	CHECK(floating->info.frameRect == Rect{ 318, 95, 962, 602 });
	CHECK(floating->info.clientRect == Rect{ 320, 120, 960, 600 });
	CHECK(floating->previousSibling == 0);
	CHECK(floating->nextSibling == 0);

	int activeCount = 0;
	for (const SwayWindow& window : windows) {
		activeCount += window.info.isActive ? 1 : 0;
		CHECK_FALSE(window.info.isMinimized);
		CHECK_FALSE(window.info.isFullscreen);
	}
	CHECK(activeCount == 1);

	SECTION("what it takes to put a window back") {
		SwayRestore restore;
		REQUIRE(SwayJson::ParseRestore(middle->info.restoreData, restore));
		CHECK_FALSE(restore.isFloating);
		CHECK(restore.border == "normal");
		CHECK(restore.borderWidth == 2);
		CHECK(std::to_string(restore.previousSibling) == left->info.id);
		CHECK(std::to_string(restore.nextSibling) == right->info.id);
		CHECK(restore.workspace == "1");

		REQUIRE(SwayJson::ParseRestore(floating->info.restoreData, restore));
		CHECK(restore.isFloating);

		// The name of a workspace may have spaces
		SwayWindow named = *middle;
		named.workspace = "3: web and mail";
		REQUIRE(SwayJson::ParseRestore(SwayJson::FormatRestore(named), restore));
		CHECK(restore.workspace == "3: web and mail");

		CHECK_FALSE(SwayJson::ParseRestore("", restore));
		CHECK_FALSE(SwayJson::ParseRestore("kwin floating normal 2 0 0 1", restore));
		CHECK_FALSE(SwayJson::ParseRestore("sway sideways normal 2 0 0 1", restore));
	}

	SECTION("a window in the scratchpad counts as minimized") {
		const std::string tree = R"({ "type": "root", "nodes": [
			{ "type": "output", "name": "__i3", "nodes": [
				{ "type": "workspace", "name": "__i3_scratch", "nodes": [], "floating_nodes": [
					{ "id": 9, "type": "floating_con", "name": "hidden", "pid": 77, "app_id": "x", "border": "none",
						"rect": { "x": 10, "y": 20, "width": 300, "height": 200 },
						"window_rect": { "x": 0, "y": 0, "width": 300, "height": 200 },
						"deco_rect": { "x": 0, "y": 0, "width": 0, "height": 0 } }
				] }
			] }
		] })";
		REQUIRE(SwayJson::ParseTree(tree, windows));
		REQUIRE(windows.size() == 1);
		CHECK(windows[0].info.isMinimized);
		CHECK(windows[0].info.hasNoBorder);
		CHECK(windows[0].output.empty());
		CHECK(windows[0].info.frameRect == Rect{ 10, 20, 310, 220 });
	}

	CHECK_FALSE(SwayJson::ParseTree("not JSON", windows));
	CHECK_FALSE(SwayJson::ParseTree("[]", windows));
}

TEST_CASE("Outputs, pointer devices and key bindings are read from Sway's answers", "[sway]") {
	std::vector<OutputInfo> outputs;
	REQUIRE(SwayJson::ParseOutputs(ReadFile(std::filesystem::path(MAGPIE_TEST_DATA_DIR) / "sway_outputs.json"), outputs));
	REQUIRE(outputs.size() == 1);
	CHECK(outputs[0].name == "HEADLESS-1");
	CHECK(outputs[0].rect == Rect{ 0, 0, 1280, 720 });
	// An output without a screen and without a mode reports no rate
	CHECK(outputs[0].refreshRate == 60'000);

	REQUIRE(SwayJson::ParseOutputs(R"([
		{ "name": "DP-1", "active": true, "rect": { "x": 1920, "y": 0, "width": 2560, "height": 1440 },
			"current_mode": { "width": 2560, "height": 1440, "refresh": 143998 } },
		{ "name": "HDMI-A-1", "active": false, "rect": { "x": 0, "y": 0, "width": 0, "height": 0 } }
	])", outputs));
	REQUIRE(outputs.size() == 1);
	CHECK(outputs[0].rect == Rect{ 1920, 0, 4480, 1440 });
	CHECK(outputs[0].refreshRate == 143'998);

	std::vector<PointerDevice> devices;
	REQUIRE(SwayJson::ParsePointerDevices(R"([
		{ "identifier": "1133:16505:Logitech_G_Pro", "type": "pointer", "libinput": { "accel_speed": -0.25, "accel_profile": "flat" } },
		{ "identifier": "2:14:SynPS/2_Synaptics_TouchPad", "type": "touchpad", "libinput": { "accel_speed": 0.5 } },
		{ "identifier": "1:1:AT_Translated_Set_2_keyboard", "type": "keyboard", "libinput": { "send_events": "enabled" } },
		{ "identifier": "0:0:wlr_virtual_pointer_v1", "type": "pointer" },
		{ "identifier": "1386:890:Wacom_Pen", "type": "tablet_tool", "libinput": { "accel_speed": 0.0 } }
	])", devices));
	REQUIRE(devices.size() == 2);
	CHECK(devices[0].id == "1133:16505:Logitech_G_Pro");
	CHECK(devices[0].acceleration == -0.25);
	CHECK(devices[1].acceleration == 0.5);

	CHECK(SwayJson::ParseBindingEvent(R"({ "change": "run", "binding": { "command": "nop magpie:scale-windowed",
		"event_state_mask": ["Mod1", "Shift"], "symbol": "q", "input_type": "keyboard" } })", "nop magpie:") == "scale-windowed");
	CHECK(SwayJson::ParseBindingEvent(R"({ "change": "run", "binding": { "command": "exec foot" } })", "nop magpie:").empty());
	CHECK(SwayJson::ParseBindingEvent("{}", "nop magpie:").empty());

	CHECK(SwayJson::IsReloadEvent(R"({ "change": "reload", "current": null, "old": null })"));
	CHECK_FALSE(SwayJson::IsReloadEvent(R"({ "change": "focus", "current": {} })"));
}

TEST_CASE("The keys of a shortcut are written the way Sway takes them", "[sway]") {
	SwayKeys keys = SwayKeys::FromTrigger("ALT+SHIFT+a");
	CHECK(keys.binding == "Mod1+Shift+a");
	CHECK(keys.display == "Alt+Shift+A");

	keys = SwayKeys::FromTrigger("CTRL+LOGO+F5");
	CHECK(keys.binding == "Ctrl+Mod4+F5");
	CHECK(keys.display == "Ctrl+Super+F5");

	keys = SwayKeys::FromTrigger("7");
	CHECK(keys.binding == "7");

	keys = SwayKeys::FromTrigger("");
	CHECK(keys.binding.empty());
	CHECK(keys.display.empty());
}

TEST_CASE("Frames in shared memory are brought to four bytes per pixel", "[sway]") {
	uint32_t format = 0;
	// Four bytes per pixel are taken before three
	REQUIRE(ImageCopyFrameSource::ChooseFormat({ WL_SHM_FORMAT_BGR888, WL_SHM_FORMAT_ABGR8888 }, format));
	CHECK(format == WL_SHM_FORMAT_ABGR8888);
	REQUIRE(ImageCopyFrameSource::ChooseFormat({ WL_SHM_FORMAT_BGR888 }, format));
	CHECK(format == WL_SHM_FORMAT_BGR888);
	CHECK_FALSE(ImageCopyFrameSource::ChooseFormat({ WL_SHM_FORMAT_RGB565, WL_SHM_FORMAT_NV12 }, format));
	CHECK_FALSE(ImageCopyFrameSource::ChooseFormat({}, format));

	ImageCopyFrameSource::PixelLayout layout;
	REQUIRE(ImageCopyFrameSource::GetPixelLayout(WL_SHM_FORMAT_XRGB8888, layout));
	CHECK(layout.bytesPerPixel == 4);
	CHECK_FALSE(layout.isRedFirst);
	CHECK_FALSE(layout.hasAlpha);
	REQUIRE(ImageCopyFrameSource::GetPixelLayout(WL_SHM_FORMAT_ABGR8888, layout));
	CHECK(layout.isRedFirst);
	CHECK(layout.hasAlpha);
	// "BGR888" names the bytes from the highest; in memory red comes first
	REQUIRE(ImageCopyFrameSource::GetPixelLayout(WL_SHM_FORMAT_BGR888, layout));
	CHECK(layout.bytesPerPixel == 3);
	CHECK(layout.isRedFirst);
	CHECK_FALSE(ImageCopyFrameSource::GetPixelLayout(WL_SHM_FORMAT_RGB565, layout));

	// Two rows of three pixels with three bytes each; a row begins every 12 bytes
	const uint8_t src[24]{
		1, 2, 3, 4, 5, 6, 7, 8, 9, 0xee, 0xee, 0xee,
		11, 12, 13, 14, 15, 16, 17, 18, 19, 0xee, 0xee, 0xee
	};
	uint8_t dst[24]{};
	ImageCopyFrameSource::ConvertRows(src, 12, layout, dst, 3, 2);
	const uint8_t expected[24]{
		1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255,
		11, 12, 13, 255, 14, 15, 16, 255, 17, 18, 19, 255
	};
	CHECK(std::memcmp(dst, expected, sizeof(dst)) == 0);

	// Four bytes are copied as they are, without the padding of the rows
	REQUIRE(ImageCopyFrameSource::GetPixelLayout(WL_SHM_FORMAT_ARGB8888, layout));
	const uint8_t src4[24]{
		1, 2, 3, 4, 5, 6, 7, 8, 0xee, 0xee, 0xee, 0xee,
		9, 10, 11, 12, 13, 14, 15, 16, 0xee, 0xee, 0xee, 0xee
	};
	uint8_t dst4[16]{};
	ImageCopyFrameSource::ConvertRows(src4, 12, layout, dst4, 2, 2);
	const uint8_t expected4[16]{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
	CHECK(std::memcmp(dst4, expected4, sizeof(dst4)) == 0);
}

// ---------------------------------------------------------------------------
// From here on: with a Sway of the test's own
// ---------------------------------------------------------------------------

namespace {

// Starts Sway and connects the back end to it. `isUsable` is false if there
// is no Sway to start; the test then has nothing to check.
struct SwayFixture {
	SwayFixture() {
		if (!IsSwayInstalled()) {
			return;
		}
		sway = std::make_unique<HeadlessSway>();
		if (!sway->IsRunning()) {
			return;
		}
		isUsable = bridge.Initialize({ sway->SocketPath(), sway->WaylandDisplay() });
	}

	std::unique_ptr<TestWindow> OpenWindow(TestWindow::Options options = {}) {
		options.display = sway->WaylandDisplay();
		auto window = std::make_unique<TestWindow>(std::move(options));
		REQUIRE(window->WaitUntilShown());
		return window;
	}

	// The window with this title, once Sway lists it and what it lists
	// stays the same: a new window gets its border a moment after its place
	SwayWindow Find(std::string_view title) {
		const auto deadline = steady_clock::now() + 5s;
		std::optional<SwayWindow> last;
		while (true) {
			bridge.ListWindows();
			if (const SwayWindow* window = FindByTitle(bridge.Windows(), title)) {
				if (window->info.frameRect.Width() > 0) {
					if (last && last->info.frameRect == window->info.frameRect && last->border == window->border &&
						last->info.isActive == window->info.isActive) {
						return *window;
					}
					last = *window;
				}
			}
			REQUIRE(steady_clock::now() < deadline);
			std::this_thread::sleep_for(30ms);
		}
	}

	bool WaitFor(const std::function<bool()>& done, milliseconds timeout = 5s) {
		const auto deadline = steady_clock::now() + timeout;
		while (!done()) {
			if (steady_clock::now() > deadline) {
				return false;
			}
			std::this_thread::sleep_for(5ms);
			bridge.Dispatch();
		}
		return true;
	}

	std::unique_ptr<HeadlessSway> sway;
	SwayBridge bridge;
	bool isUsable = false;
};

#define REQUIRE_SWAY(fixture) \
	if (!(fixture).isUsable) { \
		SKIP("There is no Sway to start"); \
	}

// Frames of an output, taken until `done` says so
bool PumpUntil(FrameSource& source, const std::function<bool(FrameSourceState)>& done, milliseconds timeout = 5s) {
	const auto deadline = steady_clock::now() + timeout;
	while (steady_clock::now() < deadline) {
		pollfd fd{ source.EventFd(), POLLIN, 0 };
		poll(&fd, 1, 20);
		if (!source.Dispatch()) {
			return false;
		}
		const FrameSourceState state = source.Update();
		if (state == FrameSourceState::Error) {
			return false;
		}
		if (done(state)) {
			return true;
		}
	}
	return false;
}

// Red, green and blue of a pixel of a texture
std::array<uint8_t, 3> PixelAt(VulkanDevice& device, const Texture& texture, int32_t x, int32_t y) {
	std::vector<uint8_t> pixels;
	REQUIRE(device.ReadTexture(texture, pixels));
	REQUIRE(pixels.size() == (size_t)texture.width * texture.height * 4);
	const uint8_t* pixel = &pixels[((size_t)y * texture.width + (size_t)x) * 4];
	if (texture.format == VK_FORMAT_B8G8R8A8_UNORM) {
		return { pixel[2], pixel[1], pixel[0] };
	}
	return { pixel[0], pixel[1], pixel[2] };
}

bool IsNear(const std::array<uint8_t, 3>& a, const std::array<uint8_t, 3>& b, int tolerance = 2) {
	return std::abs(a[0] - b[0]) <= tolerance && std::abs(a[1] - b[1]) <= tolerance &&
		std::abs(a[2] - b[2]) <= tolerance;
}

// The default keymap of the system as a file, for a virtual keyboard
int CreateKeymapFile(uint32_t& size) {
	xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	xkb_keymap* keymap = context ? xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS) : nullptr;
	char* text = keymap ? xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1) : nullptr;
	int fd = -1;
	if (text) {
		size = (uint32_t)std::strlen(text) + 1;
		fd = memfd_create("magpie-test-keymap", MFD_CLOEXEC);
		if (fd >= 0 && write(fd, text, size) != (ssize_t)size) {
			close(fd);
			fd = -1;
		}
		free(text);
	}
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	return fd;
}

}

TEST_CASE("The Sway back end lists the windows and the outputs", "[sway][desktop]") {
	SwayFixture fixture;
	REQUIRE_SWAY(fixture);
	SwayBridge& bridge = fixture.bridge;

	CHECK(std::string(bridge.Name()) == "Sway");
	CHECK(bridge.MissingForScaling() == "");
	CHECK(bridge.ListWindows().empty());
	CHECK(bridge.ActiveWindowId().empty());

	const std::vector<OutputInfo> outputs = bridge.ListOutputs();
	REQUIRE(outputs.size() == 1);
	CHECK(outputs[0].name == "HEADLESS-1");
	CHECK(outputs[0].rect == Rect{ 0, 0, 1920, 1080 });
	CHECK(outputs[0].refreshRate == 120'000);
	CHECK_FALSE(outputs[0].isVirtual);

	const auto window = fixture.OpenWindow({ .title = "first", .appId = "magpie.test.first" });
	const SwayWindow first = fixture.Find("first");
	CHECK(first.info.appId == "magpie.test.first");
	CHECK(first.info.pid == (uint32_t)getpid());
	CHECK(first.info.clientRect.Width() == 640);
	CHECK(first.info.clientRect.Height() == 480);
	// A window of a fixed size floats, and Sway gives it a title bar
	CHECK(first.isFloating);
	CHECK(first.info.frameRect.Height() > 480);
	CHECK(first.info.isActive);
	CHECK(bridge.ActiveWindowId() == first.info.id);

	WindowInfo info;
	CHECK(bridge.GetWindow(first.info.id, info));
	CHECK(info.title == "first");
	CHECK_FALSE(bridge.GetWindow("123456", info));

	// A window that opens is reported without being asked for
	const auto second = fixture.OpenWindow({ .title = "second" });
	REQUIRE(fixture.WaitFor([&] { return FindByTitle(bridge.Windows(), "second") != nullptr; }));
	REQUIRE(fixture.WaitFor([&] { return bridge.ActiveWindowId() == FindByTitle(bridge.Windows(), "second")->info.id; }));

	// Nothing can be minimized on Sway
	CHECK_FALSE(bridge.SetWindowProperties(first.info.id, { .minimized = true }));
	CHECK(bridge.SetWindowProperties(first.info.id, { .keepAbove = true, .raise = true }));
	CHECK_FALSE(bridge.GetTraits().canMinimize);
}

TEST_CASE("The Sway back end places a floating window and puts it back", "[sway][desktop]") {
	SwayFixture fixture;
	REQUIRE_SWAY(fixture);
	SwayBridge& bridge = fixture.bridge;

	const auto window = fixture.OpenWindow({ .title = "floating" });
	const SwayWindow before = fixture.Find("floating");
	REQUIRE(before.isFloating);
	REQUIRE(before.border == "normal");

	// Without its decoration at an exact place
	const Rect place{ 100, 50, 740, 530 };
	REQUIRE(bridge.PlaceWindow(before.info.id, { .frameRect = place, .noBorder = true, .activate = true }));
	SwayWindow now = fixture.Find("floating");
	CHECK(now.info.frameRect == place);
	CHECK(now.info.clientRect == place);
	CHECK(now.info.hasNoBorder);
	CHECK(now.info.isActive);

	// Only the decoration, for a window whose place is not to change
	REQUIRE(bridge.PlaceWindow(before.info.id, { .noBorder = true, .setGeometry = false }));
	CHECK(fixture.Find("floating").info.frameRect == place);

	// Back, with what the window was listed with before
	REQUIRE(bridge.PlaceWindow(before.info.id, {
		.frameRect = before.info.frameRect,
		.noBorder = before.info.hasNoBorder,
		.restore = before.info.restoreData
	}));
	now = fixture.Find("floating");
	CHECK(now.info.frameRect == before.info.frameRect);
	CHECK(now.info.clientRect == before.info.clientRect);
	CHECK(now.border == "normal");
	CHECK(now.isFloating);

	CHECK_FALSE(bridge.PlaceWindow("123456", { .frameRect = place }));
}

TEST_CASE("The Sway back end puts a tiled window back among the others", "[sway][desktop]") {
	SwayFixture fixture;
	REQUIRE_SWAY(fixture);
	SwayBridge& bridge = fixture.bridge;

	// Three windows that can be resized are laid out side by side
	const auto a = fixture.OpenWindow({ .title = "a", .isFixedSize = false });
	fixture.Find("a");
	const auto b = fixture.OpenWindow({ .title = "b", .isFixedSize = false });
	fixture.Find("b");
	const auto c = fixture.OpenWindow({ .title = "c", .isFixedSize = false });
	fixture.Find("c");

	const auto order = [&] {
		std::vector<std::pair<int32_t, std::string>> byPlace;
		for (const char* title : { "a", "b", "c" }) {
			const SwayWindow window = fixture.Find(title);
			REQUIRE_FALSE(window.isFloating);
			byPlace.emplace_back(window.info.frameRect.left, title);
		}
		std::sort(byPlace.begin(), byPlace.end());
		return byPlace[0].second + byPlace[1].second + byPlace[2].second;
	};
	REQUIRE(order() == "abc");

	for (const char* title : { "b", "a", "c" }) {
		DYNAMIC_SECTION("the window " << title) {
			const SwayWindow before = fixture.Find(title);
			const Rect place{ 200, 100, 840, 580 };
			REQUIRE(bridge.PlaceWindow(before.info.id, { .frameRect = place, .noBorder = true }));
			SwayWindow now = fixture.Find(title);
			CHECK(now.isFloating);
			CHECK(now.info.frameRect == place);

			REQUIRE(bridge.PlaceWindow(before.info.id, {
				.frameRect = before.info.frameRect,
				.noBorder = before.info.hasNoBorder,
				.restore = before.info.restoreData
			}));
			now = fixture.Find(title);
			CHECK_FALSE(now.isFloating);
			CHECK(now.border == before.border);
			CHECK(order() == "abc");
			// Its share of the workspace too
			CHECK(now.info.frameRect == before.info.frameRect);
		}
	}
}

TEST_CASE("The Sway back end makes an output that no screen shows", "[sway][desktop]") {
	SwayFixture fixture;
	REQUIRE_SWAY(fixture);
	SwayBridge& bridge = fixture.bridge;

	std::string name;
	std::unique_ptr<CaptureSession> capture = bridge.CreateVirtualOutput("Magpie", { 643, 481 }, name);
	REQUIRE(capture);
	CHECK(name == "HEADLESS-2");
	CHECK(capture->GetKind() == CaptureSession::Kind::ImageCopy);
	CHECK(capture->OutputName() == name);
	CHECK(capture->WaylandDisplay() == fixture.sway->WaylandDisplay());

	// Any size and rate, far from the monitor
	REQUIRE(bridge.ConfigureOutput(name, { 643, 481 }, { 0, 17464 }, 199'610));
	std::vector<OutputInfo> outputs = bridge.ListOutputs();
	REQUIRE(outputs.size() == 2);
	for (const OutputInfo& output : outputs) {
		if (output.name == name) {
			CHECK(output.rect == Rect{ 0, 17464, 643, 17945 });
			CHECK(output.isVirtual);
			CHECK(std::abs(output.refreshRate - 199'610) <= 1);
		} else {
			// The monitor has stayed where it was
			CHECK(output.rect == Rect{ 0, 0, 1920, 1080 });
			CHECK_FALSE(output.isVirtual);
		}
	}

	SECTION("it goes away with its screen cast") {
		capture.reset();
		outputs = bridge.ListOutputs();
		REQUIRE(outputs.size() == 1);
		CHECK(outputs[0].rect == Rect{ 0, 0, 1920, 1080 });
	}

	SECTION("one that was left behind is removed, but only one that lies apart") {
		bridge.RemoveStaleVirtualOutput("HEADLESS-1");
		CHECK(bridge.ListOutputs().size() == 2);
		bridge.RemoveStaleVirtualOutput("DP-1");
		CHECK(bridge.ListOutputs().size() == 2);
		bridge.RemoveStaleVirtualOutput(name);
		CHECK(bridge.ListOutputs().size() == 1);
	}
}

TEST_CASE("An output of Sway is captured with its cursor", "[sway][desktop]") {
	SwayFixture fixture;
	REQUIRE_SWAY(fixture);
	SwayBridge& bridge = fixture.bridge;
	REQUIRE_FALSE(RenderTest::TestDevices().empty());
	VulkanDevice& device = *RenderTest::TestDevices()[0].device;
	const uint32_t messagesBefore = device.ValidationMessageCount();

	const Size size{ 640, 480 };
	const Point origin{ 0, 17464 };
	const auto window = fixture.OpenWindow({ .title = "source" });
	const SwayWindow source = fixture.Find("source");

	std::string name;
	const std::unique_ptr<CaptureSession> capture = bridge.CreateVirtualOutput("Magpie", size, name);
	REQUIRE(capture);
	REQUIRE(bridge.ConfigureOutput(name, size, origin, 120'000));
	REQUIRE(bridge.PlaceWindow(source.info.id, {
		.frameRect = { origin.x, origin.y, origin.x + size.width, origin.y + size.height },
		.noBorder = true,
		.activate = true
	}));

	{
		const std::unique_ptr<FrameSource> frames = FrameSource::Create(device, *capture, {});
		REQUIRE(frames);
		REQUIRE(PumpUntil(*frames, [](FrameSourceState state) { return state == FrameSourceState::NewFrame; }));

		const Texture& output = *frames->Output();
		CHECK(output.width == 640);
		CHECK(output.height == 480);
		CHECK(frames->SrcRect() == Rect{ 0, 0, 640, 480 });
		// One place in every quarter of the window's picture
		for (const Point pt : { Point{ 160, 120 }, Point{ 480, 120 }, Point{ 160, 360 }, Point{ 480, 360 }, Point{ 639, 479 } }) {
			INFO("at " << pt.x << "," << pt.y);
			CHECK(PixelAt(device, output, pt.x, pt.y) == TestWindow::ColorAt(size, pt.x, pt.y));
		}

		// Nothing new without a change of the window
		CHECK_FALSE(PumpUntil(*frames, [](FrameSourceState state) { return state == FrameSourceState::NewFrame; }, 300ms));
		window->Touch();
		CHECK(PumpUntil(*frames, [](FrameSourceState state) { return state == FrameSourceState::NewFrame; }));

		// The pointer on the output: its place, and the image the window gave it
		REQUIRE(bridge.MovePointer({ origin.x + 100, origin.y + 50 }));
		REQUIRE(PumpUntil(*frames, [&](FrameSourceState) {
			const CaptureCursorState& cursor = frames->CursorState();
			return cursor.isVisible && cursor.pos == Point{ 100, 50 } && frames->CursorImage().size.width == TestWindow::CURSOR_SIZE;
		}));
		const CaptureCursorImage& image = frames->CursorImage();
		CHECK(image.size == Size{ TestWindow::CURSOR_SIZE, TestWindow::CURSOR_SIZE });
		CHECK(image.hotSpot == TestWindow::CURSOR_HOT_SPOT);
		REQUIRE(image.pixels.size() == (size_t)TestWindow::CURSOR_SIZE * TestWindow::CURSOR_SIZE * 4);
		CHECK(std::memcmp(image.pixels.data(), TestWindow::CURSOR_RGBA, 4) == 0);
		CHECK(std::memcmp(&image.pixels[image.pixels.size() - 4], TestWindow::CURSOR_RGBA, 4) == 0);
		// The window has the pointer at the same place
		REQUIRE(fixture.WaitFor([&] { return window->PointerPos() == Point{ 100, 50 }; }));

		// The cursor is not painted into the picture
		window->Touch();
		REQUIRE(PumpUntil(*frames, [](FrameSourceState state) { return state == FrameSourceState::NewFrame; }));
		CHECK(PixelAt(device, *frames->Output(), 102, 52) == TestWindow::ColorAt(size, 102, 52));

		// Other places, the corners among them
		for (const Point pt : { Point{ 0, 0 }, Point{ 639, 479 }, Point{ 320, 240 } }) {
			INFO("pointer to " << pt.x << "," << pt.y);
			REQUIRE(bridge.MovePointer({ origin.x + pt.x, origin.y + pt.y }));
			CHECK(PumpUntil(*frames, [&](FrameSourceState) { return frames->CursorState().pos == pt; }));
		}

		// On the monitor it is not on this output
		REQUIRE(bridge.MovePointer({ 500, 500 }));
		CHECK(PumpUntil(*frames, [&](FrameSourceState) { return !frames->CursorState().isVisible; }));
	}

	SECTION("cropping") {
		const std::unique_ptr<FrameSource> frames = FrameSource::Create(device, *capture, {
			.cropping = { 10, 20, 30, 40 }
		});
		REQUIRE(frames);
		REQUIRE(PumpUntil(*frames, [](FrameSourceState state) { return state == FrameSourceState::NewFrame; }));
		CHECK(frames->SrcRect() == Rect{ 10, 20, 610, 440 });
		const Texture& output = *frames->Output();
		CHECK(output.width == 600);
		CHECK(output.height == 420);
		CHECK(PixelAt(device, output, 0, 0) == TestWindow::ColorAt(size, 10, 20));
		CHECK(PixelAt(device, output, 599, 419) == TestWindow::ColorAt(size, 609, 439));
		// The middle of the window is at 310,220 of what is left
		CHECK(PixelAt(device, output, 309, 219) == TestWindow::ColorAt(size, 319, 239));
		CHECK(PixelAt(device, output, 310, 220) == TestWindow::ColorAt(size, 320, 240));
	}

	SECTION("the capture ends with the output") {
		const std::unique_ptr<FrameSource> frames = FrameSource::Create(device, *capture, {});
		REQUIRE(frames);
		REQUIRE(PumpUntil(*frames, [](FrameSourceState state) { return state == FrameSourceState::NewFrame; }));
		fixture.sway->Command("output " + name + " unplug");
		CHECK_FALSE(PumpUntil(*frames, [](FrameSourceState) { return false; }, 2s));
		CHECK(frames->Update() == FrameSourceState::Error);
	}

	CHECK(device.ValidationMessageCount() == messagesBefore);
}

TEST_CASE("A program that hides the cursor has none in the capture", "[sway][desktop]") {
	SwayFixture fixture;
	REQUIRE_SWAY(fixture);
	SwayBridge& bridge = fixture.bridge;
	REQUIRE_FALSE(RenderTest::TestDevices().empty());
	VulkanDevice& device = *RenderTest::TestDevices()[0].device;

	const auto window = fixture.OpenWindow({ .title = "hides", .hidesCursor = true });
	const SwayWindow source = fixture.Find("hides");
	const std::unique_ptr<CaptureSession> capture = bridge.CaptureOutput("HEADLESS-1");
	const std::unique_ptr<FrameSource> frames = FrameSource::Create(device, *capture, {});
	REQUIRE(frames);
	REQUIRE(PumpUntil(*frames, [](FrameSourceState state) { return state == FrameSourceState::NewFrame; }));

	// Next to the window Sway shows its own arrow
	REQUIRE(bridge.MovePointer({ 5, 5 }));
	REQUIRE(PumpUntil(*frames, [&](FrameSourceState) {
		return frames->CursorState().isVisible && frames->CursorState().pos == Point{ 5, 5 };
	}));

	// On the window there is nothing to draw
	const Rect& client = source.info.clientRect;
	REQUIRE(bridge.MovePointer({ client.left + 100, client.top + 100 }));
	REQUIRE(fixture.WaitFor([&] { return window->PointerPos() == Point{ 100, 100 }; }));
	CHECK(PumpUntil(*frames, [&](FrameSourceState) { return !frames->CursorState().isVisible; }));

	REQUIRE(bridge.MovePointer({ 5, 5 }));
	CHECK(PumpUntil(*frames, [&](FrameSourceState) { return frames->CursorState().isVisible; }));
}

TEST_CASE("Key bindings made on Sway report their keys", "[sway][desktop]") {
	SwayFixture fixture;
	REQUIRE_SWAY(fixture);
	SwayBridge& bridge = fixture.bridge;

	CHECK(bridge.HasOwnShortcuts());
	const ShortcutRequest wanted[]{
		{ "scale", "ALT+SHIFT+a" },
		{ "toolbar", "ALT+SHIFT+d" },
		{ "screenshot", "" }
	};
	const auto bound = bridge.BindShortcuts(wanted);
	REQUIRE(bound.size() == 2);
	CHECK(bound[0] == std::pair<std::string, std::string>{ "scale", "Alt+Shift+A" });
	CHECK(bound[1] == std::pair<std::string, std::string>{ "toolbar", "Alt+Shift+D" });

	// A keyboard for the test, and a window that has the keyboard
	const auto window = fixture.OpenWindow({ .title = "typing" });
	fixture.Find("typing");
	WlrInput keyboard;
	REQUIRE(keyboard.Connect(fixture.sway->WaylandDisplay()));
	REQUIRE(keyboard.HasKeyboard());
	uint32_t keymapSize = 0;
	const int keymap = CreateKeymapFile(keymapSize);
	REQUIRE(keymap >= 0);
	keyboard.SetKeymap(keymap, keymapSize);
	close(keymap);
	REQUIRE(keyboard.Flush());
	REQUIRE(fixture.WaitFor([&] { return window->HasKeyboardFocus(); }));

	// A virtual keyboard has to say itself which modifiers are down; the
	// compositor does not work that out from its keys. Shift is the first
	// modifier of a keymap and Alt the fourth.
	static constexpr uint32_t SHIFT = 1, ALT = 8;
	const auto press = [&](std::initializer_list<uint32_t> keys, uint32_t modifiers = 0) {
		for (uint32_t key : keys) {
			keyboard.Key(key, true);
			if (key != KEY_A && key != KEY_B && key != KEY_D) {
				keyboard.Modifiers(modifiers, 0, 0, 0);
			}
		}
		for (auto it = std::rbegin(keys); it != std::rend(keys); ++it) {
			keyboard.Key(*it, false);
		}
		keyboard.Modifiers(0, 0, 0, 0);
		REQUIRE(keyboard.Flush());
	};

	// A key that is not bound goes to the window
	press({ KEY_B });
	REQUIRE(fixture.WaitFor([&] { return window->TakeKeyEvents().size() == 2; }));
	CHECK(bridge.TakeActivatedShortcuts().empty());

	press({ KEY_LEFTALT, KEY_LEFTSHIFT, KEY_A }, SHIFT | ALT);
	std::vector<std::string> activated;
	REQUIRE(fixture.WaitFor([&] {
		activated = bridge.TakeActivatedShortcuts();
		return !activated.empty();
	}));
	CHECK(activated == std::vector<std::string>{ "scale" });

	press({ KEY_LEFTALT, KEY_LEFTSHIFT, KEY_D }, SHIFT | ALT);
	REQUIRE(fixture.WaitFor([&] {
		activated = bridge.TakeActivatedShortcuts();
		return !activated.empty();
	}));
	CHECK(activated == std::vector<std::string>{ "toolbar" });

	// After Sway has read its configuration again the bindings are made anew
	fixture.sway->Command("reload");
	REQUIRE(fixture.WaitFor([&] { return fixture.bridge.ListWindows().size() == 1; }));
	std::this_thread::sleep_for(200ms);
	bridge.Dispatch();
	REQUIRE(fixture.WaitFor([&] { return window->HasKeyboardFocus(); }));
	press({ KEY_LEFTALT, KEY_LEFTSHIFT, KEY_A }, SHIFT | ALT);
	REQUIRE(fixture.WaitFor([&] {
		activated = bridge.TakeActivatedShortcuts();
		return !activated.empty();
	}));
	CHECK(activated == std::vector<std::string>{ "scale" });
}

namespace {

// Runs a scaling session and looks at the monitor it draws on
struct SessionFixture : SwayFixture {
	SessionFixture() {
		if (!isUsable) {
			return;
		}
		timerFd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
		itimerspec spec{};
		spec.it_interval.tv_nsec = spec.it_value.tv_nsec = 10'000'000;
		timerfd_settime(timerFd, 0, &spec, nullptr);

		options.effects.push_back({ .name = "Nearest", .scalingType = ScalingType::Fit });
		// Nothing of this test goes into the user's cache of compiled effects
		options.flags |= ScalingFlags::DisableEffectCache;
		options.fullscreenInitialToolbarState = ToolbarState::Off;
		options.windowedInitialToolbarState = ToolbarState::Off;
		config.effectsDir = MAGPIE_EFFECTS_DIR;
		config.stateFile = sway->Dir() / "scaling-state";
		config.appId = "magpie.test";
	}

	~SessionFixture() {
		monitor.reset();
		if (timerFd >= 0) {
			close(timerFd);
		}
	}

	// Lets the session work until `done` says so; false if it ends first or
	// the time runs out
	bool StepUntil(const std::function<bool()>& done, milliseconds timeout = 5s) {
		const auto deadline = steady_clock::now() + timeout;
		while (steady_clock::now() < deadline) {
			// The timer keeps Step from waiting for a frame that never comes
			uint64_t expirations;
			[[maybe_unused]] const ssize_t got = read(timerFd, &expirations, sizeof(expirations));
			const int wakeFds[]{ timerFd };
			if (!session.Step(wakeFds)) {
				return false;
			}
			if (done()) {
				return true;
			}
		}
		return false;
	}

	// The colour the monitor shows at a place, after the session has had
	// time to draw
	std::array<uint8_t, 3> MonitorPixel(VulkanDevice& device, Point pt) {
		if (!monitor) {
			monitorCapture = bridge.CaptureOutput("HEADLESS-1");
			monitor = FrameSource::Create(device, *monitorCapture, {});
			REQUIRE(monitor);
		}
		// Whatever the monitor shows by now
		bool hasFrame = monitor->Output() != nullptr;
		StepUntil([&] {
			if (!monitor->Dispatch()) {
				return true;
			}
			hasFrame = monitor->Update() == FrameSourceState::NewFrame || hasFrame;
			return false;
		}, 300ms);
		REQUIRE(hasFrame);
		return PixelAt(device, *monitor->Output(), pt.x, pt.y);
	}

	ScalingSession session;
	ScalingOptions options;
	ScalingSession::Config config;
	std::unique_ptr<CaptureSession> monitorCapture;
	std::unique_ptr<FrameSource> monitor;
	int timerFd = -1;
};

}

TEST_CASE("A window is scaled to fullscreen on Sway", "[sway][desktop][session]") {
	SessionFixture fixture;
	REQUIRE_SWAY(fixture);
	SwayBridge& bridge = fixture.bridge;
	ScalingSession& session = fixture.session;
	REQUIRE_FALSE(RenderTest::TestDevices().empty());
	VulkanDevice& device = *RenderTest::TestDevices()[0].device;

	const Size size{ 640, 480 };
	const auto window = fixture.OpenWindow({ .title = "source" });
	const SwayWindow before = fixture.Find("source");
	REQUIRE(bridge.MovePointer({ 1900, 20 }));

	REQUIRE(session.Start(bridge, before.info.id, fixture.options, fixture.config) == ScalingError::NoError);
	REQUIRE(session.IsRunning());
	CHECK(std::filesystem::exists(fixture.config.stateFile));

	// The window is on an output of its own size, without its title bar
	const SwayWindow scaled = fixture.Find("source");
	CHECK(scaled.output == "HEADLESS-2");
	CHECK(scaled.info.frameRect.Width() == 640);
	CHECK(scaled.info.frameRect.Height() == 480);
	CHECK(scaled.info.hasNoBorder);
	CHECK(scaled.info.isActive);
	CHECK(session.SourceRect() == scaled.info.frameRect);
	// 4:3 in the middle of 16:9
	CHECK(session.DestinationRect() == Rect{ 240, 0, 1680, 1080 });

	SECTION("the monitor shows the picture") {
		REQUIRE(fixture.StepUntil([&] { return session.GetStatistics().presentedFrames >= 1; }));
		// Black beside the picture, and the four quarters of the window in it
		CHECK(IsNear(fixture.MonitorPixel(device, { 100, 500 }), { 0, 0, 0 }));
		CHECK(IsNear(fixture.MonitorPixel(device, { 1800, 500 }), { 0, 0, 0 }));
		for (const Point pt : { Point{ 160, 120 }, Point{ 480, 120 }, Point{ 160, 360 }, Point{ 480, 360 } }) {
			const Point onMonitor{ 240 + pt.x * 1440 / 640, pt.y * 1080 / 480 };
			INFO("the window's " << pt.x << "," << pt.y << " at " << onMonitor.x << "," << onMonitor.y);
			CHECK(IsNear(fixture.MonitorPixel(device, onMonitor), TestWindow::ColorAt(size, pt.x, pt.y)));
		}
		// The edges of the picture are where the destination rectangle says
		CHECK(IsNear(fixture.MonitorPixel(device, { 239, 540 }), { 0, 0, 0 }));
		CHECK(IsNear(fixture.MonitorPixel(device, { 241, 300 }), TestWindow::ColorAt(size, 0, 133)));
		CHECK(IsNear(fixture.MonitorPixel(device, { 1679, 300 }), TestWindow::ColorAt(size, 639, 133)));
	}

	SECTION("the pointer acts on the window through the picture") {
		// Onto the picture, once it is shown: it is sent to the place of the
		// window that the picture shows there
		REQUIRE(fixture.StepUntil([&] { return session.GetStatistics().presentedFrames >= 1; }));
		window->TakePointerEvents();
		REQUIRE(bridge.MovePointer({ 240 + 720, 540 }));
		REQUIRE(fixture.StepUntil([&] {
			return session.GetStatistics().pointerCaptures >= 1 && window->PointerPos().has_value();
		}));
		const Point pos = *window->PointerPos();
		CHECK(std::abs(pos.x - 320) <= 1);
		CHECK(std::abs(pos.y - 240) <= 1);
		CHECK(session.GetStatistics().pointerCaptures >= 1);

		// The cursor is drawn where the pointer is in the picture
		REQUIRE(fixture.StepUntil([&] {
			return IsNear(fixture.MonitorPixel(device, { 240 + pos.x * 1440 / 640 + 8, pos.y * 1080 / 480 + 8 }),
				{ TestWindow::CURSOR_RGBA[0], TestWindow::CURSOR_RGBA[1], TestWindow::CURSOR_RGBA[2] });
		}));

		// A click arrives there
		bridge.Input().Button(BTN_LEFT, true);
		bridge.Input().Button(BTN_LEFT, false);
		bridge.Input().Flush();
		std::vector<TestWindow::PointerEvent> buttons;
		REQUIRE(fixture.StepUntil([&] {
			for (const TestWindow::PointerEvent& event : window->TakePointerEvents()) {
				if (event.type == TestWindow::PointerEvent::Type::Button) {
					buttons.push_back(event);
				}
			}
			return buttons.size() == 2;
		}));
		CHECK(buttons[0].button == BTN_LEFT);
		CHECK(buttons[0].isPressed);
		CHECK(std::abs(buttons[0].x - 320) <= 1);
		CHECK(std::abs(buttons[0].y - 240) <= 1);
		CHECK_FALSE(buttons[1].isPressed);

		// With one monitor there is no way out: pushed against the edge of
		// the output, the pointer stays on the window
		bridge.Input().MoveRelative(-5000, 0);
		bridge.Input().Flush();
		REQUIRE(fixture.StepUntil([&] { return window->PointerPos() && window->PointerPos()->x == 0; }));
		CHECK(session.GetStatistics().pointerReleases == 0);
	}

	SECTION("it ends when the window closes") {
		window->TakePointerEvents();
		fixture.sway->Command("[con_id=" + before.info.id + "] kill");
		CHECK_FALSE(fixture.StepUntil([] { return false; }, 3s));
		CHECK(session.GetEndReason() == ScalingSession::EndReason::SourceClosed);
	}

	SECTION("it ends when the window changes its size") {
		fixture.sway->Command("[con_id=" + before.info.id + "] resize set width 700 px height 500 px");
		CHECK_FALSE(fixture.StepUntil([] { return false; }, 3s));
		CHECK(session.GetEndReason() == ScalingSession::EndReason::SourceResized);
	}

	SECTION("a window of another program that opens goes to the monitor, which ends scaling") {
		// Sway opens it where the active window is: on the virtual output
		const TestWindowProcess other(MAGPIE_TEST_WINDOW_PROGRAM, fixture.sway->WaylandDisplay(),
			{ "--title", "other", "--app-id", "magpie.test.other" });
		CHECK_FALSE(fixture.StepUntil([] { return false; }, 5s));
		CHECK(session.GetEndReason() == ScalingSession::EndReason::FocusLost);
		const SwayWindow moved = fixture.Find("other");
		CHECK(moved.output == "HEADLESS-1");
		CHECK(moved.info.pid == (uint32_t)other.Pid());
	}

	SECTION("a window of the same program that opens stays in the picture") {
		const auto dialog = fixture.OpenWindow({ .size = { 200, 100 }, .title = "dialog" });
		CHECK(fixture.StepUntil([&] { return FindByTitle(bridge.Windows(), "dialog") != nullptr; }));
		CHECK_FALSE(fixture.StepUntil([] { return false; }, 500ms));
		CHECK(session.IsRunning());
		CHECK(session.GetEndReason() == ScalingSession::EndReason::None);
		CHECK(fixture.Find("dialog").output == "HEADLESS-2");
	}

	fixture.monitor.reset();
	session.Stop();
	CHECK_FALSE(session.IsRunning());
	CHECK_FALSE(std::filesystem::exists(fixture.config.stateFile));
	CHECK(bridge.ListOutputs().size() == 1);
	if (FindByTitle((bridge.ListWindows(), bridge.Windows()), "source")) {
		// Back where it was, with its title bar
		const SwayWindow after = fixture.Find("source");
		CHECK(after.output == "HEADLESS-1");
		CHECK(after.border == before.border);
		if (session.GetEndReason() != ScalingSession::EndReason::SourceResized) {
			CHECK(after.info.frameRect == before.info.frameRect);
		}
	}
}

TEST_CASE("What a scaling session on Sway left behind is undone", "[sway][desktop][session]") {
	SessionFixture fixture;
	REQUIRE_SWAY(fixture);
	SwayBridge& bridge = fixture.bridge;

	const auto window = fixture.OpenWindow({ .title = "source" });
	const SwayWindow before = fixture.Find("source");

	// What a session does to the desktop, by a second connection that then
	// goes away without undoing it
	std::string name;
	std::unique_ptr<CaptureSession> capture;
	{
		SwayBridge other;
		REQUIRE(other.Initialize({ fixture.sway->SocketPath(), fixture.sway->WaylandDisplay() }));
		capture = other.CreateVirtualOutput("Magpie", { 640, 480 }, name);
		REQUIRE(capture);
		REQUIRE(other.ConfigureOutput(name, { 640, 480 }, { 0, 17464 }, 120'000));
		REQUIRE(other.PlaceWindow(before.info.id, { .frameRect = { 0, 17464, 640, 17944 }, .noBorder = true }));
	}
	REQUIRE(fixture.Find("source").output == name);

	const Rect& frame = before.info.frameRect;
	std::ofstream(fixture.config.stateFile) << "magpie-scaling-state 1\n"
		<< "window " << before.info.id << '\n'
		<< "frame " << frame.left << ' ' << frame.top << ' ' << frame.right << ' ' << frame.bottom << '\n'
		<< "noBorder 0\n"
		<< "restore " << before.info.restoreData << '\n'
		<< "virtualOutput " << name << '\n';

	ScalingSession::RecoverAfterCrash(bridge, fixture.config.stateFile);

	const SwayWindow after = fixture.Find("source");
	CHECK(after.output == "HEADLESS-1");
	CHECK(after.info.frameRect == frame);
	CHECK(after.border == before.border);
	CHECK(bridge.ListOutputs().size() == 1);
	CHECK_FALSE(std::filesystem::exists(fixture.config.stateFile));
}
