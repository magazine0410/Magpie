// Where the frames of the source come from. Linux counterpart of
// FrameSourceBase in src/Magpie.Core: one interface, and one implementation
// per way of capturing (PipeWireFrameSource, ImageCopyFrameSource).
//
// Everything runs on the thread that calls Dispatch and Update. That thread
// waits on EventFd.
#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "Geometry.h"
#include "ScalingOptions.h"

namespace Magpie {

struct Texture;
class CaptureSession;
class VulkanDevice;

enum class FrameSourceState {
	NewFrame,
	Waiting,
	Error
};

// What the capture says about the cursor. The position is that of the hot
// spot, in pixels of the captured picture.
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

class FrameSource {
public:
	// What every frame source is told, whatever it captures with
	struct Options {
		// If not zero, ask for frames of exactly this size
		Size fixedSize{};
		// Accept DMA-BUF frames if the graphics card can import them
		bool isDmabufAllowed = true;
		// A frame for which the capture reports no changed region is not
		// taken as a new frame. Without this every frame with a picture is.
		bool isDamageUsed = true;
		// Removed from the edges of the captured picture
		Cropping cropping{};
		DuplicateFrameDetectionMode duplicateFrameDetectionMode = DuplicateFrameDetectionMode::Dynamic;
		bool isStatisticsForDynamicDetectionEnabled = false;
	};

	// No GPU work that uses Output may be pending when it is destroyed
	virtual ~FrameSource() = default;

	// The frame source for a screen cast, started. Null on failure.
	static std::unique_ptr<FrameSource> Create(
		VulkanDevice& device, const CaptureSession& capture, const Options& options) noexcept;

	// Readable when Dispatch has something to do
	virtual int EventFd() const noexcept = 0;

	// Processes what the capture has sent: frames, cursor updates, changes of
	// format. Does no GPU work. Returns false when the capture has ended or
	// failed.
	virtual bool Dispatch() noexcept = 0;

	// Makes the newest frame that Dispatch has received the output. NewFrame:
	// Output has new content. Waiting: there is no frame, or it equals the
	// previous one. The call submits GPU work and waits for it.
	virtual FrameSourceState Update() noexcept = 0;

	// The current frame, in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; null
	// before the first NewFrame. When the source changes its size or format,
	// this becomes another texture.
	virtual const std::shared_ptr<Texture>& Output() const noexcept = 0;

	// The part of the captured picture that Output holds, in its pixels
	virtual const Rect& SrcRect() const noexcept = 0;

	virtual const CaptureCursorState& CursorState() const noexcept = 0;

	virtual const CaptureCursorImage& CursorImage() const noexcept = 0;

	// When the frame that the last Update took had arrived
	virtual std::chrono::steady_clock::time_point LastFrameArrivalTime() const noexcept = 0;

	// For example "BGRx 640x480, DMA-BUF with modifier 0x300000000606014"
	virtual std::string DescribeFormat() const noexcept = 0;

	// (frames wrongly taken as new, frames not compared), see DuplicateFrameDetector
	virtual std::pair<uint32_t, uint32_t> GetStatisticsForDynamicDetection() const noexcept = 0;
};

}
