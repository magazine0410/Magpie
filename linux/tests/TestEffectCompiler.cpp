#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include "EffectCompiler.h"
#include "EffectDesc.h"
#include "EffectHelper.h"
#include "SpirvHelpers.h"
#include "TestHelpers.h"

using namespace Magpie;

// A small effect with everything the generator has to declare: two
// parameters, an intermediate texture, two samplers, a shared block with an
// #include, a PS-style pass and a CS-style pass.
static const char* TEST_EFFECT = R"(// A comment before the header
//!MAGPIE EFFECT
//!VERSION 4
//!CAPABILITY FP16
//!SORT_NAME test

//!PARAMETER
//!LABEL Strength
//!DEFAULT 0.5
//!MIN 0
//!MAX 1
//!STEP 0.01
float strength;

//!PARAMETER
//!LABEL Radius
//!DEFAULT 2
//!MIN 1
//!MAX 4
//!STEP 1
int radius;

//!TEXTURE
Texture2D INPUT;

//!TEXTURE
//!WIDTH INPUT_WIDTH * 2
//!HEIGHT INPUT_HEIGHT * 2
Texture2D OUTPUT;

//!TEXTURE
//!WIDTH INPUT_WIDTH
//!HEIGHT INPUT_HEIGHT
//!FORMAT R16G16B16A16_FLOAT
Texture2D tex1;

//!SAMPLER
//!FILTER LINEAR
SamplerState sam;

//!SAMPLER
//!FILTER POINT
//!ADDRESS WRAP
SamplerState samPoint;

//!COMMON
#include "Tint.hlsli"

//!PASS 1
//!DESC First
//!STYLE PS
//!IN INPUT
//!OUT tex1

MF4 Pass1(float2 pos) {
	return Tint(INPUT.SampleLevel(sam, pos, 0), strength);
}

//!PASS 2
//!IN INPUT, tex1
//!OUT OUTPUT
//!BLOCK_SIZE 8
//!NUM_THREADS 8, 8

void Pass2(uint2 blockStart, uint3 threadId) {
	const uint2 gxy = blockStart + threadId.xy;
	const float2 pos = (gxy + 0.5f) * GetOutputPt();
	MF4 color = tex1.SampleLevel(samPoint, pos, 0) * radius;
	OUTPUT[gxy] = color + INPUT.SampleLevel(sam, pos, 0);
}
)";

static const char* TINT_INCLUDE = R"(
MF4 Tint(MF4 color, float amount) {
	return color * MF(amount);
}
)";

// An effects directory holding the test effect as "Sub\Test".
struct EffectsDir {
	EffectsDir() {
		std::filesystem::create_directories(dir.Path() / "Sub");
		Write("Sub/Test.hlsl", TEST_EFFECT);
		Write("Sub/Tint.hlsli", TINT_INCLUDE);
	}

	void Write(const char* relativePath, std::string_view text) const {
		std::ofstream(dir.Path() / relativePath, std::ios::binary) << text;
	}

	uint32_t Compile(EffectDesc& desc, EffectCompilerFlags flags,
		const std::map<std::string, float>* inlineParams = nullptr, const char* name = "Sub\\Test") const {
		desc.name = name;
		return EffectCompiler::Compile(desc, flags | EffectCompilerFlags::NoCache, dir.Path(), inlineParams);
	}

	TempDir dir;
};

