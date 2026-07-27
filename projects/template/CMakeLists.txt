# Shared framework linked by every lab project. No main() here.
file(GLOB_RECURSE _template_srcs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

if(_template_srcs)
    add_library(template STATIC ${_template_srcs})
else()
    # Header-only for now (no .cpp yet).
    add_library(template INTERFACE)
endif()

if(_template_srcs)
    target_include_directories(template PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(template PUBLIC project_deps)
else()
    target_include_directories(template INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(template INTERFACE project_deps)
endif()
