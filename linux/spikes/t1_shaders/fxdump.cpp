// T1, step 1: run the unmodified upstream EffectCompiler over every effect and
// write the HLSL it would hand to D3DCompile, together with the macros.
//
// Usage: fxdump <effects dir> <output dir>
//
// Output: <output dir>/<variant>/<effect>_Pass<N>.hlsl, a ".defs" file next to
// each (one NAME=VALUE per line) and <output dir>/manifest.tsv.
#include "pch.h"
#include "EffectCompiler.h"
#include "EffectDesc.h"
#include "Logger.h"
#include "shim.h"
#include <fstream>
#include <iostream>

using namespace Magpie;

struct Variant {
	const char* name;
	EffectCompilerFlags flags;
};

int main(int argc, char* argv[]) {
	if (argc != 3) {
		std::cerr << "usage: fxdump <effects dir> <output dir>\n";
		return 2;
	}

	Spike::effectsRoot = std::filesystem::canonical(argv[1]);
	const std::filesystem::path outRoot = argv[2];

	// Effect names as upstream uses them: relative path, backslashes, no extension.
	std::vector<std::string> effects;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(Spike::effectsRoot)) {
		if (entry.is_regular_file() && entry.path().extension() == ".hlsl") {
			std::string name = entry.path().lexically_relative(Spike::effectsRoot).replace_extension().string();
			std::replace(name.begin(), name.end(), '/', '\\');
			effects.push_back(std::move(name));
		}
	}
	std::sort(effects.begin(), effects.end());

	static constexpr Variant VARIANTS[] = {
		{ "default", EffectCompilerFlags::NoCache },
		{ "nofp16", EffectCompilerFlags::NoCache | EffectCompilerFlags::NoFP16 },
		{ "inline", EffectCompilerFlags::NoCache | EffectCompilerFlags::InlineParams },
	};

	std::filesystem::create_directories(outRoot);
	std::ofstream manifest(outRoot / "manifest.tsv");
	manifest << "variant\teffect\tpass\tfile\tfp16\tparams\n";

	uint32_t failures = 0;
	for (const Variant& variant : VARIANTS) {
		uint32_t passCount = 0;

		for (const std::string& effect : effects) {
			Spike::dumpedPasses.clear();
			Logger::Get().messages.clear();

			EffectDesc desc;
			desc.name = effect;
			// An empty map means every parameter is inlined with its default value.
			const phmap::flat_hash_map<std::string, float> inlineParams;

			if (EffectCompiler::Compile(desc, variant.flags, &inlineParams) != 0) {
				++failures;
				std::cerr << "PARSE FAILURE [" << variant.name << "] " << effect << "\n";
				for (const std::string& msg : Logger::Get().messages) {
					std::cerr << "    " << msg << "\n";
				}
				continue;
			}

			for (const std::string& msg : Logger::Get().messages) {
				std::cerr << "[" << variant.name << "] " << effect << ": " << msg << "\n";
			}

			for (size_t i = 0; i < Spike::dumpedPasses.size(); ++i) {
				const Spike::DumpedPass& pass = Spike::dumpedPasses[i];

				std::string relPath = pass.sourceName;
				std::replace(relPath.begin(), relPath.end(), '\\', '/');
				const std::filesystem::path file = outRoot / variant.name / relPath;
				std::filesystem::create_directories(file.parent_path());

				std::ofstream(file, std::ios::binary) << pass.source;

				std::ofstream defs(std::filesystem::path(file).replace_extension(".defs"));
				for (const auto& [name, value] : pass.macros) {
					defs << name << "=" << value << "\n";
				}

				manifest << variant.name << '\t' << effect << '\t' << (i + 1) << '\t'
					<< variant.name << '/' << relPath << '\t'
					<< ((desc.flags & EffectFlags::FP16) ? 1 : 0) << '\t'
					<< desc.params.size() << '\n';
				++passCount;
			}
		}

		std::cout << variant.name << ": " << effects.size() << " effects, " << passCount << " passes\n";
	}

	std::cout << "parse failures: " << failures << "\n";
	return failures ? 1 : 0;
}
