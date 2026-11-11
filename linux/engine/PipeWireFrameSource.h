// Frames and cursor of a screen cast, received as a PipeWire stream: the
// frame source for the portal and for KWin, which hand out a PipeWire node.
// Its Update follows FrameSourceBase::Update in src/Magpie.Core.
#pragma once
#include <unordered_map>
#include "FrameSource.h"

struct pw_buffer;
struct pw_context;
struct pw_core;
struct pw_loop;
struct pw_stream;
struct spa_pod;

namespace Magpie {

struct Buffer;
class DuplicateFrameDetector;

class PipeWireFrameSource final : public FrameSource {
public:
	struct Options {
		// Connection to the PipeWire daemon that has the node, as the portal
		// hands it out; -1 for the session's own daemon. It is duplicated.
		int pipeWireFd = -1;
		uint32_t nodeId = 0;
		// If not zero, ask for frames of exactly this size. KWin takes the
		// size of a virtual output from what the consumer of its stream asks
		// for, whatever size was requested when the output was created.
		Size fixedSize{};
		// Accept DMA-BUF frames if the graphics card can import them
		bool isDmabufAllowed = true;
		// A frame for which the stream reports no changed region is not
		// taken as a new frame. Without this every frame with a picture is.
		bool isDamageUsed = true;
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
		// Pictures for which the stream reported no changed region
		uint64_t undamagedFrames = 0;
		// Of those, the ones that did differ from the frame before. Counted
		// only when the damage is not used and every frame is compared; it
		// shows whether a producer's damage can be relied on.
		uint64_t changedUndamagedFrames = 0;
		// Pictures that the comparison found equal to the one before
		uint64_t duplicateFrames = 0;
	};

	PipeWireFrameSource() noexcept;
	PipeWireFrameSource(const PipeWireFrameSource&) = delete;
	PipeWireFrameSource(PipeWireFrameSource&&) = delete;

	~PipeWireFrameSource() override;

	bool Initialize(VulkanDevice& device, const Options& options) noexcept;

	int EventFd() const noexcept override;

	bool Dispatch() noexcept override;

	FrameSourceState Update() noexcept override;

	const std::shared_ptr<Texture>& Output() const noexcept override {
		return _output;
	}

	const Rect& SrcRect() const noexcept override {
		return _srcRect;
	}

	const CaptureCursorState& CursorState() const noexcept override {
		return _cursorState;
	}

	const CaptureCursorImage& CursorImage() const noexcept override {
		return _cursorImage;
	}

	std::chrono::steady_clock::time_point LastFrameArrivalTime() const noexcept override {
		return _outputArrivalTime;
	}

	const Statistics& GetStatistics() const noexcept {
		return _statistics;
	}

	std::string DescribeFormat() const noexcept override;

	std::pair<uint32_t, uint32_t> GetStatisticsForDynamicDetection() const noexcept override;

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
	bool _isPendingUndamaged = false;
	// The stream has reported a changed region at least once. Before that an
	// empty damage list may only mean that the producer does not fill it in.
	bool _hasSeenDamage = false;
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
