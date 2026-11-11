#include "Bridge.h"
#include <algorithm>
#include <cctype>
#include <fmt/format.h>
#include "KWinBridge.h"
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

	KWinBridge kwin;
	if (kwin.Initialize()) {
		if (kwin.WindowManagementVersion() != 0) {
			report[Capability::WindowInfo].providers.push_back(fmt::format(
				"KWin (org_kde_plasma_window_management, version {})", kwin.WindowManagementVersion()));
		}
		if (kwin.ScreencastVersion() != 0) {
			report[Capability::WindowCapture].providers.push_back(fmt::format(
				"KWin (zkde_screencast_unstable_v1, version {}), without a dialog", kwin.ScreencastVersion()));
			report[Capability::CursorInfo].providers.push_back(
				"metadata of KWin's screen cast, while the pointer is over the source");
		}
	}

	PortalCapture::Info portal;
	if (PortalCapture::Probe(portal)) {
		report[Capability::WindowCapture].providers.push_back(fmt::format(
			"desktop portal (ScreenCast, version {}), the user chooses the source in a dialog", portal.version));
		if (portal.cursorModes & PortalCapture::Metadata) {
			report[Capability::CursorInfo].providers.push_back(
				"metadata of the portal's screen cast, while the pointer is over the source");
		}
	}

	// The back ends for these come in later phases of the port
	for (Capability capability : { Capability::Overlay, Capability::CursorControl, Capability::WindowControl,
		Capability::GlobalShortcuts, Capability::TouchInput, Capability::SessionServices }) {
		report[capability].note = "no back end yet";
	}

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
