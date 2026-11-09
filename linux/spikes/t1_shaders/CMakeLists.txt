# The upstream file is copied so that its quoted includes ("pch.h", "Logger.h",
# ...) resolve to the stand-ins in shim/ instead of the Windows headers that
# sit next to it in src/Magpie.Core. The copy is not edited.
configure_file(
	"${MAGPIE_SRC}/Magpie.Core/EffectCompiler.cpp"
	"${CMAKE_CURRENT_BINARY_DIR}/upstream/EffectCompiler.cpp"
	COPYONLY
)

add_executable(fxdump
	fxdump.cpp
	shim/shim.cpp
	"${CMAKE_CURRENT_BINARY_DIR}/upstream/EffectCompiler.cpp"
	"${MAGPIE_SRC}/Shared/SmallVector.cpp"
)

# Order matters: shim/ must come before the upstream directories.
target_include_directories(fxdump PRIVATE
	shim
	"${MAGPIE_SRC}/Magpie.Core/include"
	"${MAGPIE_SRC}/Shared"
	"${MAGPIE_SRC}/Magpie.Core"
)
