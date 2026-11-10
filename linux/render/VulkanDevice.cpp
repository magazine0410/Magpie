#include "VulkanDevice.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <fmt/format.h>
#include "EffectHelper.h"
#include "Logger.h"

namespace Magpie {

// Requirements for a graphics card, checked in _FindGraphicsCards:
// - Vulkan 1.3
// - a queue family for both graphics and compute commands (compute for the
//   effects, graphics because vkCmdBlitImage needs it)
// - shaderStorageImageExtendedFormats, which covers all effect texture formats
//   that Vulkan does not require anyway
// - robustImageAccess, so that reads and writes outside a texture behave as
//   on Direct3D (reads give zero, writes are dropped). Effects rely on this.
// - VK_KHR_swapchain, if presentation is requested
//
// Used if present: shaderFloat16, and the four extensions for importing
// DMA-BUFs (DMABUF_EXTENSIONS below).

static constexpr const char* DMABUF_EXTENSIONS[] = {
	VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
	VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
	VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
	VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME
};

// An imported DMA-BUF is only copied from
static constexpr VkImageUsageFlags DMABUF_USAGE = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

static bool Check(VkResult result, const char* what, const std::source_location& location = std::source_location::current()) noexcept {
	if (result == VK_SUCCESS) {
		return true;
	}

	Logger::Get().Error(fmt::format("{} failed with VkResult {}", what, (int)result), location);
	return false;
}

int SelectGraphicsCard(std::span<const GraphicsCardInfo> cards, const GraphicsCardId& id) noexcept {
	const int cardCount = (int)cards.size();
	// A card that matched the user's choice but is not usable; it is not tried again
	int failedIdx = -1;

	const auto matches = [&](int i) {
		return cards[i].vendorId == id.vendorId && cards[i].deviceId == id.deviceId;
	};

	if (id.idx >= 0) {
		// The index first
		if (id.idx < cardCount) {
			if (matches(id.idx)) {
				if (cards[id.idx].isUsable) {
					return id.idx;
				}

				failedIdx = id.idx;
				Logger::Get().Warn("The chosen graphics card does not meet the requirements");
			} else {
				Logger::Get().Warn("The set of graphics cards has changed");
			}
		}

		if (failedIdx == -1) {
			// Any other card with the same vendor and device ID
			for (int i = 0; i < cardCount; ++i) {
				if (i == id.idx || !matches(i)) {
					continue;
				}

				if (cards[i].isUsable) {
					return i;
				}

				failedIdx = i;
				Logger::Get().Warn("The chosen graphics card does not meet the requirements");
				break;
			}
		}
	}

	// The first usable card that is not a software implementation
	for (int i = 0; i < cardCount; ++i) {
		if (i != failedIdx && !cards[i].isSoftware && cards[i].isUsable) {
			return i;
		}
	}

	// As a last resort render on the CPU
	for (int i = 0; i < cardCount; ++i) {
		if (cards[i].isSoftware && cards[i].isUsable) {
			return i;
		}
	}

	return -1;
}

uint32_t BytesPerPixel(VkFormat format) noexcept {
	switch (format) {
	case VK_FORMAT_R32G32B32A32_SFLOAT:
		return 16;
	case VK_FORMAT_R16G16B16A16_SFLOAT:
	case VK_FORMAT_R16G16B16A16_UNORM:
	case VK_FORMAT_R16G16B16A16_SNORM:
	case VK_FORMAT_R32G32_SFLOAT:
		return 8;
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SNORM:
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_R16G16_SFLOAT:
	case VK_FORMAT_R16G16_UNORM:
	case VK_FORMAT_R16G16_SNORM:
	case VK_FORMAT_R32_SFLOAT:
		return 4;
	case VK_FORMAT_R8G8_UNORM:
	case VK_FORMAT_R8G8_SNORM:
	case VK_FORMAT_R16_SFLOAT:
	case VK_FORMAT_R16_UNORM:
	case VK_FORMAT_R16_SNORM:
		return 2;
	case VK_FORMAT_R8_UNORM:
	case VK_FORMAT_R8_SNORM:
		return 1;
	default:
		return 0;
	}
}

void TransitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) noexcept {
	const VkImageMemoryBarrier barrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? VkAccessFlags(0) : VkAccessFlags(VK_ACCESS_MEMORY_WRITE_BIT),
		.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
		.oldLayout = from,
		.newLayout = to,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0, 0, nullptr, 0, nullptr, 1, &barrier);
}

