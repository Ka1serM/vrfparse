# The library compiles every source below this folder. Its precompiled header holds the headers that most of them
# include.
file(GLOB_RECURSE vrfparse_sources CONFIGURE_DEPENDS *.cpp)
add_library(vrfparse STATIC ${vrfparse_sources})
add_library(vrfparse::vrfparse ALIAS vrfparse)
target_include_directories(vrfparse PUBLIC ${PROJECT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vrfparse PRIVATE TBB::tbb vrfparse_ooz)
target_compile_options(vrfparse PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers -Wconversion -Wsign-conversion)
target_precompile_headers(vrfparse PRIVATE <algorithm> <cstdint> <memory> <span> <format> <string> <string_view> <vector>)
