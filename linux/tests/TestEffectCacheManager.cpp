#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstring>
#include <fmt/format.h>
#include "EffectCacheManager.h"
#include "EffectCompiler.h"
#include "Paths.h"
#include "TestHelpers.h"

using namespace Magpie;

static EffectDesc MakeDesc() {
	EffectDesc desc;
	desc.name = "Sub\\Effect";
	desc.flags = EffectFlags::SupportFP16 | EffectFlags::FP16;

	EffectParameterDesc& floatParam = desc.params.emplace_back();
	floatParam.name = "strength";
	floatParam.label = "Strength";
	floatParam.constant = EffectConstant<float>{ 0.5f, 0.0f, 1.0f, 0.01f };
	EffectParameterDesc& intParam = desc.params.emplace_back();
	intParam.name = "radius";
	intParam.label = "半径";
	intParam.constant = EffectConstant<int>{ 2, 1, 4, 1 };

	desc.textures.resize(3);
	desc.textures[0].name = "INPUT";
	desc.textures[0].format = EffectIntermediateTextureFormat::R8G8B8A8_UNORM;
	desc.textures[1].name = "OUTPUT";
	desc.textures[1].sizeExpr = { "INPUT_WIDTH * 2", "INPUT_HEIGHT * 2" };
	desc.textures[2].name = "lut";
	desc.textures[2].source = "lut.dds";

	EffectSamplerDesc& sampler = desc.samplers.emplace_back();
	sampler.name = "sam";
	sampler.filterType = EffectSamplerFilterType::Point;
	sampler.addressType = EffectSamplerAddressType::Wrap;

	EffectPassDesc& pass = desc.passes.emplace_back();
	pass.spirv = { 0x07230203, 1, 2, 3, 4, 5 };
	pass.inputs = { 0, 2 };
	pass.outputs = { 1 };
	pass.numThreads = { 8, 8, 1 };
	pass.blockSize = { 16, 16 };
	pass.desc = "Only pass";
	pass.flags = EffectPassFlags::PSStyle;
	return desc;
}

static void CheckEqual(const EffectDesc& a, const EffectDesc& b) {
	CHECK(a.name == b.name);
	CHECK(a.flags == b.flags);

	REQUIRE(a.params.size() == b.params.size());
	for (size_t i = 0; i < a.params.size(); ++i) {
		CHECK(a.params[i].name == b.params[i].name);
		CHECK(a.params[i].label == b.params[i].label);
		REQUIRE(a.params[i].constant.index() == b.params[i].constant.index());
		if (a.params[i].constant.index() == 0) {
			const auto& x = std::get<0>(a.params[i].constant);
			const auto& y = std::get<0>(b.params[i].constant);
			CHECK((x.defaultValue == y.defaultValue && x.minValue == y.minValue &&
				x.maxValue == y.maxValue && x.step == y.step));
		} else {
			const auto& x = std::get<1>(a.params[i].constant);
			const auto& y = std::get<1>(b.params[i].constant);
			CHECK((x.defaultValue == y.defaultValue && x.minValue == y.minValue &&
				x.maxValue == y.maxValue && x.step == y.step));
		}
	}

	REQUIRE(a.textures.size() == b.textures.size());
	for (size_t i = 0; i < a.textures.size(); ++i) {
		CHECK(a.textures[i].name == b.textures[i].name);
		CHECK(a.textures[i].format == b.textures[i].format);
		CHECK(a.textures[i].source == b.textures[i].source);
		CHECK(a.textures[i].sizeExpr == b.textures[i].sizeExpr);
	}

	REQUIRE(a.samplers.size() == b.samplers.size());
	for (size_t i = 0; i < a.samplers.size(); ++i) {
		CHECK(a.samplers[i].name == b.samplers[i].name);
		CHECK(a.samplers[i].filterType == b.samplers[i].filterType);
		CHECK(a.samplers[i].addressType == b.samplers[i].addressType);
	}

	REQUIRE(a.passes.size() == b.passes.size());
	for (size_t i = 0; i < a.passes.size(); ++i) {
		CHECK(a.passes[i].spirv == b.passes[i].spirv);
		CHECK(a.passes[i].inputs == b.passes[i].inputs);
		CHECK(a.passes[i].outputs == b.passes[i].outputs);
		CHECK(a.passes[i].numThreads == b.passes[i].numThreads);
		CHECK(a.passes[i].blockSize == b.passes[i].blockSize);
		CHECK(a.passes[i].desc == b.passes[i].desc);
		CHECK(a.passes[i].flags == b.passes[i].flags);
	}
}

