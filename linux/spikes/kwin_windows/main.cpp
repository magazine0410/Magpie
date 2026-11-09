// Window information and control through org_kde_plasma_window_management.
//
// Answers, for capability C1/C6 of the plan: can an ordinary program list the
// windows with geometry, owner and focus state, and move a window to another
// output, without a KWin script?
//
// Usage:
//   kwin_windows                         list windows once
//   kwin_windows --watch                 keep printing changes
//   kwin_windows --send-to-output UUID OUTPUT_NAME
//   kwin_windows --set-state UUID FLAGS_HEX VALUE_HEX     (see the state enum)
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <wayland-client.h>
#include "plasma-window-management-client-protocol.h"

struct Window {
	std::string uuid;
	std::string title;
	std::string appId;
	std::string resourceName;
	uint32_t pid = 0;
	uint32_t state = 0;
	int32_t x = 0, y = 0;
	uint32_t w = 0, h = 0;
	int32_t cx = 0, cy = 0;
	uint32_t cw = 0, ch = 0;
	bool hasClientGeometry = false;
	bool initialDone = false;
	org_kde_plasma_window* proxy = nullptr;
};

struct Output {
	wl_output* proxy = nullptr;
	std::string name;
};

static org_kde_plasma_window_management* g_mgmt;
static std::vector<std::unique_ptr<Window>> g_windows;
static std::vector<std::unique_ptr<Output>> g_outputs;
static bool g_watch;

static std::string StateText(uint32_t s) {
	static const std::pair<uint32_t, const char*> NAMES[] = {
		{ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_ACTIVE, "active"},
		{ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_MINIMIZED, "minimized"},
		{ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_MAXIMIZED, "maximized"},
		{ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_FULLSCREEN, "fullscreen"},
		{ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_KEEP_ABOVE, "keep_above"},
		{ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_NO_BORDER, "no_border"},
	};
	std::string text;
	for (const auto& [bit, name] : NAMES) {
		if (s & bit) {
			text += text.empty() ? "" : ",";
			text += name;
		}
	}
	return text.empty() ? "-" : text;
}

static void Print(const Window& w, const char* why) {
	std::printf("%s uuid=%s pid=%u app_id=\"%s\" resource=\"%s\" frame=%ux%u%+d%+d",
		why, w.uuid.c_str(), w.pid, w.appId.c_str(), w.resourceName.c_str(), w.w, w.h, w.x, w.y);
	if (w.hasClientGeometry) {
		std::printf(" client=%ux%u%+d%+d", w.cw, w.ch, w.cx, w.cy);
	}
	std::printf(" state=%s(0x%x) title=\"%s\"\n", StateText(w.state).c_str(), w.state, w.title.c_str());
	std::fflush(stdout);
}

static void Changed(Window* w, const char* what) {
	if (g_watch && w->initialDone) {
		Print(*w, what);
	}
}

static const org_kde_plasma_window_listener WINDOW_LISTENER = {
	.title_changed = [](void* d, org_kde_plasma_window*, const char* v) {
		auto w = (Window*)d; w->title = v; Changed(w, "title");
	},
	.app_id_changed = [](void* d, org_kde_plasma_window*, const char* v) { ((Window*)d)->appId = v; },
	.state_changed = [](void* d, org_kde_plasma_window*, uint32_t v) {
		auto w = (Window*)d; w->state = v; Changed(w, "state");
	},
	.virtual_desktop_changed = [](void*, org_kde_plasma_window*, int32_t) {},
	.themed_icon_name_changed = [](void*, org_kde_plasma_window*, const char*) {},
	.unmapped = [](void* d, org_kde_plasma_window*) { Changed((Window*)d, "unmapped"); },
	.initial_state = [](void* d, org_kde_plasma_window*) { ((Window*)d)->initialDone = true; },
	.parent_window = [](void*, org_kde_plasma_window*, org_kde_plasma_window*) {},
	.geometry = [](void* d, org_kde_plasma_window*, int32_t x, int32_t y, uint32_t w_, uint32_t h) {
		auto w = (Window*)d; w->x = x; w->y = y; w->w = w_; w->h = h; Changed(w, "geometry");
	},
	.icon_changed = [](void*, org_kde_plasma_window*) {},
	.pid_changed = [](void* d, org_kde_plasma_window*, uint32_t v) { ((Window*)d)->pid = v; },
	.virtual_desktop_entered = [](void*, org_kde_plasma_window*, const char*) {},
	.virtual_desktop_left = [](void*, org_kde_plasma_window*, const char*) {},
	.application_menu = [](void*, org_kde_plasma_window*, const char*, const char*) {},
	.activity_entered = [](void*, org_kde_plasma_window*, const char*) {},
	.activity_left = [](void*, org_kde_plasma_window*, const char*) {},
	.resource_name_changed = [](void* d, org_kde_plasma_window*, const char* v) { ((Window*)d)->resourceName = v; },
	.client_geometry = [](void* d, org_kde_plasma_window*, int32_t x, int32_t y, uint32_t w_, uint32_t h) {
		auto w = (Window*)d; w->cx = x; w->cy = y; w->cw = w_; w->ch = h; w->hasClientGeometry = true;
	},
	.mapped = [](void*, org_kde_plasma_window*) {},
};

