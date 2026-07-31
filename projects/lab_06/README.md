# Lab 06 — 단일 버퍼 + Indirect + Bindless (A2 × B1)

**[한국어](#한국어) · [English](#english)**

<a id="한국어"></a>

# 한국어

전체 설계는 [`../indirect-rendering-experiment.md`](../indirect-rendering-experiment.md) 참조.

- **A2** — 단일 버퍼 + 오브젝트당 `vkCmdDrawIndexedIndirect` — **lab_05와 동일**
- **B1** — Bindless, 세트 1회 바인딩 + 인덱스 참조 — **lab_02/lab_04와 동일**

lab_05의 지오메트리 경로와 lab_02의 머티리얼 경로를 합친 조건. 새 메커니즘은 없다.

---

## 드로우 루프에 남은 것

A2와 B1이 겹치면 **프레임당 바인딩이 전부 사라지고, 루프 본문이 Vulkan 호출 하나가
된다**:

```cpp
vkCmdBindDescriptorSets(..., 0, 2, sets, ...);  // set 0 + set 1, 프레임당 1회
vkCmdBindVertexBuffers(...);                    // 프레임당 1회
vkCmdBindIndexBuffer(...);                      // 프레임당 1회

for (uint32_t i = 0; i < m_draws.size(); ++i) {
    vkCmdDrawIndexedIndirect(buf, i * stride, /*drawCount=*/1, stride);
}
```

프레임당 커맨드 = `4 + N`. 여섯 조건 중 **드로우 호출 외의 오버헤드가 가장 적은
지점**이며, 그래서 lab_07(A3)이 남은 N개 호출을 1개로 접었을 때의 차이가
`vkCmdDrawIndexedIndirect` 자체의 제출 비용으로 깨끗하게 읽힌다.

| 랩 | 조건 | 드로우 루프 본문 |
|---|---|---|
| lab_01 | A0×B0 | 세트 바인딩(조건부) + VB/IB 바인딩 + 드로우 |
| lab_02 | A0×B1 | VB/IB 바인딩 + 드로우 |
| lab_03 | A1×B0 | 세트 바인딩(조건부) + VB/IB 바인딩 + indirect (메시당) |
| lab_04 | A1×B1 | VB/IB 바인딩 + indirect (메시당) |
| lab_05 | A2×B0 | 세트 바인딩(조건부) + indirect |
| **lab_06** | **A2×B1** | **indirect 하나** |

## 구현

전부 기존 랩에서 가져왔다.

| | 출처 | 내용 |
|---|---|---|
| 지오메트리 | lab_05 | 단일 정점/인덱스 버퍼, 메시 = `firstIndex`/`vertexOffset` 범위 |
| indirect 버퍼 | lab_05 | 드로우당 커맨드 1개, 메시 오프셋 + `firstInstance = i` |
| 머티리얼 | lab_02 | `materials[]` SSBO + `texture2D textures[36]` + 공유 샘플러 1개 |
| 정점 셰이더 | 시리즈 공용 | `objects[gl_BaseInstanceARB]` |
| 프래그먼트 셰이더 | lab_02 | `materials[inMaterial]`, `nonuniformEXT` 인덱싱 |

정렬 순서는 유지한다. B1은 머티리얼 그룹핑이 필요 없고 A2는 배칭을 하지 않지만,
**렌더 순서 자체가 고정 제어**(설계 §3)이기 때문이다. 순서가 조건마다 다르면
오버드로우 패턴이 갈려 픽셀 동등성과 프래그먼트 수 비교가 무너진다.

## SPIR-V 해시 (실측)

| | mesh.vert | mesh.frag |
|---|---|---|
| lab_01 (A0×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| lab_02 (A0×B1) | `69ea042b2fa47005` | `d8aa2b932838c51d` |
| lab_03 (A1×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| lab_04 (A1×B1) | `69ea042b2fa47005` | `d8aa2b932838c51d` |
| lab_05 (A2×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| **lab_06 (A2×B1)** | `69ea042b2fa47005` | `d8aa2b932838c51d` |

여섯 조건 전부 정점 셰이더가 같고, 프래그먼트는 B 수준에만 의존한다.
A축 세 수준(A0/A1/A2)이 셰이더를 전혀 건드리지 않는다는 증거다.

필요 feature: `drawIndirectFirstInstance`, `shaderDrawParameters`(A2) +
`descriptorIndexing`(B1). `multiDrawIndirect`는 **요청하지 않는다** — A2는
`drawCount = 1`만 쓰므로 필요 없고, 그 feature의 유무가 곧 A3와의 경계다.

## Descriptor 레이아웃

```
set 0 (프레임당 1회 바인딩, frame-in-flight마다 1개)
  binding 0  UBO       Frame { viewProj, camPos, lightDir, lightColor, ambient }
  binding 1  SSBO      Object[] { model(mat4), material(uvec4) }        vert

set 1 (전체 실행 중 1회 바인딩, 단 하나) — lab_02/lab_04와 동일
  binding 0  SSBO      Material[] { baseColorFactor, mr, tex(uvec4) }   frag
  binding 1  texture2D textures[3 × 머티리얼 수]                         frag
  binding 2  sampler   공유 샘플러 1개                                   frag
```

---

## ⚠ 비교쌍 주의

lab_04(A1) vs lab_06(A2)는 버퍼 전략과 granularity가 함께 바뀌므로 **단독 결론
금지**(설계 §1). 유효한 쌍은 lab_05 ↔ lab_06 (B0 vs B1) 과 lab_06 ↔ lab_07 (A2 vs
A3, 둘 다 B1) 이다.

## 실행

```
LAB_OBJECTS=<N> LAB_FRAMES=800 ./lab_06          # 300 워밍업 + 500 측정 → results.csv
LAB_OBJECTS=128 LAB_FRAMES=401 LAB_CAPTURE=400 LAB_CAPTURE_FILE=lab06.png ./lab_06
imgdiff lab01.png lab06.png                      # 픽셀 동등성
```

CSV의 `condition` 컬럼은 `A2xB1`.

## 검증

- **픽셀 동등성** — N=128, 프레임 400, 1280×720에서 lab_01과 **bit-exact**.
  여섯 조건이 서로 모두 일치한다.
- **단일 버퍼 크기** — `A2 single buffer: 92128 vertices, 435360 indices` (lab_05와 동일).
- **검증 레이어** — 메시지 0건. B1 프래그먼트가 정점 출력 location 4를 실제로 읽으므로
  B0 랩들의 경고가 여기서는 나지 않는다.
- **SPIR-V 해시** — 위 표대로.

---

<a id="english"></a>

# English

Full design in [`../indirect-rendering-experiment.md`](../indirect-rendering-experiment.md).

- **A2** — single buffer + per-object `vkCmdDrawIndexedIndirect` — **identical to lab_05**
- **B1** — bindless, set bound once + index reference — **identical to lab_02/lab_04**

lab_05's geometry path joined to lab_02's material path. No new mechanism.

---

## What is left in the draw loop

Where A2 and B1 meet, **every per-frame bind disappears and the loop body becomes
a single Vulkan call**:

```cpp
vkCmdBindDescriptorSets(..., 0, 2, sets, ...);  // set 0 + set 1, once per frame
vkCmdBindVertexBuffers(...);                    // once per frame
vkCmdBindIndexBuffer(...);                      // once per frame

for (uint32_t i = 0; i < m_draws.size(); ++i) {
    vkCmdDrawIndexedIndirect(buf, i * stride, /*drawCount=*/1, stride);
}
```

Per-frame commands = `4 + N`. This is the **lowest non-draw overhead of the six
conditions**, which is what makes the step to lab_07 (A3, folding those N calls
into one) read cleanly as the submission cost of `vkCmdDrawIndexedIndirect`
itself.

| Lab | Condition | Draw-loop body |
|---|---|---|
| lab_01 | A0×B0 | bind set (conditional) + bind VB/IB + draw |
| lab_02 | A0×B1 | bind VB/IB + draw |
| lab_03 | A1×B0 | bind set (conditional) + bind VB/IB + indirect (per mesh) |
| lab_04 | A1×B1 | bind VB/IB + indirect (per mesh) |
| lab_05 | A2×B0 | bind set (conditional) + indirect |
| **lab_06** | **A2×B1** | **one indirect call** |

## Implementation

All of it comes from existing labs.

| | From | What |
|---|---|---|
| Geometry | lab_05 | single vertex/index buffer, mesh = `firstIndex`/`vertexOffset` range |
| Indirect buffer | lab_05 | one command per draw, mesh offsets + `firstInstance = i` |
| Material | lab_02 | `materials[]` SSBO + `texture2D textures[36]` + one shared sampler |
| Vertex shader | series-wide | `objects[gl_BaseInstanceARB]` |
| Fragment shader | lab_02 | `materials[inMaterial]`, `nonuniformEXT` indexing |

The sort order is kept. B1 needs no material grouping and A2 does no batching, but
**render order is itself a fixed control** (design §3): a per-condition order would
change the overdraw pattern and break both pixel equivalence and fragment-count
comparisons.

## SPIR-V hashes (measured)

| | mesh.vert | mesh.frag |
|---|---|---|
| lab_01 (A0×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| lab_02 (A0×B1) | `69ea042b2fa47005` | `d8aa2b932838c51d` |
| lab_03 (A1×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| lab_04 (A1×B1) | `69ea042b2fa47005` | `d8aa2b932838c51d` |
| lab_05 (A2×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| **lab_06 (A2×B1)** | `69ea042b2fa47005` | `d8aa2b932838c51d` |

All six share a vertex shader, and the fragment shader depends only on the B level
— evidence that the three A levels touch no shader at all.

Required features: `drawIndirectFirstInstance`, `shaderDrawParameters` (A2) plus
`descriptorIndexing` (B1). `multiDrawIndirect` is **not** requested: A2 only uses
`drawCount = 1`, and the presence of that feature is precisely the boundary with A3.

## Descriptor layout

```
set 0 (bound once per frame, one per frame-in-flight)
  binding 0  UBO       Frame { viewProj, camPos, lightDir, lightColor, ambient }
  binding 1  SSBO      Object[] { model(mat4), material(uvec4) }        vert

set 1 (bound once for the whole run; there is exactly one) — as lab_02/lab_04
  binding 0  SSBO      Material[] { baseColorFactor, mr, tex(uvec4) }   frag
  binding 1  texture2D textures[3 × material count]                     frag
  binding 2  sampler   one shared sampler                               frag
```

---

## ⚠ Mind the comparison pair

lab_04 (A1) vs lab_06 (A2) changes buffer strategy and granularity together, so
**no standalone conclusion** (design §1). The valid pairs are lab_05 ↔ lab_06
(B0 vs B1) and lab_06 ↔ lab_07 (A2 vs A3, both B1).

## Running

```
LAB_OBJECTS=<N> LAB_FRAMES=800 ./lab_06          # 300 warm-up + 500 measured -> results.csv
LAB_OBJECTS=128 LAB_FRAMES=401 LAB_CAPTURE=400 LAB_CAPTURE_FILE=lab06.png ./lab_06
imgdiff lab01.png lab06.png                      # pixel equivalence
```

The CSV `condition` column reads `A2xB1`.

## Verification

- **Pixel equivalence** — **bit-exact** against lab_01 at N=128, frame 400,
  1280×720. All six conditions match each other.
- **Single-buffer size** — `A2 single buffer: 92128 vertices, 435360 indices`
  (same as lab_05).
- **Validation layer** — zero messages. The B1 fragment shader actually reads the
  vertex output at location 4, so the B0 labs' warning does not occur here.
- **SPIR-V hashes** — per the table above.
