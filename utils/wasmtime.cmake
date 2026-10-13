# Provides the imported target wasmtime::wasmtime, used to run WebAssembly code mods.
#
# By default a pinned, prebuilt Wasmtime C API release is downloaded for the current
# platform. To build offline, download the matching "c-api" archive yourself and set
# ES_WASMTIME_DIR to the folder that contains its "include" and "lib" directories.

set(ES_WASMTIME_VERSION "25.0.0" CACHE STRING "Wasmtime release used for code mods")
set(ES_WASMTIME_DIR "" CACHE PATH "Folder with an extracted Wasmtime C API (include/ and lib/)")

if(NOT ES_WASMTIME_DIR)
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
		set(_wt_arch "aarch64")
	else()
		set(_wt_arch "x86_64")
	endif()

	if(WIN32 AND MSVC)
		set(_wt_os "windows")
		set(_wt_ext "zip")
	elseif(WIN32)
		set(_wt_os "mingw")
		set(_wt_ext "zip")
	elseif(APPLE)
		set(_wt_os "macos")
		set(_wt_ext "tar.xz")
	else()
		set(_wt_os "linux")
		set(_wt_ext "tar.xz")
	endif()

	set(_wt_name "wasmtime-v${ES_WASMTIME_VERSION}-${_wt_arch}-${_wt_os}-c-api")
	include(FetchContent)
	FetchContent_Declare(wasmtime_c_api
		URL "https://github.com/bytecodealliance/wasmtime/releases/download/v${ES_WASMTIME_VERSION}/${_wt_name}.${_wt_ext}"
		DOWNLOAD_EXTRACT_TIMESTAMP ON)
	FetchContent_MakeAvailable(wasmtime_c_api)
	set(ES_WASMTIME_DIR "${wasmtime_c_api_SOURCE_DIR}")
endif()

if(WIN32 AND MSVC)
	set(_wt_lib "${ES_WASMTIME_DIR}/lib/wasmtime.lib")
else()
	set(_wt_lib "${ES_WASMTIME_DIR}/lib/libwasmtime.a")
endif()
if(NOT EXISTS "${_wt_lib}")
	message(FATAL_ERROR "Wasmtime static library not found at ${_wt_lib}")
endif()

add_library(wasmtime_static STATIC IMPORTED GLOBAL)
set_target_properties(wasmtime_static PROPERTIES
	IMPORTED_LOCATION "${_wt_lib}"
	INTERFACE_INCLUDE_DIRECTORIES "${ES_WASMTIME_DIR}/include")
add_library(wasmtime::wasmtime ALIAS wasmtime_static)

# Static linking: the headers must not use dllimport, and the Rust runtime needs a few system libraries.
if(WIN32)
	set_property(TARGET wasmtime_static APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS "WASM_API_EXTERN=" "WASI_API_EXTERN=")
	set_property(TARGET wasmtime_static APPEND PROPERTY INTERFACE_LINK_LIBRARIES
		ws2_32 advapi32 userenv ntdll shell32 ole32 bcrypt)
elseif(APPLE)
	set_property(TARGET wasmtime_static APPEND PROPERTY INTERFACE_LINK_LIBRARIES "-framework CoreFoundation")
else()
	set_property(TARGET wasmtime_static APPEND PROPERTY INTERFACE_LINK_LIBRARIES dl pthread m)
endif()
