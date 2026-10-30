cmake_minimum_required(VERSION 3.29)
project(vrfparse LANGUAGES CXX)

include(cmake/CompilerOptions.cmake)

option(VRFPARSE_BUILD_TOOLS "Build the command line tools" ${PROJECT_IS_TOP_LEVEL})
option(VRFPARSE_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})

add_subdirectory(external)
add_subdirectory(src)
if(VRFPARSE_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
if(VRFPARSE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
