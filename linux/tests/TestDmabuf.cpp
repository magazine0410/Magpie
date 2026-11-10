// DMA-BUF import: a texture is exported as a DMA-BUF by plain Vulkan calls,
// imported again through VulkanDevice, and must have the same content. This
// covers the import on every graphics card without a compositor.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstring>
#include <unistd.h>
#include "RenderTestHelpers.h"

using namespace Magpie;
using namespace RenderTest;

namespace {

// An image whose memory can be exported as a DMA-BUF
struct ExportedImage {
	VkDevice device = VK_NULL_HANDLE;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	int fd = -1;
	uint64_t modifier = 0;
	uint32_t offset = 0;
	uint32_t stride = 0;

	~ExportedImage() {
		if (fd >= 0) {
			close(fd);
		}
		if (image) {
			vkDestroyImage(device, image, nullptr);
		}
		if (memory) {
			vkFreeMemory(device, memory, nullptr);
		}
	}
};

// The modifiers of `candidates` with which the card can also export
std::vector<uint64_t> ExportableModifiers(VulkanDevice& device, VkFormat format, const std::vector<uint64_t>& candidates) {
	std::vector<uint64_t> result;
	for (uint64_t modifier : candidates) {
		VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierInfo{
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
			.drmFormatModifier = modifier,
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
			.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
		};
		VkExternalImageFormatProperties externalProps{
			.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES
		};
		VkImageFormatProperties2 imageProps{
			.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
			.pNext = &externalProps
		};
		if (vkGetPhysicalDeviceImageFormatProperties2(device.PhysicalDevice(), &imageInfo, &imageProps) == VK_SUCCESS &&
			(externalProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
			result.push_back(modifier);
		}
	}
	return result;
}

bool CreateExportedImage(
	VulkanDevice& device,
	VkFormat format,
	uint32_t width,
	uint32_t height,
	const std::vector<uint64_t>& modifiers,
	const std::vector<uint8_t>& pixels,
	ExportedImage& exported
) {
	const VkDevice vkDevice = device.Device();
	exported.device = vkDevice;

	const auto getMemoryFd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(vkDevice, "vkGetMemoryFdKHR");
	const auto getModifier = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)
		vkGetDeviceProcAddr(vkDevice, "vkGetImageDrmFormatModifierPropertiesEXT");
	if (!getMemoryFd || !getModifier) {
		return false;
	}

	const VkImageDrmFormatModifierListCreateInfoEXT modifierList{
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
		.drmFormatModifierCount = (uint32_t)modifiers.size(),
		.pDrmFormatModifiers = modifiers.data()
	};
	const VkExternalMemoryImageCreateInfo externalInfo{
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		.pNext = &modifierList,
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
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
	};
	if (vkCreateImage(vkDevice, &imageInfo, nullptr, &exported.image) != VK_SUCCESS) {
		return false;
	}

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(vkDevice, exported.image, &requirements);

	VkPhysicalDeviceMemoryProperties memoryProps;
	vkGetPhysicalDeviceMemoryProperties(device.PhysicalDevice(), &memoryProps);
	uint32_t memoryType = UINT32_MAX;
	for (uint32_t i = 0; i < memoryProps.memoryTypeCount; ++i) {
		if ((requirements.memoryTypeBits & (1u << i)) &&
			(memoryProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
			memoryType = i;
			break;
		}
	}
	if (memoryType == UINT32_MAX) {
		return false;
	}

	const VkExportMemoryAllocateInfo exportInfo{
		.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
	};
	const VkMemoryDedicatedAllocateInfo dedicatedInfo{
		.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
		.pNext = &exportInfo,
		.image = exported.image
	};
	const VkMemoryAllocateInfo allocInfo{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &dedicatedInfo,
		.allocationSize = requirements.size,
		.memoryTypeIndex = memoryType
	};
	if (vkAllocateMemory(vkDevice, &allocInfo, nullptr, &exported.memory) != VK_SUCCESS ||
		vkBindImageMemory(vkDevice, exported.image, exported.memory, 0) != VK_SUCCESS) {
		return false;
	}

	// Fill it
	const std::unique_ptr<Buffer> staging = device.CreateBuffer(pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	if (!staging) {
		return false;
	}
	std::memcpy(staging->data, pixels.data(), pixels.size());
	if (!device.Execute([&](VkCommandBuffer cmd) {
		TransitionImage(cmd, exported.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		const VkBufferImageCopy region{
			.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageExtent = { width, height, 1 }
		};
		vkCmdCopyBufferToImage(cmd, staging->buffer, exported.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		TransitionImage(cmd, exported.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
	})) {
		return false;
	}

	const VkMemoryGetFdInfoKHR fdInfo{
		.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
		.memory = exported.memory,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
	};
	if (getMemoryFd(vkDevice, &fdInfo, &exported.fd) != VK_SUCCESS) {
		return false;
	}

	VkImageDrmFormatModifierPropertiesEXT modifierProps{
		.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT
	};
	if (getModifier(vkDevice, exported.image, &modifierProps) != VK_SUCCESS) {
		return false;
	}
	exported.modifier = modifierProps.drmFormatModifier;

	const VkImageSubresource plane{ VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0 };
	VkSubresourceLayout layout;
	vkGetImageSubresourceLayout(vkDevice, exported.image, &plane, &layout);
	exported.offset = (uint32_t)layout.offset;
	exported.stride = (uint32_t)layout.rowPitch;
	return true;
}

}

TEST_CASE("A DMA-BUF is imported with its content", "[dmabuf]") {
	ForEachDevice([](VulkanDevice& device, const GraphicsCardInfo&) {
		if (!device.IsDmabufImportSupported()) {
			SKIP("The graphics card cannot import DMA-BUFs");
		}

		// The two formats a screen cast delivers. The size is not a multiple of
		// anything, so that a wrong stride shows.
		const VkFormat format = GENERATE(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM);
		static constexpr uint32_t WIDTH = 301;
		static constexpr uint32_t HEIGHT = 203;

		const std::vector<uint64_t> importable = device.DmabufModifiers(format);
		const std::vector<uint64_t> modifiers = ExportableModifiers(device, format, importable);
		INFO(importable.size() << " importable modifiers, " << modifiers.size() << " of them exportable");
		if (modifiers.empty()) {
			SKIP("The graphics card cannot export a DMA-BUF to test the import with");
		}

		std::vector<uint8_t> pixels((size_t)WIDTH * HEIGHT * 4);
		for (size_t i = 0; i < pixels.size(); ++i) {
			pixels[i] = uint8_t(i * 7 + i / 4 / WIDTH * 13 + i % 4 * 50);
		}

		ExportedImage exported;
		REQUIRE(CreateExportedImage(device, format, WIDTH, HEIGHT, modifiers, pixels, exported));

		const std::shared_ptr<Texture> imported = device.ImportDmabuf(
			exported.fd, format, WIDTH, HEIGHT, exported.modifier, exported.offset, exported.stride);
		REQUIRE(imported);

		const std::shared_ptr<Texture> copy = device.CreateTexture(format, WIDTH, HEIGHT,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
		REQUIRE(copy);

		// Twice: the first acquisition starts from the undefined layout, later
		// ones do not
		for (int round = 0; round < 2; ++round) {
			REQUIRE(device.Execute([&](VkCommandBuffer cmd) {
				device.AcquireDmabuf(cmd, *imported);
				TransitionImage(cmd, copy->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

				const VkImageCopy region{
					.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
					.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
					.extent = { WIDTH, HEIGHT, 1 }
				};
				vkCmdCopyImage(cmd, imported->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					copy->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

				TransitionImage(cmd, copy->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
				device.ReleaseDmabuf(cmd, *imported);
			}));
			copy->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

			std::vector<uint8_t> readBack;
			REQUIRE(device.ReadTexture(*copy, readBack));
			CHECK(readBack == pixels);
		}
	});
}
