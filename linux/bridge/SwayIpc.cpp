#include "SwayIpc.h"
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fmt/format.h>
#include <rapidjson/document.h>
#include "Logger.h"

using namespace std::chrono;

namespace Magpie {

static constexpr std::string_view MAGIC = "i3-ipc";
static constexpr size_t HEADER_SIZE = MAGIC.size() + 2 * sizeof(uint32_t);
// Sway answers within a millisecond or two; this only bounds a hang
static constexpr milliseconds REPLY_TIMEOUT{ 3000 };
// A tree of a desktop with hundreds of windows stays far below this
static constexpr uint32_t MAX_PAYLOAD = 64u * 1024 * 1024;

SwayIpc::~SwayIpc() {
	if (_fd >= 0) {
		close(_fd);
	}
}

bool SwayIpc::Connect(const std::string& socketPath) noexcept {
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	if (socketPath.empty() || socketPath.size() >= sizeof(address.sun_path)) {
		return false;
	}
	std::memcpy(address.sun_path, socketPath.data(), socketPath.size());

	const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return false;
	}
	if (connect(fd, (const sockaddr*)&address, sizeof(address)) < 0) {
		close(fd);
		return false;
	}
	_fd = fd;
	return true;
}

std::string SwayIpc::Pack(uint32_t type, std::string_view payload) noexcept {
	const uint32_t length = (uint32_t)payload.size();
	std::string message;
	message.reserve(HEADER_SIZE + payload.size());
	message.append(MAGIC);
	message.append((const char*)&length, sizeof(length));
	message.append((const char*)&type, sizeof(type));
	message.append(payload);
	return message;
}

bool SwayIpc::Unpack(std::string& buffer, uint32_t& type, std::string& payload, bool& isBroken) noexcept {
	isBroken = false;
	if (buffer.size() < HEADER_SIZE) {
		return false;
	}
	uint32_t length;
	std::memcpy(&length, buffer.data() + MAGIC.size(), sizeof(length));
	if (std::string_view(buffer).substr(0, MAGIC.size()) != MAGIC || length > MAX_PAYLOAD) {
		isBroken = true;
		return false;
	}
	if (buffer.size() < HEADER_SIZE + length) {
		return false;
	}
	std::memcpy(&type, buffer.data() + MAGIC.size() + sizeof(length), sizeof(type));
	payload.assign(buffer, HEADER_SIZE, length);
	buffer.erase(0, HEADER_SIZE + length);
	return true;
}

bool SwayIpc::_Fill(steady_clock::time_point deadline) noexcept {
	const auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now()).count();
	pollfd fd{ _fd, POLLIN, 0 };
	const int ready = poll(&fd, 1, (int)std::max<long long>(remaining, 0));
	if (ready == 0) {
		return true;
	}
	if (ready < 0) {
		return errno == EINTR;
	}

	char chunk[16384];
	const ssize_t got = recv(_fd, chunk, sizeof(chunk), MSG_DONTWAIT);
	if (got > 0) {
		_buffer.append(chunk, (size_t)got);
		return true;
	}
	if (got < 0 && (errno == EAGAIN || errno == EINTR)) {
		return true;
	}
	// The other side has closed
	close(_fd);
	_fd = -1;
	return false;
}

bool SwayIpc::Request(Message type, std::string_view payload, std::string& reply) noexcept {
	if (_fd < 0) {
		return false;
	}

	const std::string message = Pack((uint32_t)type, payload);
	size_t sent = 0;
	while (sent < message.size()) {
		const ssize_t written = send(_fd, message.data() + sent, message.size() - sent, MSG_NOSIGNAL);
		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			close(_fd);
			_fd = -1;
			return false;
		}
		sent += (size_t)written;
	}

	const auto deadline = steady_clock::now() + REPLY_TIMEOUT;
	while (true) {
		uint32_t gotType;
		std::string gotPayload;
		bool isBroken;
		while (Unpack(_buffer, gotType, gotPayload, isBroken)) {
			if (gotType & EVENT_BIT) {
				_events.push_back({ gotType, std::move(gotPayload) });
			} else if (gotType == (uint32_t)type) {
				reply = std::move(gotPayload);
				return true;
			}
		}
		if (isBroken) {
			Logger::Get().Error("Sway sent something that is not a message of its protocol");
			close(_fd);
			_fd = -1;
			return false;
		}
		if (steady_clock::now() >= deadline) {
			Logger::Get().Error("Sway did not answer in time");
			return false;
		}
		if (!_Fill(deadline)) {
			return false;
		}
	}
}

bool SwayIpc::Command(std::string_view commands, bool quiet) noexcept {
	std::string reply;
	if (!Request(Message::RunCommand, commands, reply)) {
		return false;
	}

	// An array with one object per command: { "success": true } or
	// { "success": false, "error": "..." }
	rapidjson::Document document;
	document.Parse(reply.data(), reply.size());
	if (document.HasParseError() || !document.IsArray()) {
		return false;
	}
	bool succeeded = true;
	for (const auto& result : document.GetArray()) {
		if (result.IsObject() && result.HasMember("success") && result["success"].IsBool() &&
			!result["success"].GetBool()) {
			succeeded = false;
			if (!quiet) {
				const char* error = result.HasMember("error") && result["error"].IsString() ?
					result["error"].GetString() : "no reason given";
				Logger::Get().Warn(fmt::format("Sway refused \"{}\": {}", commands, error));
			}
		}
	}
	return succeeded;
}

bool SwayIpc::Subscribe(std::string_view kindsAsJsonArray) noexcept {
	std::string reply;
	return Request(Message::Subscribe, kindsAsJsonArray, reply) && reply.find("true") != std::string::npos;
}

bool SwayIpc::ReadEvents() noexcept {
	if (_fd < 0) {
		return false;
	}

	// Until nothing more is waiting
	while (true) {
		pollfd fd{ _fd, POLLIN, 0 };
		if (poll(&fd, 1, 0) <= 0) {
			break;
		}
		if (!_Fill(steady_clock::now())) {
			return false;
		}
	}

	uint32_t type;
	std::string payload;
	bool isBroken;
	while (Unpack(_buffer, type, payload, isBroken)) {
		if (type & EVENT_BIT) {
			_events.push_back({ type, std::move(payload) });
		}
	}
	return !isBroken;
}

std::vector<SwayIpc::Event> SwayIpc::TakeEvents() noexcept {
	return std::exchange(_events, {});
}

}