TEST_CASE("An effect is parsed into its description", "[EffectCompiler]") {
	EffectsDir effects;
	EffectDesc desc;
	REQUIRE(effects.Compile(desc, EffectCompilerFlags::None) == 0);

	CHECK((desc.flags & EffectFlags::SupportFP16) != 0);
	CHECK((desc.flags & EffectFlags::FP16) != 0);

	REQUIRE(desc.params.size() == 2);
	CHECK(desc.params[0].name == "strength");
	CHECK(desc.params[0].label == "Strength");
	REQUIRE(desc.params[0].constant.index() == 0);
	CHECK(std::get<0>(desc.params[0].constant).defaultValue == 0.5f);
	CHECK(std::get<0>(desc.params[0].constant).maxValue == 1.0f);
	CHECK(desc.params[1].name == "radius");
	REQUIRE(desc.params[1].constant.index() == 1);
	CHECK(std::get<1>(desc.params[1].constant).defaultValue == 2);
	CHECK(std::get<1>(desc.params[1].constant).maxValue == 4);

	REQUIRE(desc.textures.size() == 3);
	CHECK(desc.textures[0].name == "INPUT");
	CHECK(desc.textures[1].name == "OUTPUT");
	// Size expressions are stored without white space
	CHECK(desc.GetOutputSizeExpr() == std::pair<std::string, std::string>("INPUT_WIDTH*2", "INPUT_HEIGHT*2"));
	CHECK(desc.textures[2].name == "tex1");
	CHECK(desc.textures[2].format == EffectIntermediateTextureFormat::R16G16B16A16_FLOAT);

	REQUIRE(desc.samplers.size() == 2);
	CHECK(desc.samplers[0].filterType == EffectSamplerFilterType::Linear);
	CHECK(desc.samplers[1].filterType == EffectSamplerFilterType::Point);
	CHECK(desc.samplers[1].addressType == EffectSamplerAddressType::Wrap);

	REQUIRE(desc.passes.size() == 2);
	CHECK(desc.passes[0].desc == "First");
	CHECK((desc.passes[0].flags & EffectPassFlags::PSStyle) != 0);
	CHECK(desc.passes[0].inputs.size() == 1);
	CHECK(desc.passes[0].outputs.size() == 1);
	CHECK(desc.passes[1].flags == 0);
	CHECK(desc.passes[1].inputs.size() == 2);
	CHECK(desc.passes[1].blockSize == std::pair<uint32_t, uint32_t>(8, 8));
	CHECK(desc.passes[1].numThreads == std::array<uint32_t, 3>{ 8, 8, 1 });
}

TEST_CASE("The compiled passes use the bindings of EffectHelper", "[EffectCompiler]") {
	EffectsDir effects;
	EffectDesc desc;
	REQUIRE(effects.Compile(desc, EffectCompilerFlags::None) == 0);

	// Pass 1: one input, one output
	const SpirvInfo pass1 = ParseSpirv(desc.passes[0].spirv);
	REQUIRE(pass1.valid);
	CHECK(pass1.bindings.at("__CB1") == EffectHelper::BINDING_CB1);
	CHECK(pass1.bindings.at("INPUT") == EffectHelper::InputBinding(0));
	CHECK(pass1.bindings.at("tex1") == EffectHelper::OutputBinding(1, 0));
	CHECK(pass1.bindings.at("sam") == EffectHelper::SamplerBinding(1, 1, 0));
	// The output is R16G16B16A16_FLOAT: SPIR-V image format Rgba16f
	REQUIRE(pass1.storageImageFormats.size() == 1);
	CHECK(pass1.storageImageFormats[0] == 2);

	// Pass 2: two inputs, one output, both samplers
	const SpirvInfo pass2 = ParseSpirv(desc.passes[1].spirv);
	REQUIRE(pass2.valid);
	CHECK(pass2.bindings.at("__CB1") == EffectHelper::BINDING_CB1);
	CHECK(pass2.bindings.at("INPUT") == EffectHelper::InputBinding(0));
	CHECK(pass2.bindings.at("tex1") == EffectHelper::InputBinding(1));
	CHECK(pass2.bindings.at("OUTPUT") == EffectHelper::OutputBinding(2, 0));
	CHECK(pass2.bindings.at("sam") == EffectHelper::SamplerBinding(2, 1, 0));
	CHECK(pass2.bindings.at("samPoint") == EffectHelper::SamplerBinding(2, 1, 1));
	// OUTPUT is R8G8B8A8_UNORM: SPIR-V image format Rgba8
	REQUIRE(pass2.storageImageFormats.size() == 1);
	CHECK(pass2.storageImageFormats[0] == 4);
}

TEST_CASE("The FP16 flags select the float width", "[EffectCompiler]") {
	EffectsDir effects;

	EffectDesc relaxed;
	REQUIRE(effects.Compile(relaxed, EffectCompilerFlags::None) == 0);
	CHECK_FALSE(ParseSpirv(relaxed.passes[0].spirv).capabilities.contains(Spirv::CAPABILITY_FLOAT16));

	EffectDesc native;
	REQUIRE(effects.Compile(native, EffectCompilerFlags::NativeFP16) == 0);
	CHECK((native.flags & EffectFlags::FP16) != 0);
	CHECK(ParseSpirv(native.passes[0].spirv).capabilities.contains(Spirv::CAPABILITY_FLOAT16));

	// NoFP16 wins over NativeFP16
	EffectDesc disabled;
	REQUIRE(effects.Compile(disabled, EffectCompilerFlags::NoFP16 | EffectCompilerFlags::NativeFP16) == 0);
	CHECK((disabled.flags & EffectFlags::FP16) == 0);
	CHECK_FALSE(ParseSpirv(disabled.passes[0].spirv).capabilities.contains(Spirv::CAPABILITY_FLOAT16));
}

