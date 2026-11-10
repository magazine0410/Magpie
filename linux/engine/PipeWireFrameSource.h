// Frames and cursor of a screen cast, received as a PipeWire stream. This is
// the Linux frame source for every strategy that captures: the portal, KWin
// and the other compositors all hand out a PipeWire node. It takes the place
// of the four frame sources in src/Magpie.Core and of FrameSourceBase, whose
// Update it follows.
//
// Everything runs on the thread that calls Dispatch and Update. That thread
// waits on EventFd.
#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "Geometry.h"
#include "ScalingOptions.h"

struct pw_buffer;
struct pw_context;
struct pw_core;
struct pw_loop;
struct pw_stream;
struct spa_pod;

namespace Magpie {

struct Buffer;
struct Texture;
class DuplicateFrameDetector;
class VulkanDevice;

enum class FrameSourceState {
	NewFrame,
	Waiting,
	Error
};

// What the stream says about the cursor. The position is that of the hot
// spot, in pixels of the stream's picture.
struct CaptureCursorState {
	bool isVisible = false;
	Point pos;
	// Changes whenever a new image arrives
	uint32_t imageSerial = 0;
};

struct CaptureCursorImage {
	// Four bytes per pixel in the order red, green, blue, alpha, with the
	// colours multiplied by alpha, without padding between the rows
	std::vector<uint8_t> pixels;
	Size size;
	Point hotSpot;
};

class PipeWireFrameSource {
public:
	struct Options {
		// Connection to the PipeWire daemon that has the node, as the portal
		// hands it out; -1 for the session's own daemon. It is duplicated.
		int pipeWireFd = -1;
		uint32_t nodeId = 0;
		// Accept DMA-BUF frames if the graphics card can import them
		bool isDmabufAllowed = true;
		// Removed from the edges of the stream's picture
		Cropping cropping{};
		DuplicateFrameDetectionMode duplicateFrameDetectionMode = DuplicateFrameDetectionMode::Dynamic;
		bool isStatisticsForDynamicDetectionEnabled = false;
	};

	struct Statistics {
		// Buffers with a picture, and of those the ones that were replaced by
		// a newer one before they were used
		uint64_t frames = 0;
		uint64_t skippedFrames = 0;
		// Buffers that only carried the cursor
		uint64_t cursorUpdates = 0;
		// Pictures that had no damage, or equalled the previous one
		uint64_t undamagedFrames = 0;
		uint64_t duplicateFrames = 0;
	};

	PipeWireFrameSource() noexcept;
	PipeWireFrameSource(const PipeWireFrameSource&) = delete;
	PipeWireFrameSource(PipeWireFrameSource&&) = delete;

	// No GPU work that uses Output may be pending
	~PipeWireFrameSource();

	bool Initialize(VulkanDevice& device, const Options& options) noexcept;

	// Readable when Dispatch has something to do
	int EventFd() const noexcept;

	// Processes what the stream has sent: frames, cursor updates, changes of
	// format. Does no GPU work. Returns false when the stream has ended or
	// failed.
	bool Dispatch() noexcept;

	// Makes the newest frame that Dispatch has received the output. NewFrame:
	// Output has new content. Waiting: there is no frame, or it equals the
	// previous one. The call submits GPU work and waits for it.
	FrameSourceState Update() noexcept;

	// The current frame, in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; null
	// before the first NewFrame. When the source changes its size or format,
	// this becomes another texture.
	const std::shared_ptr<Texture>& Output() const noexcept {
		return _output;
	}

	// The part of the stream's picture that Output holds, in stream pixels
	const Rect& SrcRect() const noexcept {
		return _srcRect;
	}

	const CaptureCursorState& CursorState() const noexcept {
		return _cursorState;
	}

	const CaptureCursorImage& CursorImage() const noexcept {
		return _cursorImage;
	}

	// When the frame that the last Update took had arrived
	std::chrono::steady_clock::time_point LastFrameArrivalTime() const noexcept {
		return _outputArrivalTime;
	}

	const Statistics& GetStatistics() const noexcept {
		return _statistics;
	}

	// For example "BGRx 640x480, DMA-BUF with modifier 0x300000000606014"
	std::string DescribeFormat() const noexcept;

	// (frames wrongly taken as new, frames not compared), see DuplicateFrameDetector
	std::pair<uint32_t, uint32_t> GetStatisticsForDynamicDetection() const noexcept;

private:
	// The negotiated format, and PipeWire's listener structures together with
	// the functions they call. Defined in the source file to keep the
	// PipeWire headers out of this one.
	struct StreamFormat;
	struct Listeners;

	void _ReadCursor(pw_buffer* buffer) noexcept;

	bool _PrepareOutput() noexcept;

	bool _CopyFrame(pw_buffer* buffer) noexcept;

	VulkanDevice* _device = nullptr;
	Options _options;

	pw_loop* _loop = nullptr;
	pw_context* _context = nullptr;
	pw_core* _core = nullptr;
	pw_stream* _stream = nullptr;
	std::unique_ptr<Listeners> _listeners;
	bool _isLoopEntered = false;
	bool _isPipeWireInitialized = false;

	std::unique_ptr<StreamFormat> _format;
	// The newest buffer with a picture that has not been used yet
	pw_buffer* _pendingBuffer = nullptr;
	std::chrono::steady_clock::time_point _pendingArrivalTime;
	bool _failed = false;

	// One texture per DMA-BUF of the stream. PipeWire reuses a small set of
	// buffers, so each is imported once.
	std::unordered_map<pw_buffer*, std::shared_ptr<Texture>> _importedBuffers;
	// For frames in shared memory
	std::unique_ptr<Buffer> _stagingBuffer;

	std::shared_ptr<Texture> _output;
	Rect _srcRect;
	std::chrono::steady_clock::time_point _outputArrivalTime;
	std::unique_ptr<DuplicateFrameDetector> _duplicateFrameDetector;

	CaptureCursorState _cursorState;
	CaptureCursorImage _cursorImage;
	Statistics _statistics;
};

}
