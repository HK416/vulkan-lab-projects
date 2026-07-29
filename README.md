# Vulkan Lab Projects

**[한국어](#한국어) · [English](#english)**

<a id="한국어"></a>

# 한국어

모던 C++20으로 Vulkan API를 학습하기 위한 다중 프로젝트 워크스페이스.

## 클론

저장소는 [vcpkg](https://github.com/microsoft/vcpkg)를 git 서브모듈로 쓰므로
재귀적으로 클론한다.

```sh
git clone --recurse-submodules <repo-url>
```

`--recurse-submodules` 없이 이미 클론했다면 나중에 서브모듈을 받는다.

```sh
git submodule update --init --recursive
```

## 사전 요구사항

- CMake 3.21+
- C++20 컴파일러
- [Vulkan SDK](https://vulkan.lunarg.com/) — `VULKAN_SDK` 환경변수 설정

## 빌드

의존성(spdlog, VMA, stb, ktx, glm, SDL3, cgltf)은 첫 configure 때 vcpkg가 자동 설치.

```sh
cmake --preset default            # configure (의존성 설치)
cmake --build --preset default    # 전체 빌드
```

단일 프로젝트 빌드:

```sh
cmake --build --preset default --target lab_00
```

## 랩 추가

1. `projects/lab_XX/`에 `src/` 디렉터리와 함께 생성.
2. `projects/lab_00/CMakeLists.txt`를 복사 (타깃 이름은 폴더명 자동 추종).
3. `cmake --preset default` 재실행.

모든 랩은 공유 `template` 프레임워크(`lab::` 네임스페이스)를 링크한다.
`App` 베이스 클래스, Vulkan context, swapchain, command buffer, 동기화 프리미티브 제공.

---

<a id="english"></a>

# English

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
- The [Vulkan SDK](https://vulkan.lunarg.com/) with the `VULKAN_SDK` environment variable set

## Build

Dependencies (spdlog, VMA, stb, ktx, glm, SDL3, cgltf) are installed automatically by
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
