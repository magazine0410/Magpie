// Reads the few facts the tests need out of a SPIR-V module.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

struct SpirvInfo {
	bool valid = false;
	std::set<uint32_t> capabilities;
	// Name of a variable -> its descriptor binding
	std::map<std::string, uint32_t> bindings;
	// Image format operand of every storage image type
	std::vector<uint32_t> storageImageFormats;
};

namespace Spirv {
constexpr uint32_t MAGIC = 0x07230203;
constexpr uint32_t OP_NAME = 5;
constexpr uint32_t OP_CAPABILITY = 17;
constexpr uint32_t OP_TYPE_IMAGE = 25;
constexpr uint32_t OP_DECORATE = 71;
constexpr uint32_t DECORATION_BINDING = 33;
constexpr uint32_t CAPABILITY_FLOAT16 = 9;
}

inline SpirvInfo ParseSpirv(const std::vector<uint32_t>& words) {
	SpirvInfo info;
	if (words.size() < 5 || words[0] != Spirv::MAGIC) {
		return info;
	}

	std::map<uint32_t, std::string> names;
	std::map<uint32_t, uint32_t> bindingOfId;

	for (size_t i = 5; i < words.size();) {
		const uint32_t opcode = words[i] & 0xFFFF;
		const uint32_t wordCount = words[i] >> 16;
		if (wordCount == 0 || i + wordCount > words.size()) {
			return info;
		}

		if (opcode == Spirv::OP_CAPABILITY && wordCount >= 2) {
			info.capabilities.insert(words[i + 1]);
		} else if (opcode == Spirv::OP_NAME && wordCount >= 3) {
			// The name is a NUL-terminated string packed into the words
			names[words[i + 1]] = std::string((const char*)&words[i + 2]);
		} else if (opcode == Spirv::OP_DECORATE && wordCount >= 4 && words[i + 2] == Spirv::DECORATION_BINDING) {
			bindingOfId[words[i + 1]] = words[i + 3];
		} else if (opcode == Spirv::OP_TYPE_IMAGE && wordCount >= 9 && words[i + 7] == 2) {
			// Operand "Sampled" == 2 means a storage image
			info.storageImageFormats.push_back(words[i + 8]);
		}

		i += wordCount;
	}

	for (const auto& [id, binding] : bindingOfId) {
		if (auto it = names.find(id); it != names.end()) {
			info.bindings[it->second] = binding;
		}
	}

	info.valid = true;
	return info;
}
