// Vulkan instance, device and the resources everything that renders shares.
// Linux counterpart of src/Magpie.Core/DeviceResources.h.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>
#include "EffectDesc.h"
#include "ScalingOptions.h"

namespace Magpie {

struct GraphicsCardInfo {
	uint32_t vendorId = 0;
	uint32_t deviceId = 0;
	std::string name;
	// A CPU implementation such as llvmpipe; the counterpart of WARP on Windows
	bool isSoftware = false;
	// Meets the requirements listed in VulkanDevice.cpp
	bool isUsable = false;
};

// Chooses a graphics card with the rules of DeviceResources::_ObtainAdapterAndDevice:
// the card at id.idx if its vendor and device IDs match, otherwise any card
// with these IDs, otherwise the first usable card that is not a software
// implementation, otherwise a software implementation. Returns the position in
// `cards`, or -1 if no card is usable.
int SelectGraphicsCard(std::span<const GraphicsCardInfo> cards, const GraphicsCardId& id) noexcept;

// Size of one pixel of an uncompressed format that the renderer uses, or 0.
uint32_t BytesPerPixel(VkFormat format) noexcept;

// Changes the layout of a whole colour image. Conservative about what it
// waits for, so only for uses outside the per-pass path.
void TransitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) noexcept;

struct Texture {
	Texture() = default;
	Texture(const Texture&) = delete;
	Texture& operator=(const Texture&) = delete;
	~Texture();

	VkDevice device = VK_NULL_HANDLE;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	uint32_t width = 0;
	uint32_t height = 0;
	// The layout the image has between command buffers. Code that changes it
	// inside a command buffer changes it back before the buffer ends.
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// A buffer in host-visible memory that stays mapped
struct Buffer {
	Buffer() = default;
	Buffer(const Buffer&) = delete;
	Buffer& operator=(const Buffer&) = delete;
	~Buffer();

	VkDevice device = VK_NULL_HANDLE;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void* data = nullptr;
	VkDeviceSize size = 0;
};

class VulkanDevice {
public:
	struct Options {
		GraphicsCardId graphicsCardId;
		// Load the validation layers and count their messages. Fails if the
		// layers are not installed.
		bool validation = false;
		// Enable what a Wayland swap chain needs
		bool presentation = false;
	};

	// Usage of the textures an effect reads and writes
	static constexpr VkImageUsageFlags EFFECT_TEXTURE_USAGE =
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	VulkanDevice() = default;
	VulkanDevice(const VulkanDevice&) = delete;
	VulkanDevice& operator=(const VulkanDevice&) = delete;
	~VulkanDevice();

	bool Initialize(const Options& options) noexcept;

	// All cards found, in the order GraphicsCardId::idx refers to
	const std::vector<GraphicsCardInfo>& GraphicsCards() const noexcept {
		return _graphicsCards;
	}

	// Position of the card in use in GraphicsCards()
	int GraphicsCardIdx() const noexcept {
		return _graphicsCardIdx;
	}

	VkInstance Instance() const noexcept { return _instance; }
	VkPhysicalDevice PhysicalDevice() const noexcept { return _physicalDevice; }
	VkDevice Device() const noexcept { return _device; }
	VkQueue Queue() const noexcept { return _queue; }
	uint32_t QueueFamily() const noexcept { return _queueFamily; }
	VkCommandPool CommandPool() const noexcept { return _commandPool; }

	// Shaders may use real 16-bit floats (EffectCompilerFlags::NativeFP16)
	bool IsFP16Supported() const noexcept {
		return _isFP16Supported;
	}

	// The format can be sampled with linear filtering and written by a compute shader
	bool IsFormatSupported(EffectIntermediateTextureFormat format) const noexcept {
		return !(_unsupportedFormats & (1u << (uint32_t)format));
	}

	// Nanoseconds per timestamp tick; 0 if the queue has no timestamps
	float TimestampPeriod() const noexcept {
		return _timestampPeriod;
	}

	// Warnings and errors reported by the validation layers so far
	uint32_t ValidationMessageCount() const noexcept {
		return _validationMessageCount;
	}

	// The sampler is owned by the device
	VkSampler GetSampler(VkFilter filter, VkSamplerAddressMode addressMode) noexcept;

	// The image starts in VK_IMAGE_LAYOUT_UNDEFINED
	std::shared_ptr<Texture> CreateTexture(
		VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage) noexcept;

	std::unique_ptr<Buffer> CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage) noexcept;

	// Records commands with `record`, submits them and waits until the GPU
	// has executed them.
	bool Execute(const std::function<void(VkCommandBuffer)>& record) noexcept;

	// Gives a new texture its layout without defining its content
	bool SetInitialLayout(Texture& texture, VkImageLayout layout) noexcept;

	// Fills a new texture. `pixels` is tightly packed in the texture's format.
	bool UploadTexture(Texture& texture, std::span<const uint8_t> pixels, VkImageLayout layout) noexcept;

	// Copies the content of a texture, tightly packed in its format
	bool ReadTexture(const Texture& texture, std::vector<uint8_t>& pixels) noexcept;

private:
	bool _CreateInstance(const Options& options) noexcept;
	bool _FindGraphicsCards(const Options& options) noexcept;
	bool _CreateDevice(const Options& options) noexcept;

	static VKAPI_ATTR VkBool32 VKAPI_CALL _DebugCallback(
		VkDebugUtilsMessageSeverityFlagBitsEXT severity,
		VkDebugUtilsMessageTypeFlagsEXT types,
		const VkDebugUtilsMessengerCallbackDataEXT* data,
		void* userData);

	struct CardDetails {
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		uint32_t queueFamily = 0;
		bool isFP16Supported = false;
	};

	VkInstance _instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT _debugMessenger = VK_NULL_HANDLE;
	VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
	VkDevice _device = VK_NULL_HANDLE;
	VkQueue _queue = VK_NULL_HANDLE;
	uint32_t _queueFamily = 0;
	VkCommandPool _commandPool = VK_NULL_HANDLE;
	VkCommandBuffer _commandBuffer = VK_NULL_HANDLE;
	VkFence _fence = VK_NULL_HANDLE;

	std::vector<GraphicsCardInfo> _graphicsCards;
	std::vector<CardDetails> _cardDetails;
	int _graphicsCardIdx = -1;

	VkPhysicalDeviceMemoryProperties _memoryProperties{};
	std::map<std::pair<VkFilter, VkSamplerAddressMode>, VkSampler> _samplers;

	float _timestampPeriod = 0.0f;
	uint32_t _unsupportedFormats = 0;
	uint32_t _validationMessageCount = 0;
	bool _isFP16Supported = false;
};

}