Texture::~Texture() {
	if (!device) {
		return;
	}

	if (view) {
		vkDestroyImageView(device, view, nullptr);
	}
	if (image) {
		vkDestroyImage(device, image, nullptr);
	}
	if (memory) {
		vkFreeMemory(device, memory, nullptr);
	}
}

Buffer::~Buffer() {
	if (!device) {
		return;
	}

	if (buffer) {
		vkDestroyBuffer(device, buffer, nullptr);
	}
	if (memory) {
		// Freeing the memory also unmaps it
		vkFreeMemory(device, memory, nullptr);
	}
}

VulkanDevice::~VulkanDevice() {
	if (_device) {
		vkDeviceWaitIdle(_device);

		for (const auto& [key, sampler] : _samplers) {
			vkDestroySampler(_device, sampler, nullptr);
		}
		if (_fence) {
			vkDestroyFence(_device, _fence, nullptr);
		}
		if (_commandPool) {
			vkDestroyCommandPool(_device, _commandPool, nullptr);
		}
		vkDestroyDevice(_device, nullptr);
	}

	if (_instance) {
		if (_debugMessenger) {
			const auto destroyMessenger = (PFN_vkDestroyDebugUtilsMessengerEXT)
				vkGetInstanceProcAddr(_instance, "vkDestroyDebugUtilsMessengerEXT");
			if (destroyMessenger) {
				destroyMessenger(_instance, _debugMessenger, nullptr);
			}
		}
		vkDestroyInstance(_instance, nullptr);
	}
}

bool VulkanDevice::Initialize(const Options& options) noexcept {
	if (!_CreateInstance(options) || !_FindGraphicsCards(options)) {
		return false;
	}

	_graphicsCardIdx = SelectGraphicsCard(_graphicsCards, options.graphicsCardId);
	if (_graphicsCardIdx < 0) {
		Logger::Get().Error("No usable graphics card found");
		return false;
	}

	return _CreateDevice(options);
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDevice::_DebugCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT /*types*/,
	const VkDebugUtilsMessengerCallbackDataEXT* data,
	void* userData
) {
	VulkanDevice* that = (VulkanDevice*)userData;
	++that->_validationMessageCount;

	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
		Logger::Get().Error(fmt::format("Vulkan validation: {}", data->pMessage));
	} else {
		Logger::Get().Warn(fmt::format("Vulkan validation: {}", data->pMessage));
	}

	return VK_FALSE;
}

bool VulkanDevice::_CreateInstance(const Options& options) noexcept {
	static constexpr const char* VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";

	std::vector<const char*> layers;
	std::vector<const char*> extensions;

	if (options.validation) {
		uint32_t count = 0;
		vkEnumerateInstanceLayerProperties(&count, nullptr);
		std::vector<VkLayerProperties> available(count);
		vkEnumerateInstanceLayerProperties(&count, available.data());

		const bool found = std::any_of(available.begin(), available.end(), [](const VkLayerProperties& layer) {
			return std::strcmp(layer.layerName, VALIDATION_LAYER) == 0;
		});
		if (!found) {
			Logger::Get().Error("The Vulkan validation layers are not installed");
			return false;
		}

		layers.push_back(VALIDATION_LAYER);
		extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
	}

	if (options.presentation) {
		extensions.push_back("VK_KHR_surface");
		extensions.push_back("VK_KHR_wayland_surface");
	}

	// Also reports missing barriers between passes
	static constexpr VkValidationFeatureEnableEXT ENABLED_VALIDATION[] = {
		VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
	};
	const VkValidationFeaturesEXT validationFeatures{
		.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
		.enabledValidationFeatureCount = (uint32_t)std::size(ENABLED_VALIDATION),
		.pEnabledValidationFeatures = ENABLED_VALIDATION
	};
	const VkDebugUtilsMessengerCreateInfoEXT messengerInfo{
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		.pNext = &validationFeatures,
		.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
		// Not the general type: the loader uses it for notes about drivers
		// it skips, which say nothing about this program
		.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
		.pfnUserCallback = _DebugCallback,
		.pUserData = this
	};

	const VkApplicationInfo appInfo{
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "Magpie",
		.apiVersion = VK_API_VERSION_1_3
	};
	const VkInstanceCreateInfo instanceInfo{
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		// Chaining the messenger here also covers instance creation itself
		.pNext = options.validation ? &messengerInfo : nullptr,
		.pApplicationInfo = &appInfo,
		.enabledLayerCount = (uint32_t)layers.size(),
		.ppEnabledLayerNames = layers.data(),
		.enabledExtensionCount = (uint32_t)extensions.size(),
		.ppEnabledExtensionNames = extensions.data()
	};
	if (!Check(vkCreateInstance(&instanceInfo, nullptr, &_instance), "vkCreateInstance")) {
		return false;
	}

	if (options.validation) {
		const auto createMessenger = (PFN_vkCreateDebugUtilsMessengerEXT)
			vkGetInstanceProcAddr(_instance, "vkCreateDebugUtilsMessengerEXT");
		VkDebugUtilsMessengerCreateInfoEXT info = messengerInfo;
		info.pNext = nullptr;
		if (!createMessenger || !Check(createMessenger(_instance, &info, nullptr, &_debugMessenger),
			"vkCreateDebugUtilsMessengerEXT")) {
			return false;
		}
	}

	return true;
}

