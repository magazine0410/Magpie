#include "Settings.h"

namespace Magpie {

void Settings::SetInitialSettings() noexcept {
	scalingModes.clear();
	scalingModes.resize(7);

	// Lanczos
	{
		auto& lanczos = scalingModes[0];
		lanczos.name = "Lanczos";

		auto& lanczosEffect = lanczos.effects.emplace_back();
		lanczosEffect.name = "Lanczos";
		lanczosEffect.scalingType = ScalingType::Fit;
	}
	// FSR
	{
		auto& fsr = scalingModes[1];
		fsr.name = "FSR";

		fsr.effects.resize(2);
		auto& easu = fsr.effects[0];
		easu.name = "FSR\\FSR_EASU";
		easu.scalingType = ScalingType::Fit;
		auto& rcas = fsr.effects[1];
		rcas.name = "FSR\\FSR_RCAS";
		rcas.parameters["sharpness"] = 0.87f;
	}
	// FSRCNNX
	{
		auto& fsrcnnx = scalingModes[2];
		fsrcnnx.name = "FSRCNNX";
		fsrcnnx.effects.emplace_back().name = "FSRCNNX\\FSRCNNX";
	}
	// CuNNy
	{
		auto& cunny = scalingModes[3];
		cunny.name = "CuNNy";
		cunny.effects.emplace_back().name = "CuNNy2\\CuNNy-4x12-NVL";
	}
	// Anime4K
	{
		auto& anime4k = scalingModes[4];
		anime4k.name = "Anime4K";
		anime4k.effects.emplace_back().name = "Anime4K\\Anime4K_Upscale_Denoise_L";
	}
	// CRT-Geom
	{
		auto& crtGeom = scalingModes[5];
		crtGeom.name = "CRT-Geom";

		auto& crtGeomEffect = crtGeom.effects.emplace_back();
		crtGeomEffect.name = "CRT\\CRT_Geom";
		crtGeomEffect.scalingType = ScalingType::Fit;
		crtGeomEffect.parameters["curvature"] = 0.0f;
		crtGeomEffect.parameters["cornerSize"] = 0.001f;
		crtGeomEffect.parameters["CRTGamma"] = 1.5f;
		crtGeomEffect.parameters["monitorGamma"] = 2.2f;
		crtGeomEffect.parameters["interlace"] = 0.0f;
	}
	// Integer Scale 2x
	{
		auto& integer2x = scalingModes[6];
		integer2x.name = "Integer Scale 2x";

		auto& nearest = integer2x.effects.emplace_back();
		nearest.name = "Nearest";
		nearest.scalingType = ScalingType::Normal;
		nearest.scale = { 2.0f,2.0f };
	}

	// The global scaling mode defaults to Lanczos
	defaultProfile.scalingMode = 0;
}

}
