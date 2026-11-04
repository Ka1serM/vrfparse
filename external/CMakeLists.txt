if(NOT TARGET TBB::tbb)
    set(TBB_TEST OFF CACHE BOOL "" FORCE)
    set(TBB_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(TBB_STRICT OFF CACHE BOOL "" FORCE)
    set(TBBMALLOC_BUILD OFF CACHE BOOL "" FORCE)
    set(TBB_INSTALL OFF CACHE BOOL "" FORCE)
    #! TBBBind and the Thread Composability Manager need hwloc, and a second configure fails without it.
    set(TCM_BUILD OFF CACHE BOOL "" FORCE)
    set(TBB_DISABLE_HWLOC_AUTOMATIC_SEARCH ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory(oneTBB EXCLUDE_FROM_ALL SYSTEM)
endif()

if(TARGET ue5parse_ooz)
    add_library(vrfparse_ooz INTERFACE)
    target_link_libraries(vrfparse_ooz INTERFACE ue5parse_ooz)
else()
    add_library(vrfparse_ooz STATIC ooz/kraken.cpp ooz/bitknit.cpp ooz/lzna.cpp)
    target_include_directories(vrfparse_ooz SYSTEM PRIVATE ooz ooz/simde)
    #! OOZ_BUILD_DLL removes the main() and the DLL loading of ooz.
    target_compile_definitions(vrfparse_ooz PRIVATE OOZ_BUILD_DLL=1)
    #! ooz reads unaligned integers through casts, which a sanitized build traps on.
    target_compile_options(vrfparse_ooz PRIVATE -w -fno-sanitize=all)
    #! ooz takes _rotl from <x86gprintrin.h>, which declares it only in GCC.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT EMSCRIPTEN)
        target_compile_options(vrfparse_ooz PRIVATE -include x86intrin.h)
    endif()
    set_target_properties(vrfparse_ooz PROPERTIES CXX_STANDARD 17)
endif()