bool VulkanDevice::_FindGraphicsCards(const Options& options) noexcept {
	uint32_t deviceCount = 0;
	vkEnumeratePhysicalDevices(_instance, &deviceCount, nullptr);
	std::vector<VkPhysicalDevice> devices(deviceCount);
	if (deviceCount == 0 || !Check(vkEnumeratePhysicalDevices(_instance, &deviceCount, devices.data()),
		"vkEnumeratePhysicalDevices")) {
		Logger::Get().Error("No Vulkan device found");
		return false;
	}
	devices.resize(deviceCount);

	for (VkPhysicalDevice physicalDevice : devices) {
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(physicalDevice, &props);

		GraphicsCardInfo& info = _graphicsCards.emplace_back();
		info.vendorId = props.vendorID;
		info.deviceId = props.deviceID;
		info.name = props.deviceName;
		info.isSoftware = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;

		CardDetails& details = _cardDetails.emplace_back();
		details.physicalDevice = physicalDevice;

		const char* missing = nullptr;
		if (props.apiVersion < VK_API_VERSION_1_3) {
			missing = "Vulkan 1.3";
		}

		if (!missing) {
			uint32_t familyCount = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
			std::vector<VkQueueFamilyProperties> families(familyCount);
			vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());

			static constexpr VkQueueFlags NEEDED = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
			const auto it = std::find_if(families.begin(), families.end(), [](const VkQueueFamilyProperties& family) {
				return (family.queueFlags & NEEDED) == NEEDED;
			});
			if (it == families.end()) {
				missing = "a queue for graphics and compute";
			} else {
				details.queueFamily = uint32_t(it - families.begin());
			}
		}

		if (!missing) {
			VkPhysicalDeviceVulkan13Features features13{
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES
			};
			VkPhysicalDeviceVulkan12Features features12{
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
				.pNext = &features13
			};
			VkPhysicalDeviceFeatures2 features{
				.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
				.pNext = &features12
			};
			vkGetPhysicalDeviceFeatures2(physicalDevice, &features);

			details.isFP16Supported = features12.shaderFloat16;

			if (!features.features.shaderStorageImageExtendedFormats) {
				missing = "shaderStorageImageExtendedFormats";
			} else if (!features13.robustImageAccess) {
				missing = "robustImageAccess";
			}
		}

		if (!missing) {
			uint32_t extensionCount = 0;
			vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
			std::vector<VkExtensionProperties> available(extensionCount);
			vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, available.data());

			const auto has = [&](const char* name) {
				return std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& extension) {
					return std::strcmp(extension.extensionName, name) == 0;
				});
			};

			if (options.presentation && !has(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
				missing = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
			}

			details.isDmabufImportSupported = std::all_of(
				std::begin(DMABUF_EXTENSIONS), std::end(DMABUF_EXTENSIONS), has);
		}

		info.isUsable = !missing;

		Logger::Get().Info(fmt::format("Graphics card {}: {} ({:04x}:{:04x}){}{}",
			_graphicsCards.size() - 1, info.name, info.vendorId, info.deviceId,
			info.isSoftware ? ", software" : "",
			missing ? fmt::format(", not usable: no {}", missing) : std::string()));
	}

	return true;
}

