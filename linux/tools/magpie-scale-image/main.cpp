// magpie-scale-image: scales a PNG file with the effects of a scaling mode,
// off screen. Shows what an effect chain does to an image without a
// compositor or a source window, and serves the renderer tests.
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <spdlog/sinks/stdout_sinks.h>
#include "ImageScaler.h"
#include "Logger.h"
#include "ScalingMode.h"
#include "Settings.h"
#include "SettingsJson.h"
#include "SettingsStore.h"
#include "StrHelper.h"
#include "VulkanDevice.h"

using namespace Magpie;

static void PrintUsage() {
	std::puts(
		"Usage: magpie-scale-image [options] INPUT.png OUTPUT.png\n"
		"       magpie-scale-image --list-gpus\n"
		"\n"
		"Scales an image with the effects of a scaling mode, as Magpie scales a\n"
		"window of that size.\n"
		"\n"
		"Effects:\n"
		"  --effect SPEC      add an effect; may be given several times. SPEC is\n"
		"                     NAME[,KEY=VALUE]... NAME is the path below the effects\n"
		"                     directory without extension. KEY is \"type\" (normal, fit,\n"
		"                     absolute or fill), \"scale\" (X or XxY) or a parameter of\n"
		"                     the effect. Example:\n"
		"                       --effect Lanczos,type=fit --effect FSR/FSR_RCAS,sharpness=0.9\n"
		"  --mode NAME        use the effects of scaling mode NAME from the configuration\n"
		"  --config FILE      config.json to read scaling modes from (default: Magpie's\n"
		"                     own; without one the initial scaling modes exist)\n"
		"  --effects-dir DIR  directory of the .hlsl files\n"
		"\n"
		"Geometry:\n"
		"  --size WxH         size of the screen or window that shows the result\n"
		"                     (default: twice the input)\n"
		"  --windowed         apply the rules of windowed scaling\n"
		"  --crop L,T,R,B     remove this many pixels from the edges of the input\n"
		"  --frame            write the whole area given by --size, with the result\n"
		"                     placed by --align, instead of the result alone\n"
		"  --align POS        lefttop, top, righttop, left, center (default), right,\n"
		"                     leftbottom, bottom or rightbottom\n"
		"\n"
		"Rendering:\n"
		"  --gpu N            use graphics card N of --list-gpus (default: the first\n"
		"                     that is not a software implementation)\n"
		"  --no-fp16          do not use 16-bit floats even if an effect supports them\n"
		"  --relaxed-fp16     relaxed precision instead of real 16-bit floats\n"
		"  --inline-params    compile the parameters into the shaders\n"
		"  --no-cache         do not use the effect cache\n"
		"  --frame-count N    frame counter that dynamic effects see (default 0)\n"
		"  --timings N        render N more frames and print the average GPU time of\n"
		"                     each pass\n"
		"  --validate         use the Vulkan validation layers; exit status 2 if they\n"
		"                     report anything\n"
		"  -v                 print the log\n"
		"\n"
		"Exit status: 0 on success, 1 on failure, 2 for validation messages.");
}

static bool ParseFloat(std::string_view text, float& value) noexcept {
	const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
	return ec == std::errc() && end == text.data() + text.size();
}

static bool ParseInt(std::string_view text, int& value) noexcept {
	const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
	return ec == std::errc() && end == text.data() + text.size();
}

// "AxB" or, with allowSingle, "A" for both
static bool ParsePair(std::string_view text, float& first, float& second, bool allowSingle) noexcept {
	const size_t delim = text.find('x');
	if (delim == std::string_view::npos) {
		if (!allowSingle || !ParseFloat(text, first)) {
			return false;
		}
		second = first;
		return true;
	}
	return ParseFloat(text.substr(0, delim), first) && ParseFloat(text.substr(delim + 1), second);
}

static bool ParseEffectSpec(std::string_view spec, EffectOption& effect) noexcept {
	const auto parts = StrHelper::Split(spec, ',');
	if (parts.empty() || parts[0].empty()) {
		return false;
	}

	// Effect names use the Windows separator, as in config.json
	effect.name = parts[0];
	std::replace(effect.name.begin(), effect.name.end(), '/', '\\');

	for (size_t i = 1; i < parts.size(); ++i) {
		const size_t delim = parts[i].find('=');
		if (delim == std::string_view::npos) {
			return false;
		}
		const std::string_view key = parts[i].substr(0, delim);
		const std::string_view value = parts[i].substr(delim + 1);

		if (key == "type") {
			if (value == "normal") {
				effect.scalingType = ScalingType::Normal;
			} else if (value == "fit") {
				effect.scalingType = ScalingType::Fit;
			} else if (value == "absolute") {
				effect.scalingType = ScalingType::Absolute;
			} else if (value == "fill") {
				effect.scalingType = ScalingType::Fill;
			} else {
				return false;
			}
		} else if (key == "scale") {
			if (!ParsePair(value, effect.scale.first, effect.scale.second, true)) {
				return false;
			}
		} else {
			float number;
			if (key.empty() || !ParseFloat(value, number)) {
				return false;
			}
			effect.parameters[std::string(key)] = number;
		}
	}

	return true;
}

