#include "EffectsService.h"
#include <algorithm>
#include "EffectCompiler.h"
#include "Logger.h"

namespace Magpie {

EffectsService::~EffectsService() {
	_WaitForInitialize();
}

static std::vector<std::string> ListEffects(const std::filesystem::path& effectsDir) {
	std::vector<std::string> names;
	std::error_code error;
	for (std::filesystem::recursive_directory_iterator it(effectsDir, error), end; !error && it != end; it.increment(error)) {
		if (!it->is_regular_file(error) || it->path().extension() != ".hlsl") {
			continue;
		}

		std::string name = it->path().lexically_relative(effectsDir).replace_extension().generic_string();
		std::ranges::replace(name, '/', '\\');
		names.push_back(std::move(name));
	}
	if (error) {
		Logger::Get().Error("The effects directory cannot be read: " + effectsDir.string());
	}
	std::ranges::sort(names);
	return names;
}

void EffectsService::Initialize(const std::filesystem::path& effectsDir) {
	_thread = std::thread([this, effectsDir] {
		for (std::string& name : ListEffects(effectsDir)) {
			EffectDesc desc;
			desc.name = name;
			if (EffectCompiler::Compile(desc, EffectCompilerFlags::NoCompile, effectsDir) != 0) {
				continue;
			}

			EffectInfo& effect = _effects.emplace_back();
			if (desc.sortName.empty()) {
				effect.sortName = name;
			} else {
				// Ordered by this name within its folder
				const size_t pos = name.rfind('\\');
				effect.sortName = (pos == std::string::npos ? std::string() : name.substr(0, pos + 1)) + desc.sortName;
			}
			effect.params = std::move(desc.params);
			effect.canScale = desc.GetOutputSizeExpr().first.empty();
			effect.name = std::move(name);
			_effectsMap.emplace(effect.name, _effects.size() - 1);
		}
	});
}

void EffectsService::_WaitForInitialize() {
	if (_thread.joinable()) {
		_thread.join();
	}
}

const std::vector<EffectInfo>& EffectsService::Effects() {
	_WaitForInitialize();
	return _effects;
}

const EffectInfo* EffectsService::GetEffect(std::string_view name) {
	_WaitForInitialize();
	const auto it = _effectsMap.find(std::string(name));
	return it == _effectsMap.end() ? nullptr : &_effects[it->second];
}

std::string_view EffectsService::DisplayName(std::string_view name) noexcept {
	const size_t pos = name.rfind('\\');
	return pos == std::string_view::npos ? name : name.substr(pos + 1);
}

}
