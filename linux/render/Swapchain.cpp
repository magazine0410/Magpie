#include "Swapchain.h"
#include <algorithm>
#include <fmt/format.h>
#include "Logger.h"
#include "VulkanDevice.h"

namespace Magpie {

static bool Check(VkResult result, const char* what, const std::source_location& location = std::source_location::current()) noexcept {
	if (result == VK_SUCCESS) {
		return true;
	}

	Logger::Get().Error(fmt::format("{} failed with VkResult {}", what, (int)result), location);
	return false;
}

Swapchain::~Swapchain() {
	if (!_device) {
		return;
	}

	const VkDevice device = _device->Device();
	vkDeviceWaitIdle(device);

	if (_fence) {
		vkDestroyFence(device, _fence, nullptr);
	}
	if (_renderSemaphore) {
		vkDestroySemaphore(device, _renderSemaphore, nullptr);
	}
	if (_acquireSemaphore) {
		vkDestroySemaphore(device, _acquireSemaphore, nullptr);
	}
	if (_commandBuffer) {
		vkFreeCommandBuffers(device, _device->CommandPool(), 1, &_commandBuffer);
	}
	if (_swapchain) {
		vkDestroySwapchainKHR(device, _swapchain, nullptr);
	}
	if (_surface) {
		vkDestroySurfaceKHR(_device->Instance(), _surface, nullptr);
	}
}

bool Swapchain::Initialize(
	VulkanDevice& device,
	VkSurfaceKHR surface,
	Size size,
	VkPresentModeKHR presentMode
) noexcept {
	_device = &device;
	_surface = surface;
	const VkPhysicalDevice physicalDevice = device.PhysicalDevice();

	VkBool32 isSupported = VK_FALSE;
	if (!Check(vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, device.QueueFamily(), surface, &isSupported),
		"vkGetPhysicalDeviceSurfaceSupportKHR") || !isSupported) {
		Logger::Get().Error("The graphics card cannot present on this surface");
		return false;
	}

	// The values are passed on unchanged, as on Windows, so the format must
	// not convert between linear and sRGB
	{
		uint32_t count = 0;
		vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &count, nullptr);
		std::vector<VkSurfaceFormatKHR> formats(count);
		vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &count, formats.data());

		const auto find = [&](VkFormat format) {
			return std::find_if(formats.begin(), formats.end(), [&](const VkSurfaceFormatKHR& candidate) {
				return candidate.format == format && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
			});
		};
		auto it = find(VK_FORMAT_B8G8R8A8_UNORM);
		if (it == formats.end()) {
			it = find(VK_FORMAT_R8G8B8A8_UNORM);
		}
		if (it == formats.end()) {
			Logger::Get().Error("The surface offers no 8-bit UNORM format");
			return false;
		}
		_format = it->format;
		_colorSpace = it->colorSpace;
	}

	{
		uint32_t count = 0;
		vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &count, nullptr);
		_availablePresentModes.resize(count);
		vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &count, _availablePresentModes.data());

		const bool isAvailable = std::find(_availablePresentModes.begin(), _availablePresentModes.end(),
			presentMode) != _availablePresentModes.end();
		// Every surface has FIFO
		_presentMode = isAvailable ? presentMode : VK_PRESENT_MODE_FIFO_KHR;
	}

	const VkDevice vkDevice = device.Device();
	const VkCommandBufferAllocateInfo allocInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = device.CommandPool(),
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1
	};
	const VkSemaphoreCreateInfo semaphoreInfo{ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	const VkFenceCreateInfo fenceInfo{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	if (!Check(vkAllocateCommandBuffers(vkDevice, &allocInfo, &_commandBuffer), "vkAllocateCommandBuffers") ||
		!Check(vkCreateSemaphore(vkDevice, &semaphoreInfo, nullptr, &_acquireSemaphore), "vkCreateSemaphore") ||
		!Check(vkCreateSemaphore(vkDevice, &semaphoreInfo, nullptr, &_renderSemaphore), "vkCreateSemaphore") ||
		!Check(vkCreateFence(vkDevice, &fenceInfo, nullptr, &_fence), "vkCreateFence")) {
		return false;
	}

	return _Create(size);
}

bool Swapchain::Resize(Size size) noexcept {
	vkDeviceWaitIdle(_device->Device());
	return _Create(size);
}

bool Swapchain::_Create(Size size) noexcept {
	const VkDevice device = _device->Device();

	VkSurfaceCapabilitiesKHR caps;
	if (!Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_device->PhysicalDevice(), _surface, &caps),
		"vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
		return false;
	}

	// The image is filled by a blit and a clear
	if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
		Logger::Get().Error("The surface does not allow transfers to its images");
		return false;
	}

	// A Wayland surface has no size of its own and reports 0xFFFFFFFF
	if (caps.currentExtent.width != UINT32_MAX) {
		_extent = caps.currentExtent;
	} else {
		_extent.width = std::clamp((uint32_t)size.width, caps.minImageExtent.width, caps.maxImageExtent.width);
		_extent.height = std::clamp((uint32_t)size.height, caps.minImageExtent.height, caps.maxImageExtent.height);
	}

	uint32_t imageCount = std::max(caps.minImageCount + 1, 3u);
	if (caps.maxImageCount != 0) {
		imageCount = std::min(imageCount, caps.maxImageCount);
	}

	const VkSwapchainKHR oldSwapchain = _swapchain;
	const VkSwapchainCreateInfoKHR swapchainInfo{
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = _surface,
		.minImageCount = imageCount,
		.imageFormat = _format,
		.imageColorSpace = _colorSpace,
		.imageExtent = _extent,
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.preTransform = caps.currentTransform,
		.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
			? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
		.presentMode = _presentMode,
		.clipped = VK_TRUE,
		.oldSwapchain = oldSwapchain
	};
	const bool created = Check(vkCreateSwapchainKHR(device, &swapchainInfo, nullptr, &_swapchain), "vkCreateSwapchainKHR");
	if (oldSwapchain) {
		vkDestroySwapchainKHR(device, oldSwapchain, nullptr);
	}
	if (!created) {
		_swapchain = VK_NULL_HANDLE;
		return false;
	}

	uint32_t count = 0;
	vkGetSwapchainImagesKHR(device, _swapchain, &count, nullptr);
	_images.resize(count);
	return Check(vkGetSwapchainImagesKHR(device, _swapchain, &count, _images.data()), "vkGetSwapchainImagesKHR");
}

