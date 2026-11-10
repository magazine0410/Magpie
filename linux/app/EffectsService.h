// What the user interface knows of the effects: their names, parameters and
// whether they can scale. Linux counterpart of EffectsService (src/Magpie).
// The effects are read in the background from the start of the program;
// whoever asks before that is done waits.
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include "EffectDesc.h"

namespace Magpie {

struct EffectInfo {
	// Path below the effects directory without extension, with backslashes
	// as on Windows (for example "FSR\FSR_EASU")
	std::string name;
	// What the effects of a folder are ordered by in the menu
	std::string sortName;
	std::vector<EffectParameterDesc> params;
	// The effect has no output size of its own, so the user chooses one
	bool canScale = false;
};

class EffectsService {
public:
	EffectsService() = default;
	EffectsService(const EffectsService&) = delete;
	EffectsService(EffectsService&&) = delete;
	~EffectsService();

	// Starts reading the effects below this directory
	void Initialize(const std::filesystem::path& effectsDir);

	// Ordered by name
	const std::vector<EffectInfo>& Effects();

	// Null for an effect that does not exist or cannot be read
	const EffectInfo* GetEffect(std::string_view name);

	// The name without the folder: "FSR_EASU" for "FSR\FSR_EASU"
	static std::string_view DisplayName(std::string_view name) noexcept;

private:
	void _WaitForInitialize();

	std::thread _thread;
	std::vector<EffectInfo> _effects;
	std::unordered_map<std::string, size_t> _effectsMap;
};

}
