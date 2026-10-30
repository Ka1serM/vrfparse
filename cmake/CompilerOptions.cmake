# The compiler and linker settings of the build. Verylia, CppUE5Parse and vrfparse have the same copy of this file. The
# top-level project applies it to every target, its dependencies included; a project that a parent adds takes the
# parent's settings.
if(NOT PROJECT_IS_TOP_LEVEL)
    return()
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(BUILD_SHARED_LIBS OFF)

get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ccache gives a rebuild the objects whose sources did not change. Clang's precompiled headers need its setting
# sloppiness = pch_defines,time_macros,include_file_mtime,include_file_ctime, and the PCH without a timestamp below.
find_program(CCACHE ccache)
if(CCACHE)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE}")
endif()
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:SHELL:-Xclang -fno-pch-timestamp>")

# Release optimizes with CMake's -O3 and links with ThinLTO. Debug and RelWithDebInfo keep the fast link.
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# Clang compiles every platform, with libc++ and LLVM's linker. The code targets x86-64 CPUs from 2013 on (Haswell,
# AVX2 and FMA) and browsers with WebAssembly SIMD and threads. SIMDe (external/CppUE5Parse/external/simde) maps x86
# intrinsics to WebAssembly SIMD.
if(EMSCRIPTEN)
    # Every object shares one memory with the worker threads, and C++ throws native WebAssembly exceptions. wasm-ld
    # rejects a mix, so the flags apply to every target.
    add_compile_options(-pthread -msimd128 -fwasm-exceptions -sSUPPORT_LONGJMP=wasm)
    add_link_options(-pthread -msimd128 -fwasm-exceptions -sSUPPORT_LONGJMP=wasm)
else()
    add_compile_options(-march=x86-64-v3 "$<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>")
    add_link_options(-stdlib=libc++)
    set(CMAKE_LINKER_TYPE LLD)
    add_compile_options("$<$<CONFIG:Release>:-ffunction-sections;-fdata-sections;-fno-math-errno>")
    # --icf=all folds functions with the same code.
    add_link_options("$<$<CONFIG:Release>:LINKER:--gc-sections;LINKER:--icf=all>")
endif()
if(WIN32)
    # The development builds write the debug information to a PDB next to the executable, which keeps it out of the
    # objects' link. The executable links the C and C++ runtimes statically: it needs no DLL next to it.
    add_compile_options("$<$<CONFIG:Debug,RelWithDebInfo>:-gcodeview>")
    add_link_options("$<$<CONFIG:Debug,RelWithDebInfo>:LINKER:--pdb=>" -static)
elseif(NOT EMSCRIPTEN)
    # The development builds write the debug information next to the objects, which keeps it out of the link.
    add_compile_options("$<$<CONFIG:Debug,RelWithDebInfo>:-gsplit-dwarf>")
    add_compile_options("$<$<CONFIG:Release>:-fno-plt>")
endif()

# RelWithDebInfo is a development configuration like Debug: it keeps the asserts. Only Release defines NDEBUG.
foreach(language C CXX)
    string(REGEX REPLACE "[-/]DNDEBUG" "" CMAKE_${language}_FLAGS_RELWITHDEBINFO "${CMAKE_${language}_FLAGS_RELWITHDEBINFO}")
endforeach()