static const org_kde_plasma_window_management_listener MGMT_LISTENER = {
	.show_desktop_changed = [](void*, org_kde_plasma_window_management*, uint32_t) {},
	.window = [](void*, org_kde_plasma_window_management*, uint32_t) {},
	.stacking_order_changed = [](void*, org_kde_plasma_window_management*, wl_array*) {},
	.stacking_order_uuid_changed = [](void*, org_kde_plasma_window_management*, const char*) {},
	.window_with_uuid = [](void*, org_kde_plasma_window_management* mgmt, uint32_t, const char* uuid) {
		auto w = std::make_unique<Window>();
		w->uuid = uuid;
		w->proxy = org_kde_plasma_window_management_get_window_by_uuid(mgmt, uuid);
		org_kde_plasma_window_add_listener(w->proxy, &WINDOW_LISTENER, w.get());
		g_windows.push_back(std::move(w));
	},
	.stacking_order_changed_2 = [](void*, org_kde_plasma_window_management*) {},
};

static const wl_output_listener OUTPUT_LISTENER = {
	.geometry = [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {},
	.mode = [](void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {},
	.done = [](void*, wl_output*) {},
	.scale = [](void*, wl_output*, int32_t) {},
	.name = [](void* d, wl_output*, const char* name) { ((Output*)d)->name = name; },
	.description = [](void*, wl_output*, const char*) {},
};

static const wl_registry_listener REGISTRY_LISTENER = {
	.global = [](void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
		if (std::strcmp(interface, org_kde_plasma_window_management_interface.name) == 0) {
			g_mgmt = (org_kde_plasma_window_management*)wl_registry_bind(
				registry, name, &org_kde_plasma_window_management_interface, std::min(version, 18u));
			org_kde_plasma_window_management_add_listener(g_mgmt, &MGMT_LISTENER, nullptr);
		} else if (std::strcmp(interface, wl_output_interface.name) == 0 && version >= 4) {
			auto o = std::make_unique<Output>();
			o->proxy = (wl_output*)wl_registry_bind(registry, name, &wl_output_interface, 4);
			wl_output_add_listener(o->proxy, &OUTPUT_LISTENER, o.get());
			g_outputs.push_back(std::move(o));
		}
	},
	.global_remove = [](void*, wl_registry*, uint32_t) {},
};

static Window* Find(const char* uuid) {
	for (auto& w : g_windows) {
		if (w->uuid == uuid) {
			return w.get();
		}
	}
	std::fprintf(stderr, "no window with uuid %s\n", uuid);
	return nullptr;
}

int main(int argc, char* argv[]) {
	wl_display* display = wl_display_connect(nullptr);
	if (!display) {
		std::fprintf(stderr, "cannot connect to the Wayland display\n");
		return 1;
	}

	wl_registry* registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &REGISTRY_LISTENER, nullptr);
	wl_display_roundtrip(display);	// globals
	if (!g_mgmt) {
		std::fprintf(stderr, "RESULT: org_kde_plasma_window_management is not offered to this client\n");
		return 1;
	}
	wl_display_roundtrip(display);	// window list, output names
	wl_display_roundtrip(display);	// window properties

	if (argc >= 4 && std::strcmp(argv[1], "--send-to-output") == 0) {
		Window* w = Find(argv[2]);
		wl_output* target = nullptr;
		for (auto& o : g_outputs) {
			if (o->name == argv[3]) {
				target = o->proxy;
			}
		}
		if (!w || !target) {
			std::fprintf(stderr, "window or output not found; outputs:");
			for (auto& o : g_outputs) {
				std::fprintf(stderr, " %s", o->name.c_str());
			}
			std::fprintf(stderr, "\n");
			return 1;
		}
		org_kde_plasma_window_send_to_output(w->proxy, target);
		wl_display_roundtrip(display);
		wl_display_roundtrip(display);
		Print(*w, "after send_to_output");
		return 0;
	}

	if (argc >= 5 && std::strcmp(argv[1], "--set-state") == 0) {
		Window* w = Find(argv[2]);
		if (!w) {
			return 1;
		}
		org_kde_plasma_window_set_state(w->proxy,
			(uint32_t)std::strtoul(argv[3], nullptr, 16), (uint32_t)std::strtoul(argv[4], nullptr, 16));
		wl_display_roundtrip(display);
		wl_display_roundtrip(display);
		Print(*w, "after set_state");
		return 0;
	}

	std::printf("outputs:");
	for (auto& o : g_outputs) {
		std::printf(" %s", o->name.c_str());
	}
	std::printf("\n");
	for (auto& w : g_windows) {
		Print(*w, "window");
	}

	g_watch = argc >= 2 && std::strcmp(argv[1], "--watch") == 0;
	while (g_watch && wl_display_dispatch(display) != -1) {
	}
	return 0;
}
