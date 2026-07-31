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

## 프로젝트 구성

| 프로젝트 | 내용 |
|---|---|
| `template` | 모든 랩이 링크하는 공유 프레임워크(`lab::`). `App` 베이스 클래스, Vulkan context/swapchain/command/sync, glTF 로더, 씬·카메라 생성, GPU/CPU 계측, CSV·PNG 출력. `main()` 없음. |
| `lab_00` | Vulkan 기본기 학습용 스크래치 랩. |
| `lab_01`~ | Indirect 렌더링 성능 실험 조건들 — 아래 참조. |
| `imgdiff` | 캡처한 PNG 두 장을 허용오차로 비교하는 호스트 도구. GPU 불필요. |

## Indirect 렌더링 실험

**지오메트리 버퍼 전략(A)** × **머티리얼 바인딩 전략(B)**이 성능에 미치는 영향을
측정하는 실험 시리즈. 전체 설계·요인·측정 규약은
[`projects/indirect-rendering-experiment.md`](projects/indirect-rendering-experiment.md)가
단일 기준점(single source of truth)이다.

| 랩 | 조건 | 지오메트리 / 드로우 | 머티리얼 |
|---|---|---|---|
| [lab_01](projects/lab_01/README.md) | A0 × B0 | 메시당 VkBuffer + `vkCmdDrawIndexed` | 머티리얼당 descriptor set |
| [lab_02](projects/lab_02/README.md) | A0 × B1 | 〃 | Bindless (세트 1회 바인딩) |
| [lab_03](projects/lab_03/README.md) | A1 × B0 | 메시당 VkBuffer + **버퍼당 indirect** | 머티리얼당 descriptor set |
| [lab_04](projects/lab_04/README.md) | A1 × B1 | 〃 | Bindless |
| [lab_05](projects/lab_05/README.md) | A2 × B0 | **단일 버퍼** + 오브젝트당 indirect | 머티리얼당 descriptor set |
| [lab_06](projects/lab_06/README.md) | A2 × B1 | 〃 | Bindless |

유효 조건은 총 7개(A3 × B0은 불가능)이며 **A3(lab_07) 하나만 미구현**이다.

**비교쌍 주의** — A 요인은 직교가 아니다. 단독 결론에 쓸 수 있는 쌍은
**A0 vs A1**(lab_01↔03, lab_02↔04)과 **A2 vs A3**(lab_05/06↔07)뿐이다.
A1 vs A2는 버퍼 전략과 granularity가 함께 바뀌므로 금지. B0 vs B1은 같은 A 수준
안에서 세 번(lab_01↔02, 03↔04, 05↔06) 독립 비교할 수 있다.

### 랩 실행 (환경변수)

랩은 argv 대신 환경변수로 조건을 받는다 — 스윕 스크립트가 모든 랩을 같은 방식으로
구동하고, 각 `main()`이 인자 파서를 갖지 않게 하기 위해서다.

| 변수 | 뜻 |
|---|---|
| `LAB_OBJECTS=N` | 오브젝트 수 N (기본 128) |
| `LAB_FRAMES=N` | N 프레임 렌더 후 종료 (미설정 = 창 닫을 때까지) |
| `LAB_CAPTURE=K` | 프레임 K를 PNG로 덤프 |
| `LAB_CAPTURE_FILE=p` | 캡처 출력 경로 (기본 `capture.png`) |

```sh
# 측정: 300 워밍업 폐기 + 500 프레임 → results.csv, env.json, scene.json
LAB_OBJECTS=2048 LAB_FRAMES=800 ./lab_01

# 픽셀 동등성: 두 조건이 같은 그림을 그리는지
LAB_FRAMES=401 LAB_CAPTURE=400 LAB_CAPTURE_FILE=a.png ./lab_01
LAB_FRAMES=401 LAB_CAPTURE=400 LAB_CAPTURE_FILE=b.png ./lab_03
./imgdiff a.png b.png
```

에셋(glTF 샘플 모델)은 버전 관리되지 않는다. `assets/`에 배치할 것.

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

## Projects

| Project | What it is |
|---|---|
| `template` | The shared framework every lab links (`lab::`). `App` base class, Vulkan context/swapchain/command/sync, glTF loader, scene + camera generation, GPU/CPU instrumentation, CSV and PNG output. No `main()`. |
| `lab_00` | Scratch lab for Vulkan fundamentals. |
| `lab_01`+ | Conditions of the indirect-rendering experiment — see below. |
| `imgdiff` | Host tool comparing two captured PNGs against a tolerance. No GPU needed. |

## The indirect-rendering experiment

A series measuring how a **geometry buffer strategy (A)** and a **material
binding strategy (B)** affect performance.
[`projects/indirect-rendering-experiment.md`](projects/indirect-rendering-experiment.md)
is the single source of truth for the design, factors and measurement protocol.

| Lab | Condition | Geometry / draw | Material |
|---|---|---|---|
| [lab_01](projects/lab_01/README.md) | A0 × B0 | VkBuffer per mesh + `vkCmdDrawIndexed` | one descriptor set per material |
| [lab_02](projects/lab_02/README.md) | A0 × B1 | same | bindless (set bound once) |
| [lab_03](projects/lab_03/README.md) | A1 × B0 | VkBuffer per mesh + **per-buffer indirect** | one descriptor set per material |
| [lab_04](projects/lab_04/README.md) | A1 × B1 | same | bindless |
| [lab_05](projects/lab_05/README.md) | A2 × B0 | **single buffer** + per-object indirect | one descriptor set per material |
| [lab_06](projects/lab_06/README.md) | A2 × B1 | same | bindless |

There are 7 valid conditions in total (A3 × B0 is impossible); **only A3 (lab_07)
is left**.

**Mind the comparison pairs** — factor A is not orthogonal. The only pairs valid
for a standalone conclusion are **A0 vs A1** (lab_01↔03, lab_02↔04) and **A2 vs
A3** (lab_05/06↔07). A1 vs A2 changes buffer strategy and granularity together and
is forbidden. B0 vs B1 can be compared three times independently, once per A level
(lab_01↔02, 03↔04, 05↔06).

### Running a lab (environment variables)

Labs take their condition from the environment rather than argv, so a sweep
script can drive every lab identically and no `main()` needs an argument parser.

| Variable | Meaning |
|---|---|
| `LAB_OBJECTS=N` | object count N (default 128) |
| `LAB_FRAMES=N` | quit after N rendered frames (unset = until the window closes) |
| `LAB_CAPTURE=K` | dump frame K as a PNG |
| `LAB_CAPTURE_FILE=p` | capture output path (default `capture.png`) |

```sh
# Measure: discard 300 warm-up frames, record 500 -> results.csv, env.json, scene.json
LAB_OBJECTS=2048 LAB_FRAMES=800 ./lab_01

# Pixel equivalence: do two conditions draw the same image?
LAB_FRAMES=401 LAB_CAPTURE=400 LAB_CAPTURE_FILE=a.png ./lab_01
LAB_FRAMES=401 LAB_CAPTURE=400 LAB_CAPTURE_FILE=b.png ./lab_03
./imgdiff a.png b.png
```

Assets (glTF sample models) are not versioned; place them under `assets/`.

## Adding a lab

1. Create `projects/lab_XX/` with a `src/` directory.
2. Copy `projects/lab_00/CMakeLists.txt` into it (the target name follows the
   folder name automatically).
3. Re-run `cmake --preset default`.

Every lab links the shared `template` framework (`lab::` namespace), which
provides the `App` base class, the Vulkan context, swapchain, command buffers
and synchronization primitives.
