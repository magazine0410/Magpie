#include "Bridge.h"
#include <algorithm>
#include <cctype>
#include <fmt/format.h>
#include "KWinBridge.h"
#include "NestedCompositor.h"
#include "PortalCapture.h"
#include "SwayBridge.h"

namespace Magpie {

const char* CapabilityReport::Name(Capability capability) noexcept {
	static constexpr const char* NAMES[] = {
		"C1 window information",
		"C2 window capture",
		"C3 overlay",
		"C4 cursor information",
		"C5 cursor control",
		"C6 window control",
		"C7 global shortcuts",
		"C8 touch input",
		"C9 session services"
	};
	static_assert(std::size(NAMES) == (size_t)Capability::COUNT);
	return NAMES[(size_t)capability];
}

CapabilityReport ProbeCapabilities() noexcept {
	CapabilityReport report;
	const auto add = [&](Capability capability, std::string provider) {
		report[capability].providers.push_back(std::move(provider));
	};

	if (SwayBridge::IsSessionCompositor()) {
		SwayBridge sway;
		if (sway.Initialize()) {
			add(Capability::WindowInfo, "Sway (control socket)");
			add(Capability::WindowControl, "Sway (control socket), with an output without a screen as virtual output");
			add(Capability::GlobalShortcuts, "Sway (key bindings made at run time)");
			const std::string missing = sway.MissingForScaling();
			if (missing.empty()) {
				add(Capability::WindowCapture, "Sway (ext-image-copy-capture of the virtual output), without a dialog");
				add(Capability::CursorInfo, "cursor session of ext-image-copy-capture");
				add(Capability::Overlay, "layer shell");
				add(Capability::CursorControl, "Sway (wlr-virtual-pointer; pointer speed over the control socket)");
			} else {
				report[Capability::WindowCapture].note = "Sway lacks: " + missing;
			}
		}
	} else {
		KWinBridge kwin;
		if (kwin.Initialize()) {
			if (kwin.WindowManagementVersion() != 0) {
				add(Capability::WindowInfo, fmt::format(
					"KWin (org_kde_plasma_window_management, version {})", kwin.WindowManagementVersion()));
				add(Capability::WindowControl, "KWin (scripting over D-Bus), with a virtual output of the screen cast");
				add(Capability::Overlay, "layer shell");
			}
			if (kwin.ScreencastVersion() != 0) {
				add(Capability::WindowCapture, fmt::format(
					"KWin (zkde_screencast_unstable_v1, version {}), without a dialog", kwin.ScreencastVersion()));
				add(Capability::CursorInfo, "metadata of KWin's screen cast, while the pointer is over the source");
			}
			if (kwin.HasFakeInput()) {
				add(Capability::CursorControl, "KWin (org_kde_kwin_fake_input; pointer speed over D-Bus)");
			}
		}
	}

	PortalCapture::Info portal;
	if (PortalCapture::Probe(portal)) {
		add(Capability::WindowCapture, fmt::format(
			"desktop portal (ScreenCast, version {}), the user chooses the source in a dialog; "
			"for the viewer only, which shows a scaled copy without input", portal.version));
		if (portal.cursorModes & PortalCapture::Metadata) {
			add(Capability::CursorInfo, "metadata of the portal's screen cast, while the pointer is over the source");
		}
		add(Capability::GlobalShortcuts, "desktop portal (GlobalShortcuts), where the desktop implements it");
	}

	// What needs nothing from the desktop
	if (NestedCompositor::IsAvailable()) {
		for (Capability capability : { Capability::WindowInfo, Capability::WindowCapture, Capability::CursorInfo,
			Capability::CursorControl, Capability::WindowControl }) {
			add(capability, "nested session (a Sway of Magpie's own), for programs that Magpie starts");
		}
	} else {
		report[Capability::WindowControl].note = "a nested session, which works on every desktop, needs the program \"sway\"";
	}

	add(Capability::SessionServices, "session bus (screen saver and notifications), where the desktop implements them");
	report[Capability::TouchInput].note = "not built";
	return report;
}

double PointerDevice::Slowed(double acceleration, double scale) noexcept {
	if (scale <= 0.0) {
		return acceleration;
	}
	return std::clamp((1.0 + acceleration) / scale - 1.0, -1.0, 1.0);
}

std::unique_ptr<DesktopBridge> DesktopBridge::Create(std::string& error) noexcept {
	if (SwayBridge::IsSessionCompositor()) {
		auto sway = std::make_unique<SwayBridge>();
		if (sway->Initialize()) {
			return sway;
		}
	}

	auto kwin = std::make_unique<KWinBridge>();
	if (kwin->Initialize()) {
		return kwin;
	}

	error = "This desktop has no back end for scaling windows that are open already.";
	return nullptr;
}

static bool EqualsIgnoringCase(std::string_view a, std::string_view b) noexcept {
	return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
		return std::tolower((unsigned char)x) == std::tolower((unsigned char)y);
	});
}

std::vector<const WindowInfo*> FindWindows(std::span<const WindowInfo> windows, std::string_view query) noexcept {
	std::vector<const WindowInfo*> result;
	if (query.empty()) {
		return result;
	}

	for (const WindowInfo& window : windows) {
		if (window.id == query) {
			return { &window };
		}
	}

	for (const WindowInfo& window : windows) {
		if (EqualsIgnoringCase(window.appId, query) || EqualsIgnoringCase(window.resourceName, query)) {
			result.push_back(&window);
		}
	}
	if (!result.empty()) {
		return result;
	}

	for (const WindowInfo& window : windows) {
		if (window.title.find(query) != std::string::npos) {
			result.push_back(&window);
		}
	}
	return result;
}

}
