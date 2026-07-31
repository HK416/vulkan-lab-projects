# Target name = folder name. Links the shared framework (which carries project_deps).
get_filename_component(PROJ_NAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)

file(GLOB_RECURSE _srcs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(${PROJ_NAME} ${_srcs})
target_link_libraries(${PROJ_NAME} PRIVATE template)

# --- shader embedding (glslc -mfmt=c → C initializer, #included by main.cpp) ---
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/bin" REQUIRED)

set(_shader_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${_shader_dir})

function(embed_shader SRC)
    set(_out ${_shader_dir}/${SRC}.h)
    add_custom_command(
        OUTPUT ${_out}
        COMMAND ${GLSLC} --target-env=vulkan1.3 -mfmt=c
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SRC} -o ${_out}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SRC}
        COMMENT "glslc ${SRC}")
    target_sources(${PROJ_NAME} PRIVATE ${_out})
endfunction()

embed_shader(mesh.vert)
embed_shader(mesh.frag)

target_include_directories(${PROJ_NAME} PRIVATE ${_shader_dir})
