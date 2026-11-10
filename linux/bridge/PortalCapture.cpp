#include "PortalCapture.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <unistd.h>
#include <fmt/format.h>
#include <systemd/sd-bus.h>
#include "Logger.h"
#include "PortalRequest.h"

namespace Magpie {

using PortalRequest::PORTAL_NAME;
using PortalRequest::PORTAL_PATH;
using PortalRequest::ReadStringVariant;
static constexpr const char* SCREENCAST_INTERFACE = "org.freedesktop.portal.ScreenCast";

// Restore tokens exist from this version of the interface on
static constexpr uint32_t MIN_VERSION_FOR_RESTORE = 4;

namespace {

struct BusDeleter {
	void operator()(sd_bus* bus) const noexcept {
		sd_bus_flush_close_unref(bus);
	}
};
using BusPtr = std::unique_ptr<sd_bus, BusDeleter>;

// What the portal answers to a request. Which members are filled depends on
// the request.
struct Response {
	std::string sessionHandle;
	std::string restoreToken;
	bool hasStream = false;
	uint32_t nodeId = 0;
	uint32_t sourceType = 0;
	Size size;
};

// The read position is at the variant of "streams": a(ua{sv}), one entry per
// stream with its PipeWire node and properties. One source is asked for, so
// the first stream is the one.
void ReadStreams(sd_bus_message* message, Response& response) noexcept {
	if (sd_bus_message_enter_container(message, 'v', "a(ua{sv})") <= 0) {
		sd_bus_message_skip(message, "v");
		return;
	}

	sd_bus_message_enter_container(message, 'a', "(ua{sv})");
	while (sd_bus_message_enter_container(message, 'r', "ua{sv}") > 0) {
		uint32_t nodeId = 0;
		sd_bus_message_read(message, "u", &nodeId);

		if (response.hasStream) {
			sd_bus_message_skip(message, "a{sv}");
		} else {
			response.hasStream = true;
			response.nodeId = nodeId;

			sd_bus_message_enter_container(message, 'a', "{sv}");
			while (sd_bus_message_enter_container(message, 'e', "sv") > 0) {
				const char* key = nullptr;
				sd_bus_message_read(message, "s", &key);

				char type = 0;
				const char* contents = nullptr;
				sd_bus_message_peek_type(message, &type, &contents);
				if (key && contents && std::strcmp(key, "size") == 0 && std::strcmp(contents, "(ii)") == 0) {
					sd_bus_message_read(message, "v", "(ii)", &response.size.width, &response.size.height);
				} else if (key && contents && std::strcmp(key, "source_type") == 0 && std::strcmp(contents, "u") == 0) {
					sd_bus_message_read(message, "v", "u", &response.sourceType);
				} else {
					sd_bus_message_skip(message, "v");
				}
				sd_bus_message_exit_container(message);
			}
			sd_bus_message_exit_container(message);
		}

		sd_bus_message_exit_container(message);
	}
	sd_bus_message_exit_container(message);
	sd_bus_message_exit_container(message);
}

// Calls a method of the screen cast interface that answers through a request
// object, and waits for the answer; with the Start method that is until the
// user has closed the dialog. `appendArguments` adds the method's arguments,
// with the given token as "handle_token" of its options.
bool CallRequest(
	sd_bus* bus,
	const char* method,
	const std::function<int(sd_bus_message*, const char*)>& appendArguments,
	Response& response
) noexcept {
	const PortalRequest::Outcome outcome = PortalRequest::Call(bus, SCREENCAST_INTERFACE, method, appendArguments,
		[&](const char* key, sd_bus_message* message) {
			if (std::strcmp(key, "session_handle") == 0) {
				ReadStringVariant(message, response.sessionHandle);
			} else if (std::strcmp(key, "restore_token") == 0) {
				ReadStringVariant(message, response.restoreToken);
			} else if (std::strcmp(key, "streams") == 0) {
				ReadStreams(message, response);
			} else {
				sd_bus_message_skip(message, "v");
			}
		});

	if (outcome == PortalRequest::Outcome::Cancelled) {
		Logger::Get().Error("The screen cast was cancelled in the portal's dialog");
	}
	return outcome == PortalRequest::Outcome::Success;
}

bool ReadInfo(sd_bus* bus, PortalCapture::Info& info) noexcept {
	const auto read = [&](const char* property, uint32_t& value) {
		sd_bus_error error = SD_BUS_ERROR_NULL;
		const int result = sd_bus_get_property_trivial(bus, PORTAL_NAME, PORTAL_PATH,
			SCREENCAST_INTERFACE, property, &error, 'u', &value);
		sd_bus_error_free(&error);
		return result >= 0;
	};
	return read("version", info.version) &&
		read("AvailableSourceTypes", info.sourceTypes) &&
		read("AvailableCursorModes", info.cursorModes);
}

class PortalSession final : public CaptureSession {
public:
	PortalSession(BusPtr bus, std::string handle, int pipeWireFd, uint32_t nodeId) noexcept :
		_bus(std::move(bus)), _handle(std::move(handle)), _pipeWireFd(pipeWireFd), _nodeId(nodeId) {
		// The portal closes the session when the source is gone or the user
		// stops the screen cast from the desktop
		sd_bus_match_signal(_bus.get(), &_closedSlot, nullptr, _handle.c_str(),
			"org.freedesktop.portal.Session", "Closed", OnClosed, this);
	}

