// magpie-fxc: compiles MagpieFX effects to SPIR-V from the command line.
//
// Used to check effects (bundled or custom) without starting the application,
// and by the tests and the CI as the Phase 2 exit check.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <spdlog/sinks/stdout_sinks.h>
#include "EffectCompiler.h"
#include "EffectDesc.h"
#include "Logger.h"

using namespace Magpie;

static void PrintUsage() {
	std::puts(
		"Usage: magpie-fxc [options] EFFECTS_DIR EFFECT...\n"
		"       magpie-fxc [options] --all EFFECTS_DIR\n"
		"\n"
		"Compiles MagpieFX effects (.hlsl files below EFFECTS_DIR) to SPIR-V.\n"
		"EFFECT is the path below EFFECTS_DIR without extension, for example\n"
		"\"FSR/FSR_EASU\".\n"
		"\n"
		"Options:\n"
		"  --all            compile every effect found below EFFECTS_DIR\n"
		"  --no-fp16        do not use 16-bit floats even if the effect supports them\n"
		"  --native-fp16    real 16-bit floats instead of relaxed precision\n"
		"  --inline         inline the parameters with their default values\n"
		"  --matrix         compile each effect in all six combinations of the three\n"
		"                   options above\n"
		"  --werror         treat compiler warnings as errors\n"
		"  --cache          use and fill the effect cache (off by default)\n"
		"  --save-sources   write the generated pass sources to the cache directory\n"
		"  -o DIR           write EFFECT_PassN.spv files to DIR\n"
		"  -q               print failures and the summary only\n"
		"\n"
		"Exit status: 0 if everything compiled, 1 otherwise.");
}

struct Variant {
	const char* name;
	EffectCompilerFlags flags;
};

struct Job {
	std::string effect;	// with '\' as separator, as in config.json
	Variant variant;
};

static std::vector<std::string> FindEffects(const std::filesystem::path& effectsDir) {
	std::vector<std::string> effects;
	std::error_code ec;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(effectsDir, ec)) {
		if (entry.is_regular_file() && entry.path().extension() == ".hlsl") {
			std::string name = entry.path().lexically_relative(effectsDir).replace_extension().string();
			std::replace(name.begin(), name.end(), '/', '\\');
			effects.push_back(std::move(name));
		}
	}
	std::sort(effects.begin(), effects.end());
	return effects;
}