bool VulkanDevice::_CreateDevice(const Options& options) noexcept {
	const CardDetails& details = _cardDetails[_graphicsCardIdx];
	_physicalDevice = details.physicalDevice;
	_queueFamily = details.queueFamily;
	_isFP16Supported = details.isFP16Supported;
	_isDmabufImportSupported = details.isDmabufImportSupported;

	Logger::Get().Info(fmt::format("Using graphics card {}: {}\n\tFP16: {}\n\tDMA-BUF import: {}",
		_graphicsCardIdx, _graphicsCards[_graphicsCardIdx].name, _isFP16Supported ? "yes" : "no",
		_isDmabufImportSupported ? "yes" : "no"));

	const float priority = 1.0f;
	const VkDeviceQueueCreateInfo queueInfo{
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = _queueFamily,
		.queueCount = 1,
		.pQueuePriorities = &priority
	};

	VkPhysicalDeviceVulkan13Features features13{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
		.robustImageAccess = VK_TRUE,
		// For drawing the cursor. Every Vulkan 1.3 device has it.
		.dynamicRendering = VK_TRUE
	};
	VkPhysicalDeviceVulkan12Features features12{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = &features13,
		.shaderFloat16 = _isFP16Supported
	};
	const VkPhysicalDeviceFeatures2 features{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.pNext = &features12,
		.features = { .shaderStorageImageExtendedFormats = VK_TRUE }
	};

	std::vector<const char*> extensions;
	if (options.presentation) {
		extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
	}
	if (_isDmabufImportSupported) {
		extensions.insert(extensions.end(), std::begin(DMABUF_EXTENSIONS), std::end(DMABUF_EXTENSIONS));
	}

	const VkDeviceCreateInfo deviceInfo{
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &features,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queueInfo,
		.enabledExtensionCount = (uint32_t)extensions.size(),
		.ppEnabledExtensionNames = extensions.data()
	};
	if (!Check(vkCreateDevice(_physicalDevice, &deviceInfo, nullptr, &_device), "vkCreateDevice")) {
		return false;
	}

	vkGetDeviceQueue(_device, _queueFamily, 0, &_queue);
	vkGetPhysicalDeviceMemoryProperties(_physicalDevice, &_memoryProperties);

	if (_isDmabufImportSupported) {
		_getMemoryFdProperties = (PFN_vkGetMemoryFdPropertiesKHR)
			vkGetDeviceProcAddr(_device, "vkGetMemoryFdPropertiesKHR");
		_isDmabufImportSupported = _getMemoryFdProperties != nullptr;
	}

	{
		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(_physicalDevice, &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(_physicalDevice, &familyCount, families.data());

		if (families[_queueFamily].timestampValidBits == 64) {
			VkPhysicalDeviceProperties props;
			vkGetPhysicalDeviceProperties(_physicalDevice, &props);
			_timestampPeriod = props.limits.timestampPeriod;
		}
	}

	// Formats that cannot hold an effect texture on this card. Effects that
	// name one of them fail with a message instead of rendering wrongly.
	for (uint32_t i = 0; i < (uint32_t)EffectIntermediateTextureFormat::UNKNOWN; ++i) {
		static constexpr VkFormatFeatureFlags NEEDED = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
			VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;

		VkFormatProperties props;
		vkGetPhysicalDeviceFormatProperties(_physicalDevice, EffectHelper::FORMAT_DESCS[i].vkFormat, &props);
		if ((props.optimalTilingFeatures & NEEDED) != NEEDED) {
			_unsupportedFormats |= 1u << i;
			Logger::Get().Warn(fmt::format("The graphics card does not support {} for effect textures",
				EffectHelper::FORMAT_DESCS[i].name));
		}
	}

	const VkCommandPoolCreateInfo poolInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = _queueFamily
	};
	if (!Check(vkCreateCommandPool(_device, &poolInfo, nullptr, &_commandPool), "vkCreateCommandPool")) {
		return false;
	}

	const VkCommandBufferAllocateInfo allocInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = _commandPool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1
	};
	if (!Check(vkAllocateCommandBuffers(_device, &allocInfo, &_commandBuffer), "vkAllocateCommandBuffers")) {
		return false;
	}

	const VkFenceCreateInfo fenceInfo{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	return Check(vkCreateFence(_device, &fenceInfo, nullptr, &_fence), "vkCreateFence");
}

