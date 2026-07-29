# Indirect 렌더링 성능 실험 설계 / Indirect Rendering Performance Experiment

**[한국어](#한국어) · [English](#english)**

<a id="한국어"></a>

# 한국어

다양한 지오메트리·머티리얼을 가진 장면에서 **지오메트리 버퍼 전략**과
**머티리얼 바인딩 전략**이 Indirect 렌더링 성능에 미치는 영향을 측정한다.
이 문서는 실험 시리즈 전체의 기준점(single source of truth)이다.

---

## 1. 요인 설계

### 주요인 A — 지오메트리 버퍼 전략

| 코드 | 버퍼 레이아웃 | 드로우 기법 |
|------|--------------|------------|
| A0 | 다중 버퍼 (메시당 VkBuffer) | 일반 `vkCmdDrawIndexed` |
| A1 | 다중 버퍼 | 버퍼당 indirect |
| A2 | 단일 버퍼 (오프셋 참조) | 오브젝트당 indirect |
| A3 | 단일 버퍼 | multi-draw indirect (`vkCmdDrawIndexedIndirect(Count)`) |

### 주요인 B — 머티리얼 바인딩 전략

| 코드 | 방식 |
|------|------|
| B0 | 머티리얼마다 `vkCmdBindDescriptorSets` |
| B1 | Bindless (descriptor indexing) — 세트 1회 바인딩 + 인덱스 참조 |

**제약: A3 × B0 불가능.** multi-draw는 드로우 사이 재바인딩 지점이 없으므로
머티리얼별 descriptor set 교체 불가. Bindless(B1) 필수.

→ 유효 파이프라인 조건 = **4 × 2 − 1 = 7개**.

### ⚠ A 요인은 직교가 아니다 (비교쌍 주의)

A는 두 축(버퍼 레이아웃 × 드로우 기법)이 섞인 목적형 집합이다.
깔끔한 2×2가 아니므로 **아래 비교쌍만 단독 결론에 사용**한다.

- **A0 vs A1** — 버퍼 동일(다중). 직접 드로우 vs indirect의 CPU 제출 비용 차이.
- **A2 vs A3** — 버퍼 동일(단일). multi-draw 배칭 이득.
- **A1 vs A2** — 버퍼 전략 + granularity가 함께 변함 → **단독 결론 금지**.

---

## 2. 부요인

| 부요인 | 수준 |
|--------|------|
| 해상도 | 1280×720 / 1920×1080 / 3840×2160 |
| 오브젝트 수 | 128 / 512 / 2048 / 8192 / 32768 |
| 컬링 | 1차 실험 off / 2차 실험 on |

**해상도 해석 주의:** 컬링 off면 프래그먼트 부하는 드로우 전략과 무관하다.
- 720p = 제출 비용 격리 (드로우 전략 차이가 드러나는 해상도)
- 4K = 프래그먼트 바운드 → 실사용에서 전략 차이가 묻히는지 확인
- 해상도별로 **가설을 분리해** 서술할 것.

전체 조건 수 = 7(파이프라인) × 3(해상도) × 5(오브젝트) = **105 조건** (1차, 컬링 off).

---

## 3. 반드시 고정

- **카메라 경로** — 하드코딩 스플라인, 프레임 인덱스로 결정론적 샘플링. `dt`도 고정값.
- **삼각형/정점 수** — pipeline statistics로 조건 간 동일함을 검증.
- **셰이더** — 동일 소스, `#define`으로만 분기, SPIR-V 해시 기록.
- **프레젠트 모드** — `VK_PRESENT_MODE_IMMEDIATE_KHR` (vsync 제거).
- **프레임 인플라이트 수, 스왑체인 이미지 수.**
- **깊이 프리패스 유무, MSAA, 텍스처 밉/이방성 필터링.**
- **텍스처 총량·포맷** — 모든 조건이 동일한 무압축 RGBA8(stb 로드)로 통일.
- **렌더 순서** — 조건 간 동일한 그리기 순서 (오버드로우 패턴 고정).
- **Bindless 인덱스 경로** — 오브젝트 인덱스가 셰이더에 도달하는 법을 고정.
  `gl_DrawID`(멀티드로우) vs `gl_BaseInstance` 트릭(오브젝트별)이 조건마다
  다르면 셰이더 분기가 갈려 SPIR-V 해시도 달라진다. 경로 고정 후 기록.

---

## 4. 반드시 기록

- GPU / 드라이버 버전, OS, CPU, RAM
- GPU 클럭 (측정 시점 실측)
- 전원 프로파일 (노트북은 유선 전원 고정)
- **워밍업** — 파이프라인 컴파일·셰이더 캐시·GPU 클럭 부스트가 안정될 때까지
  최소 **300 프레임 버리고** 시작. 조건 전환 시마다 반복.

---

## 5. 측정 지표와 계측

1. **GPU 타임스탬프** — `timestampPeriod`·`validQueryBits` 반영, top/bottom-of-pipe.
2. **파이프라인 통계** — vertex/primitive/fragment invocation, 삼각형 수 검증.
3. **CPU 측정** — 커맨드 레코딩 시간과 서브밋 시간을 분리 계측.
4. **메모리** — VkDeviceMemory **할당 개수**와 총 바이트 둘 다. 다중 버퍼는
   서브얼로케이션 패딩, 단일 버퍼는 큰 연속 할당.
5. **픽셀 동등성 검증** — 조건 간 출력 일치.

### 측정 신뢰성 (설계 필수)

- **반복 횟수** — 조건당 1회는 노이즈. **조건당 5~10회** 돌려
  **중앙값 + IQR** 보고.
- **조건 전환 순서 = 열 드리프트 편향** — 105 조건 순차 실행하면 뒤쪽이 더 뜨거운
  GPU에서 측정된다. 반복마다 **조건 순서 무작위화**(seed 고정) 또는 조건 간 쿨다운.
  GPU 클럭 기록만으론 편향 제거 안 됨.
- **컴포지터 간섭** — IMMEDIATE로 vsync는 뺐지만 present 자체가 OS 컴포지터를 탄다.
  순수 측정하려면 **오프스크린 헤드리스 렌더** 권장 (present 없이 GPU 타임스탬프만).
  present 유지 시 이유 기록.
- **픽셀 동등성 허용오차 정의** — 드로우 경로가 다르면 래스터 순서 미세차로
  부동소수점 결과가 달라진다. exact match 아니라 임계값 명시
  (예: 채널당 ≤1 LSB, 불일치 픽셀 < 0.01%).
- **Indirect 버퍼 빌드 비용** — 1차(컬링 off)는 indirect 버퍼가 정적(1회 빌드)
  → 프레임당 CPU 0, 기록만. **2차(컬링 on)는 프레임당 재빌드 비용이 진짜 승부처**
  → CPU 측정에 그 비용이 포함되게 미리 설계.

---

## 6. 장면 구성 절차

1. 모델 세트 M = {m1, m2, … mk} 선택.
2. 고정 시드 PRNG(seed=42)로 격자 배치.
3. `for i in 0..N-1: model = M[rng() % k]; position = grid(i) + jitter(rng); rotation = rng();`
   → 배치 결과를 **JSON으로 덤프**해 조건 간 동일 사용.
4. 카메라 경로가 격자 위를 훑도록 설계 (초당 일정 속도, **720 프레임 1루프**).
5. 프러스텀 내 오브젝트 비율이 경로 전체에서 균일하도록 조정.

---

## 7. 장치 기능 (Vulkan feature) 체크

- `multiDrawIndirect` — A1/A2/A3
- `drawIndirectCount` (`vkCmdDrawIndexedIndirectCount`) — 컬링 2차, A3
- `shaderDrawParameters` (VK 1.1+) — `gl_DrawID` 인덱스 경로
- `drawIndirectFirstInstance` — `firstInstance ≠ 0`로 인덱싱할 때
- descriptor indexing (`descriptorIndexing`) — B1 bindless

lab_01의 `DeviceFeatures`에 이미 상당수 요청됨. 조건별로 필요한 것만 opt-in.

---

## 8. 랩 분할 계획

| 랩 | 조건 | 목적 |
|----|------|------|
| **lab_01** | **A0 × B0** | **기준점(baseline).** 다중 버퍼 + 직접 드로우 + 머티리얼별 바인딩. 이후 모든 조건이 이것 대비로 해석됨. |
| lab_02+ | A1/A2/A3 × B0/B1 | 나머지 6개 조건. 공통 하니스(장면 로더·계측·리포터) 공유. |
| (후속) | 컬링 on | 2차 실험. indirect 버퍼 GPU 재빌드(compute cull). |

### lab_01 (A0 × B0) 범위

- 다중 버퍼: 메시당 VkBuffer.
- 오브젝트마다 `vkCmdDrawIndexed` 루프.
- 머티리얼마다 `vkCmdBindDescriptorSets`.
- 공통 하니스 씨앗: 결정론적 씬 생성(seed=42, JSON 덤프), 스플라인 카메라(720프레임 루프),
  GPU 타임스탬프 + 파이프라인 통계 + CPU 레코딩/서브밋 분리 계측, 워밍업 300프레임,
  IMMEDIATE 프레젠트.
- 이 계측·씬·리포터 골격을 재사용 가능하게 만들어 lab_02+가 조건만 갈아끼우게 한다.

---

<a id="english"></a>

# English

Measures how a **geometry buffer strategy** and a **material binding strategy**
affect indirect-rendering performance in a scene with diverse geometry and
materials. This document is the single source of truth for the whole experiment
series.

---

## 1. Factor design

### Main factor A — geometry buffer strategy

| Code | Buffer layout | Draw technique |
|------|--------------|----------------|
| A0 | Multi-buffer (one VkBuffer per mesh) | Plain `vkCmdDrawIndexed` |
| A1 | Multi-buffer | Per-buffer indirect |
| A2 | Single buffer (offset references) | Per-object indirect |
| A3 | Single buffer | Multi-draw indirect (`vkCmdDrawIndexedIndirect(Count)`) |

### Main factor B — material binding strategy

| Code | Method |
|------|--------|
| B0 | `vkCmdBindDescriptorSets` per material |
| B1 | Bindless (descriptor indexing) — bind set once + index reference |

**Constraint: A3 × B0 is impossible.** Multi-draw has no rebind point between
draws, so per-material descriptor set swaps are impossible. Bindless (B1) is
required.

→ Valid pipeline conditions = **4 × 2 − 1 = 7**.

### ⚠ Factor A is not orthogonal (mind the comparison pairs)

A is a purposive set mixing two axes (buffer layout × draw technique). It is not
a clean 2×2, so **use only the pairs below for standalone conclusions**.

- **A0 vs A1** — same buffer (multi). Direct draw vs indirect CPU submission cost.
- **A2 vs A3** — same buffer (single). Multi-draw batching gain.
- **A1 vs A2** — buffer strategy + granularity both change → **no standalone conclusion**.

---

## 2. Sub-factors

| Sub-factor | Levels |
|------------|--------|
| Resolution | 1280×720 / 1920×1080 / 3840×2160 |
| Object count | 128 / 512 / 2048 / 8192 / 32768 |
| Culling | off in experiment 1 / on in experiment 2 |

**Resolution interpretation:** with culling off, fragment load is independent of
draw strategy.
- 720p = isolates submission cost (resolution where draw-strategy differences surface)
- 4K = fragment-bound → checks whether strategy differences get masked in practice
- State a **separate hypothesis per resolution**.

Total conditions = 7 (pipeline) × 3 (resolution) × 5 (object count) = **105
conditions** (experiment 1, culling off).

---

## 3. Must fix

- **Camera path** — hardcoded spline, deterministic sampling by frame index. `dt` fixed too.
- **Triangle/vertex count** — verify identical across conditions via pipeline statistics.
- **Shaders** — same source, branch only via `#define`, record SPIR-V hash.
- **Present mode** — `VK_PRESENT_MODE_IMMEDIATE_KHR` (remove vsync).
- **Frames in flight, swapchain image count.**
- **Depth pre-pass presence, MSAA, texture mip/anisotropic filtering.**
- **Total texture amount and format** — unify as uncompressed RGBA8 (stb-loaded) across all conditions.
- **Render order** — identical draw order across conditions (fixed overdraw pattern).
- **Bindless index path** — fix how the object index reaches the shader.
  If `gl_DrawID` (multi-draw) vs the `gl_BaseInstance` trick (per-object) differs
  per condition, the shader branch diverges and so does the SPIR-V hash. Fix the
  path, then record it.

---

## 4. Must record

- GPU / driver version, OS, CPU, RAM
- GPU clock (measured at capture time)
- Power profile (laptops: pin to wired power)
- **Warm-up** — discard at least **300 frames** until pipeline compilation, shader
  cache, and GPU clock boost stabilize. Repeat on every condition switch.

---

## 5. Metrics and instrumentation

1. **GPU timestamps** — apply `timestampPeriod`·`validQueryBits`, top/bottom-of-pipe.
2. **Pipeline statistics** — vertex/primitive/fragment invocations, verify triangle count.
3. **CPU measurement** — measure command recording time and submit time separately.
4. **Memory** — both VkDeviceMemory **allocation count** and total bytes. Multi-buffer
   incurs sub-allocation padding; single buffer is one large contiguous allocation.
5. **Pixel equivalence check** — output matches across conditions.

### Measurement reliability (design-critical)

- **Repetition count** — one run per condition is noise. Run **5–10 times per
  condition**, report **median + IQR**.
- **Condition switch order = thermal-drift bias** — running 105 conditions in
  sequence measures later ones on a hotter GPU. **Randomize condition order** per
  repetition (fixed seed) or cool down between conditions. Recording GPU clock
  alone does not remove the bias.
- **Compositor interference** — IMMEDIATE removes vsync, but present itself goes
  through the OS compositor. For a clean measurement, prefer **offscreen headless
  rendering** (GPU timestamps only, no present). If keeping present, record why.
- **Define pixel-equivalence tolerance** — different draw paths shift raster order,
  so floating-point results differ. Specify a threshold instead of exact match
  (e.g. ≤1 LSB per channel, mismatched pixels < 0.01%).
- **Indirect buffer build cost** — experiment 1 (culling off) has a static indirect
  buffer (built once) → 0 per-frame CPU, record only. **Experiment 2 (culling on)
  makes the per-frame rebuild cost the real battleground** → design so CPU
  measurement includes that cost.

---

## 6. Scene construction procedure

1. Choose model set M = {m1, m2, … mk}.
2. Grid placement with fixed-seed PRNG (seed=42).
3. `for i in 0..N-1: model = M[rng() % k]; position = grid(i) + jitter(rng); rotation = rng();`
   → **dump the placement to JSON** and reuse the same file across conditions.
4. Design the camera path to sweep over the grid (constant speed, **720-frame loop**).
5. Tune so the in-frustum object ratio is uniform across the whole path.

---

## 7. Device feature (Vulkan feature) checklist

- `multiDrawIndirect` — A1/A2/A3
- `drawIndirectCount` (`vkCmdDrawIndexedIndirectCount`) — culling exp 2, A3
- `shaderDrawParameters` (VK 1.1+) — `gl_DrawID` index path
- `drawIndirectFirstInstance` — when indexing with `firstInstance ≠ 0`
- descriptor indexing (`descriptorIndexing`) — B1 bindless

Many are already requested in lab_01's `DeviceFeatures`. Opt in per condition to
only what it needs.

---

## 8. Lab breakdown

| Lab | Condition | Purpose |
|-----|-----------|---------|
| **lab_01** | **A0 × B0** | **Baseline.** Multi-buffer + direct draw + per-material binding. Every later condition is interpreted relative to this. |
| lab_02+ | A1/A2/A3 × B0/B1 | The remaining 6 conditions. Share the common harness (scene loader, instrumentation, reporter). |
| (later) | Culling on | Experiment 2. GPU rebuild of the indirect buffer (compute cull). |

### lab_01 (A0 × B0) scope

- Multi-buffer: one VkBuffer per mesh.
- `vkCmdDrawIndexed` loop per object.
- `vkCmdBindDescriptorSets` per material.
- Common harness seed: deterministic scene generation (seed=42, JSON dump), spline
  camera (720-frame loop), GPU timestamps + pipeline statistics + separate
  CPU recording/submit measurement, 300-frame warm-up, IMMEDIATE present.
- Make this instrumentation/scene/reporter skeleton reusable so lab_02+ only swaps
  the condition.
