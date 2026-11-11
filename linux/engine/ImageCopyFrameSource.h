// Frames and cursor of an output of a Wayland compositor, copied with the
// protocol ext-image-copy-capture: the frame source for Sway and for the
// nested session. The program asks for a frame, and the compositor fills the
// program's buffer when the output has changed.
//
// The cursor is not part of the picture. A cursor session of the protocol
// reports its position, and a second capture delivers its image.
//
// The frames are in shared memory, so every one is read back from the
// compositor's graphics card and uploaded again.
#pragma once
#include "FrameSource.h"

namespace Magpie {

struct Buffer;
class DuplicateFrameDetector;

class ImageCopyFrameSource final : public FrameSource {
public:
	struct Options {
		// The name of the compositor's socket; empty for the one of the session
		std::string waylandDisplay;
		std::string outputName;
		// Removed from the edges of the output's picture
		Cropping cropping{};
		DuplicateFrameDetectionMode duplicateFrameDetectionMode = DuplicateFrameDetectionMode::Dynamic;
		bool isStatisticsForDynamicDetectionEnabled = false;
	};

	struct Statistics {
		// Frames the compositor delivered
		uint64_t frames = 0;
		// Frames that the comparison found equal to the one before
		uint64_t duplicateFrames = 0;
		uint64_t cursorImages = 0;
	};

	// How the pixels of a frame in shared memory lie in memory
	struct PixelLayout {
		// Bytes per pixel: 3 or 4
		uint32_t bytesPerPixel = 0;
		// The first byte is red; otherwise blue
		bool isRedFirst = false;
		bool hasAlpha = false;
	};

	// The layout of a format of wl_shm; false for one that is not handled
	static bool GetPixelLayout(uint32_t shmFormat, PixelLayout& layout) noexcept;

	// The format to ask for among those the compositor offers; false if
	// none can be handled. Formats with four bytes per pixel come first,
	// because they need no conversion.
	static bool ChooseFormat(const std::vector<uint32_t>& offered, uint32_t& chosen) noexcept;

	// Copies `rowCount` rows of `width` pixels to four bytes per pixel, in
	// the order the layout has, with full alpha where the source has none
	static void ConvertRows(
		const uint8_t* src, size_t srcStride, const PixelLayout& layout,
		uint8_t* dst, uint32_t width, uint32_t rowCount) noexcept;

	ImageCopyFrameSource() noexcept;
	ImageCopyFrameSource(const ImageCopyFrameSource&) = delete;
	ImageCopyFrameSource(ImageCopyFrameSource&&) = delete;
	~ImageCopyFrameSource() override;

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
	// The Wayland objects and their listeners; defined in the source file to
	// keep the Wayland headers out of this one
	struct State;

	bool _PrepareOutput(Size frameSize, bool isRedFirst) noexcept;

	void _ReadCursorImage() noexcept;

	VulkanDevice* _device = nullptr;
	Options _options;
	std::unique_ptr<State> _state;

	// For the upload of a frame
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