static std::vector<std::string> CacheFiles() {
	std::vector<std::string> names;
	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(Paths::CacheDir() / "effects", ec)) {
		names.push_back(entry.path().filename().string());
	}
	std::sort(names.begin(), names.end());
	return names;
}

TEST_CASE("A saved effect is read back from the file", "[EffectCacheManager]") {
	TempDir dir;
	ScopedEnv cache("XDG_CACHE_HOME", dir.Path().c_str());

	const EffectDesc saved = MakeDesc();
	const std::string key = "source text\ncompiler=1\n";
	const uint64_t hash = EffectCacheManager::GetHash(key);

	EffectCacheManager writer;
	writer.Save("Sub\\Effect", 1, hash, key, saved);
	CHECK(CacheFiles() == std::vector<std::string>{ fmt::format("Sub#Effect_0001_{:016x}", hash) });

	// Another instance has nothing in memory and must read the file
	EffectCacheManager reader;
	EffectDesc loaded;
	REQUIRE(reader.Load("Sub\\Effect", 1, hash, key, loaded));
	CheckEqual(saved, loaded);

	// From memory the second time
	EffectDesc again;
	REQUIRE(reader.Load("Sub\\Effect", 1, hash, key, again));
	CheckEqual(saved, again);
}

TEST_CASE("An entry is only used for the same key, flags and name", "[EffectCacheManager]") {
	TempDir dir;
	ScopedEnv cache("XDG_CACHE_HOME", dir.Path().c_str());

	const std::string key = "source";
	const uint64_t hash = EffectCacheManager::GetHash(key);
	EffectCacheManager writer;
	writer.Save("Effect", 0, hash, key, MakeDesc());

	EffectCacheManager reader;
	EffectDesc desc;
	// Same hash, different key: a hash collision must not return the entry
	CHECK_FALSE(reader.Load("Effect", 0, hash, "another source", desc));
	CHECK_FALSE(reader.Load("Effect", 2, hash, key, desc));
	CHECK_FALSE(reader.Load("Other", 0, hash, key, desc));
	CHECK_FALSE(reader.Load("Effect", 0, hash + 1, key, desc));
	CHECK(reader.Load("Effect", 0, hash, key, desc));
}

TEST_CASE("Saving replaces older entries with the same flags only", "[EffectCacheManager]") {
	TempDir dir;
	ScopedEnv cache("XDG_CACHE_HOME", dir.Path().c_str());

	EffectCacheManager manager;
	const EffectDesc desc = MakeDesc();
	manager.Save("Effect", 0, 0x1111, "v1", desc);
	manager.Save("Effect", 2, 0x2222, "v1 other flags", desc);
	manager.Save("Effect_x2", 0, 0x3333, "another effect whose name starts the same", desc);
	manager.Save("Effect", 0, 0x4444, "v2", desc);

	CHECK(CacheFiles() == std::vector<std::string>{
		"Effect_0000_0000000000004444",
		"Effect_0002_0000000000002222",
		"Effect_x2_0000_0000000000003333",
	});
}

