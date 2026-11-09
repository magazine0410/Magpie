#include "EffectCacheManager.h"
#include <cassert>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <fmt/format.h>
#include "Logger.h"
#include "Paths.h"

namespace Magpie {

// Raise when the layout of a cache file or the generated pass source changes,
// to invalidate old files.
static constexpr uint32_t EFFECT_CACHE_VERSION = 1;
static constexpr char MAGIC[4] = { 'M', 'P', 'F', 'X' };
static constexpr size_t MAX_MEM_CACHE_COUNT = 64;

////////////////////////////////////////////////////////////////////////////////
// Serialisation: fixed-width little-endian integers, strings and arrays with a
// 32-bit length in front.
////////////////////////////////////////////////////////////////////////////////

class Writer {
public:
	void U32(uint32_t value) {
		_buffer.append((const char*)&value, sizeof(value));
	}

	void String(std::string_view value) {
		U32((uint32_t)value.size());
		_buffer.append(value);
	}

	template <typename Container>
	void U32Array(const Container& values) {
		U32((uint32_t)values.size());
		for (uint32_t value : values) {
			U32(value);
		}
	}

	std::string& Buffer() {
		return _buffer;
	}

private:
	std::string _buffer;
};

// Every read checks the remaining size; after a failed read all further reads
// fail and Ok() is false.
class Reader {
public:
	explicit Reader(std::string_view data) : _data(data) {}

	bool Ok() const {
		return _ok;
	}

	bool AtEnd() const {
		return _data.empty();
	}

	uint32_t U32() {
		uint32_t value = 0;
		if (!_Take(sizeof(value))) {
			return 0;
		}
		std::memcpy(&value, _taken, sizeof(value));
		return value;
	}

	std::string String() {
		const uint32_t size = U32();
		if (!_Take(size)) {
			return {};
		}
		return std::string(_taken, size);
	}

	// Number of elements of an array whose elements take at least
	// `minElementSize` bytes each. Guards against allocating for a length that
	// the file cannot hold.
	uint32_t Count(size_t minElementSize) {
		const uint32_t count = U32();
		if (_ok && (size_t)count * minElementSize > _data.size()) {
			_ok = false;
			return 0;
		}
		return count;
	}

	template <typename Container>
	void U32Array(Container& values) {
		const uint32_t count = Count(sizeof(uint32_t));
		values.resize(count);
		for (uint32_t i = 0; i < count; ++i) {
			values[i] = U32();
		}
	}

private:
	bool _Take(size_t size) {
		if (!_ok || size > _data.size()) {
			_ok = false;
			return false;
		}
		_taken = _data.data();
		_data.remove_prefix(size);
		return true;
	}

