# Target name = folder name. Links the shared framework (which carries project_deps).
get_filename_component(PROJ_NAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)

file(GLOB_RECURSE _srcs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(${PROJ_NAME} ${_srcs})
target_link_libraries(${PROJ_NAME} PRIVATE template)

# Shaders are added once the first pipeline lands. See lab_00/CMakeLists.txt for
# the glslc -mfmt=c embedding pattern to copy when shaders/ gets populated.
