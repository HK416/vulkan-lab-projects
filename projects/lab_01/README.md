# Lab 01 — Baseline (A0 × B0)

**[한국어](#한국어) · [English](#english)**

<a id="한국어"></a>

# 한국어

Indirect 렌더링 실험 시리즈의 **기준점**. 전체 설계는
[`../indirect-rendering-experiment.md`](../indirect-rendering-experiment.md) 참조.

- **A0** — 다중 버퍼(메시당 VkBuffer) + 일반 `vkCmdDrawIndexed` 루프
- **B0** — 머티리얼마다 `vkCmdBindDescriptorSets`

이후 모든 조건(A1~A3 × B0/B1)은 이 조합 대비로 해석된다. lab_02+가 재사용할
하니스(씬 로더·카메라·계측·리포터)는 **이미 `template`에 구현**되어 있다
(`lab::asset`, `lab::scene`, `lab::bench`, `lab::render`). 따라서 lab_01은 하니스를
새로 짓지 않고 **template API를 A0 × B0 전략으로 배선**한다. A0(메시당 버퍼)와
B0(머티리얼당 descriptor set)는 실험 대상 자체라 lab 안에 남고, 중립적 빌딩블록만
template가 제공한다.

---

## 모듈 설계 (src/)

> 하니스는 template에 있음. lab_01은 `main.cpp` 하나로 template를 배선.

```
lab_01/src/
  main.cpp          IndirectLab : App. 씬 로드 → GPU 업로드 → onRender 드로우 루프 + 계측 훅.

template/src/ (재사용, 구현 완료)
  asset/loader.*    cgltf glTF 로드 → CpuModel(메시/머티리얼). standardVertexInput().
  asset/texture.*   KTX2(BC7/큐브맵) → GpuImage. loadKtx / loadIbl.
  render/buffer.*   GpuBuffer(device-local/host-visible), StagingUploader(배치 업로드).
  render/descriptor.* DescriptorSetLayout/Pool/Writer (B0 머티리얼 세트 + 이후 B1 bindless).
  render/pipeline.* PipelineBuilder(고정 렌더 상태 bake), PipelineLayout, GraphicsPipeline.
  render/shader.*   Shader::fromSpirv/fromFile + SPIR-V 해시.
  scene/scene.*     generateScene(seed=42) + dumpSceneJson + sceneCenter/HalfExtent.
  scene/camera.*    CameraPath/makeSweepPath, sampleCamera(720프레임 루프, 프레임 인덱스).
  bench/instrument.* GpuQueries(타임스탬프+파이프라인 통계), CpuTimer.
  bench/reporter.*  CsvReporter, dumpEnvironmentJson.
```

의존성: **cgltf**, **ktx** — 이미 `vcpkg.json` + 최상위 `project_deps`에 추가됨.

---

## 데이터 흐름

1. `asset::loadModel` — 모델 세트 M의 glTF를 `CpuModel`로 로드(프리미티브 → 정점/인덱스
   + 머티리얼 인덱스). 텍스처 경로는 BC7 사전 압축 KTX2를 `asset::loadKtx`로 로드.
2. **A0 GPU 업로드** — 메시마다 `GpuBuffer::createDeviceLocal` 2개(vertex, index).
   `StagingUploader`로 배치 업로드(오프셋 공유 없음, 메시당 바인딩).
3. **B0 머티리얼** — 머티리얼마다 descriptor set 1개(`DescriptorSetLayout`/`Pool`,
   `DescriptorWriter`로 텍스처 + 상수 버퍼 기록).
4. `scene::generateScene` — 인스턴스 배열 생성 후 `dumpSceneJson`으로 덤프.
5. `onRender` — `sampleCamera`로 뷰/투영 갱신 → 인스턴스 루프:
   - 머티리얼 바뀌면 `vkCmdBindDescriptorSets` (B0)
   - 메시 버퍼 `vkCmdBindVertexBuffers`/`vkCmdBindIndexBuffer` (A0)
   - per-object 변환은 push constant
   - `vkCmdDrawIndexed`
   - 렌더 순서 = 조건 간 동일하게 **정렬 고정**(머티리얼→메시 그룹핑, 오버드로우 패턴 고정)

---

## 씬 생성 (고정 절차)

`scene::generateScene`(구현 완료). splitmix64 정수 PRNG — std distribution 안 씀,
플랫폼 무관 결정성.

```
rng = splitmix64(seed = 42)
for i in 0..N-1:
    model    = rng() % k            # modelCount
    position = XZ 그리드(i) + jitter(rng)
    rotation = 균일 랜덤 quat(Shoemake)
```

- 결과를 **JSON으로 덤프** → 조건 간 동일 파일 재사용 (조건 코드가 씬을 다시 굴리지 않음).
- N ∈ {128, 512, 2048, 8192, 32768} — 런타임 인자/설정으로 선택.
- JSON 스키마(`dumpSceneJson` 실제 출력):
  `{ "params": {"seed":42,"count":N,"modelCount":k,"spacing":..,"jitter":..},
     "instances": [{"model":i,"position":[x,y,z],"rotation":[w,x,y,z]}] }`

## 카메라 (결정론적)

`scene::CameraPath` + `sampleCamera`(구현 완료).

- `makeSweepPath`로 제어점 생성, **닫힌 Catmull-Rom 루프**, 720프레임 1루프.
- **프레임 인덱스**로 샘플링(벽시계·누적 dt 안 씀 → 같은 프레임 = 같은 뷰).
- 프러스텀 내 오브젝트 비율이 경로 전체에서 균일하도록 radius/height 조정.
- proj는 Vulkan 클립공간(depth 0..1, Y flip) 이미 반영.

---

## 계측 (bench/)

`bench::GpuQueries` + `CsvReporter`(구현 완료).

- **GPU 타임스탬프** — `GpuQueries` timestamp pool, 프레임 top/bottom-of-pipe.
  `timestampPeriod` 곱. 미지원(일부 MoltenVK) 시 0 반환 + `gpuSupported()` 플래그.
- **파이프라인 통계** — input assembly vertices/primitives, vertex/fragment shader
  invocations, clipping primitives. 삼각형·정점 수 조건 간 동일 검증.
  (template `Context`가 `pipelineStatisticsQuery`를 지원 시 baseline으로 활성.)
- **CPU** — `CpuTimer`로 커맨드 레코딩 구간과 서브밋 구간 **분리** 계측
  (A0는 레코딩에 N개 드로우 콜 비용 집중).
- **워밍업** — 시작·조건 전환 시 **300프레임 폐기**.
- **반복** — 조건당 5~10회, 중앙값 + IQR. 실행 순서 무작위화(seed 고정) 또는 쿨다운.
- **리포터** — `CsvReporter`: 한 행 = (조건, 해상도, N, 반복#) → 측정치.
  `dumpEnvironmentJson`: GPU/드라이버/API/OS 메타.

---

## 하니스 변경 (template)

- **Swapchain 프레젠트 모드 (미완)** — 현재 `swapchain.cpp`에서 `VK_PRESENT_MODE_FIFO_KHR`
  하드코딩. `VK_PRESENT_MODE_IMMEDIATE_KHR` opt-in 추가 필요(지원 시 선택, 미지원 시
  폴백 로그). vsync 제거는 실험 필수 조건. **lab_01에서 처리할 유일한 template 변경.**
- 나머지(Context/CommandPool/sync/FrameContext depth, render/asset/scene/bench)는 그대로 재사용.

---

## 검증 (구현 후)

- 픽셀 동등성 — 이 베이스라인 출력이 이후 조건의 기준 이미지. 허용오차 임계값 정의(채널당 ≤1 LSB, 불일치 < 0.01%).
- 파이프라인 통계로 삼각형/정점 수가 N·모델셋에 대해 결정론적인지 확인.
- SPIR-V 해시 기록 (`Shader::hash()`/`hashHex()`; 셰이더 분기 없는 베이스라인이지만 이후 `#define` 조건과 비교 기준).

---

<a id="english"></a>

# English

The **baseline** for the indirect-rendering experiment series. Full design in
[`../indirect-rendering-experiment.md`](../indirect-rendering-experiment.md).

- **A0** — multi-buffer (one VkBuffer per mesh) + plain `vkCmdDrawIndexed` loop
- **B0** — `vkCmdBindDescriptorSets` per material

Every later condition (A1–A3 × B0/B1) is interpreted relative to this combination.
The harness (scene loader, camera, instrumentation, reporter) that lab_02+ reuses
is **already implemented in `template`** (`lab::asset`, `lab::scene`, `lab::bench`,
`lab::render`). So lab_01 does not build the harness — it **wires the template API
into the A0 × B0 strategy**. A0 (per-mesh buffers) and B0 (per-material descriptor
sets) are the thing under test, so they live in the lab; `template` supplies only
the neutral building blocks.

---

## Module design (src/)

> The harness lives in `template`. lab_01 is a single `main.cpp` that wires it.

```
lab_01/src/
  main.cpp          IndirectLab : App. Load scene → GPU upload → draw loop + instrumentation in onRender.

template/src/ (reused, implemented)
  asset/loader.*    cgltf glTF → CpuModel (mesh/material). standardVertexInput().
  asset/texture.*   KTX2 (BC7/cubemap) → GpuImage. loadKtx / loadIbl.
  render/buffer.*   GpuBuffer (device-local/host-visible), StagingUploader (batched upload).
  render/descriptor.* DescriptorSetLayout/Pool/Writer (B0 material sets + later B1 bindless).
  render/pipeline.* PipelineBuilder (bakes fixed render state), PipelineLayout, GraphicsPipeline.
  render/shader.*   Shader::fromSpirv/fromFile + SPIR-V hash.
  scene/scene.*     generateScene(seed=42) + dumpSceneJson + sceneCenter/HalfExtent.
  scene/camera.*    CameraPath/makeSweepPath, sampleCamera (720-frame loop, by frame index).
  bench/instrument.* GpuQueries (timestamp + pipeline statistics), CpuTimer.
  bench/reporter.*  CsvReporter, dumpEnvironmentJson.
```

Dependencies: **cgltf**, **ktx** — already in `vcpkg.json` + the top-level `project_deps`.

---

## Data flow

1. `asset::loadModel` — load the glTF files of model set M into `CpuModel` (each
   primitive → vertex/index arrays + material index). Textures loaded from
   pre-compressed BC7 KTX2 via `asset::loadKtx`.
2. **A0 GPU upload** — two `GpuBuffer::createDeviceLocal` per mesh (vertex, index),
   batched through `StagingUploader` (no shared offsets, bind per mesh).
3. **B0 materials** — one descriptor set per material (`DescriptorSetLayout`/`Pool`,
   `DescriptorWriter` records texture + constants buffer).
4. `scene::generateScene` — build the instance array, then `dumpSceneJson`.
5. `onRender` — update view/proj via `sampleCamera` → loop over instances:
   - if material changed, `vkCmdBindDescriptorSets` (B0)
   - bind mesh vertex/index buffers `vkCmdBindVertexBuffers`/`vkCmdBindIndexBuffer` (A0)
   - per-object transform via push constant
   - `vkCmdDrawIndexed`
   - render order = **fixed sort** identical across conditions (material→mesh grouping, fixed overdraw pattern)

---

## Scene generation (fixed procedure)

`scene::generateScene` (implemented). splitmix64 integer PRNG — no std
distributions, so determinism is platform-independent.

```
rng = splitmix64(seed = 42)
for i in 0..N-1:
    model    = rng() % k            # modelCount
    position = XZ grid(i) + jitter(rng)
    rotation = uniform random quat (Shoemake)
```

- **Dump the result to JSON** → reuse the same file across conditions (condition code never re-rolls the scene).
- N ∈ {128, 512, 2048, 8192, 32768} — selected via runtime argument/config.
- JSON schema (actual `dumpSceneJson` output):
  `{ "params": {"seed":42,"count":N,"modelCount":k,"spacing":..,"jitter":..},
     "instances": [{"model":i,"position":[x,y,z],"rotation":[w,x,y,z]}] }`

## Camera (deterministic)

`scene::CameraPath` + `sampleCamera` (implemented).

- Control points from `makeSweepPath`, a **closed Catmull-Rom loop**, 720-frame loop.
- Sampled by **frame index** (no wall-clock, no accumulated dt → same frame = same view).
- Tune radius/height so the in-frustum object ratio is uniform across the path.
- proj already in Vulkan clip space (depth 0..1, Y-flipped).

---

## Instrumentation (bench/)

`bench::GpuQueries` + `CsvReporter` (implemented).

- **GPU timestamps** — `GpuQueries` timestamp pool, frame top/bottom-of-pipe,
  multiplied by `timestampPeriod`. Resolves to 0 with a `gpuSupported()` flag on
  unsupported paths (some MoltenVK).
- **Pipeline statistics** — input assembly vertices/primitives, vertex/fragment
  shader invocations, clipping primitives. Verify triangle/vertex count is
  identical across conditions. (template `Context` enables `pipelineStatisticsQuery`
  as a baseline when supported.)
- **CPU** — `CpuTimer` measures the command recording span and submit span
  **separately** (A0 concentrates the N draw-call cost in recording).
- **Warm-up** — **discard 300 frames** at start and on every condition switch.
- **Repetition** — 5–10 runs per condition, median + IQR. Randomize run order (fixed seed) or cool down.
- **Reporter** — `CsvReporter`: one row = (condition, resolution, N, repetition#) →
  measurements. `dumpEnvironmentJson`: GPU/driver/API/OS metadata.

---

## Harness changes (template)

- **Swapchain present mode (pending)** — currently hardcoded to `VK_PRESENT_MODE_FIFO_KHR`
  in `swapchain.cpp`. Add a `VK_PRESENT_MODE_IMMEDIATE_KHR` opt-in (select if
  supported, log a fallback otherwise). Removing vsync is a mandatory experiment
  condition. **The one template change lab_01 still has to make.**
- Everything else (Context/CommandPool/sync/FrameContext depth, render/asset/scene/bench) is reused as-is.

---

## Verification (after implementation)

- Pixel equivalence — this baseline output is the reference image for later conditions. Define a tolerance threshold (≤1 LSB per channel, mismatch < 0.01%).
- Use pipeline statistics to confirm triangle/vertex counts are deterministic for a given N and model set.
- Record the SPIR-V hash (`Shader::hash()`/`hashHex()`; baseline has no shader branch, but it is the comparison reference for later `#define` conditions).