	std::string_view _data;
	const char* _taken = nullptr;
	bool _ok = true;
};

static void WriteDesc(Writer& w, const EffectDesc& desc) {
	w.String(desc.name);
	w.U32(desc.flags);

	w.U32((uint32_t)desc.params.size());
	for (const EffectParameterDesc& param : desc.params) {
		w.String(param.name);
		w.String(param.label);
		w.U32((uint32_t)param.constant.index());
		// Both alternatives are four 32-bit values
		uint32_t raw[4];
		if (param.constant.index() == 0) {
			std::memcpy(raw, &std::get<0>(param.constant), sizeof(raw));
		} else {
			std::memcpy(raw, &std::get<1>(param.constant), sizeof(raw));
		}
		for (uint32_t value : raw) {
			w.U32(value);
		}
	}

	w.U32((uint32_t)desc.textures.size());
	for (const EffectIntermediateTextureDesc& texture : desc.textures) {
		w.U32((uint32_t)texture.format);
		w.String(texture.name);
		w.String(texture.source);
		w.String(texture.sizeExpr.first);
		w.String(texture.sizeExpr.second);
	}

	w.U32((uint32_t)desc.samplers.size());
	for (const EffectSamplerDesc& sampler : desc.samplers) {
		w.U32((uint32_t)sampler.filterType);
		w.U32((uint32_t)sampler.addressType);
		w.String(sampler.name);
	}

	w.U32((uint32_t)desc.passes.size());
	for (const EffectPassDesc& pass : desc.passes) {
		w.U32Array(pass.spirv);
		w.U32Array(pass.inputs);
		w.U32Array(pass.outputs);
		w.U32(pass.numThreads[0]);
		w.U32(pass.numThreads[1]);
		w.U32(pass.numThreads[2]);
		w.U32(pass.blockSize.first);
		w.U32(pass.blockSize.second);
		w.String(pass.desc);
		w.U32(pass.flags);
	}
}

static bool ReadDesc(Reader& r, EffectDesc& desc) {
	desc.name = r.String();
	desc.flags = r.U32();

	desc.params.resize(r.Count(4 * 7));
	for (EffectParameterDesc& param : desc.params) {
		param.name = r.String();
		param.label = r.String();
		const uint32_t index = r.U32();
		uint32_t raw[4];
		for (uint32_t& value : raw) {
			value = r.U32();
		}

		if (index == 0) {
			EffectConstant<float> constant;
			std::memcpy(&constant, raw, sizeof(raw));
			param.constant = constant;
		} else if (index == 1) {
			EffectConstant<int> constant;
			std::memcpy(&constant, raw, sizeof(raw));
			param.constant = constant;
		} else {
			return false;
		}
	}

	desc.textures.resize(r.Count(4 * 5));
	for (EffectIntermediateTextureDesc& texture : desc.textures) {
		const uint32_t format = r.U32();
		if (format > (uint32_t)EffectIntermediateTextureFormat::UNKNOWN) {
			return false;
		}
		texture.format = (EffectIntermediateTextureFormat)format;
		texture.name = r.String();
		texture.source = r.String();
		texture.sizeExpr.first = r.String();
		texture.sizeExpr.second = r.String();
	}

	desc.samplers.resize(r.Count(4 * 3));
	for (EffectSamplerDesc& sampler : desc.samplers) {
		const uint32_t filterType = r.U32();
		const uint32_t addressType = r.U32();
		if (filterType > (uint32_t)EffectSamplerFilterType::Point ||
			addressType > (uint32_t)EffectSamplerAddressType::Wrap) {
			return false;
		}
		sampler.filterType = (EffectSamplerFilterType)filterType;
		sampler.addressType = (EffectSamplerAddressType)addressType;
		sampler.name = r.String();
	}

	desc.passes.resize(r.Count(4 * 10));
	for (EffectPassDesc& pass : desc.passes) {
		r.U32Array(pass.spirv);
		r.U32Array(pass.inputs);
		r.U32Array(pass.outputs);
		pass.numThreads[0] = r.U32();
		pass.numThreads[1] = r.U32();
		pass.numThreads[2] = r.U32();
		pass.blockSize.first = r.U32();
		pass.blockSize.second = r.U32();
		pass.desc = r.String();
		pass.flags = r.U32();
	}

	return r.Ok() && r.AtEnd();
}

////////////////////////////////////////////////////////////////////////////////
// Files
////////////////////////////////////////////////////////////////////////////////

static std::filesystem::path CacheDir() noexcept {
	return Paths::CacheDir() / "effects";
}

// "FSR\FSR_EASU" -> "FSR#FSR_EASU": the cache has no subdirectories.
static std::string GetLinearEffectName(std::string_view effectName) noexcept {
	std::string result(effectName);
	for (char& c : result) {
		if (c == '\\' || c == '/') {
			c = '#';
		}
	}
	return result;
}

// {effect name}_{flags, 4 digits}_{hash, 16 digits}
static std::string GetCacheFileName(std::string_view linearEffectName, uint32_t flags, uint64_t hash) noexcept {
	assert(flags <= 0xFFFF);
	return fmt::format("{}_{:04x}_{:016x}", linearEffectName, flags, hash);
}

static bool IsLowerHex(std::string_view text) noexcept {
	for (char c : text) {
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			return false;
		}
	}
	return true;
}

const EffectDesc* EffectCacheManager::_FindInMemory(const std::string& cacheFileName) noexcept {
	for (auto it = _memCache.begin(); it != _memCache.end(); ++it) {
		if (it->first == cacheFileName) {
			_memCache.splice(_memCache.begin(), _memCache, it);
			return &_memCache.front().second;
		}
	}
	return nullptr;
}

void EffectCacheManager::_AddToMemory(const std::string& cacheFileName, const EffectDesc& desc) noexcept {
	_memCache.remove_if([&](const auto& entry) { return entry.first == cacheFileName; });
	_memCache.emplace_front(cacheFileName, desc);
	if (_memCache.size() > MAX_MEM_CACHE_COUNT) {
		_memCache.pop_back();
	}
}