VkSampler VulkanDevice::GetSampler(VkFilter filter, VkSamplerAddressMode addressMode) noexcept {
	const auto key = std::make_pair(filter, addressMode);
	if (const auto it = _samplers.find(key); it != _samplers.end()) {
		return it->second;
	}

	// The textures have one mip level, so the mip settings have no effect
	const VkSamplerCreateInfo samplerInfo{
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = filter,
		.minFilter = filter,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = addressMode,
		.addressModeV = addressMode,
		.addressModeW = addressMode
	};
	VkSampler sampler = VK_NULL_HANDLE;
	if (!Check(vkCreateSampler(_device, &samplerInfo, nullptr, &sampler), "vkCreateSampler")) {
		return VK_NULL_HANDLE;
	}

	_samplers.emplace(key, sampler);
	return sampler;
}

// Returns UINT32_MAX if there is no such memory type
static uint32_t FindMemoryType(
	const VkPhysicalDeviceMemoryProperties& memoryProperties,
	uint32_t typeBits,
	VkMemoryPropertyFlags flags
) noexcept {
	for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
		if ((typeBits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & flags) == flags) {
			return i;
		}
	}
	return UINT32_MAX;
}

std::shared_ptr<Texture> VulkanDevice::CreateTexture(
	VkFormat format,
	uint32_t width,
	uint32_t height,
	VkImageUsageFlags usage
) noexcept {
	auto texture = std::make_shared<Texture>();
	texture->device = _device;
	texture->format = format;
	texture->width = width;
	texture->height = height;

	const VkImageCreateInfo imageInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent = { width, height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
	};
	if (!Check(vkCreateImage(_device, &imageInfo, nullptr, &texture->image), "vkCreateImage")) {
		return nullptr;
	}

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(_device, texture->image, &requirements);

	const VkMemoryAllocateInfo allocInfo{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = FindMemoryType(_memoryProperties, requirements.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
	};
	if (allocInfo.memoryTypeIndex == UINT32_MAX) {
		Logger::Get().Error("No memory type for a texture");
		return nullptr;
	}
	if (!Check(vkAllocateMemory(_device, &allocInfo, nullptr, &texture->memory), "vkAllocateMemory") ||
		!Check(vkBindImageMemory(_device, texture->image, texture->memory, 0), "vkBindImageMemory")) {
		return nullptr;
	}

	const VkImageViewCreateInfo viewInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = texture->image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = format,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
	};
	if (!Check(vkCreateImageView(_device, &viewInfo, nullptr, &texture->view), "vkCreateImageView")) {
		return nullptr;
	}

	return texture;
}

