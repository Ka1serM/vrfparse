# Each folder with a main.cpp is a command line tool of the same name.
file(GLOB tool_mains CONFIGURE_DEPENDS */main.cpp)
foreach(tool_main IN LISTS tool_mains)
    cmake_path(GET tool_main PARENT_PATH tool_dir)
    cmake_path(GET tool_dir FILENAME tool_name)
    add_executable(${tool_name} ${tool_main})
    target_link_libraries(${tool_name} PRIVATE vrfparse::vrfparse)
endforeach()
