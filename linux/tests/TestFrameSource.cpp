// PipeWireFrameSource, fed from a stream that the test produces itself. This
// covers what does not depend on the compositor: frames in shared memory,
// cursor metadata, cropping, a change of size, duplicate frames and the end
// of the stream. DMA-BUF frames from a compositor are covered by TestDmabuf
// (the import) and by running magpie-viewer on a desktop.
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <poll.h>
#include "PipeWireFrameSource.h"
#include "RenderTestHelpers.h"
#include "TestStreamProducer.h"

using namespace Magpie;
using namespace RenderTest;
using namespace std::chrono_literals;

namespace {

// Lets the frame source process its events until `done` holds
bool PumpUntil(PipeWireFrameSource& source, const std::function<bool()>& done, std::chrono::milliseconds timeout = 5s) {
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!done()) {
		if (std::chrono::steady_clock::now() > deadline) {
			return false;
		}

		pollfd fd{ source.EventFd(), POLLIN, 0 };
		poll(&fd, 1, 10);
		if (!source.Dispatch()) {
			return done();
		}
	}
	return true;
}

// A picture in which every pixel differs from its neighbours and from the
// same pixel of a picture with another seed
std::vector<uint8_t> MakePicture(uint32_t width, uint32_t height, uint32_t seed) {
	std::vector<uint8_t> pixels((size_t)width * height * 4);
	for (uint32_t y = 0; y < height; ++y) {
		for (uint32_t x = 0; x < width; ++x) {
			uint8_t* pixel = &pixels[((size_t)y * width + x) * 4];
			pixel[0] = uint8_t(x * 3 + seed * 17);
			pixel[1] = uint8_t(y * 5 + seed * 29);
			pixel[2] = uint8_t(x + y + seed * 41);
			pixel[3] = 255;
		}
	}
	return pixels;
}

// The part `rect` of a picture
std::vector<uint8_t> CutOut(const std::vector<uint8_t>& pixels, uint32_t width, const Rect& rect) {
	std::vector<uint8_t> result;
	for (int32_t y = rect.top; y < rect.bottom; ++y) {
		const uint8_t* row = &pixels[((size_t)y * width + rect.left) * 4];
		result.insert(result.end(), row, row + (size_t)rect.Width() * 4);
	}
	return result;
}

struct Capture {
	TestStreamProducer producer;
	PipeWireFrameSource source;
	uint64_t sentFrames = 0;

	// Starts the producer and connects the frame source to it. With
	// `fakeDmabufModifiers` the producer offers DMA-BUFs that cannot be
	// imported.
	void Start(VulkanDevice& device, uint32_t width, uint32_t height, PipeWireFrameSource::Options options = {},
		std::vector<uint64_t> fakeDmabufModifiers = {}) {
		if (!producer.Start(width, height, std::move(fakeDmabufModifiers))) {
			SKIP("There is no PipeWire daemon to run the test stream on");
		}
		options.nodeId = producer.NodeId();
		REQUIRE(source.Initialize(device, options));
	}

	// Sends a buffer and waits until the frame source has received it
	void Send(const TestStreamProducer::Frame& frame) {
		const PipeWireFrameSource::Statistics before = source.GetStatistics();
		// The producer refuses while the two sides are still negotiating
		REQUIRE(PumpUntil(source, [&] { return producer.Push(frame); }));
		REQUIRE(PumpUntil(source, [&] {
			const PipeWireFrameSource::Statistics& now = source.GetStatistics();
			return now.frames + now.cursorUpdates > before.frames + before.cursorUpdates;
		}));
	}

	void SendPicture(const std::vector<uint8_t>& pixels) {
		Send({ .pixels = pixels });
	}

	std::vector<uint8_t> ReadOutput(VulkanDevice& device) {
		std::vector<uint8_t> pixels;
		REQUIRE(source.Output());
		REQUIRE(device.ReadTexture(*source.Output(), pixels));
		return pixels;
	}
};

}

