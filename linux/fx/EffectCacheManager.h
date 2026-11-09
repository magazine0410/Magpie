// Stores compiled effects on disk and in memory, so that an effect is only
// compiled when its source, the compile flags, the inlined parameters or the
// compiler change. Linux counterpart of src/Magpie.Core/EffectCacheManager.h.
#pragma once
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include "EffectDesc.h"

namespace Magpie {

class EffectCacheManager {
public:
	EffectCacheManager() = default;
	EffectCacheManager(const EffectCacheManager&) = delete;
	EffectCacheManager(EffectCacheManager&&) = delete;

	// The instance the effect compiler uses. Separate instances share the
	// files but not the in-memory cache.
	static EffectCacheManager& Get() noexcept {
		static EffectCacheManager instance;
		return instance;
	}

	// `key` is everything the compiled code depends on; `hash` is its hash.
	// A cache entry is used only if its stored key equals `key`.
	bool Load(std::string_view effectName, uint32_t flags, uint64_t hash, std::string_view key, EffectDesc& desc) noexcept;

	// Replaces older entries of the same effect and flags.
	void Save(std::string_view effectName, uint32_t flags, uint64_t hash, std::string_view key, const EffectDesc& desc) noexcept;

	static uint64_t GetHash(std::string_view key) noexcept;

private:
	const EffectDesc* _FindInMemory(const std::string& cacheFileName) noexcept;
	void _AddToMemory(const std::string& cacheFileName, const EffectDesc& desc) noexcept;

	std::mutex _lock;
	// Most recently used first
	std::list<std::pair<std::string, EffectDesc>> _memCache;
};

}