	~PortalSession() override {
		sd_bus_slot_unref(_closedSlot);
		if (!_isClosed) {
			sd_bus_call_method(_bus.get(), PORTAL_NAME, _handle.c_str(),
				"org.freedesktop.portal.Session", "Close", nullptr, nullptr, "");
		}
		close(_pipeWireFd);
	}

	int PipeWireFd() const noexcept override {
		return _pipeWireFd;
	}

	uint32_t NodeId() const noexcept override {
		return _nodeId;
	}

	int EventFd() const noexcept override {
		return sd_bus_get_fd(_bus.get());
	}

	bool Dispatch() noexcept override {
		int result;
		while ((result = sd_bus_process(_bus.get(), nullptr)) > 0) {
		}
		if (result < 0) {
			Logger::Get().Error(fmt::format("The D-Bus connection failed: {}", std::strerror(-result)));
			_isClosed = true;
		}
		return !_isClosed;
	}

private:
	static int OnClosed(sd_bus_message*, void* userData, sd_bus_error*) noexcept {
		Logger::Get().Info("The portal has closed the screen cast");
		((PortalSession*)userData)->_isClosed = true;
		return 0;
	}

	BusPtr _bus;
	std::string _handle;
	sd_bus_slot* _closedSlot = nullptr;
	int _pipeWireFd;
	uint32_t _nodeId;
	bool _isClosed = false;
};

}

bool PortalCapture::Probe(Info& info) noexcept {
	sd_bus* rawBus = nullptr;
	if (sd_bus_open_user(&rawBus) < 0) {
		return false;
	}
	const BusPtr bus(rawBus);
	return ReadInfo(bus.get(), info);
}

PortalCapture::Result PortalCapture::Start(const Request& request) noexcept {
	Result result;

	sd_bus* rawBus = nullptr;
	if (const int r = sd_bus_open_user(&rawBus); r < 0) {
		Logger::Get().Error(fmt::format("Connecting to the session bus failed: {}", std::strerror(-r)));
		return result;
	}
	BusPtr bus(rawBus);

	Info info;
	if (!ReadInfo(bus.get(), info)) {
		Logger::Get().Error("No desktop portal with the screen cast interface is running");
		return result;
	}

	const uint32_t sourceTypes = request.sourceTypes & info.sourceTypes;
	if (sourceTypes == 0) {
		Logger::Get().Error("The portal offers none of the requested kinds of source");
		return result;
	}

	// The cursor as metadata, so that it can be drawn scaled. If the portal
	// can only paint it into the picture, that is still better than none.
	const uint32_t cursorMode = (info.cursorModes & Metadata) ? Metadata
		: (info.cursorModes & Embedded) ? Embedded : Hidden;
	result.isCursorEmbedded = cursorMode == Embedded;

	const std::string sessionToken = fmt::format("magpie{}", getpid());
	Response created;
	if (!CallRequest(bus.get(), "CreateSession", [&](sd_bus_message* call, const char* token) {
		return sd_bus_message_append(call, "a{sv}", 2,
			"handle_token", "s", token,
			"session_handle_token", "s", sessionToken.c_str());
	}, created)) {
		return result;
	}
	if (created.sessionHandle.empty()) {
		Logger::Get().Error("The portal returned no session");
		return result;
	}
	const char* session = created.sessionHandle.c_str();

	Response selected;
	if (!CallRequest(bus.get(), "SelectSources", [&](sd_bus_message* call, const char* token) {
		int r = sd_bus_message_append(call, "o", session);
		if (r >= 0) {
			r = sd_bus_message_open_container(call, 'a', "{sv}");
		}
		if (r >= 0) {
			r = sd_bus_message_append(call, "{sv}{sv}{sv}{sv}",
				"handle_token", "s", token,
				"types", "u", sourceTypes,
				"multiple", "b", 0,
				"cursor_mode", "u", cursorMode);
		}
		if (r >= 0 && info.version >= MIN_VERSION_FOR_RESTORE) {
			// 2: the permission lasts until the user revokes it
			r = sd_bus_message_append(call, "{sv}", "persist_mode", "u", 2u);
			if (r >= 0 && !request.restoreToken.empty()) {
				r = sd_bus_message_append(call, "{sv}", "restore_token", "s", request.restoreToken.c_str());
			}
		}
		if (r >= 0) {
			r = sd_bus_message_close_container(call);
		}
		return r;
	}, selected)) {
		return result;
	}

	// Shows the dialog, unless the restore token stands for a source that
	// still exists
	Response started;
	if (!CallRequest(bus.get(), "Start", [&](sd_bus_message* call, const char* token) {
		return sd_bus_message_append(call, "osa{sv}", session, "", 1, "handle_token", "s", token);
	}, started)) {
		return result;
	}
	if (!started.hasStream) {
		Logger::Get().Error("The portal returned no stream");
		return result;
	}

	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message* reply = nullptr;
	int pipeWireFd = -1;
	int r = sd_bus_call_method(bus.get(), PORTAL_NAME, PORTAL_PATH, SCREENCAST_INTERFACE,
		"OpenPipeWireRemote", &error, &reply, "oa{sv}", session, 0);
	if (r >= 0) {
		int fd = -1;
		r = sd_bus_message_read(reply, "h", &fd);
		if (r >= 0) {
			// The descriptor in the message is closed with the message
			pipeWireFd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
		}
	}
	if (r < 0 || pipeWireFd < 0) {
		Logger::Get().Error(fmt::format("The portal gave no PipeWire connection: {}",
			error.message ? error.message : std::strerror(r < 0 ? -r : errno)));
	}
	sd_bus_error_free(&error);
	sd_bus_message_unref(reply);
	if (pipeWireFd < 0) {
		return result;
	}

	Logger::Get().Info(fmt::format("The portal casts a {} as PipeWire node {}",
		started.sourceType == Window ? "window" : started.sourceType == Monitor ? "monitor" : "source",
		started.nodeId));

	result.restoreToken = std::move(started.restoreToken);
	result.sourceType = started.sourceType;
	result.size = started.size;
	result.session = std::make_unique<PortalSession>(
		std::move(bus), std::move(created.sessionHandle), pipeWireFd, started.nodeId);
	return result;
}

}
