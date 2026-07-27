# Vulkan Lab Projects

A multi-project workspace for learning the Vulkan API in modern C++20.

## Clone

The repository uses a git submodule for [vcpkg](https://github.com/microsoft/vcpkg),
so clone recursively:

```sh
git clone --recurse-submodules <repo-url>
```

Already cloned without `--recurse-submodules`? Fetch the submodule after the fact:

```sh
git submodule update --init --recursive
```

## Prerequisites

- CMake 3.21+
- A C++20 compiler
- The [Vulkan SDK](https://vulkan.lunarg.com/) with the `VULKAN_SDK` environment
  variable set (on macOS, `source ~/VulkanSDK/<version>/setup-env.sh`)

## Build

Dependencies (spdlog, VMA, stb, ktx, glm, SDL3) are installed automatically by
vcpkg on the first configure.

```sh
cmake --preset default            # configure (installs deps)
cmake --build --preset default    # build everything
```

Build a single project:

```sh
cmake --build --preset default --target lab_00
```

## Adding a lab

1. Create `projects/lab_XX/` with a `src/` directory.
2. Copy `projects/lab_00/CMakeLists.txt` into it (the target name follows the
   folder name automatically).
3. Re-run `cmake --preset default`.

Every lab links the shared `template` framework (`lab::` namespace), which
provides the `App` base class, the Vulkan context, swapchain, command buffers
and synchronization primitives.
