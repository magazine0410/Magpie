#pragma once
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace Spike {

struct DumpedPass {
	std::string sourceName;
	std::string source;
	std::vector<std::pair<std::string, std::string>> macros;
};

// Directory that stands for "effects\" in upstream paths (src/Effects).
extern std::filesystem::path effectsRoot;
// Filled by the DirectXHelper stand-in, one entry per generated pass.
extern std::vector<DumpedPass> dumpedPasses;

}