TEST_CASE("Damaged cache files are ignored", "[EffectCacheManager]") {
	TempDir dir;
	ScopedEnv cache("XDG_CACHE_HOME", dir.Path().c_str());

	const std::string key = "source";
	const uint64_t hash = EffectCacheManager::GetHash(key);
	EffectCacheManager writer;
	writer.Save("Effect", 0, hash, key, MakeDesc());

	const std::filesystem::path file = Paths::CacheDir() / "effects" / CacheFiles().at(0);
	const std::string content = ReadFile(file);
	REQUIRE(content.size() > 64);

	auto loadWith = [&](const std::string& newContent) {
		std::ofstream(file, std::ios::binary | std::ios::trunc) << newContent;
		EffectCacheManager reader;
		EffectDesc desc;
		return reader.Load("Effect", 0, hash, key, desc);
	};

	CHECK(loadWith(content));
	// Cut off at every length: must fail without crashing
	for (size_t length = 0; length < content.size(); length += 7) {
		INFO("length " << length);
		CHECK_FALSE(loadWith(content.substr(0, length)));
	}
	CHECK_FALSE(loadWith(content + "extra"));

	std::string wrongMagic = content;
	wrongMagic[0] = 'X';
	CHECK_FALSE(loadWith(wrongMagic));

	std::string wrongVersion = content;
	wrongVersion[4] = (char)0x7f;
	CHECK_FALSE(loadWith(wrongVersion));

	// A length field that claims more data than the file has
	std::string hugeCount = content;
	const size_t afterKey = 4 + 4 + 4 + key.size();
	hugeCount.replace(afterKey, 4, std::string("\xff\xff\xff\x7f", 4));
	CHECK_FALSE(loadWith(hugeCount));
}

TEST_CASE("The compiler stores its result and finds it again", "[EffectCacheManager]") {
	TempDir cacheDir;
	ScopedEnv cache("XDG_CACHE_HOME", cacheDir.Path().c_str());

	TempDir effectsDir;
	const std::string source = R"(//!MAGPIE EFFECT
//!VERSION 4

//!TEXTURE
Texture2D INPUT;

//!TEXTURE
Texture2D OUTPUT;

//!SAMPLER
//!FILTER POINT
SamplerState sam;

//!PASS 1
//!STYLE PS
//!IN INPUT
//!OUT OUTPUT
float4 Pass1(float2 pos) {
	return INPUT.SampleLevel(sam, pos, 0);
}
)";
	// A name no other test uses: the compiler's cache instance is shared
	const std::filesystem::path file = effectsDir.Path() / "CacheRoundTrip.hlsl";
	std::ofstream(file, std::ios::binary) << source;

	EffectDesc first;
	first.name = "CacheRoundTrip";
	REQUIRE(EffectCompiler::Compile(first, EffectCompilerFlags::None, effectsDir.Path()) == 0);
	const std::vector<std::string> filesAfterFirst = CacheFiles();
	REQUIRE(filesAfterFirst.size() == 1);
	CHECK(filesAfterFirst[0].starts_with("CacheRoundTrip_0000_"));

	// The entry on disk holds what the compiler produced
	EffectCacheManager reader;
	const uint64_t hash = std::stoull(filesAfterFirst[0].substr(std::strlen("CacheRoundTrip_0000_")), nullptr, 16);
	std::string key = ReadFile(Paths::CacheDir() / "effects" / filesAfterFirst[0]);
	const uint32_t keySize = *(const uint32_t*)(key.data() + 8);
	key = key.substr(12, keySize);
	EffectDesc cached;
	REQUIRE(reader.Load("CacheRoundTrip", 0, hash, key, cached));
	CheckEqual(first, cached);

	// Other flags get their own entry
	EffectDesc inlined;
	inlined.name = "CacheRoundTrip";
	const std::map<std::string, float> noParams;
	REQUIRE(EffectCompiler::Compile(inlined, EffectCompilerFlags::InlineParams, effectsDir.Path(), &noParams) == 0);
	CHECK(CacheFiles().size() == 2);

	// A changed source replaces the entry
	std::ofstream(file, std::ios::binary) << "// changed\n" << source;
	EffectDesc second;
	second.name = "CacheRoundTrip";
	REQUIRE(EffectCompiler::Compile(second, EffectCompilerFlags::None, effectsDir.Path()) == 0);
	const std::vector<std::string> filesAfterSecond = CacheFiles();
	REQUIRE(filesAfterSecond.size() == 2);
	CHECK(std::find(filesAfterSecond.begin(), filesAfterSecond.end(), filesAfterFirst[0]) == filesAfterSecond.end());
}