static std::optional<OutputAlignment> ParseAlignment(std::string_view text) noexcept {
	static constexpr const char* NAMES[] = {
		"lefttop", "top", "righttop", "left", "center", "right", "leftbottom", "bottom", "rightbottom"
	};
	for (size_t i = 0; i < std::size(NAMES); ++i) {
		if (text == NAMES[i]) {
			return (OutputAlignment)i;
		}
	}
	return std::nullopt;
}

static bool LoadMode(const char* configFile, std::string_view modeName, std::vector<EffectOption>& effects) {
	Settings settings;
	if (configFile) {
		std::ifstream file(configFile, std::ios::binary);
		std::ostringstream content;
		content << file.rdbuf();
		if (!file || !ReadSettingsJson(content.str(), settings)) {
			std::fprintf(stderr, "Cannot read %s\n", configFile);
			return false;
		}
	} else if (SettingsStore::Load(settings) == SettingsStore::LoadResult::Failed) {
		std::fprintf(stderr, "Cannot read %s\n", SettingsStore::ConfigPath().c_str());
		return false;
	}

	for (const ScalingMode& mode : settings.scalingModes) {
		if (mode.name == modeName) {
			for (const EffectItem& item : mode.effects) {
				effects.push_back((EffectOption)item);
			}
			return true;
		}
	}

	std::fprintf(stderr, "There is no scaling mode \"%.*s\". Available:\n", (int)modeName.size(), modeName.data());
	for (const ScalingMode& mode : settings.scalingModes) {
		std::fprintf(stderr, "  %s\n", mode.name.c_str());
	}
	return false;
}

static int ListGpus() {
	VulkanDevice device;
	if (!device.Initialize({})) {
		std::fprintf(stderr, "No usable graphics card found.\n");
		return 1;
	}

	const auto& cards = device.GraphicsCards();
	for (size_t i = 0; i < cards.size(); ++i) {
		std::printf("%zu: %s (%04x:%04x)%s%s%s\n", i, cards[i].name.c_str(), cards[i].vendorId, cards[i].deviceId,
			cards[i].isSoftware ? ", software" : "",
			cards[i].isUsable ? "" : ", not usable",
			(int)i == device.GraphicsCardIdx() ? ", default" : "");
	}
	return 0;
}