bool EffectCacheManager::Load(
	std::string_view effectName,
	uint32_t flags,
	uint64_t hash,
	std::string_view key,
	EffectDesc& desc
) noexcept {
	assert(!effectName.empty() && !key.empty());

	const std::string cacheFileName = GetCacheFileName(GetLinearEffectName(effectName), flags, hash);

	{
		std::scoped_lock lk(_lock);
		if (const EffectDesc* cache = _FindInMemory(cacheFileName)) {
			desc = *cache;
			return true;
		}
	}

	std::string content;
	{
		std::ifstream file(CacheDir() / cacheFileName, std::ios::binary);
		if (!file) {
			return false;
		}
		std::ostringstream stream;
		stream << file.rdbuf();
		content = std::move(stream).str();
	}

	Reader reader(content);
	if (content.size() < sizeof(MAGIC) || std::memcmp(content.data(), MAGIC, sizeof(MAGIC)) != 0) {
		Logger::Get().Info("Not an effect cache file");
		return false;
	}
	reader.U32();	// the magic, checked above

	if (reader.U32() != EFFECT_CACHE_VERSION) {
		Logger::Get().Info("The effect cache file has another version");
		return false;
	}

	if (reader.String() != key) {
		Logger::Get().Info("The effect cache key does not match");
		return false;
	}

	EffectDesc cached;
	if (!ReadDesc(reader, cached)) {
		Logger::Get().Error("The effect cache file is damaged");
		return false;
	}

	desc = std::move(cached);

	std::scoped_lock lk(_lock);
	_AddToMemory(cacheFileName, desc);
	return true;
}

void EffectCacheManager::Save(
	std::string_view effectName,
	uint32_t flags,
	uint64_t hash,
	std::string_view key,
	const EffectDesc& desc
) noexcept {
	const std::string linearEffectName = GetLinearEffectName(effectName);
	const std::string cacheFileName = GetCacheFileName(linearEffectName, flags, hash);
	const std::filesystem::path cacheDir = CacheDir();

	Writer writer;
	writer.Buffer().append(MAGIC, sizeof(MAGIC));
	writer.U32(EFFECT_CACHE_VERSION);
	writer.String(key);
	WriteDesc(writer, desc);

	std::error_code ec;
	std::filesystem::create_directories(cacheDir, ec);
	if (ec) {
		Logger::Get().ErrnoError("Cannot create the effect cache directory", ec.value());
		return;
	}

	// Remove older entries of this effect with the same flags. Entries with
	// other flags stay.
	const std::string prefix = fmt::format("{}_{:04x}_", linearEffectName, flags);
	for (const auto& entry : std::filesystem::directory_iterator(cacheDir, ec)) {
		const std::string fileName = entry.path().filename().string();
		if (fileName == cacheFileName || fileName.size() != prefix.size() + 16 ||
			!fileName.starts_with(prefix) || !IsLowerHex(std::string_view(fileName).substr(prefix.size()))) {
			continue;
		}

		std::error_code removeError;
		if (!std::filesystem::remove(entry.path(), removeError)) {
			Logger::Get().ErrnoError(fmt::format("Cannot remove the cache file {}", fileName), removeError.value());
		}
	}

	// Write to a temporary name first: another instance may be reading.
	const std::filesystem::path target = cacheDir / cacheFileName;
	std::filesystem::path temporary = target;
	temporary += fmt::format(".{}.tmp", getpid());
	{
		std::ofstream file(temporary, std::ios::binary);
		file.write(writer.Buffer().data(), (std::streamsize)writer.Buffer().size());
		if (!file.good()) {
			Logger::Get().ErrnoError("Cannot write the effect cache file");
			file.close();
			std::filesystem::remove(temporary, ec);
			return;
		}
	}
	std::filesystem::rename(temporary, target, ec);
	if (ec) {
		Logger::Get().ErrnoError("Cannot store the effect cache file", ec.value());
		std::filesystem::remove(temporary, ec);
		return;
	}

	std::scoped_lock lk(_lock);
	_AddToMemory(cacheFileName, desc);
}

// FNV-1a. The hash only names the file; the full key is compared on load.
uint64_t EffectCacheManager::GetHash(std::string_view key) noexcept {
	uint64_t hash = 0xcbf29ce484222325ull;
	for (unsigned char c : key) {
		hash ^= c;
		hash *= 0x100000001b3ull;
	}
	return hash;
}

}
