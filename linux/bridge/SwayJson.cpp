#include "SwayJson.h"
#include <charconv>
#include <cmath>
#include <sstream>
#include <fmt/format.h>
#include <rapidjson/document.h>

namespace Magpie {

using Value = rapidjson::Value;

static const char* GetString(const Value& object, const char* name) noexcept {
	const auto it = object.FindMember(name);
	return it != object.MemberEnd() && it->value.IsString() ? it->value.GetString() : "";
}

static int64_t GetInt(const Value& object, const char* name, int64_t fallback = 0) noexcept {
	const auto it = object.FindMember(name);
	return it != object.MemberEnd() && it->value.IsInt64() ? it->value.GetInt64() : fallback;
}

static bool GetBool(const Value& object, const char* name) noexcept {
	const auto it = object.FindMember(name);
	return it != object.MemberEnd() && it->value.IsBool() && it->value.GetBool();
}

static const Value* GetObject(const Value& object, const char* name) noexcept {
	const auto it = object.FindMember(name);
	return it != object.MemberEnd() && it->value.IsObject() ? &it->value : nullptr;
}

static const Value* GetArray(const Value& object, const char* name) noexcept {
	const auto it = object.FindMember(name);
	return it != object.MemberEnd() && it->value.IsArray() ? &it->value : nullptr;
}

static Rect GetRect(const Value& object, const char* name) noexcept {
	const Value* rect = GetObject(object, name);
	if (!rect) {
		return {};
	}
	const int32_t x = (int32_t)GetInt(*rect, "x");
	const int32_t y = (int32_t)GetInt(*rect, "y");
	return { x, y, x + (int32_t)GetInt(*rect, "width"), y + (int32_t)GetInt(*rect, "height") };
}

namespace {

struct TreeWalk {
	std::vector<SwayWindow>& windows;
	std::string output;
	std::string workspace;

