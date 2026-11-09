# Provides the imported target DXC::dxcompiler: Microsoft's DirectX Shader
# Compiler, used to compile the HLSL effects to SPIR-V.
#
# Search order:
#   1. DXC_ROOT, a directory with include/dxc/dxcapi.h and lib/libdxcompiler.so
#   2. the system's default locations
#   3. if MAGPIE_FETCH_DXC is ON: the pinned release, downloaded into the build
#      directory and checked against its SHA-256
#
# Fedora does not package DXC, which is why 3 exists. Distribution packagers
# should set MAGPIE_FETCH_DXC=OFF and provide DXC_ROOT.

option(MAGPIE_FETCH_DXC "Download the pinned DirectX Shader Compiler release if none is found" ON)
set(DXC_ROOT "" CACHE PATH "Directory of a DirectX Shader Compiler installation")

set(MAGPIE_DXC_VERSION "v1.9.2609")
set(MAGPIE_DXC_ARCHIVE "linux_dxc_2026_09_28.x86_x64.tar.gz")
set(MAGPIE_DXC_SHA256 "96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1")
set(MAGPIE_DXC_URL
	"https://github.com/microsoft/DirectXShaderCompiler/releases/download/${MAGPIE_DXC_VERSION}/${MAGPIE_DXC_ARCHIVE}")

if(DXC_ROOT)
	find_path(DXC_INCLUDE_DIR dxc/dxcapi.h HINTS "${DXC_ROOT}/include" NO_DEFAULT_PATH)
	find_library(DXC_LIBRARY dxcompiler HINTS "${DXC_ROOT}/lib" NO_DEFAULT_PATH)
else()
	find_path(DXC_INCLUDE_DIR dxc/dxcapi.h)
	find_library(DXC_LIBRARY dxcompiler)
endif()

if(NOT DXC_INCLUDE_DIR OR NOT DXC_LIBRARY)
	if(DXC_ROOT)
		message(FATAL_ERROR "DXC_ROOT=${DXC_ROOT} does not contain include/dxc/dxcapi.h and lib/libdxcompiler.so")
	endif()
	if(NOT MAGPIE_FETCH_DXC)
		message(FATAL_ERROR
			"The DirectX Shader Compiler was not found. Set DXC_ROOT to an installation, "
			"or configure with -DMAGPIE_FETCH_DXC=ON to download release ${MAGPIE_DXC_VERSION}.")
	endif()
	if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
		message(FATAL_ERROR
			"Microsoft publishes DXC binaries for x86_64 only; this is ${CMAKE_SYSTEM_PROCESSOR}. "
			"Build DXC from source and set DXC_ROOT.")
	endif()

	set(dxc_dir "${CMAKE_BINARY_DIR}/_deps/dxc-${MAGPIE_DXC_VERSION}")
	set(dxc_archive "${CMAKE_BINARY_DIR}/_deps/${MAGPIE_DXC_ARCHIVE}")
	if(NOT EXISTS "${dxc_dir}/lib/libdxcompiler.so")
		message(STATUS "Downloading DirectX Shader Compiler ${MAGPIE_DXC_VERSION}")
		file(DOWNLOAD "${MAGPIE_DXC_URL}" "${dxc_archive}"
			EXPECTED_HASH SHA256=${MAGPIE_DXC_SHA256}
			TLS_VERIFY ON
			STATUS dxc_status)
		list(GET dxc_status 0 dxc_code)
		if(NOT dxc_code EQUAL 0)
			list(GET dxc_status 1 dxc_message)
			message(FATAL_ERROR "Downloading ${MAGPIE_DXC_URL} failed: ${dxc_message}")
		endif()
		file(ARCHIVE_EXTRACT INPUT "${dxc_archive}" DESTINATION "${dxc_dir}")
	endif()

	# Replace the NOTFOUND cache entries of the search above.
	set(DXC_INCLUDE_DIR "${dxc_dir}/include" CACHE PATH "DXC include directory" FORCE)
	set(DXC_LIBRARY "${dxc_dir}/lib/libdxcompiler.so" CACHE FILEPATH "DXC library" FORCE)
endif()

message(STATUS "DirectX Shader Compiler: ${DXC_LIBRARY}")

add_library(DXC::dxcompiler SHARED IMPORTED)
set_target_properties(DXC::dxcompiler PROPERTIES
	IMPORTED_LOCATION "${DXC_LIBRARY}"
	INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DXC_INCLUDE_DIR}"
	INTERFACE_INCLUDE_DIRECTORIES "${DXC_INCLUDE_DIR}"
)
