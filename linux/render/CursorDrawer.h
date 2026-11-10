// Draws the cursor over the scaled picture. Linux counterpart of
// src/Magpie.Core/CursorDrawer.h.
//
// The Windows class reads the cursor from the system and knows three kinds:
// colour, monochrome and masked colour, the last two with pixels that invert
// the screen. A screen cast delivers every cursor as a colour image with
// alpha, so only that kind exists here. Hiding the cursor after a time
// without movement is not ported yet.
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <vulkan/vulkan.h>
#include "Geometry.h"
#include "ScalingOptions.h"

namespace Magpie {

struct Texture;
class VulkanDevice;

class CursorDrawer {
public:
	// The image the cursor is drawn on
	struct Target {
		VkImage image = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		Size size;
		// The layout the image is in, and is left in
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	};

	CursorDrawer() = default;
	CursorDrawer(const CursorDrawer&) = delete;
	CursorDrawer(CursorDrawer&&) = delete;

	// No GPU work that uses the drawer may be pending
	~CursorDrawer();

	// `targetFormat` is the format of the images that Draw is given
	bool Initialize(VulkanDevice& device, VkFormat targetFormat) noexcept;

	// Sets the cursor image: four bytes per pixel in the order red, green,
	// blue, alpha, with the colours multiplied by alpha, without padding. An
	// empty image means that there is nothing to draw. No GPU work that uses
	// the drawer may be pending.
	bool SetImage(std::span<const uint8_t> pixels, Size size, Point hotSpot) noexcept;

	// Records the commands that draw the cursor with its hot spot at
	// `cursorPos`. `destRect` is where the scaled picture is; the cursor is
	// cut off at its edges, as on Windows. Both are in pixels of the target.
	// `srcSize` is the size of the source that was scaled to `destRect`. Of
	// `options` the cursor scale factor and interpolation mode are used.
	void Draw(
		VkCommandBuffer cmd,
		const Target& target,
		Point cursorPos,
		Size srcSize,
		const Rect& destRect,
		const ScalingOptions& options
	) const noexcept;

	// The factor by which the cursor image is enlarged: the option, or if
	// that is zero the factor by which the source was scaled
	static float CalcScaleFactor(float optionValue, Size srcSize, const Rect& destRect) noexcept;

	// The pixels that the enlarged image covers
	static Rect CalcCursorRect(Point cursorPos, Size imageSize, Point hotSpot, float scaleFactor) noexcept;

private:
	VulkanDevice* _device = nullptr;

	VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
	VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
	VkPipeline _pipeline = VK_NULL_HANDLE;
	VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
	// The cursor texture with nearest-neighbour and with bilinear sampling
	VkDescriptorSet _descriptorSets[2]{};

	// Colour cursor as on Windows: the colours are multiplied by alpha and
	// alpha is inverted, so that blending is
	// result = screen * texture.a + texture.rgb
	std::shared_ptr<Texture> _texture;
	Size _imageSize;
	Point _hotSpot;
};

}
