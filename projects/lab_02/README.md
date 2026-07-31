# Lab 02 — Bindless (A0 × B1)

**[한국어](#한국어) · [English](#english)**

<a id="한국어"></a>

# 한국어

전체 설계는 [`../indirect-rendering-experiment.md`](../indirect-rendering-experiment.md) 참조.

- **A0** — 다중 버퍼(메시당 VkBuffer) + 일반 `vkCmdDrawIndexed` 루프 — **lab_01과 동일**
- **B1** — Bindless: descriptor set을 프레임당 **1회** 바인딩, 드로우마다 인덱스로 참조

A축이 lab_01과 완전히 같으므로 **lab_01 vs lab_02 = B0 vs B1 단독 비교**다.
하니스(씬·카메라·계측·리포터)는 `template`에 이미 있고, lab_02는 `main.cpp` 하나로
template를 A0 × B1 전략에 배선한다.

---

## lab_01 대비 실제 변경점

| | lab_01 (B0) | lab_02 (B1) |
|---|---|---|
| 머티리얼 텍스처 | set 1 × 머티리얼 수, 머티리얼 바뀔 때 `vkCmdBindDescriptorSets` | 단일 `sampler2D textures[]` 배열, **리바인드 0회** |
| 머티리얼 상수 | 머티리얼당 UBO | `materials[]` SSBO 1개 |
| 오브젝트 변환 | push constant (`mat4 model`) | `objects[]` SSBO (setup에서 1회 빌드) |
| 인덱스 전달 | 없음(바인딩이 곧 상태) | `firstInstance` → `gl_BaseInstanceARB` |
| 드로우 루프 내 명령 | bind set(조건부) + bind VB/IB + push + draw | bind VB/IB + draw |

지오메트리 업로드·씬 파라미터(seed 42, N=25, k=1)·카메라·워밍업(300프레임)·
IMMEDIATE 프레젠트·라이팅 수식은 lab_01과 **바이트 단위로 동일**하게 유지한다.

---

## Bindless 인덱스 경로 (시리즈 전체 고정)

실험 설계 §3이 요구하는 "인덱스가 셰이더에 도달하는 법"을 여기서 **확정**한다.

```
드로우 i → vkCmdDrawIndexed(..., firstInstance = i)
         → 정점 셰이더 objects[gl_BaseInstanceARB] → { model, materialIndex }
         → 프래그먼트 materials[materialIndex] → { factors, 텍스처 인덱스 3개 }
```

- `gl_DrawID`가 아니라 `gl_BaseInstance`를 쓴 이유: **7개 조건 전부에서 동작하는
  유일한 경로**다. indirect 커맨드(A1/A2/A3)도 `firstInstance` 필드를 실어 나르므로
  셰이더 소스가 조건마다 갈리지 않고, 따라서 SPIR-V 해시가 시리즈 내내 고정된다.
  (A3에서 `gl_DrawID`를 쓰면 A0/A2용 셰이더가 따로 필요해진다.)
- 그 대가로 `model` 행렬이 push constant에서 SSBO로 이동한다. multi-draw(A3)에는
  드로우 사이 push 지점이 없으므로 선택의 여지가 없다. **B1의 CPU 레코딩에서
  per-object 행렬 계산이 사라진다는 점을 해석 시 반드시 기록할 것** — 이건 B1의
  이득 중 하나이지 측정 오류가 아니다.
- 필요 feature: `descriptorIndexing`, `shaderDrawParameters`. 둘 중 하나라도
  미지원이면 생성자가 즉시 throw한다(조용히 다른 조건을 측정하지 않기 위해).
  A1~A3로 갈 때 `drawIndirectFirstInstance`가 추가로 필요하다.

## Descriptor 레이아웃

```
set 0 (프레임당 1회 바인딩, frame-in-flight마다 1개)
  binding 0  UBO      Frame { viewProj, camPos, lightDir, lightColor, ambient }

set 1 (전체 실행 중 1회 바인딩, 단 하나)
  binding 0  SSBO     Material[] { baseColorFactor, mr, tex(uvec4) }   frag
  binding 1  SSBO     Object[]   { model(mat4), material(uvec4) }      vert
  binding 2  sampler2D textures[3 × 머티리얼 수]                        frag
```

- 텍스처 배열은 **정확한 크기로 할당**(`PARTIALLY_BOUND`/`UPDATE_AFTER_BIND` 없음).
  전 슬롯을 setup에서 1회 기록하고 이후 변경이 없기 때문. 이 플래그들은 2차 실험
  (컬링 on, 프레임당 재빌드)에서 필요해진다.
- 머티리얼 i의 텍스처 슬롯 = `3i`(baseColor), `3i+1`(metalRough), `3i+2`(normal).
  이 규칙은 `MaterialGpu::tex`에 그대로 저장되므로 셰이더는 규칙을 모른다.
- SSBO는 device-local. host-visible로 두면 GPU 타임스탬프가 드로우 전략이 아니라
  호스트 메모리 대역폭을 재게 된다.
- `nonuniformEXT`: 현재는 드로우당 인덱스가 dynamically uniform이라 불필요하지만,
  이후 조건이 드로우를 병합해도 정합성이 유지되도록 붙여 둔다.

---

## 드로우 순서 (조건 간 동일)

`m_draws`는 setup에서 **인스턴스 major, 메시 minor** 순으로 평탄화된다 —
lab_01의 이중 루프와 같은 순서. 오버드로우 패턴이 같아야 프래그먼트 수와 픽셀
출력이 베이스라인과 일치한다.

> lab_01 주석은 "실제 연구에서는 머티리얼→메시로 정렬해야 한다"고 적었지만, 그
> 정렬은 **B0의 리바인드 횟수를 줄이는** 최적화다. 두 랩 모두에 적용하지 않는 한
> 순서가 갈려 비교가 깨진다. 현재 모델셋은 머티리얼 1개라 두 순서가 동일하므로
> 보류. 모델셋을 늘릴 때 **lab_01과 lab_02를 함께** 바꿀 것.

---

## 계측

lab_01과 동일(`bench::GpuQueries` + `CsvReporter`). CSV `condition` 컬럼만
`A0xB1`로 바뀐다. 워밍업 300프레임 폐기, frame-in-flight마다 쿼리 풀 1개,
결과 해석은 조건당 5~10회 반복의 중앙값 + IQR.

SPIR-V 해시는 시작 시 로그로 남는다(`spirv mesh.vert=… mesh.frag=…`) —
A1~A3가 같은 셰이더를 쓴다는 증거로 기록할 것.

---

## 검증

- **픽셀 동등성** — lab_01 출력이 기준 이미지. 라이팅 수식이 동일하므로 차이는
  래스터 순서 미세차뿐이어야 한다(채널당 ≤1 LSB, 불일치 픽셀 < 0.01%).
- **삼각형/정점 수** — 파이프라인 통계가 lab_01과 일치해야 한다. (Apple/MoltenVK는
  `pipelineStatisticsQuery` 미지원 → 0으로 기록됨.)
- **디스크립터 회계** — B1에서 프레임당 `vkCmdBindDescriptorSets` 호출은 정확히 1회.

---

## 남은 것

- N 스윕(128~32768)과 해상도 스윕은 아직 하드코딩(N=25, 1280×720). 조건 스윕
  드라이버는 A축 랩들이 다 생긴 뒤 공통으로 붙이는 편이 낫다.
- CPU 서브밋 구간은 여전히 측정 불가(`App`이 submit을 소유). lab_01과 동일한
  제약이라 B0 vs B1 델타 자체는 유효하다.

---

<a id="english"></a>

# English

Full design in [`../indirect-rendering-experiment.md`](../indirect-rendering-experiment.md).

- **A0** — multi-buffer (one VkBuffer per mesh) + plain `vkCmdDrawIndexed` loop — **identical to lab_01**
- **B1** — bindless: descriptor sets bound **once** per frame, indexed per draw

Because the A axis is unchanged from lab_01, **lab_01 vs lab_02 is a standalone
B0 vs B1 comparison**. The harness (scene, camera, instrumentation, reporter)
already lives in `template`; lab_02 is a single `main.cpp` wiring it into A0 × B1.

---

## What actually changed vs lab_01

| | lab_01 (B0) | lab_02 (B1) |
|---|---|---|
| Material textures | one set 1 per material, `vkCmdBindDescriptorSets` on change | one `sampler2D textures[]` array, **zero rebinds** |
| Material constants | one UBO per material | a single `materials[]` SSBO |
| Object transform | push constant (`mat4 model`) | `objects[]` SSBO, built once at setup |
| Index delivery | none (binding *is* the state) | `firstInstance` → `gl_BaseInstanceARB` |
| Commands in the draw loop | bind set (conditional) + bind VB/IB + push + draw | bind VB/IB + draw |

Geometry upload, scene parameters (seed 42, N=25, k=1), camera, 300-frame warm-up,
IMMEDIATE present and the lighting math are kept **byte-for-byte identical** to lab_01.

---

## Bindless index path (fixed for the whole series)

This lab pins down "how the index reaches the shader", which §3 of the experiment
design requires to be constant across conditions.

```
draw i → vkCmdDrawIndexed(..., firstInstance = i)
       → vertex: objects[gl_BaseInstanceARB] → { model, materialIndex }
       → fragment: materials[materialIndex]  → { factors, 3 texture indices }
```

- `gl_BaseInstance` rather than `gl_DrawID` because it is **the only path that
  works in all 7 conditions**: indirect commands (A1/A2/A3) carry a `firstInstance`
  field too, so the shader source never forks and the SPIR-V hash stays fixed
  across the series. (Using `gl_DrawID` for A3 would require a second shader for
  A0/A2.)
- The cost is that `model` moves from a push constant into the SSBO. Multi-draw
  (A3) has no per-draw push point, so this is not a choice. **Record that B1's CPU
  recording therefore loses lab_01's per-object matrix multiply** — that is part
  of what B1 buys, not a measurement error.
- Required features: `descriptorIndexing`, `shaderDrawParameters`. The constructor
  throws if either is missing, rather than quietly measuring a different condition.
  A1–A3 will additionally need `drawIndirectFirstInstance`.

## Descriptor layout

```
set 0 (bound once per frame, one per frame-in-flight)
  binding 0  UBO      Frame { viewProj, camPos, lightDir, lightColor, ambient }

set 1 (bound once for the whole run; there is exactly one)
  binding 0  SSBO     Material[] { baseColorFactor, mr, tex(uvec4) }   frag
  binding 1  SSBO     Object[]   { model(mat4), material(uvec4) }      vert
  binding 2  sampler2D textures[3 × material count]                    frag
```

- The texture array is **exactly sized** — no `PARTIALLY_BOUND` /
  `UPDATE_AFTER_BIND`, because every slot is written once at setup and never
  changes. Those flags become necessary in experiment 2 (culling on, per-frame
  rebuild).
- Material *i* owns texture slots `3i` (baseColor), `3i+1` (metalRough),
  `3i+2` (normal). That rule is baked into `MaterialGpu::tex`, so the shader
  never knows it.
- The SSBOs are device-local. Leaving them host-visible would make the GPU
  timestamp measure host memory bandwidth instead of the draw strategy.
- `nonuniformEXT` is not strictly needed today (the index is dynamically uniform
  per draw) but keeps the shader correct if a later condition merges draws.

---

## Draw order (identical across conditions)

`m_draws` is flattened at setup **instance-major, mesh-minor** — the same order as
lab_01's nested loop. The overdraw pattern must match for fragment counts and
pixel output to match the baseline.

> lab_01's comment says a real study should sort by material→mesh. That sort is an
> optimization **for B0's rebind count**; applying it to only one lab would break
> the comparison. With the current single-material model set both orders are
> identical, so it is deferred — change **lab_01 and lab_02 together** when the
> model set grows.

---

## Instrumentation

Same as lab_01 (`bench::GpuQueries` + `CsvReporter`); only the CSV `condition`
column changes to `A0xB1`. 300 warm-up frames discarded, one query pool per
frame-in-flight, results interpreted as median + IQR over 5–10 repetitions.

SPIR-V hashes are logged at startup (`spirv mesh.vert=… mesh.frag=…`) — record
them as evidence that A1–A3 run the identical shaders.

---

## Verification

- **Pixel equivalence** — lab_01's output is the reference. The lighting math is
  identical, so any difference should be raster-order noise only (≤1 LSB per
  channel, mismatched pixels < 0.01%).
- **Triangle/vertex counts** — pipeline statistics must match lab_01. (Apple /
  MoltenVK does not support `pipelineStatisticsQuery`, so these log as 0.)
- **Descriptor accounting** — B1 must issue exactly one `vkCmdBindDescriptorSets`
  per frame.

---

## Not done yet

- The N sweep (128–32768) and resolution sweep are still hardcoded (N=25,
  1280×720). A condition-sweep driver is better added once the A-axis labs exist,
  as shared code.
- The CPU submit span is still unmeasurable (`App` owns submit). Same constraint
  as lab_01, so the B0 vs B1 delta itself remains valid.
