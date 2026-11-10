#include "PortalRequest.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <fmt/format.h>
#include "Logger.h"

namespace Magpie::PortalRequest {

namespace {

struct Pending {
	const std::function<void(const char*, sd_bus_message*)>* readResult = nullptr;
	bool hasArrived = false;
	// 0: success, 1: the user cancelled, 2: ended in another way
	uint32_t code = 2;
};

// org.freedesktop.portal.Request.Response: (u response, a{sv} results)
int OnResponse(sd_bus_message* message, void* userData, sd_bus_error*) noexcept {
	Pending& pending = *(Pending*)userData;
	pending.hasArrived = true;

	if (sd_bus_message_read(message, "u", &pending.code) < 0) {
		pending.code = 2;
		return 0;
	}

	if (sd_bus_message_enter_container(message, 'a', "{sv}") <= 0) {
		return 0;
	}
	while (sd_bus_message_enter_container(message, 'e', "sv") > 0) {
		const char* key = nullptr;
		sd_bus_message_read(message, "s", &key);
		if (key) {
			(*pending.readResult)(key, message);
		} else {
			sd_bus_message_skip(message, "v");
		}
		sd_bus_message_exit_container(message);
	}
	sd_bus_message_exit_container(message);
	return 0;
}

}

void ReadStringVariant(sd_bus_message* message, std::string& value) noexcept {
	char type = 0;
	const char* contents = nullptr;
	if (sd_bus_message_peek_type(message, &type, &contents) > 0 && type == 'v' && contents &&
		(std::strcmp(contents, "s") == 0 || std::strcmp(contents, "o") == 0)) {
		const char* text = nullptr;
		if (sd_bus_message_read(message, "v", contents, &text) >= 0 && text) {
			value = text;
		}
	} else {
		sd_bus_message_skip(message, "v");
	}
}

Outcome Call(
	sd_bus* bus,
	const char* interface,
	const char* method,
	const std::function<int(sd_bus_message*, const char*)>& appendArguments,
	const std::function<void(const char*, sd_bus_message*)>& readResult
) noexcept {
	// The portal creates the request object at a path made of the caller's
	// bus name and a token of the caller's choice. Subscribing before the
	// call makes sure the answer is not missed.
	const char* uniqueName = nullptr;
	if (sd_bus_get_unique_name(bus, &uniqueName) < 0 || !uniqueName || uniqueName[0] != ':') {
		Logger::Get().Error("The D-Bus connection has no name");
		return Outcome::Failed;
	}
	std::string sender = uniqueName + 1;
	std::replace(sender.begin(), sender.end(), '.', '_');

	static uint32_t tokenCount = 0;
	const std::string token = fmt::format("magpie{}_{}", getpid(), ++tokenCount);
	const std::string requestPath = fmt::format("/org/freedesktop/portal/desktop/request/{}/{}", sender, token);

	Pending pending{ .readResult = &readResult };
	sd_bus_slot* slot = nullptr;
	sd_bus_message* call = nullptr;
	sd_bus_message* reply = nullptr;
	sd_bus_error error = SD_BUS_ERROR_NULL;

	// The path names this request alone, so the sender needs no check
	int result = sd_bus_match_signal(bus, &slot, nullptr, requestPath.c_str(),
		"org.freedesktop.portal.Request", "Response", OnResponse, &pending);
	if (result >= 0) {
		result = sd_bus_message_new_method_call(bus, &call, PORTAL_NAME, PORTAL_PATH, interface, method);
	}
	if (result >= 0) {
		result = appendArguments(call, token.c_str());
	}
	if (result >= 0) {
		result = sd_bus_call(bus, call, 0, &error, &reply);
	}

	if (result < 0) {
		Logger::Get().Error(fmt::format("The portal call {} failed: {}", method,
			error.message ? error.message : std::strerror(-result)));
	} else {
		while (!pending.hasArrived) {
			result = sd_bus_process(bus, nullptr);
			if (result == 0) {
				result = sd_bus_wait(bus, UINT64_MAX);
			}
			if (result < 0 && result != -EINTR) {
				Logger::Get().Error(fmt::format("Waiting for the portal failed: {}", std::strerror(-result)));
				break;
			}
		}
	}

	sd_bus_error_free(&error);
	sd_bus_message_unref(reply);
	sd_bus_message_unref(call);
	sd_bus_slot_unref(slot);

	if (!pending.hasArrived) {
		return Outcome::Failed;
	}
	if (pending.code == 1) {
		return Outcome::Cancelled;
	}
	if (pending.code != 0) {
		Logger::Get().Error(fmt::format("The portal ended the request {} without a result", method));
		return Outcome::Failed;
	}
	return Outcome::Success;
}

}