int main(int argc, char* argv[]) {
	bool all = false, matrix = false, quiet = false;
	EffectCompilerFlags flags = EffectCompilerFlags::NoCache;
	std::filesystem::path outputDir;
	std::vector<std::string> positional;

	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--all") {
			all = true;
		} else if (arg == "--no-fp16") {
			flags |= EffectCompilerFlags::NoFP16;
		} else if (arg == "--native-fp16") {
			flags |= EffectCompilerFlags::NativeFP16;
		} else if (arg == "--inline") {
			flags |= EffectCompilerFlags::InlineParams;
		} else if (arg == "--matrix") {
			matrix = true;
		} else if (arg == "--werror") {
			flags |= EffectCompilerFlags::WarningsAreErrors;
		} else if (arg == "--cache") {
			flags &= ~EffectCompilerFlags::NoCache;
		} else if (arg == "--save-sources") {
			flags |= EffectCompilerFlags::SaveSources;
		} else if (arg == "-o" && i + 1 < argc) {
			outputDir = argv[++i];
		} else if (arg == "-q") {
			quiet = true;
		} else if (arg == "-h" || arg == "--help") {
			PrintUsage();
			return 0;
		} else if (arg.starts_with("-")) {
			std::fprintf(stderr, "magpie-fxc: unknown option %s\n", arg.c_str());
			return 2;
		} else {
			positional.push_back(arg);
		}
	}

	if (positional.empty() || (all && positional.size() != 1) || (!all && positional.size() < 2)) {
		PrintUsage();
		return 2;
	}

	const std::filesystem::path effectsDir = positional[0];
	if (!std::filesystem::is_directory(effectsDir)) {
		std::fprintf(stderr, "magpie-fxc: %s is not a directory\n", effectsDir.c_str());
		return 2;
	}

	std::vector<std::string> effects;
	if (all) {
		effects = FindEffects(effectsDir);
		if (effects.empty()) {
			std::fprintf(stderr, "magpie-fxc: no .hlsl files below %s\n", effectsDir.c_str());
			return 2;
		}
	} else {
		for (size_t i = 1; i < positional.size(); ++i) {
			std::string name = positional[i];
			std::replace(name.begin(), name.end(), '/', '\\');
			if (name.ends_with(".hlsl")) {
				name.resize(name.size() - 5);
			}
			effects.push_back(std::move(name));
		}
	}

	std::vector<Variant> variants;
	if (matrix) {
		using enum EffectCompilerFlags;
		variants = {
			{ "fp16", flags },
			{ "fp16 inline", flags | InlineParams },
			{ "no-fp16", flags | NoFP16 },
			{ "no-fp16 inline", flags | NoFP16 | InlineParams },
			{ "native-fp16", flags | NativeFP16 },
			{ "native-fp16 inline", flags | NativeFP16 | InlineParams },
		};
	} else {
		variants = { { "", flags } };
	}

	// Compiler messages go to standard error through the logger.
	Logger::Get().InitializeWithSink(spdlog::level::warn, std::make_shared<spdlog::sinks::stderr_sink_mt>());

	std::vector<Job> jobs;
	for (const Variant& variant : variants) {
		for (const std::string& effect : effects) {
			jobs.push_back({ effect, variant });
		}
	}

	std::atomic<size_t> next = 0;
	std::atomic<uint32_t> failures = 0, passCount = 0;
	std::mutex printLock;
	// An empty map inlines every parameter with its default value.
	const std::map<std::string, float> inlineParams;

	auto worker = [&]() {
		for (size_t i = next++; i < jobs.size(); i = next++) {
			const Job& job = jobs[i];

			EffectDesc desc;
			desc.name = job.effect;
			const uint32_t result = EffectCompiler::Compile(desc, job.variant.flags, effectsDir, &inlineParams);

			std::string displayName = job.effect;
			std::replace(displayName.begin(), displayName.end(), '\\', '/');

			if (result == 0 && !outputDir.empty()) {
				for (size_t pass = 0; pass < desc.passes.size(); ++pass) {
					std::filesystem::path file = outputDir / (displayName + "_Pass" + std::to_string(pass + 1) + ".spv");
					std::error_code ec;
					std::filesystem::create_directories(file.parent_path(), ec);
					std::ofstream out(file, std::ios::binary);
					out.write((const char*)desc.passes[pass].spirv.data(),
						(std::streamsize)(desc.passes[pass].spirv.size() * sizeof(uint32_t)));
				}
			}

			std::scoped_lock lk(printLock);
			if (result != 0) {
				++failures;
				std::printf("FAILED  %s%s%s%s\n", displayName.c_str(),
					*job.variant.name ? " [" : "", job.variant.name, *job.variant.name ? "]" : "");
			} else {
				passCount += (uint32_t)desc.passes.size();
				if (!quiet) {
					std::printf("ok      %s%s%s%s (%zu passes)\n", displayName.c_str(),
						*job.variant.name ? " [" : "", job.variant.name, *job.variant.name ? "]" : "",
						desc.passes.size());
				}
			}
			std::fflush(stdout);
		}
	};

	const size_t threadCount = std::min<size_t>(jobs.size(), std::max(1u, std::thread::hardware_concurrency()));
	std::vector<std::thread> threads;
	for (size_t i = 1; i < threadCount; ++i) {
		threads.emplace_back(worker);
	}
	worker();
	for (std::thread& thread : threads) {
		thread.join();
	}

	Logger::Get().Flush();
	std::printf("%zu effects, %zu variants: %u compilations failed, %u passes compiled\n",
		effects.size(), variants.size(), failures.load(), passCount.load());
	return failures == 0 ? 0 : 1;
}