int main(int argc, char* argv[]) {
	ImageScaler::Request request;
	request.config.effectsDir = MAGPIE_DEFAULT_EFFECTS_DIR;

	const char* configFile = nullptr;
	const char* modeName = nullptr;
	std::optional<Size> size;
	int gpu = -1;
	bool frame = false;
	bool validate = false;
	bool verbose = false;
	bool listGpus = false;
	std::vector<const char*> files;

	for (int i = 1; i < argc; ++i) {
		const std::string_view arg = argv[i];

		// Options with a value
		const auto value = [&]() -> const char* {
			if (i + 1 >= argc) {
				std::fprintf(stderr, "%s needs a value.\n", argv[i]);
				std::exit(1);
			}
			return argv[++i];
		};
		const auto invalid = [&](const char* text) {
			std::fprintf(stderr, "Invalid value for %.*s: %s\n", (int)arg.size(), arg.data(), text);
			std::exit(1);
		};

		if (arg == "--effect") {
			const char* spec = value();
			if (!ParseEffectSpec(spec, request.options.effects.emplace_back())) {
				invalid(spec);
			}
		} else if (arg == "--mode") {
			modeName = value();
		} else if (arg == "--config") {
			configFile = value();
		} else if (arg == "--effects-dir") {
			request.config.effectsDir = value();
		} else if (arg == "--size") {
			const char* text = value();
			float width, height;
			if (!ParsePair(text, width, height, false) || width < 1 || height < 1) {
				invalid(text);
			}
			size = Size{ (int32_t)width, (int32_t)height };
		} else if (arg == "--windowed") {
			request.options.IsWindowedMode(true);
		} else if (arg == "--crop") {
			const char* text = value();
			const auto parts = StrHelper::Split(text, ',');
			Cropping& cropping = request.options.cropping;
			if (parts.size() != 4 || !ParseFloat(parts[0], cropping.Left) || !ParseFloat(parts[1], cropping.Top) ||
				!ParseFloat(parts[2], cropping.Right) || !ParseFloat(parts[3], cropping.Bottom) ||
				cropping.Left < 0 || cropping.Top < 0 || cropping.Right < 0 || cropping.Bottom < 0) {
				invalid(text);
			}
		} else if (arg == "--frame") {
			frame = true;
		} else if (arg == "--align") {
			const char* text = value();
			const std::optional<OutputAlignment> alignment = ParseAlignment(text);
			if (!alignment) {
				invalid(text);
			}
			request.options.outputAlignment = *alignment;
		} else if (arg == "--gpu") {
			const char* text = value();
			if (!ParseInt(text, gpu) || gpu < 0) {
				invalid(text);
			}
		} else if (arg == "--no-fp16") {
			request.options.IsFP16Disabled(true);
		} else if (arg == "--relaxed-fp16") {
			request.config.relaxedFP16 = true;
		} else if (arg == "--inline-params") {
			request.options.IsInlineParams(true);
		} else if (arg == "--no-cache") {
			request.options.IsEffectCacheDisabled(true);
		} else if (arg == "--frame-count") {
			const char* text = value();
			int count;
			if (!ParseInt(text, count) || count < 0) {
				invalid(text);
			}
			request.frameCount = (uint32_t)count;
		} else if (arg == "--timings") {
			const char* text = value();
			int count;
			if (!ParseInt(text, count) || count < 1) {
				invalid(text);
			}
			request.timedFrames = (uint32_t)count;
		} else if (arg == "--validate") {
			validate = true;
		} else if (arg == "-v") {
			verbose = true;
		} else if (arg == "--list-gpus") {
			listGpus = true;
		} else if (arg == "-h" || arg == "--help") {
			PrintUsage();
			return 0;
		} else if (arg.starts_with("-")) {
			std::fprintf(stderr, "Unknown option %s\n", argv[i]);
			return 1;
		} else {
			files.push_back(argv[i]);
		}
	}

	// Errors go to stderr; the whole log with -v
	Logger::Get().InitializeWithSink(
		verbose ? spdlog::level::info : spdlog::level::warn,
		std::make_shared<spdlog::sinks::stderr_sink_mt>());

	if (listGpus) {
		return ListGpus();
	}

	if (files.size() != 2) {
		PrintUsage();
		return 1;
	}

	if (modeName) {
		if (!request.options.effects.empty()) {
			std::fprintf(stderr, "--mode and --effect cannot be combined.\n");
			return 1;
		}
		if (!LoadMode(configFile, modeName, request.options.effects)) {
			return 1;
		}
	}
	if (request.options.effects.empty()) {
		std::fprintf(stderr, "Give at least one --effect, or a --mode.\n");
		return 1;
	}

	ImageData input;
	if (!ImageFile::LoadPng(files[0], input)) {
		return 1;
	}

	request.input = &input;
	request.config.rendererSize = size.value_or(Size{ (int32_t)input.width * 2, (int32_t)input.height * 2 });

	VulkanDevice device;
	{
		VulkanDevice::Options deviceOptions;
		deviceOptions.validation = validate;
		if (!device.Initialize(deviceOptions)) {
			return 1;
		}

		// The card with number N. GraphicsCardId needs its IDs as well, which
		// only the device list has.
		if (gpu >= 0) {
			const auto& cards = device.GraphicsCards();
			if ((size_t)gpu >= cards.size() || !cards[gpu].isUsable) {
				std::fprintf(stderr, "Graphics card %d does not exist or is not usable.\n", gpu);
				return 1;
			}
			request.options.graphicsCardId = { gpu, cards[gpu].vendorId, cards[gpu].deviceId };
		}
	}

	// A second device only if another card than the default was asked for
	std::optional<VulkanDevice> chosenDevice;
	VulkanDevice* activeDevice = &device;
	if (gpu >= 0 && gpu != device.GraphicsCardIdx()) {
		chosenDevice.emplace();
		if (!chosenDevice->Initialize({ request.options.graphicsCardId, validate, false })) {
			return 1;
		}
		activeDevice = &*chosenDevice;
	}

	ImageScaler::Result result;
	if (!ImageScaler::Scale(*activeDevice, request, result)) {
		return 1;
	}

	const ImageData& output = frame ? ImageScaler::ComposeFrame(result, request.config.rendererSize) : result.output;
	if (!ImageFile::SavePng(files[1], output, false)) {
		return 1;
	}

	std::printf("%ux%u -> %ux%u on %s\n", input.width, input.height, output.width, output.height,
		activeDevice->GraphicsCards()[activeDevice->GraphicsCardIdx()].name.c_str());

	if (!result.timings.empty()) {
		float total = 0.0f;
		for (const ImageScaler::PassTiming& timing : result.timings) {
			std::printf("  %-48s %8.3f ms\n", timing.name.c_str(), timing.gpuTime);
			total += timing.gpuTime;
		}
		std::printf("  %-48s %8.3f ms\n", "total", total);
	}

	if (activeDevice->ValidationMessageCount() > 0) {
		std::fprintf(stderr, "The validation layers reported %u messages.\n", activeDevice->ValidationMessageCount());
		return 2;
	}

	return 0;
}
