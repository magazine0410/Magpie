#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include "EffectDesc.h"
#include "EffectHelper.h"
#include "ShaderCompiler.h"
#include "SpirvHelpers.h"
#include "TestHelpers.h"

using namespace Magpie;

static const char* MINIMAL_SHADER = R"(
[[vk::binding(0, 0)]] RWTexture2D<float4> output;

[numthreads(8, 8, 1)]
void __M(uint3 id : SV_DispatchThreadID) {
	output[id.xy] = float4(VALUE, 0, 0, 1);
}
)";

TEST_CASE("A compute shader compiles to SPIR-V", "[ShaderCompiler]") {
	ShaderCompiler::Options options;
	options.macros.emplace_back("VALUE", "0.5");

	std::vector<uint32_t> spirv;
	std::string messages;
	REQUIRE(ShaderCompiler::CompileComputeShader(MINIMAL_SHADER, "__M", "minimal.hlsl", options, spirv, messages));
	CHECK(messages.empty());

	const SpirvInfo info = ParseSpirv(spirv);
	REQUIRE(info.valid);
	CHECK(info.bindings.at("output") == 0);
}

TEST_CASE("A compile error names the source and the line", "[ShaderCompiler]") {
	// VALUE is not defined
	std::vector<uint32_t> spirv;
	std::string messages;
	CHECK_FALSE(ShaderCompiler::CompileComputeShader(MINIMAL_SHADER, "__M", "broken.hlsl", {}, spirv, messages));
	CHECK(messages.find("broken.hlsl:6") != std::string::npos);
	CHECK(messages.find("VALUE") != std::string::npos);
}

TEST_CASE("A macro without a value is defined", "[ShaderCompiler]") {
	const char* shader = R"(
[[vk::binding(0, 0)]] RWTexture2D<float4> output;
[numthreads(1, 1, 1)]
void __M(uint3 id : SV_DispatchThreadID) {
#ifdef FLAG
	output[id.xy] = 1;
#else
	this does not compile
#endif
}
)";
	ShaderCompiler::Options options;
	options.macros.emplace_back("FLAG", "");

	std::vector<uint32_t> spirv;
	std::string messages;
	INFO(messages);
	CHECK(ShaderCompiler::CompileComputeShader(shader, "__M", "flag.hlsl", options, spirv, messages));
}

TEST_CASE("Include files are found in the include directory", "[ShaderCompiler]") {
	TempDir dir;
	// A directory name outside ASCII, as in a user's home directory
	const std::filesystem::path includeDir = dir.Path() / "効果";
	std::filesystem::create_directories(includeDir);
	std::ofstream(includeDir / "value.hlsli") << "#define VALUE 0.25\n";

	const std::string shader = std::string("#include \"value.hlsli\"\n") + MINIMAL_SHADER;

	ShaderCompiler::Options options;
	options.includeDir = includeDir;
	std::vector<uint32_t> spirv;
	std::string messages;
	INFO(messages);
	CHECK(ShaderCompiler::CompileComputeShader(shader, "__M", "include.hlsl", options, spirv, messages));

	options.includeDir = dir.Path();
	CHECK_FALSE(ShaderCompiler::CompileComputeShader(shader, "__M", "include.hlsl", options, spirv, messages));
}

TEST_CASE("min16float is 16 bits wide only with nativeFP16", "[ShaderCompiler]") {
	const char* shader = R"(
[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> output;
[numthreads(1, 1, 1)]
void __M(uint3 id : SV_DispatchThreadID) {
	min16float4 value = min16float4(id.x, id.y, 0, 1) * 0.5;
	output[id.xy] = value * value;
}
)";
	std::vector<uint32_t> spirv;
	std::string messages;

	ShaderCompiler::Options relaxed;
	REQUIRE(ShaderCompiler::CompileComputeShader(shader, "__M", "fp16.hlsl", relaxed, spirv, messages));
	CHECK_FALSE(ParseSpirv(spirv).capabilities.contains(Spirv::CAPABILITY_FLOAT16));

	ShaderCompiler::Options native;
	native.nativeFP16 = true;
	REQUIRE(ShaderCompiler::CompileComputeShader(shader, "__M", "fp16.hlsl", native, spirv, messages));
	CHECK(ParseSpirv(spirv).capabilities.contains(Spirv::CAPABILITY_FLOAT16));
}

