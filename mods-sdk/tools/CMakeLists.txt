# Standalone build of modcheck, which runs a code mod without the game.
#   cmake -S mods-sdk/tools -B build-modcheck && cmake --build build-modcheck
cmake_minimum_required(VERSION 3.19)
project(modcheck LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../utils/wasmtime.cmake)
add_executable(modcheck
	modcheck.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../source/mod/ModManifest.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/../../source/mod/WasmMod.cpp)
target_link_libraries(modcheck PRIVATE wasmtime::wasmtime)
