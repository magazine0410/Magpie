#include "vk_import.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <libdrm/drm_fourcc.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

struct VkImporter {
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queueFamily = 0;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkPhysicalDeviceMemoryProperties memProps{};
	// Extension entry point, not exported by the loader library.
	PFN_vkGetMemoryFdPropertiesKHR getMemoryFdProperties = nullptr;
};

static const VkImageUsageFlags USAGE = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

static VkFormat ToVkFormat(uint32_t fourcc) {
	switch (fourcc) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		return VK_FORMAT_B8G8R8A8_UNORM;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		return VK_FORMAT_R8G8B8A8_UNORM;
	default:
		return VK_FORMAT_UNDEFINED;
	}
}

VkImporter* VkImporterCreate(int deviceIndex) {
	auto importer = new VkImporter;

	VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.apiVersion = VK_API_VERSION_1_2;
	VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	ici.pApplicationInfo = &app;
	if (vkCreateInstance(&ici, nullptr, &importer->instance) != VK_SUCCESS) {
		std::printf("vk: vkCreateInstance failed\n");
		return nullptr;
	}

	uint32_t count = 0;
	vkEnumeratePhysicalDevices(importer->instance, &count, nullptr);
	std::vector<VkPhysicalDevice> devices(count);
	vkEnumeratePhysicalDevices(importer->instance, &count, devices.data());
	if (deviceIndex < 0 || (uint32_t)deviceIndex >= count) {
		std::printf("vk: no device with index %d (%u devices)\n", deviceIndex, count);
		return nullptr;
	}
	importer->physical = devices[deviceIndex];

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(importer->physical, &props);
	std::printf("vk: device %d = %s\n", deviceIndex, props.deviceName);

	static const char* const EXTENSIONS[] = {
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
		VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
		VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
	};

	uint32_t extCount = 0;
	vkEnumerateDeviceExtensionProperties(importer->physical, nullptr, &extCount, nullptr);
	std::vector<VkExtensionProperties> available(extCount);
	vkEnumerateDeviceExtensionProperties(importer->physical, nullptr, &extCount, available.data());
	for (const char* needed : EXTENSIONS) {
		bool found = false;
		for (const auto& ext : available) {
			found = found || std::strcmp(ext.extensionName, needed) == 0;
		}
		if (!found) {
			std::printf("vk: device lacks %s\n", needed);
			return nullptr;
		}
	}

	uint32_t qfCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(importer->physical, &qfCount, nullptr);
	std::vector<VkQueueFamilyProperties> families(qfCount);
	vkGetPhysicalDeviceQueueFamilyProperties(importer->physical, &qfCount, families.data());
	for (uint32_t i = 0; i < qfCount; ++i) {
		if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			importer->queueFamily = i;
			break;
		}
	}

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	qci.queueFamilyIndex = importer->queueFamily;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;
	VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = (uint32_t)std::size(EXTENSIONS);
	dci.ppEnabledExtensionNames = EXTENSIONS;
	if (vkCreateDevice(importer->physical, &dci, nullptr, &importer->device) != VK_SUCCESS) {
		std::printf("vk: vkCreateDevice failed\n");
		return nullptr;
	}
	vkGetDeviceQueue(importer->device, importer->queueFamily, 0, &importer->queue);
	importer->getMemoryFdProperties = (PFN_vkGetMemoryFdPropertiesKHR)
		vkGetDeviceProcAddr(importer->device, "vkGetMemoryFdPropertiesKHR");

	VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = importer->queueFamily;
	vkCreateCommandPool(importer->device, &pci, nullptr, &importer->pool);

	vkGetPhysicalDeviceMemoryProperties(importer->physical, &importer->memProps);
	return importer;
}

