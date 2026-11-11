// A connection to Sway's control socket, which speaks the IPC protocol of
// i3: a message has a type and a text in JSON, and gets one reply; after a
// subscription, events arrive on the same connection in between.
#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Magpie {

class SwayIpc {
public:
	enum class Message : uint32_t {
		RunCommand = 0,
		Subscribe = 2,
		GetOutputs = 3,
		GetTree = 4,
		GetVersion = 7,
		GetInputs = 100
	};

	// The types of the events that are used; an event has the highest bit set
	static constexpr uint32_t EVENT_BIT = 0x80000000u;
	static constexpr uint32_t EVENT_WORKSPACE = EVENT_BIT | 0;
	static constexpr uint32_t EVENT_OUTPUT = EVENT_BIT | 1;
	static constexpr uint32_t EVENT_WINDOW = EVENT_BIT | 3;
	static constexpr uint32_t EVENT_BINDING = EVENT_BIT | 5;
	static constexpr uint32_t EVENT_SHUTDOWN = EVENT_BIT | 6;

	struct Event {
		uint32_t type = 0;
		std::string payload;
	};

	SwayIpc() = default;
	SwayIpc(const SwayIpc&) = delete;
	SwayIpc(SwayIpc&&) = delete;
	~SwayIpc();

	// `socketPath` is what Sway has in the variable SWAYSOCK
	bool Connect(const std::string& socketPath) noexcept;

	bool IsConnected() const noexcept {
		return _fd >= 0;
	}

	// Readable when ReadEvents has something to do
	int Fd() const noexcept {
		return _fd;
	}

	// Sends a message and waits for its reply. Events that arrive before the
	// reply are kept for TakeEvents. Returns false if the connection is lost
	// or no reply comes in time.
	bool Request(Message type, std::string_view payload, std::string& reply) noexcept;

	// Runs commands as Sway's configuration file has them. Returns false if
	// the connection is lost or Sway reports an error for one of them; the
	// error is logged unless `quiet`.
	bool Command(std::string_view commands, bool quiet = false) noexcept;

	// Events of these kinds ("window", "workspace", "output", "binding",
	// "shutdown") arrive from now on
	bool Subscribe(std::string_view kindsAsJsonArray) noexcept;

	// Takes in what has arrived without waiting. Returns false when the
	// connection is lost.
	bool ReadEvents() noexcept;

	std::vector<Event> TakeEvents() noexcept;

	// Events are waiting for TakeEvents
	bool HasQueuedEvents() const noexcept {
		return !_events.empty();
	}

	// The header and the text of a message, as they go over the socket
	static std::string Pack(uint32_t type, std::string_view payload) noexcept;

	// Takes the first complete message off `buffer`. Returns false if there
	// is none yet; `isBroken` if what is there cannot be a message.
	static bool Unpack(std::string& buffer, uint32_t& type, std::string& payload, bool& isBroken) noexcept;

private:
	// Reads from the socket into _buffer; waits at most until `deadline`
	bool _Fill(std::chrono::steady_clock::time_point deadline) noexcept;

	int _fd = -1;
	std::string _buffer;
	std::vector<Event> _events;
};

}
