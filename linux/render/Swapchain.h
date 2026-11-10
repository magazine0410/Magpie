// Presents frames on a window surface. Linux counterpart of the swap chain
// part of src/Magpie.Core/AdaptivePresenter.cpp. The surface comes from the
// caller, so this class does not depend on a window system.
#pragma once
#include <functional>
#include <vector>
#include <vulkan/vulkan.h>
#include "Geometry.h"

namespace Magpie {

struct Texture;
class VulkanDevice;

class Swapchain {
public:
	enum class FrameResult {
		Presented,
		// The surface changed; call Resize and draw again
		NeedsResize,
		Failed
	};

	Swapchain() = default;
	Swapchain(const Swapchain&) = delete;
	Swapchain(Swapchain&&) = delete;
	~Swapchain();

	// Takes ownership of `surface`. The device must have been created for
	// presentation. `presentMode` is used if the surface offers it, and
	// VK_PRESENT_MODE_FIFO_KHR otherwise. The Windows version presents with
	// sync interval 0 on a flip swap chain, which shows the newest frame
	// without tearing; VK_PRESENT_MODE_MAILBOX_KHR is the counterpart.
	bool Initialize(VulkanDevice& device, VkSurfaceKHR surface, Size size, VkPresentModeKHR presentMode) noexcept;

	bool Resize(Size size) noexcept;

	// The swap chain image of one frame
	struct Frame {
		VkImage image = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
	};

	// Acquires an image, has `draw` record the commands that fill it, and
	// presents it. `draw` receives the image in
	// VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL and leaves it in that layout.
	// Returns when the GPU has executed the commands.
	FrameResult PresentFrame(const std::function<void(VkCommandBuffer, const Frame&)>& draw) noexcept;

	Size GetSize() const noexcept {
		return { (int32_t)_extent.width, (int32_t)_extent.height };
	}

	VkFormat Format() const noexcept {
		return _format;
	}

	VkPresentModeKHR PresentMode() const noexcept {
		return _presentMode;
	}

	const std::vector<VkPresentModeKHR>& AvailablePresentModes() const noexcept {
		return _availablePresentModes;
	}

	// Records the commands that put `source` at `destRect` of `target` and
	// black around it, as Renderer::_FrontendRender does. The source is not
	// scaled: destRect has its size. `target` is in
	// VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL and `source` in its general layout.
	static void Compose(
		VkCommandBuffer cmd,
		const Texture& source,
		VkImage target,
		Size targetSize,
		const Rect& destRect
	) noexcept;

private:
	bool _Create(Size size) noexcept;

	void _DestroyImageResources() noexcept;

	VulkanDevice* _device = nullptr;
	VkSurfaceKHR _surface = VK_NULL_HANDLE;
	VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
	std::vector<VkImage> _images;
	// For what is drawn onto the image and not copied to it: the cursor
	std::vector<VkImageView> _imageViews;
	// Signalled when the commands of a frame have been executed, and waited
	// for by the presentation. One per image, because the presentation may
	// hold on to it until the image is acquired again.
	std::vector<VkSemaphore> _renderSemaphores;
	VkFormat _format = VK_FORMAT_UNDEFINED;
	VkColorSpaceKHR _colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	VkExtent2D _extent{};
	VkPresentModeKHR _presentMode = VK_PRESENT_MODE_FIFO_KHR;
	std::vector<VkPresentModeKHR> _availablePresentModes;

	VkCommandBuffer _commandBuffer = VK_NULL_HANDLE;
	VkSemaphore _acquireSemaphore = VK_NULL_HANDLE;
	VkFence _fence = VK_NULL_HANDLE;
};

}