	void Visit(const Value& node, bool isFloating, int64_t previousSibling, int64_t nextSibling) {
		if (!node.IsObject()) {
			return;
		}

		const std::string_view type = GetString(node, "type");
		if (type == "output") {
			output = GetString(node, "name");
		} else if (type == "workspace") {
			workspace = GetString(node, "name");
		}

		// A window is a container that belongs to a program
		if ((type == "con" || type == "floating_con") && node.HasMember("pid") && node["pid"].IsInt64()) {
			SwayWindow window;
			WindowInfo& info = window.info;
			info.id = std::to_string(GetInt(node, "id"));
			info.title = GetString(node, "name");
			info.pid = (uint32_t)GetInt(node, "pid");
			// A Wayland program has an app ID, an X11 program a window class
			info.appId = GetString(node, "app_id");
			if (const Value* properties = GetObject(node, "window_properties")) {
				info.resourceName = GetString(*properties, "instance");
				if (info.appId.empty()) {
					info.appId = GetString(*properties, "class");
				}
			}

			window.border = GetString(node, "border");
			window.borderWidth = (int32_t)GetInt(node, "current_border_width");
			// Sway reports the rectangle of the container without its title
			// bar, although its commands for size and place count the title
			// bar in; the frame here is what the commands mean. The content
			// is given relative to the reported rectangle.
			const Rect rect = GetRect(node, "rect");
			const Rect content = GetRect(node, "window_rect");
			const Rect titleBar = GetRect(node, "deco_rect");
			window.titleBarHeight = window.border == "normal" ? titleBar.Height() : 0;
			info.frameRect = { rect.left, rect.top - window.titleBarHeight, rect.right, rect.bottom };
			info.clientRect = {
				rect.left + content.left, rect.top + content.top,
				rect.left + content.right, rect.top + content.bottom
			};
			if (info.clientRect.Width() <= 0 || info.clientRect.Height() <= 0) {
				info.clientRect = info.frameRect;
			}

			info.isActive = GetBool(node, "focused");
			info.isFullscreen = GetInt(node, "fullscreen_mode") != 0;
			info.hasNoBorder = window.border == "none" || window.border == "csd";
			// Sway has no minimized windows. What comes closest is a window
			// that was put away in the scratchpad.
			const bool isHidden = output == "__i3";
			info.isMinimized = isHidden;

			window.output = isHidden ? std::string() : output;
			window.workspace = isHidden ? std::string() : workspace;
			window.isFloating = isFloating || type == "floating_con";
			window.previousSibling = window.isFloating ? 0 : previousSibling;
			window.nextSibling = window.isFloating ? 0 : nextSibling;
			info.restoreData = SwayJson::FormatRestore(window);
			windows.push_back(std::move(window));
		}

		if (const Value* children = GetArray(node, "nodes")) {
			const auto& array = children->GetArray();
			for (rapidjson::SizeType i = 0; i < array.Size(); ++i) {
				const int64_t previous = i > 0 && array[i - 1].IsObject() ? GetInt(array[i - 1], "id") : 0;
				const int64_t next = i + 1 < array.Size() && array[i + 1].IsObject() ? GetInt(array[i + 1], "id") : 0;
				Visit(array[i], isFloating, previous, next);
			}
		}
		if (const Value* children = GetArray(node, "floating_nodes")) {
			for (const Value& child : children->GetArray()) {
				Visit(child, true, 0, 0);
			}
		}
	}
};

}

bool SwayJson::ParseTree(std::string_view json, std::vector<SwayWindow>& windows) noexcept {
	windows.clear();
	rapidjson::Document document;
	document.Parse(json.data(), json.size());
	if (document.HasParseError() || !document.IsObject()) {
		return false;
	}
	TreeWalk{ windows, {}, {} }.Visit(document, false, 0, 0);
	return true;
}

bool SwayJson::ParseOutputs(std::string_view json, std::vector<OutputInfo>& outputs) noexcept {
	outputs.clear();
	rapidjson::Document document;
	document.Parse(json.data(), json.size());
	if (document.HasParseError() || !document.IsArray()) {
		return false;
	}

	for (const Value& output : document.GetArray()) {
		if (!output.IsObject() || !GetBool(output, "active")) {
			continue;
		}
		OutputInfo info;
		info.name = GetString(output, "name");
		info.rect = GetRect(output, "rect");
		if (const Value* mode = GetObject(output, "current_mode")) {
			info.refreshRate = (int32_t)GetInt(*mode, "refresh");
		}
		if (info.refreshRate <= 0) {
			// An output without a screen that was given no mode
			info.refreshRate = 60'000;
		}
		outputs.push_back(std::move(info));
	}
	return true;
}

bool SwayJson::ParsePointerDevices(std::string_view json, std::vector<PointerDevice>& devices) noexcept {
	devices.clear();
	rapidjson::Document document;
	document.Parse(json.data(), json.size());
	if (document.HasParseError() || !document.IsArray()) {
		return false;
	}

	for (const Value& input : document.GetArray()) {
		if (!input.IsObject()) {
			continue;
		}
		const std::string_view type = GetString(input, "type");
		const Value* libinput = GetObject(input, "libinput");
		if ((type != "pointer" && type != "touchpad") || !libinput) {
			continue;
		}
		// Only devices with a speed setting have this value
		const auto speed = libinput->FindMember("accel_speed");
		const std::string identifier = GetString(input, "identifier");
		if (speed == libinput->MemberEnd() || !speed->value.IsNumber() || identifier.empty() ||
			identifier.find(' ') != std::string::npos) {
			continue;
		}
		devices.push_back({ identifier, speed->value.GetDouble() });
	}
	return true;
}

std::string SwayJson::FormatRestore(const SwayWindow& window) noexcept {
	// The workspace comes last, because its name may contain spaces
	return fmt::format("sway {} {} {} {} {} {}", window.isFloating ? "floating" : "tiled",
		window.border.empty() ? "none" : window.border, window.borderWidth,
		window.previousSibling, window.nextSibling, window.workspace);
}

bool SwayJson::ParseRestore(std::string_view text, SwayRestore& restore) noexcept {
	std::istringstream stream{ std::string(text) };
	std::string tag, kind;
	if (!(stream >> tag >> kind >> restore.border >> restore.borderWidth >>
		restore.previousSibling >> restore.nextSibling) || tag != "sway" || (kind != "floating" && kind != "tiled")) {
		return false;
	}
	restore.isFloating = kind == "floating";
	std::getline(stream >> std::ws, restore.workspace);
	return true;
}

std::string SwayJson::ParseBindingEvent(std::string_view json, std::string_view prefix) noexcept {
	rapidjson::Document document;
	document.Parse(json.data(), json.size());
	if (document.HasParseError() || !document.IsObject()) {
		return {};
	}
	const Value* binding = GetObject(document, "binding");
	if (!binding) {
		return {};
	}
	const std::string_view command = GetString(*binding, "command");
	return command.starts_with(prefix) ? std::string(command.substr(prefix.size())) : std::string();
}

bool SwayJson::IsReloadEvent(std::string_view json) noexcept {
	rapidjson::Document document;
	document.Parse(json.data(), json.size());
	return !document.HasParseError() && document.IsObject() &&
		std::string_view(GetString(document, "change")) == "reload";
}

}