std::unique_ptr<Buffer> VulkanDevice::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage) noexcept {
	auto buffer = std::make_unique<Buffer>();
	buffer->device = _device;
	buffer->size = size;

	const VkBufferCreateInfo bufferInfo{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE
	};
	if (!Check(vkCreateBuffer(_device, &bufferInfo, nullptr, &buffer->buffer), "vkCreateBuffer")) {
		return nullptr;
	}

	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(_device, buffer->buffer, &requirements);

	const VkMemoryAllocateInfo allocInfo{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = FindMemoryType(_memoryProperties, requirements.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
	};
	if (allocInfo.memoryTypeIndex == UINT32_MAX) {
		Logger::Get().Error("No memory type for a buffer");
		return nullptr;
	}
	if (!Check(vkAllocateMemory(_device, &allocInfo, nullptr, &buffer->memory), "vkAllocateMemory") ||
		!Check(vkBindBufferMemory(_device, buffer->buffer, buffer->memory, 0), "vkBindBufferMemory") ||
		!Check(vkMapMemory(_device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->data), "vkMapMemory")) {
		return nullptr;
	}

	return buffer;
}

std::vector<uint64_t> VulkanDevice::DmabufModifiers(VkFormat format) const noexcept {
	std::vector<uint64_t> result;
	if (!_isDmabufImportSupported) {
		return result;
	}

	VkDrmFormatModifierPropertiesListEXT list{
		.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT
	};
	VkFormatProperties2 formatProps{
		.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
		.pNext = &list
	};
	// The first call returns the count, the second the modifiers
	vkGetPhysicalDeviceFormatProperties2(_physicalDevice, format, &formatProps);
	std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(list.drmFormatModifierCount);
	list.pDrmFormatModifierProperties = modifiers.data();
	vkGetPhysicalDeviceFormatProperties2(_physicalDevice, format, &formatProps);

	for (const VkDrmFormatModifierPropertiesEXT& modifier : modifiers) {
		if (modifier.drmFormatModifierPlaneCount != 1) {
			continue;
		}

		VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierInfo{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
			.drmFormatModifier = modifier.drmFormatModifier,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE
		};
		VkPhysicalDeviceExternalImageFormatInfo externalInfo{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
			.pNext = &modifierInfo,
			.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
		};
		const VkPhysicalDeviceImageFormatInfo2 imageInfo{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
			.pNext = &externalInfo,
			.format = format,
			.type = VK_IMAGE_TYPE_2D,
			.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
			.usage = DMABUF_USAGE
		};
		VkExternalImageFormatProperties externalProps{
			.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES
		};
		VkImageFormatProperties2 imageProps{
			.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
			.pNext = &externalProps
		};
		if (vkGetPhysicalDeviceImageFormatProperties2(_physicalDevice, &imageInfo, &imageProps) == VK_SUCCESS &&
			(externalProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
			result.push_back(modifier.drmFormatModifier);
		}
	}

	return result;
}

std::shared_ptr<Texture> VulkanDevice::ImportDmabuf(
	int fd,
	VkFormat format,
	uint32_t width,
	uint32_t height,
	uint64_t modifier,
	uint32_t offset,
	uint32_t stride
) noexcept {
	if (!_isDmabufImportSupported) {
		Logger::Get().Error("The graphics card cannot import DMA-BUFs");
		return nullptr;
	}

	auto texture = std::make_shared<Texture>();
	texture->device = _device;
	texture->format = format;
	texture->width = width;
	texture->height = height;

	const VkSubresourceLayout planeLayout{
		.offset = offset,
		.rowPitch = stride
	};
	const VkImageDrmFormatModifierExplicitCreateInfoEXT modifierInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
		.drmFormatModifier = modifier,
		.drmFormatModifierPlaneCount = 1,
		.pPlaneLayouts = &planeLayout
	};
	const VkExternalMemoryImageCreateInfo externalInfo{
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.pNext = &modifierInfo,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
	};
	const VkImageCreateInfo imageInfo{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = &externalInfo,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = format,
		.extent = { width, height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
		.usage = DMABUF_USAGE,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
	};
	if (!Check(vkCreateImage(_device, &imageInfo, nullptr, &texture->image), "vkCreateImage")) {
		return nullptr;
	}

	VkMemoryFdPropertiesKHR fdProps{ .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
	if (!Check(_getMemoryFdProperties(_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdProps),
		"vkGetMemoryFdPropertiesKHR")) {
		return nullptr;
	}

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(_device, texture->image, &requirements);
	const uint32_t typeBits = requirements.memoryTypeBits & fdProps.memoryTypeBits;
	if (typeBits == 0) {
		Logger::Get().Error("No memory type accepts the DMA-BUF");
		return nullptr;
	}

	// Vulkan takes ownership of the descriptor it is given, but only if the
	// import succeeds
	const int ownFd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (ownFd < 0) {
		Logger::Get().Error("Duplicating the DMA-BUF descriptor failed");
		return nullptr;
	}

	const VkImportMemoryFdInfoKHR importInfo{
		.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		.fd = ownFd
	};
	const VkMemoryDedicatedAllocateInfo dedicatedInfo{
		.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
		.pNext = &importInfo,
		.image = texture->image
	};
	const VkMemoryAllocateInfo allocInfo{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &dedicatedInfo,
		.allocationSize = requirements.size,
		.memoryTypeIndex = (uint32_t)std::countr_zero(typeBits)
	};
	if (!Check(vkAllocateMemory(_device, &allocInfo, nullptr, &texture->memory), "vkAllocateMemory")) {
		close(ownFd);
		return nullptr;
	}
	if (!Check(vkBindImageMemory(_device, texture->image, texture->memory, 0), "vkBindImageMemory")) {
		return nullptr;
	}

	return texture;
}

void VulkanDevice::AcquireDmabuf(VkCommandBuffer cmd, Texture& texture) const noexcept {
	// A new import is in the undefined layout. Its content is kept all the
	// same, because the transfer from the foreign queue family comes with it;
	// wlroots and other importers do the same.
	const VkImageMemoryBarrier barrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = texture.layout,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
		.dstQueueFamilyIndex = _queueFamily,
		.image = texture.image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void VulkanDevice::ReleaseDmabuf(VkCommandBuffer cmd, Texture& texture) const noexcept {
	const VkImageMemoryBarrier barrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = _queueFamily,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
		.image = texture.image,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		0, 0, nullptr, 0, nullptr, 1, &barrier);
	texture.layout = VK_IMAGE_LAYOUT_GENERAL;
}

bool VulkanDevice::Execute(const std::function<void(VkCommandBuffer)>& record) noexcept {
	const VkCommandBufferBeginInfo beginInfo{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
	};
	if (!Check(vkResetCommandBuffer(_commandBuffer, 0), "vkResetCommandBuffer") ||
		!Check(vkBeginCommandBuffer(_commandBuffer, &beginInfo), "vkBeginCommandBuffer")) {
		return false;
	}

	record(_commandBuffer);

	const VkSubmitInfo submitInfo{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &_commandBuffer
	};
	return Check(vkEndCommandBuffer(_commandBuffer), "vkEndCommandBuffer") &&
		Check(vkResetFences(_device, 1, &_fence), "vkResetFences") &&
		Check(vkQueueSubmit(_queue, 1, &submitInfo, _fence), "vkQueueSubmit") &&
		Check(vkWaitForFences(_device, 1, &_fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
}

bool VulkanDevice::SetInitialLayout(Texture& texture, VkImageLayout layout) noexcept {
	if (!Execute([&](VkCommandBuffer cmd) {
		TransitionImage(cmd, texture.image, VK_IMAGE_LAYOUT_UNDEFINED, layout);
	})) {
		return false;
	}

	texture.layout = layout;
	return true;
}

static constexpr VkImageSubresourceLayers COLOR_LAYER{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };

bool VulkanDevice::UploadTexture(Texture& texture, std::span<const uint8_t> pixels, VkImageLayout layout) noexcept {
	const size_t expectedSize = (size_t)texture.width * texture.height * BytesPerPixel(texture.format);
	if (expectedSize == 0 || pixels.size() != expectedSize) {
		Logger::Get().Error("The pixel data does not match the texture");
		return false;
	}

	const std::unique_ptr<Buffer> staging = CreateBuffer(pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	if (!staging) {
		return false;
	}
	std::memcpy(staging->data, pixels.data(), pixels.size());

	if (!Execute([&](VkCommandBuffer cmd) {
		TransitionImage(cmd, texture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		const VkBufferImageCopy region{
			.imageSubresource = COLOR_LAYER,
			.imageExtent = { texture.width, texture.height, 1 }
		};
		vkCmdCopyBufferToImage(cmd, staging->buffer, texture.image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		TransitionImage(cmd, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout);
	})) {
		return false;
	}

	texture.layout = layout;
	return true;
}

bool VulkanDevice::ReadTexture(const Texture& texture, std::vector<uint8_t>& pixels) noexcept {
	const size_t size = (size_t)texture.width * texture.height * BytesPerPixel(texture.format);
	if (size == 0) {
		Logger::Get().Error("Textures of this format cannot be read");
		return false;
	}

	const std::unique_ptr<Buffer> staging = CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	if (!staging) {
		return false;
	}

	// An image can be copied from in these two layouts
	const bool needsTransition = texture.layout != VK_IMAGE_LAYOUT_GENERAL &&
		texture.layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	const VkImageLayout copyLayout = needsTransition ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : texture.layout;

	if (!Execute([&](VkCommandBuffer cmd) {
		if (needsTransition) {
			TransitionImage(cmd, texture.image, texture.layout, copyLayout);
		}

		const VkBufferImageCopy region{
			.imageSubresource = COLOR_LAYER,
			.imageExtent = { texture.width, texture.height, 1 }
		};
		vkCmdCopyImageToBuffer(cmd, texture.image, copyLayout, staging->buffer, 1, &region);

		if (needsTransition) {
			TransitionImage(cmd, texture.image, copyLayout, texture.layout);
		}
	})) {
		return false;
	}

	pixels.resize(size);
	std::memcpy(pixels.data(), staging->data, size);
	return true;
}

}