TEST_CASE("NoCompile parses parameters and sizes without compiling", "[EffectCompiler]") {
	EffectsDir effects;
	EffectDesc desc;
	REQUIRE(effects.Compile(desc, EffectCompilerFlags::NoCompile) == 0);

	CHECK(desc.params.size() == 2);
	CHECK(desc.GetOutputSizeExpr().first == "INPUT_WIDTH*2");
	// The sort name is only kept in this mode, which the user interface uses
	CHECK(desc.sortName == "test");
	CHECK(desc.passes.empty());
}

TEST_CASE("Parameters can be compiled into the shader", "[EffectCompiler]") {
	EffectsDir effects;

	const std::map<std::string, float> values{ { "strength", 0.75f }, { "radius", 3.0f } };
	EffectDesc desc;
	REQUIRE(effects.Compile(desc, EffectCompilerFlags::InlineParams, &values) == 0);
	CHECK((desc.flags & EffectFlags::InlineParams) != 0);
	CHECK(desc.passes.size() == 2);

	// A name the effect does not have is an error
	const std::map<std::string, float> unknown{ { "sharpness", 1.0f } };
	EffectDesc rejected;
	CHECK(effects.Compile(rejected, EffectCompilerFlags::InlineParams, &unknown) != 0);
}

TEST_CASE("Broken effects are rejected", "[EffectCompiler]") {
	EffectsDir effects;
	EffectDesc desc;

	SECTION("missing file") {
		CHECK(effects.Compile(desc, EffectCompilerFlags::None, nullptr, "Sub\\Missing") != 0);
	}

	SECTION("no MagpieFX header") {
		effects.Write("Sub/Plain.hlsl", "float4 main() : SV_Target { return 0; }\n");
		CHECK(effects.Compile(desc, EffectCompilerFlags::None, nullptr, "Sub\\Plain") != 0);
	}

	SECTION("shader error in a pass") {
		std::string source = TEST_EFFECT;
		source.replace(source.find("return Tint("), 12, "return Missing(");
		effects.Write("Sub/Broken.hlsl", source);
		CHECK(effects.Compile(desc, EffectCompilerFlags::None, nullptr, "Sub\\Broken") != 0);
	}

	SECTION("the same name declared twice") {
		std::string source = TEST_EFFECT;
		source.replace(source.find("SamplerState samPoint;"), 22, "SamplerState sam;");
		effects.Write("Sub/Duplicate.hlsl", source);
		CHECK(effects.Compile(desc, EffectCompilerFlags::None, nullptr, "Sub\\Duplicate") != 0);
	}

	SECTION("the last pass does not write OUTPUT") {
		std::string source = TEST_EFFECT;
		source.replace(source.find("//!OUT OUTPUT"), 13, "//!OUT tex1");
		effects.Write("Sub/NoOutput.hlsl", source);
		CHECK(effects.Compile(desc, EffectCompilerFlags::None, nullptr, "Sub\\NoOutput") != 0);
	}

	SECTION("no pass") {
		const std::string source(TEST_EFFECT, std::strstr(TEST_EFFECT, "//!PASS 1") - TEST_EFFECT);
		effects.Write("Sub/NoPass.hlsl", source);
		CHECK(effects.Compile(desc, EffectCompilerFlags::None, nullptr, "Sub\\NoPass") != 0);
	}
}

TEST_CASE("A bundled effect compiles", "[EffectCompiler]") {
	EffectDesc desc;
	desc.name = "FSR\\FSR_EASU";
	REQUIRE(EffectCompiler::Compile(desc, EffectCompilerFlags::NoCache, MAGPIE_EFFECTS_DIR) == 0);
	REQUIRE(desc.passes.size() == 1);
	CHECK(ParseSpirv(desc.passes[0].spirv).valid);
}