std::vector<uint64_t> VkImporterModifiers(VkImporter* importer, uint32_t drmFourcc) {
	std::vector<uint64_t> result;
	const VkFormat format = ToVkFormat(drmFourcc);
	if (format == VK_FORMAT_UNDEFINED) {
		return result;
	}

	VkDrmFormatModifierPropertiesListEXT list{ VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT };
	VkFormatProperties2 props{ VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2 };
	props.pNext = &list;
	vkGetPhysicalDeviceFormatProperties2(importer->physical, format, &props);

	std::vector<VkDrmFormatModifierPropertiesEXT> mods(list.drmFormatModifierCount);
	list.pDrmFormatModifierProperties = mods.data();
	vkGetPhysicalDeviceFormatProperties2(importer->physical, format, &props);

	for (const auto& mod : mods) {
		if (mod.drmFormatModifierPlaneCount != 1) {
			continue;
		}

		VkPhysicalDeviceImageDrmFormatModifierInfoEXT modInfo{
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT };
		modInfo.drmFormatModifier = mod.drmFormatModifier;
		modInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VkPhysicalDeviceExternalImageFormatInfo extInfo{
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO };
		extInfo.pNext = &modInfo;
		extInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		VkPhysicalDeviceImageFormatInfo2 info{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2 };
		info.pNext = &extInfo;
		info.format = format;
		info.type = VK_IMAGE_TYPE_2D;
		info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
		info.usage = USAGE;

		VkExternalImageFormatProperties extProps{ VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
		VkImageFormatProperties2 out{ VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
		out.pNext = &extProps;
		if (vkGetPhysicalDeviceImageFormatProperties2(importer->physical, &info, &out) == VK_SUCCESS &&
			(extProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
			result.push_back(mod.drmFormatModifier);
		}
	}

	return result;
}

std::string VkImporterReadback(
	VkImporter* importer, int fd, uint32_t width, uint32_t height, uint32_t drmFourcc,
	uint64_t modifier, uint32_t offset, uint32_t stride, uint32_t rows,
	std::vector<uint8_t>& out, double& milliseconds
) {
	const auto start = std::chrono::steady_clock::now();
	const VkDevice dev = importer->device;
	std::string error;

	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	rows = rows < height ? rows : height;

	do {
		VkSubresourceLayout plane{};
		plane.offset = offset;
		plane.rowPitch = stride;
		VkImageDrmFormatModifierExplicitCreateInfoEXT modCreate{
			VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT };
		modCreate.drmFormatModifier = modifier;
		modCreate.drmFormatModifierPlaneCount = 1;
		modCreate.pPlaneLayouts = &plane;
		VkExternalMemoryImageCreateInfo extCreate{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
		extCreate.pNext = &modCreate;
		extCreate.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		ici.pNext = &extCreate;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = ToVkFormat(drmFourcc);
		ici.extent = { width, height, 1 };
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
		ici.usage = USAGE;
		ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (VkResult r = vkCreateImage(dev, &ici, nullptr, &image); r != VK_SUCCESS) {
			error = "vkCreateImage " + std::to_string(r);
			break;
		}

		VkMemoryFdPropertiesKHR fdProps{ VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
		if (VkResult r = importer->getMemoryFdProperties(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdProps);
			r != VK_SUCCESS) {
			error = "vkGetMemoryFdPropertiesKHR " + std::to_string(r);
			break;
		}

		VkMemoryRequirements req;
		vkGetImageMemoryRequirements(dev, image, &req);
		const uint32_t typeBits = req.memoryTypeBits & fdProps.memoryTypeBits;
		if (!typeBits) {
			error = "no memory type accepts this DMA-BUF";
			break;
		}

		const int dupFd = dup(fd);
		VkImportMemoryFdInfoKHR importInfo{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR };
		importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		importInfo.fd = dupFd;
		VkMemoryDedicatedAllocateInfo dedicated{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
		dedicated.pNext = &importInfo;
		dedicated.image = image;
		VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		alloc.pNext = &dedicated;
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = (uint32_t)__builtin_ctz(typeBits);
		if (VkResult r = vkAllocateMemory(dev, &alloc, nullptr, &memory); r != VK_SUCCESS) {
			close(dupFd);	// ownership passes to Vulkan only on success
			error = "vkAllocateMemory(import) " + std::to_string(r);
			break;
		}
		if (VkResult r = vkBindImageMemory(dev, image, memory, 0); r != VK_SUCCESS) {
			error = "vkBindImageMemory " + std::to_string(r);
			break;
		}

		// Host-visible buffer for the read-back.
		const VkDeviceSize size = (VkDeviceSize)width * rows * 4;
		VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bci.size = size;
		bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		vkCreateBuffer(dev, &bci, nullptr, &buffer);
		VkMemoryRequirements breq;
		vkGetBufferMemoryRequirements(dev, buffer, &breq);
		uint32_t hostType = UINT32_MAX;
		const VkMemoryPropertyFlags wanted =
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		for (uint32_t i = 0; i < importer->memProps.memoryTypeCount; ++i) {
			if ((breq.memoryTypeBits & (1u << i)) &&
				(importer->memProps.memoryTypes[i].propertyFlags & wanted) == wanted) {
				hostType = i;
				break;
			}
		}
		VkMemoryAllocateInfo balloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		balloc.allocationSize = breq.size;
		balloc.memoryTypeIndex = hostType;
		if (hostType == UINT32_MAX || vkAllocateMemory(dev, &balloc, nullptr, &bufferMemory) != VK_SUCCESS) {
			error = "host buffer allocation";
			break;
		}
		vkBindBufferMemory(dev, buffer, bufferMemory, 0);

		VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		cai.commandPool = importer->pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		vkAllocateCommandBuffers(dev, &cai, &cmd);

		VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cmd, &begin);

		// Take the image over from the exporting side. As in wlroots, the old
		// layout is UNDEFINED on first use; drivers keep the contents.
		VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
		barrier.dstQueueFamilyIndex = importer->queueFamily;
		barrier.image = image;
		barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &barrier);

		VkBufferImageCopy region{};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.imageExtent = { width, rows, 1 };
		vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
		vkEndCommandBuffer(cmd);

		VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		vkCreateFence(dev, &fci, nullptr, &fence);
		VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &cmd;
		if (VkResult r = vkQueueSubmit(importer->queue, 1, &submit, fence); r != VK_SUCCESS) {
			error = "vkQueueSubmit " + std::to_string(r);
			break;
		}
		if (VkResult r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 2'000'000'000ull); r != VK_SUCCESS) {
			error = "vkWaitForFences " + std::to_string(r);
			break;
		}

		void* mapped = nullptr;
		vkMapMemory(dev, bufferMemory, 0, size, 0, &mapped);
		out.resize(size);
		std::memcpy(out.data(), mapped, size);
		vkUnmapMemory(dev, bufferMemory);
	} while (false);

	if (fence) vkDestroyFence(dev, fence, nullptr);
	if (cmd) vkFreeCommandBuffers(dev, importer->pool, 1, &cmd);
	if (buffer) vkDestroyBuffer(dev, buffer, nullptr);
	if (bufferMemory) vkFreeMemory(dev, bufferMemory, nullptr);
	if (image) vkDestroyImage(dev, image, nullptr);
	if (memory) vkFreeMemory(dev, memory, nullptr);

	milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	return error;
}