Swapchain::FrameResult Swapchain::PresentFrame(const std::function<void(VkCommandBuffer, VkImage)>& draw) noexcept {
	const VkDevice device = _device->Device();

	uint32_t imageIndex = 0;
	VkResult result = vkAcquireNextImageKHR(device, _swapchain, UINT64_MAX, _acquireSemaphore, VK_NULL_HANDLE, &imageIndex);
	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		return FrameResult::NeedsResize;
	}
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		Check(result, "vkAcquireNextImageKHR");
		return FrameResult::Failed;
	}

	const VkImage image = _images[imageIndex];

	const VkCommandBufferBeginInfo beginInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
	};
	if (!Check(vkResetCommandBuffer(_commandBuffer, 0), "vkResetCommandBuffer") ||
		!Check(vkBeginCommandBuffer(_commandBuffer, &beginInfo), "vkBeginCommandBuffer")) {
		return FrameResult::Failed;
	}

	// The old content is not needed
	TransitionImage(_commandBuffer, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	draw(_commandBuffer, image);
	TransitionImage(_commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

	// TransitionImage waits for all earlier commands, so the whole buffer
	// waits for the image
	const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	const VkSubmitInfo submitInfo{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &_acquireSemaphore,
		.pWaitDstStageMask = &waitStage,
		.commandBufferCount = 1,
		.pCommandBuffers = &_commandBuffer,
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &_renderSemaphore
	};
	if (!Check(vkEndCommandBuffer(_commandBuffer), "vkEndCommandBuffer") ||
		!Check(vkResetFences(device, 1, &_fence), "vkResetFences") ||
		!Check(vkQueueSubmit(_device->Queue(), 1, &submitInfo, _fence), "vkQueueSubmit") ||
		!Check(vkWaitForFences(device, 1, &_fence, VK_TRUE, UINT64_MAX), "vkWaitForFences")) {
		return FrameResult::Failed;
	}

	const VkPresentInfoKHR presentInfo{
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &_renderSemaphore,
		.swapchainCount = 1,
		.pSwapchains = &_swapchain,
		.pImageIndices = &imageIndex
	};
	result = vkQueuePresentKHR(_device->Queue(), &presentInfo);
	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		return FrameResult::NeedsResize;
	}
	if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		Check(result, "vkQueuePresentKHR");
		return FrameResult::Failed;
	}

	return FrameResult::Presented;
}

void Swapchain::Compose(
	VkCommandBuffer cmd,
	const Texture& source,
	VkImage target,
	Size targetSize,
	const Rect& destRect
) noexcept {
	static constexpr VkImageSubresourceRange COLOR_RANGE{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	static constexpr VkImageSubresourceLayers COLOR_LAYER{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };

	if (destRect != Rect{ 0, 0, targetSize.width, targetSize.height }) {
		// With black bars the background is filled first
		static constexpr VkClearColorValue BLACK{ .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
		vkCmdClearColorImage(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &BLACK, 1, &COLOR_RANGE);

		// The blit writes to pixels that the clear has written
		const VkImageMemoryBarrier barrier{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = target,
			.subresourceRange = COLOR_RANGE
		};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &barrier);
	}

	// The part of the destination inside the target, and the matching part of
	// the source
	const Rect clipped{
		std::max(destRect.left, 0), std::max(destRect.top, 0),
		std::min(destRect.right, targetSize.width), std::min(destRect.bottom, targetSize.height)
	};
	if (clipped.Width() <= 0 || clipped.Height() <= 0) {
		return;
	}

	// A blit, not a copy: it converts between RGBA and BGRA
	const VkImageBlit region{
		.srcSubresource = COLOR_LAYER,
		.srcOffsets = {
			{ clipped.left - destRect.left, clipped.top - destRect.top, 0 },
			{ clipped.right - destRect.left, clipped.bottom - destRect.top, 1 }
		},
		.dstSubresource = COLOR_LAYER,
		.dstOffsets = { { clipped.left, clipped.top, 0 }, { clipped.right, clipped.bottom, 1 } }
	};
	vkCmdBlitImage(cmd, source.image, source.layout, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &region, VK_FILTER_NEAREST);
}

}
