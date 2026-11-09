// Stand-in: the spike never uses the effect cache.
#pragma once
#include "EffectDesc.h"

namespace Magpie {
class EffectCacheManager {
public:
	static EffectCacheManager& Get() noexcept {
		static EffectCacheManager instance;
		return instance;
	}
	bool Load(std::wstring_view, uint32_t, uint64_t, std::string_view, EffectDesc&) { return false; }
	void Save(std::wstring_view, uint32_t, uint64_t, std::string, const EffectDesc&) {}
	static uint64_t GetHash(std::string_view) { return 0; }
};
}
