// DMA-BUF import into Vulkan, for test T2.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct VkImporter;

// deviceIndex is the position in vkEnumeratePhysicalDevices. Returns nullptr
// (after printing the reason) if the device lacks the needed extensions.
VkImporter* VkImporterCreate(int deviceIndex);

// DRM format modifiers the device can import for a single-plane 8-bit format.
std::vector<uint64_t> VkImporterModifiers(VkImporter* importer, uint32_t drmFourcc);

// Imports the buffer as a VkImage, copies the first `rows` rows to host memory
// (4 bytes per pixel, tightly packed) and releases everything again.
// Returns an empty string on success, otherwise the failing step.
std::string VkImporterReadback(
	VkImporter* importer, int fd, uint32_t width, uint32_t height, uint32_t drmFourcc,
	uint64_t modifier, uint32_t offset, uint32_t stride, uint32_t rows,
	std::vector<uint8_t>& out, double& milliseconds);
