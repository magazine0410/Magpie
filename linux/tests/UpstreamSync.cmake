# Fails when an upstream source file that the Linux tree was ported from has
# changed, so that the change is not missed after merging upstream.
#
# Each entry: upstream file | SHA-256 it had when ported | Linux files derived
# from it. To accept a change: port it (or decide it does not apply) and
# replace the hash with the output of `sha256sum <file>`.
#
# Run by CTest: cmake -DREPO_ROOT=<repository> -P UpstreamSync.cmake
set(ENTRIES
	"src/Magpie.Core/EffectCompiler.cpp|2bde264aaf68021329ce94301e64a6d1d20743f64afc612f547081c51bb22f7d|linux/fx/EffectCompiler.cpp"
	"src/Magpie.Core/include/EffectCompiler.h|6eff2772a7092a4fea58ac4647b523c2e4e07f8a6bb5e0346f497999530b4966|linux/fx/EffectCompiler.h"
	"src/Magpie.Core/EffectHelper.h|e2b59b3d5c5c7b5fb78553133a1cfa813814cba3fb5d93385a0effcd23eabc1e|linux/fx/EffectHelper.h"
	"src/Magpie.Core/EffectCacheManager.cpp|4c97c233cdebe7fd926a6227a73e3f363ebb4e5a98f2e86017a59927fd9dfe8f|linux/fx/EffectCacheManager.cpp"
	"src/Magpie.Core/include/EffectDesc.h|2aabc62db49e9e895b032b6aabbcb421a00bb25e65830457adf6f97968237c4a|linux/core/EffectDesc.h"
	"src/Magpie.Core/include/ScalingOptions.h|d6db2e690c584b42475cbff6e6d8e2ba8ee12da0e4af91839cf90b15a71b3252|linux/core/ScalingOptions.h"
	"src/Magpie.Core/ScalingOptions.cpp|51ffa0f7b6a8cbc8154c5370bbe6f8c649b54fa4bb5ad4b67ebf037c6a07d7ab|linux/core/ScalingOptions.cpp"
	"src/Magpie/AppSettings.cpp|09063be00186e54db4d44d76c8ba335c31d2cd2b91405c9c2a7c2d2dfa705d14|linux/core/SettingsJson.cpp, linux/core/Settings.cpp, linux/core/SettingsStore.cpp"
	"src/Magpie/AppSettings.h|db7e30d4e50dca627c5b8325809332089b617023458d250f14a9a9a47e98a745|linux/core/Settings.h"
	"src/Magpie/ScalingModesService.cpp|73c208e8dc44bd114d1625305d36fe42d1b4f46875e55282a6144748fce85c5d|linux/core/SettingsJson.cpp (scaling modes)"
	"src/Magpie/JsonHelper.cpp|929e35c5247db399ffcadd0155333d42b14b62fa6c5f5a9def4a383dcf17088b|linux/core/SettingsJson.cpp (reading helpers)"
	"src/Magpie/Profile.h|081fd77a07a24bfea2d720778b5feee49871930a59e2ada172717404b5e2f214|linux/core/Profile.h"
	"src/Magpie/ScalingMode.h|19097098efc10244c6a76cab8b9b50fca30d6e6a497226ce6a94d5466083191b|linux/core/ScalingMode.h"
	"src/Magpie/Shortcut.h|f64f67bf390d642ab693772ea79ddca2f6a7145c2ce1dba7436f3fe0263d7c1d|linux/core/Shortcut.h"
	"src/Shared/SmallVector.h|aac9497b9c5215d73eb86cffa04763822f58e0d5314d72fee2b171329e15b3e6|linux/core/SmallVector.h"
	"src/Shared/SmallVector.cpp|8ec8f42967f7fdfe640be7f9562393627c06a571b30c0a8e97f7291792ce8a2d|linux/core/SmallVector.cpp"
	"src/Shared/StrHelper.h|8cb4f8213fc93d5406a59f22d55b2b75c9dd48dbc4d0e4e1e1670392c1eee2fc|linux/core/StrHelper.h"
	"src/Shared/Version.cpp|4261e966fa11d29692c27d477e87ace6335bdafc9c3bff11e19586a89f122f65|linux/core/Version.cpp"
	"src/Shared/Logger.h|278f648ccaa416049bc5b3dea1e400f53f4a567848ec6ff9ff3184dc83ae418e|linux/core/Logger.h"
	"src/Magpie.Core/EffectDrawer.cpp|0af59fd04035ff473c3703832a634217b31dd0ee2588fd01973303a774aca7e0|linux/render/EffectDrawer.cpp"
	"src/Magpie.Core/EffectDrawer.h|b047b627761ea1c1868b6619e4b0c2cefc167747e00e818e007bf66f7e67b829|linux/render/EffectDrawer.h"
	"src/Magpie.Core/Renderer.cpp|7e506baddffb73fcc7f085491820ed418f3d5cfc6853ed9037cb5cc7a32ed55d|linux/render/EffectChain.cpp (effect handling), linux/render/ScalingGeometry.cpp (_UpdateDestRect)"
	"src/Magpie.Core/DeviceResources.cpp|94d70011478046be41166b4440ab83fa0adbbb59f6d0e69e8d3839acd4d8c35a|linux/render/VulkanDevice.cpp"
	"src/Magpie.Core/EffectsProfiler.cpp|13292caad4970eb4312ac5d07d1f66901f8ef82c3e0c739b8fa48e3fe3bfa0f8|linux/render/EffectsProfiler.cpp"
	"src/Magpie.Core/FrameSourceBase.cpp|a6f7d997f7b84bb5b3219bb6b06542c553629a0c43f2d74de9fb77bc72bd9620|linux/render/DuplicateFrameDetector.cpp, linux/engine/PipeWireFrameSource.cpp (Update)"
	"src/Magpie.Core/shaders/DuplicateFrameCS.hlsl|f66d01447c98dc848ae9c70ea723087502101abb57a219bbe1310524f4191c30|linux/render/DuplicateFrameDetector.cpp (DUPLICATE_FRAME_CS)"
	"src/Magpie.Core/StepTimer.cpp|53eae394dd7fcbd1f7e8d4872a944255cdd422836b78260aa1f400ec51b4e3b7|linux/render/StepTimer.cpp"
	"src/Magpie.Core/SrcTracker.cpp|ecaeba5806414b1167a9f706a9e9fb1ca6fd2e12dc0ff96418c4df3317f41199|linux/render/ScalingGeometry.cpp (only the cropping at the end of _CalcSrcRect)"
	"src/Magpie.Core/GraphicsCaptureFrameSource.cpp|aea66d5a5e20a18c5046d06d7afbf241b921b532dec42f68a9a93c88ed8bf5b5|linux/engine/PipeWireFrameSource.cpp (only taking the newest frame in _Update)"
	"src/Magpie.Core/CursorDrawer.cpp|70904c8d5454b4e2a278df85c561ddc7c1db931905f542b54e0364a4c5d1ed4a|linux/render/CursorDrawer.cpp (colour cursors, without hiding after a delay)"
	"src/Magpie.Core/shaders/SimpleVS.hlsl|32b2aaf5bd9879680a6fbc5d4e118e31cc1fb9b6bac4b251d4caef030e9b6f51|linux/render/CursorDrawer.cpp (CURSOR_VS)"
	"src/Magpie.Core/shaders/SimplePS.hlsl|aae5c4aa3abbf7900f940e829bde895fa5906d9d6469c96722ac650770e4b8d2|linux/render/CursorDrawer.cpp (CURSOR_PS)"
	"src/Magpie.Core/CursorManager.cpp|c072b221e9abb6c6fc48bd2fa26122907f00bcf005f05b58359701119ab76e6d|linux/render/ScalingGeometry.cpp (only SrcToScaling)"
)

set(changed "")
foreach(entry IN LISTS ENTRIES)
	string(REPLACE "|" ";" fields "${entry}")
	list(GET fields 0 file)
	list(GET fields 1 expected)
	list(GET fields 2 derived)

	if(NOT EXISTS "${REPO_ROOT}/${file}")
		string(APPEND changed "  ${file}: removed or renamed upstream (ported to ${derived})\n")
		continue()
	endif()

	file(SHA256 "${REPO_ROOT}/${file}" actual)
	if(NOT actual STREQUAL expected)
		string(APPEND changed "  ${file}: now ${actual} (ported to ${derived})\n")
	endif()
endforeach()

if(changed)
	message(FATAL_ERROR
		"Upstream files changed since they were ported to linux/:\n${changed}"
		"Review each change, port it if it applies, then update the hash in linux/tests/UpstreamSync.cmake.")
endif()
message(STATUS "All ported upstream files are unchanged")
