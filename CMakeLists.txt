cmake_minimum_required(VERSION 3.21)
project(vulkan-lab-projects LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Vulkan REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(Ktx CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(SDL3 CONFIG REQUIRED)

# cgltf is a header-only single-file lib; the vcpkg port only copies the header.
find_path(CGLTF_INCLUDE_DIR NAMES cgltf.h REQUIRED)

find_path(VMA_HEADER_DIR NAMES vk_mem_alloc.h PATH_SUFFIXES vma REQUIRED)
get_filename_component(_vma_leaf "${VMA_HEADER_DIR}" NAME)
if(_vma_leaf STREQUAL "vma")
    get_filename_component(VMA_INCLUDE_ROOT "${VMA_HEADER_DIR}" DIRECTORY)
else()
    set(VMA_INCLUDE_ROOT "${CMAKE_BINARY_DIR}/vma-compat")
    file(WRITE "${VMA_INCLUDE_ROOT}/vma/vk_mem_alloc.h"
        "#include \"${VMA_HEADER_DIR}/vk_mem_alloc.h\"\n")
endif()

# Shared deps. Every project links this one interface target.
add_library(project_deps INTERFACE)
# Vulkan clip space (depth 0..1) for every glm::perspective in the workspace, so
# projection matrices are consistent no matter which TU builds them.
target_compile_definitions(project_deps INTERFACE GLM_FORCE_DEPTH_ZERO_TO_ONE)
target_link_libraries(project_deps INTERFACE
    Vulkan::Vulkan
    spdlog::spdlog
    glm::glm
    GPUOpen::VulkanMemoryAllocator
    KTX::ktx
    SDL3::SDL3
)
# SYSTEM: silence third-party header warnings (e.g. VMA nullability on Apple clang).
target_include_directories(project_deps SYSTEM INTERFACE ${VMA_INCLUDE_ROOT})
target_include_directories(project_deps SYSTEM INTERFACE ${Stb_INCLUDE_DIR})
target_include_directories(project_deps SYSTEM INTERFACE ${CGLTF_INCLUDE_DIR})


# Shared framework first — every lab links it.
add_subdirectory(projects/template)

# Each other projects/* with a CMakeLists.txt becomes its own target.
file(GLOB _project_dirs LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/projects/*")
foreach(_dir ${_project_dirs})
    get_filename_component(_name "${_dir}" NAME)
    if(_name STREQUAL "template")
        continue()
    endif()
    if(EXISTS "${_dir}/CMakeLists.txt")
        add_subdirectory("${_dir}")
    endif()
endforeach()