TEST_CASE("Warnings fail the compilation only with warningsAreErrors", "[ShaderCompiler]") {
	// Truncating a vector gives a warning
	const char* shader = R"(
[[vk::binding(0, 0)]] RWTexture2D<float4> output;
[numthreads(1, 1, 1)]
void __M(uint3 id : SV_DispatchThreadID) {
	float2 truncated = float4(1, 2, 3, 4);
	output[id.xy] = truncated.xyxy;
}
)";
	std::vector<uint32_t> spirv;
	std::string messages;

	REQUIRE(ShaderCompiler::CompileComputeShader(shader, "__M", "warn.hlsl", {}, spirv, messages));
	CHECK(messages.find("warning") != std::string::npos);

	ShaderCompiler::Options strict;
	strict.warningsAreErrors = true;
	CHECK_FALSE(ShaderCompiler::CompileComputeShader(shader, "__M", "warn.hlsl", strict, spirv, messages));
}

TEST_CASE("The compiler reports its version", "[ShaderCompiler]") {
	const std::string version = ShaderCompiler::Version();
	INFO(version);
	CHECK(version.starts_with("1."));
}

// The image format names in EffectHelper are passed to DXC as text. This
// checks that each one produces the SPIR-V format that belongs to the Vulkan
// format in the same row.
TEST_CASE("Every texture format maps to the matching SPIR-V image format", "[ShaderCompiler]") {
	struct Expected {
		EffectIntermediateTextureFormat format;
		VkFormat vkFormat;
		uint32_t spirvImageFormat;	// enumerant ImageFormat of the SPIR-V specification
	};
	static const Expected EXPECTED[] = {
		{ EffectIntermediateTextureFormat::R32G32B32A32_FLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, 1 },
		{ EffectIntermediateTextureFormat::R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 2 },
		{ EffectIntermediateTextureFormat::R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_UNORM, 10 },
		{ EffectIntermediateTextureFormat::R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_SNORM, 16 },
		{ EffectIntermediateTextureFormat::R32G32_FLOAT, VK_FORMAT_R32G32_SFLOAT, 6 },
		{ EffectIntermediateTextureFormat::R10G10B10A2_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 11 },
		{ EffectIntermediateTextureFormat::R11G11B10_FLOAT, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 8 },
		{ EffectIntermediateTextureFormat::R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 4 },
		{ EffectIntermediateTextureFormat::R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_SNORM, 5 },
		{ EffectIntermediateTextureFormat::R16G16_FLOAT, VK_FORMAT_R16G16_SFLOAT, 7 },
		{ EffectIntermediateTextureFormat::R16G16_UNORM, VK_FORMAT_R16G16_UNORM, 12 },
		{ EffectIntermediateTextureFormat::R16G16_SNORM, VK_FORMAT_R16G16_SNORM, 17 },
		{ EffectIntermediateTextureFormat::R32_FLOAT, VK_FORMAT_R32_SFLOAT, 3 },
		{ EffectIntermediateTextureFormat::R8G8_UNORM, VK_FORMAT_R8G8_UNORM, 13 },
		{ EffectIntermediateTextureFormat::R8G8_SNORM, VK_FORMAT_R8G8_SNORM, 18 },
		{ EffectIntermediateTextureFormat::R16_FLOAT, VK_FORMAT_R16_SFLOAT, 9 },
		{ EffectIntermediateTextureFormat::R16_UNORM, VK_FORMAT_R16_UNORM, 14 },
		{ EffectIntermediateTextureFormat::R16_SNORM, VK_FORMAT_R16_SNORM, 19 },
		{ EffectIntermediateTextureFormat::R8_UNORM, VK_FORMAT_R8_UNORM, 15 },
		{ EffectIntermediateTextureFormat::R8_SNORM, VK_FORMAT_R8_SNORM, 20 },
	};
	// One row per format except UNKNOWN
	REQUIRE(std::size(EXPECTED) == (size_t)EffectIntermediateTextureFormat::UNKNOWN);

	for (const Expected& expected : EXPECTED) {
		const auto& desc = EffectHelper::FORMAT_DESCS[(size_t)expected.format];
		INFO(desc.name);
		CHECK(desc.vkFormat == expected.vkFormat);

		// The element type of an output with MF replaced by float, as the
		// generator declares it
		std::string texelType = desc.uavTexelType;
		if (const size_t pos = texelType.find("MF"); pos != std::string::npos) {
			texelType.replace(pos, 2, "float");
		}
		const std::string shader = fmt::format(R"(
[[vk::binding(0, 0)]] [[vk::image_format("{}")]] RWTexture2D<{}> output;
[numthreads(1, 1, 1)]
void __M(uint3 id : SV_DispatchThreadID) {{
	output[id.xy] = 0.5;
}}
)", desc.imageFormat, texelType);

		std::vector<uint32_t> spirv;
		std::string messages;
		const bool compiled = ShaderCompiler::CompileComputeShader(shader, "__M", "format.hlsl", {}, spirv, messages);
		INFO(messages);
		REQUIRE(compiled);

		const SpirvInfo info = ParseSpirv(spirv);
		REQUIRE(info.storageImageFormats.size() == 1);
		CHECK(info.storageImageFormats[0] == expected.spirvImageFormat);
	}
}
