// Test T6: move the pointer and press buttons through KWin's
// org_kde_kwin_fake_input protocol.
//
// Usage: kwin_pointer COMMAND...
//   move X Y        absolute position in compositor (global) coordinates
//   rel DX DY       relative motion
//   click           left button press and release
//   sleep MS
// Commands run in the order given.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <wayland-client.h>
#include "fake-input-client-protocol.h"

static org_kde_kwin_fake_input* g_fakeInput;
static uint32_t g_version;

static const wl_registry_listener REGISTRY_LISTENER = {
	.global = [](void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
		if (std::strcmp(interface, org_kde_kwin_fake_input_interface.name) == 0) {
			g_version = std::min(version, 4u);
			g_fakeInput = (org_kde_kwin_fake_input*)wl_registry_bind(
				registry, name, &org_kde_kwin_fake_input_interface, g_version);
		}
	},
	.global_remove = [](void*, wl_registry*, uint32_t) {},
};

int main(int argc, char* argv[]) {
	wl_display* display = wl_display_connect(nullptr);
	if (!display) {
		std::fprintf(stderr, "cannot connect to the Wayland display\n");
		return 1;
	}
	wl_registry_add_listener(wl_display_get_registry(display), &REGISTRY_LISTENER, nullptr);
	wl_display_roundtrip(display);

	if (!g_fakeInput || g_version < 3) {
		std::printf("RESULT: org_kde_kwin_fake_input (v3+) is not offered to this client\n");
		return 1;
	}

	org_kde_kwin_fake_input_authenticate(g_fakeInput, "Magpie feasibility test", "pointer transfer test");
	wl_display_roundtrip(display);

	static constexpr uint32_t BTN_LEFT = 0x110;
	for (int i = 1; i < argc; ++i) {
		const std::string cmd = argv[i];
		if (cmd == "move" && i + 2 < argc) {
			const double x = std::atof(argv[++i]);
			const double y = std::atof(argv[++i]);
			org_kde_kwin_fake_input_pointer_motion_absolute(
				g_fakeInput, wl_fixed_from_double(x), wl_fixed_from_double(y));
		} else if (cmd == "rel" && i + 2 < argc) {
			const double dx = std::atof(argv[++i]);
			const double dy = std::atof(argv[++i]);
			org_kde_kwin_fake_input_pointer_motion(g_fakeInput, wl_fixed_from_double(dx), wl_fixed_from_double(dy));
		} else if (cmd == "click") {
			org_kde_kwin_fake_input_button(g_fakeInput, BTN_LEFT, 1);
			wl_display_roundtrip(display);
			usleep(30000);
			org_kde_kwin_fake_input_button(g_fakeInput, BTN_LEFT, 0);
		} else if (cmd == "sleep" && i + 1 < argc) {
			wl_display_roundtrip(display);
			usleep((useconds_t)std::atoi(argv[++i]) * 1000);
		} else {
			std::fprintf(stderr, "bad command %s\n", cmd.c_str());
			return 2;
		}
		wl_display_roundtrip(display);
	}

	return 0;
}
