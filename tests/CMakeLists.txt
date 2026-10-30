# The tests of the library's parts, which reach its private headers.
add_executable(payload_transform_test payload_transform_test.cpp)
target_include_directories(payload_transform_test PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(payload_transform_test PRIVATE vrfparse::vrfparse)
add_test(NAME payload_transform COMMAND payload_transform_test)
