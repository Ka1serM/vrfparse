cmake_minimum_required(VERSION 3.28)
project(vrfparse LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VRFPARSE_BUILD_TOOLS "Build the command line tools" ${PROJECT_IS_TOP_LEVEL})

# fmt: the parent project's target when it has one.
if(NOT TARGET fmt::fmt)
    add_subdirectory(external/fmt EXCLUDE_FROM_ALL)
endif()

# ooz: the Oodle decoder of the replay data chunks. OOZ_BUILD_DLL removes its main() and its DLL loading.
# A parent project that builds ooz already passes its target in VRFPARSE_OOZ_TARGET, so the decoder is linked once.
set(VRFPARSE_OOZ_TARGET "" CACHE STRING "An existing ooz target to link instead of building ooz")
if(NOT VRFPARSE_OOZ_TARGET)
    add_library(vrfparse_ooz STATIC external/ooz/kraken.cpp external/ooz/bitknit.cpp external/ooz/lzna.cpp)
    target_include_directories(vrfparse_ooz SYSTEM PRIVATE external/ooz external/ooz/simde)
    target_compile_definitions(vrfparse_ooz PRIVATE OOZ_BUILD_DLL=1)
    # ooz reads unaligned integers through casts (fine on x86, ARM64 and wasm); a sanitized build would trap on them.
    target_compile_options(vrfparse_ooz PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w -fno-sanitize=all>)
    set_target_properties(vrfparse_ooz PROPERTIES CXX_STANDARD 17 POSITION_INDEPENDENT_CODE ON)
    set(VRFPARSE_OOZ_TARGET vrfparse_ooz)
endif()

add_library(vrfparse STATIC
    src/net/bit_reader.cpp
    src/net/unreal_names.cpp
    src/replay/byte_reader.cpp
    src/replay/component_data_stream.cpp
    src/replay/demo_reader.cpp
    src/replay/event.cpp
    src/replay/local_file_replay.cpp
    src/replay/match.cpp
    src/replay/package_map.cpp
    src/replay/payload_transform.cpp
    src/replay/roster.cpp
    src/replay/timeline.cpp)
add_library(vrfparse::vrfparse ALIAS vrfparse)
target_include_directories(vrfparse PUBLIC include)
target_link_libraries(vrfparse PUBLIC fmt::fmt PRIVATE ${VRFPARSE_OOZ_TARGET})
set_target_properties(vrfparse PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_compile_options(vrfparse PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers -Wconversion -Wsign-conversion)
endif()

if(VRFPARSE_BUILD_TOOLS)
    add_executable(replay_dump tools/replay_dump/main.cpp)
    target_link_libraries(replay_dump PRIVATE vrfparse::vrfparse)
endif()