TEST_CASE("A frame arrives unchanged", "[capture]") {
	ForEachDevice([](VulkanDevice& device, const GraphicsCardInfo&) {
		// A width that makes the rows end off any alignment
		static constexpr uint32_t WIDTH = 317, HEIGHT = 211;
		Capture capture;
		capture.Start(device, WIDTH, HEIGHT);

		CHECK(capture.source.Update() == FrameSourceState::Waiting);
		CHECK_FALSE(capture.source.Output());

		for (uint32_t seed = 1; seed <= 3; ++seed) {
			const std::vector<uint8_t> picture = MakePicture(WIDTH, HEIGHT, seed);
			capture.SendPicture(picture);
			REQUIRE(capture.source.Update() == FrameSourceState::NewFrame);

			const Texture& output = *capture.source.Output();
			CHECK(output.width == WIDTH);
			CHECK(output.height == HEIGHT);
			CHECK(output.format == VK_FORMAT_B8G8R8A8_UNORM);
			CHECK(output.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			CHECK(capture.source.SrcRect() == Rect{ 0, 0, (int32_t)WIDTH, (int32_t)HEIGHT });
			CHECK(capture.ReadOutput(device) == picture);

			// The frame is used once
			CHECK(capture.source.Update() == FrameSourceState::Waiting);
		}

		CHECK(capture.source.GetStatistics().frames == 3);
		CHECK(capture.source.GetStatistics().skippedFrames == 0);
	});
}

TEST_CASE("A DMA-BUF that cannot be imported is replaced by shared memory", "[capture]") {
	ForEachDevice([](VulkanDevice& device, const GraphicsCardInfo& card) {
		const std::vector<uint64_t> modifiers = device.DmabufModifiers(VK_FORMAT_B8G8R8A8_UNORM);
		if (!device.IsDmabufImportSupported() || modifiers.empty()) {
			SKIP("The graphics card cannot import DMA-BUFs");
		}
		if (card.isSoftware) {
			// It maps whatever it is given, so the buffer of this test,
			// which is ordinary memory, works as a DMA-BUF there, and there
			// is nothing to fall back from
			return;
		}

		static constexpr uint32_t WIDTH = 317, HEIGHT = 211;
		Capture capture;
		capture.Start(device, WIDTH, HEIGHT, {}, modifiers);

		// The stream offers DMA-BUFs with modifiers the card knows, so the
		// two agree on them. The buffer is no DMA-BUF, the import fails,
		// and the frame is lost.
		capture.SendPicture(MakePicture(WIDTH, HEIGHT, 1));
		CHECK(capture.producer.DmabufNegotiations() == 1);
		CHECK(capture.source.Update() == FrameSourceState::Waiting);
		CHECK_FALSE(capture.source.Output());

		// The frames that follow are lost as well until the stream has
		// changed to shared memory, some of them on the way; from then on
		// they arrive
		const std::vector<uint8_t> picture = MakePicture(WIDTH, HEIGHT, 2);
		bool arrived = false;
		for (int i = 0; i < 100 && !arrived; ++i) {
			REQUIRE(PumpUntil(capture.source, [&] { return capture.producer.Push({ .pixels = picture }); }));
			const uint64_t before = capture.source.GetStatistics().frames;
			PumpUntil(capture.source, [&] { return capture.source.GetStatistics().frames > before; }, 100ms);
			const FrameSourceState state = capture.source.Update();
			REQUIRE(state != FrameSourceState::Error);
			arrived = state == FrameSourceState::NewFrame;
		}
		REQUIRE(arrived);
		CHECK(capture.ReadOutput(device) == picture);

		const std::vector<uint8_t> next = MakePicture(WIDTH, HEIGHT, 3);
		capture.SendPicture(next);
		REQUIRE(capture.source.Update() == FrameSourceState::NewFrame);
		CHECK(capture.ReadOutput(device) == next);
		CHECK(capture.producer.DmabufNegotiations() == 1);
		CHECK(capture.source.DescribeFormat().find("shared memory") != std::string::npos);
	});
}

TEST_CASE("Of several waiting frames the newest is used", "[capture]") {
	ForEachDevice([](VulkanDevice& device, const GraphicsCardInfo&) {
		static constexpr uint32_t WIDTH = 128, HEIGHT = 96;
		Capture capture;
		capture.Start(device, WIDTH, HEIGHT);

		for (uint32_t seed = 1; seed <= 3; ++seed) {
			capture.SendPicture(MakePicture(WIDTH, HEIGHT, seed));
		}

		REQUIRE(capture.source.Update() == FrameSourceState::NewFrame);
		CHECK(capture.ReadOutput(device) == MakePicture(WIDTH, HEIGHT, 3));
		CHECK(capture.source.GetStatistics().frames == 3);
		CHECK(capture.source.GetStatistics().skippedFrames == 2);
		CHECK(capture.source.Update() == FrameSourceState::Waiting);
	});
}

TEST_CASE("The cursor is read from the stream", "[capture]") {
	ForEachDevice([](VulkanDevice& device, const GraphicsCardInfo&) {
		static constexpr uint32_t WIDTH = 128, HEIGHT = 96;
		Capture capture;
		capture.Start(device, WIDTH, HEIGHT);

		CHECK_FALSE(capture.source.CursorState().isVisible);

		// A picture with the cursor and its image
		TestStreamProducer::Cursor cursor{
			.x = 40, .y = 30, .hotSpotX = 3, .hotSpotY = 5,
			.bitmapWidth = 7, .bitmapHeight = 9
		};
		// Colours multiplied by alpha are never above it
		cursor.bitmap.resize(7 * 9 * 4);
		for (size_t i = 0; i < cursor.bitmap.size(); i += 4) {
			const uint8_t alpha = uint8_t(i * 3 + 40);
			cursor.bitmap[i] = uint8_t(alpha / 2);
			cursor.bitmap[i + 1] = uint8_t(alpha / 3);
			cursor.bitmap[i + 2] = alpha;
			cursor.bitmap[i + 3] = alpha;
		}
		capture.Send({ .pixels = MakePicture(WIDTH, HEIGHT, 1), .cursor = cursor });

		CHECK(capture.source.CursorState().isVisible);
		CHECK(capture.source.CursorState().pos == Point{ 40, 30 });
		CHECK(capture.source.CursorState().imageSerial == 1);
		CHECK(capture.source.CursorImage().size == Size{ 7, 9 });
		CHECK(capture.source.CursorImage().hotSpot == Point{ 3, 5 });
		CHECK(capture.source.CursorImage().pixels == cursor.bitmap);
		CHECK(capture.source.Update() == FrameSourceState::NewFrame);

		// Only the cursor moves: no picture, no new image
		TestStreamProducer::Cursor moved{ .x = 41, .y = 33 };
		capture.Send({ .cursor = moved });
		CHECK(capture.source.CursorState().isVisible);
		CHECK(capture.source.CursorState().pos == Point{ 41, 33 });
		CHECK(capture.source.CursorState().imageSerial == 1);
		CHECK(capture.source.CursorImage().size == Size{ 7, 9 });
		CHECK(capture.source.Update() == FrameSourceState::Waiting);
		CHECK(capture.source.GetStatistics().cursorUpdates == 1);

		// Another image, with blue first: it is stored with red first
		TestStreamProducer::Cursor other{
			.x = 41, .y = 33, .hotSpotX = 0, .hotSpotY = 0,
			.bitmap = { 10, 20, 30, 40, 50, 60, 70, 80 },
			.bitmapWidth = 2, .bitmapHeight = 1,
			.bitmapFormat = SPA_VIDEO_FORMAT_BGRA
		};
		capture.Send({ .cursor = other });
		CHECK(capture.source.CursorState().imageSerial == 2);
		CHECK(capture.source.CursorImage().size == Size{ 2, 1 });
		CHECK(capture.source.CursorImage().pixels == std::vector<uint8_t>{ 30, 20, 10, 40, 70, 60, 50, 80 });

		// The pointer leaves the source
		capture.Send({ .cursor = TestStreamProducer::Cursor{ .isVisible = false } });
		CHECK_FALSE(capture.source.CursorState().isVisible);
		CHECK(capture.source.CursorState().imageSerial == 2);

		// And returns; the image is the one sent before
		capture.Send({ .cursor = moved });
		CHECK(capture.source.CursorState().isVisible);
		CHECK(capture.source.CursorImage().size == Size{ 2, 1 });
	});
}

TEST_CASE("The edges of the source are cropped", "[capture]") {
	ForEachDevice([](VulkanDevice& device, const GraphicsCardInfo&) {
		static constexpr uint32_t WIDTH = 200, HEIGHT = 150;
		Capture capture;
		capture.Start(device, WIDTH, HEIGHT, { .cropping = { 7, 30, 13, 4 } });

		const std::vector<uint8_t> picture = MakePicture(WIDTH, HEIGHT, 5);
		capture.SendPicture(picture);
		REQUIRE(capture.source.Update() == FrameSourceState::NewFrame);

		const Rect expected{ 7, 30, 187, 146 };
		CHECK(capture.source.SrcRect() == expected);
		CHECK(capture.source.Output()->width == 180);
		CHECK(capture.source.Output()->height == 116);
		CHECK(capture.ReadOutput(device) == CutOut(picture, WIDTH, expected));
	});
}

TEST_CASE("Cropping that leaves too little is an error", "[capture]") {
	ForEachDevice([](VulkanDevice& device, const GraphicsCardInfo&) {
		static constexpr uint32_t WIDTH = 128, HEIGHT = 96;
		Capture capture;
		capture.Start(device, WIDTH, HEIGHT, { .cropping = { 40, 0, 40, 0 } });

		capture.SendPicture(MakePicture(WIDTH, HEIGHT, 1));
		CHECK(capture.source.Update() == FrameSourceState::Error);
		CHECK_FALSE(capture.source.Dispatch());
	});
}

TEST_CASE("The source changes its size", "[capture]") {
	ForEachDevice([](VulkanDevice& device, const GraphicsCardInfo&) {
		Capture capture;
		capture.Start(device, 160, 120);

		capture.SendPicture(MakePicture(160, 120, 1));
		REQUIRE(capture.source.Update() == FrameSourceState::NewFrame);
		const std::shared_ptr<Texture> first = capture.source.Output();
		CHECK(first->width == 160);

		for (const auto& [width, height] : { std::pair{ 251u, 187u }, std::pair{ 96u, 64u } }) {
			capture.producer.Resize(width, height);
			const std::vector<uint8_t> picture = MakePicture(width, height, width);
			capture.SendPicture(picture);
			REQUIRE(capture.source.Update() == FrameSourceState::NewFrame);

			CHECK(capture.source.Output() != first);
			CHECK(capture.source.Output()->width == width);
			CHECK(capture.source.Output()->height == height);
			CHECK(capture.source.SrcRect() == Rect{ 0, 0, (int32_t)width, (int32_t)height });
			CHECK(capture.ReadOutput(device) == picture);
		}
	});
}

TEST_CASE("Equal frames are dropped", "[capture]") {
	ForEachDevice([](VulkanDevice& device, const GraphicsCardInfo&) {
		static constexpr uint32_t WIDTH = 128, HEIGHT = 96;

		SECTION("when every frame is compared") {
			Capture capture;
			capture.Start(device, WIDTH, HEIGHT,
				{ .duplicateFrameDetectionMode = DuplicateFrameDetectionMode::Always });

			capture.SendPicture(MakePicture(WIDTH, HEIGHT, 1));
			CHECK(capture.source.Update() == FrameSourceState::NewFrame);
			capture.SendPicture(MakePicture(WIDTH, HEIGHT, 1));
			CHECK(capture.source.Update() == FrameSourceState::Waiting);
			capture.SendPicture(MakePicture(WIDTH, HEIGHT, 2));
			CHECK(capture.source.Update() == FrameSourceState::NewFrame);
			capture.SendPicture(MakePicture(WIDTH, HEIGHT, 2));
			CHECK(capture.source.Update() == FrameSourceState::Waiting);
			CHECK(capture.source.GetStatistics().duplicateFrames == 2);
		}

		SECTION("and not when nothing is compared") {
			Capture capture;
			capture.Start(device, WIDTH, HEIGHT,
				{ .duplicateFrameDetectionMode = DuplicateFrameDetectionMode::Never });

			capture.SendPicture(MakePicture(WIDTH, HEIGHT, 1));
			CHECK(capture.source.Update() == FrameSourceState::NewFrame);
			capture.SendPicture(MakePicture(WIDTH, HEIGHT, 1));
			CHECK(capture.source.Update() == FrameSourceState::NewFrame);
			CHECK(capture.source.GetStatistics().duplicateFrames == 0);
		}

		SECTION("also after the source has changed its size") {
			Capture capture;
			capture.Start(device, WIDTH, HEIGHT,
				{ .duplicateFrameDetectionMode = DuplicateFrameDetectionMode::Always });

			capture.SendPicture(MakePicture(WIDTH, HEIGHT, 1));
			CHECK(capture.source.Update() == FrameSourceState::NewFrame);

			capture.producer.Resize(100, 80);
			capture.SendPicture(MakePicture(100, 80, 1));
			CHECK(capture.source.Update() == FrameSourceState::NewFrame);
			capture.SendPicture(MakePicture(100, 80, 1));
			CHECK(capture.source.Update() == FrameSourceState::Waiting);
		}
	});
}

TEST_CASE("Frames without damage are not new frames", "[capture]") {
	ForEachDevice([](VulkanDevice& device, const GraphicsCardInfo&) {
		static constexpr uint32_t WIDTH = 128, HEIGHT = 96;

		SECTION("once the stream has reported damage") {
			Capture capture;
			capture.Start(device, WIDTH, HEIGHT,
				{ .duplicateFrameDetectionMode = DuplicateFrameDetectionMode::Never });

			capture.SendPicture(MakePicture(WIDTH, HEIGHT, 1));
			CHECK(capture.source.Update() == FrameSourceState::NewFrame);

			// Another picture, but the stream says that nothing has changed
			capture.Send({ .pixels = MakePicture(WIDTH, HEIGHT, 2), .hasDamage = false });
			CHECK(capture.source.Update() == FrameSourceState::Waiting);
			CHECK(capture.ReadOutput(device) == MakePicture(WIDTH, HEIGHT, 1));
			CHECK(capture.source.GetStatistics().undamagedFrames == 1);

			capture.SendPicture(MakePicture(WIDTH, HEIGHT, 3));
			CHECK(capture.source.Update() == FrameSourceState::NewFrame);
			CHECK(capture.ReadOutput(device) == MakePicture(WIDTH, HEIGHT, 3));
		}

		SECTION("but a stream that never reports damage is not trusted") {
			Capture capture;
			capture.Start(device, WIDTH, HEIGHT,
				{ .duplicateFrameDetectionMode = DuplicateFrameDetectionMode::Never });

			for (uint32_t seed = 1; seed <= 3; ++seed) {
				capture.Send({ .pixels = MakePicture(WIDTH, HEIGHT, seed), .hasDamage = false });
				CHECK(capture.source.Update() == FrameSourceState::NewFrame);
				CHECK(capture.ReadOutput(device) == MakePicture(WIDTH, HEIGHT, seed));
			}
			CHECK(capture.source.GetStatistics().undamagedFrames == 0);
		}

		SECTION("and with the damage switched off the comparison shows whether it was right") {
			Capture capture;
			capture.Start(device, WIDTH, HEIGHT, {
				.isDamageUsed = false,
				.duplicateFrameDetectionMode = DuplicateFrameDetectionMode::Always
			});

			capture.SendPicture(MakePicture(WIDTH, HEIGHT, 1));
			CHECK(capture.source.Update() == FrameSourceState::NewFrame);

			// No damage and indeed the same picture
			capture.Send({ .pixels = MakePicture(WIDTH, HEIGHT, 1), .hasDamage = false });
			CHECK(capture.source.Update() == FrameSourceState::Waiting);
			// No damage, but another picture
			capture.Send({ .pixels = MakePicture(WIDTH, HEIGHT, 2), .hasDamage = false });
			CHECK(capture.source.Update() == FrameSourceState::NewFrame);

			const PipeWireFrameSource::Statistics& statistics = capture.source.GetStatistics();
			CHECK(statistics.undamagedFrames == 2);
			CHECK(statistics.duplicateFrames == 1);
			CHECK(statistics.changedUndamagedFrames == 1);
		}
	});
}

TEST_CASE("The end of the stream is reported", "[capture]") {
	ForEachDevice([](VulkanDevice& device, const GraphicsCardInfo&) {
		static constexpr uint32_t WIDTH = 128, HEIGHT = 96;
		Capture capture;
		capture.Start(device, WIDTH, HEIGHT);

		capture.SendPicture(MakePicture(WIDTH, HEIGHT, 1));
		REQUIRE(capture.source.Update() == FrameSourceState::NewFrame);

		capture.producer.Stop();
		CHECK(PumpUntil(capture.source, [&] { return !capture.source.Dispatch(); }));
		CHECK(capture.source.Update() == FrameSourceState::Error);
	});
}

TEST_CASE("A node that does not exist gives no frames and no crash", "[capture]") {
	ForEachDevice([](VulkanDevice& device, const GraphicsCardInfo&) {
		{
			TestStreamProducer probe;
			if (!probe.Start(64, 64)) {
				SKIP("There is no PipeWire daemon");
			}
		}

		PipeWireFrameSource source;
		// Far above the ids a session hands out
		REQUIRE(source.Initialize(device, { .nodeId = 0x7FFFFFF0 }));
		PumpUntil(source, [] { return false; }, 300ms);
		CHECK(source.Update() != FrameSourceState::NewFrame);
		CHECK_FALSE(source.Output());
	});
}
