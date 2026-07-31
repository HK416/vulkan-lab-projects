# Host-side image comparison for the pixel-equivalence check. Links project_deps
# only for the stb include dir — it must not depend on Vulkan or the framework.
get_filename_component(PROJ_NAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)

add_executable(${PROJ_NAME} src/main.cpp)
target_link_libraries(${PROJ_NAME} PRIVATE project_deps)
