# Lab 04 — 버퍼당 Indirect + Bindless (A1 × B1)

**[한국어](#한국어) · [English](#english)**

<a id="한국어"></a>

# 한국어

전체 설계는 [`../indirect-rendering-experiment.md`](../indirect-rendering-experiment.md) 참조.

- **A1** — 다중 버퍼 + **버퍼당** `vkCmdDrawIndexedIndirect` — **lab_03과 동일**
- **B1** — Bindless, 세트 1회 바인딩 + 인덱스 참조 — **lab_02와 동일**

새로 만든 코드가 거의 없다. lab_03의 드로우 경로와 lab_02의 머티리얼 경로를
그대로 합친 조건이며, 그래서 **2×2 격자가 닫힌다**.

---

## 2×2 격자에서의 위치

| | B0 (머티리얼당 세트) | B1 (Bindless) |
|---|---|---|
| **A0** (오브젝트당 드로우) | lab_01 | lab_02 |
| **A1** (버퍼당 indirect) | lab_03 | **lab_04** |

lab_04가 생기면서 **네 방향 비교가 모두 성립**한다:

| 비교 | 격리되는 것 |
|---|---|
| lab_01 ↔ lab_02 | B0 vs B1 (A0에서) |
| lab_03 ↔ lab_04 | B0 vs B1 (A1에서) |
| lab_01 ↔ lab_03 | A0 vs A1 (B0에서) |
| lab_02 ↔ lab_04 | A0 vs A1 (B1에서) |

같은 축의 두 비교가 서로를 검증한다. B0 vs B1 효과가 A0과 A1에서 크게 다르면
두 요인 사이에 상호작용이 있다는 뜻이고, 그건 단독 결론을 쓰기 전에 반드시
확인해야 할 사항이다.

---

## 구현

lab_03과 lab_02에서 그대로 가져왔다. 고유한 부분은 없다.

| | 출처 | 내용 |
|---|---|---|
| 지오메트리 / 드로우 | lab_03 | 메시당 VkBuffer, 메시별 `MeshBatch` → `vkCmdDrawIndexedIndirect` |
| indirect 버퍼 | lab_03 | 드로우당 커맨드 1개, `firstInstance = i`, setup에서 1회 빌드 |
| 머티리얼 | lab_02 | `materials[]` SSBO + `texture2D textures[36]` + 공유 샘플러 1개 |
| 정점 셰이더 | 시리즈 공용 | `objects[gl_BaseInstanceARB]` |
| 프래그먼트 셰이더 | lab_02 | `materials[inMaterial]`, `nonuniformEXT` 인덱싱 |

드로우 루프에서 **바인딩이 완전히 사라진다** (버퍼 바인딩 제외):

```cpp
// set 0(프레임+오브젝트) + set 1(bindless) 프레임당 1회
vkCmdBindDescriptorSets(handle, ..., 0, 2, sets.data(), 0, nullptr);

for (batch : m_batches) {                    // 14회 (메시 수)
    vkCmdBindVertexBuffers(...);             // A1은 여전히 다중 버퍼
    vkCmdBindIndexBuffer(...);
    vkCmdDrawIndexedIndirect(buf, batch.firstDraw*stride, batch.drawCount, stride);
}
```

머티리얼 리바인드가 없으므로 프레임당 커맨드는 `2 + 14×3`으로 고정된다 — **N과
완전히 무관**하다. 남은 것은 메시 버퍼 바인딩뿐이고, 그걸 없애는 것이 단일 버퍼
조건(A2/A3)의 몫이다.

### 정렬 순서는 그대로 유지한다

B1은 머티리얼 그룹핑이 필요 없지만 정렬을 유지한다. 두 가지 이유:

1. **A1이 정렬을 요구한다.** 한 메시의 드로우가 연속이어야 배치를 만들 수 있다.
2. **렌더 순서는 고정 제어다** (설계 §3). 조건마다 순서가 다르면 오버드로우
   패턴이 갈려 픽셀 동등성과 프래그먼트 수 비교가 깨진다.

## SPIR-V 해시 (실측)

| | mesh.vert | mesh.frag |
|---|---|---|
| lab_01 (A0×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| lab_02 (A0×B1) | `69ea042b2fa47005` | `d8aa2b932838c51d` |
| lab_03 (A1×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| **lab_04 (A1×B1)** | `69ea042b2fa47005` | `d8aa2b932838c51d` |

정점 셰이더는 4조건 전부 동일하고, 프래그먼트는 **B 수준에만** 의존한다.
A축을 바꿔도 셰이더가 안 바뀐다는 것 — 즉 A 비교에 셰이더 차이가 섞이지 않는다는
것이 이 표로 증명된다.

필요 feature: `multiDrawIndirect`, `drawIndirectFirstInstance`,
`shaderDrawParameters`(A1) + `descriptorIndexing`(B1). 하나라도 미지원이면
생성자가 즉시 throw한다.

## Descriptor 레이아웃

```
set 0 (프레임당 1회 바인딩, frame-in-flight마다 1개)
  binding 0  UBO       Frame { viewProj, camPos, lightDir, lightColor, ambient }
  binding 1  SSBO      Object[] { model(mat4), material(uvec4) }        vert

set 1 (전체 실행 중 1회 바인딩, 단 하나) — lab_02와 동일
  binding 0  SSBO      Material[] { baseColorFactor, mr, tex(uvec4) }   frag
  binding 1  texture2D textures[3 × 머티리얼 수]                         frag
  binding 2  sampler   공유 샘플러 1개                                   frag
```

---

## 알려진 이슈

lab_03과 동일하다 — **MoltenVK**에는 네이티브 multi-draw-indirect가 없어
`drawCount > 1`이 루프로 에뮬레이션되므로 Apple 하드웨어에서는 A1의 제출 절감이
나타나지 않을 수 있다. A0 vs A1 수치에는 플랫폼을 함께 기록할 것.

(lab_03의 검증 레이어 경고는 여기서는 나지 않는다 — B1 프래그먼트가 정점 출력
location 4를 실제로 읽기 때문이다.)

## 실행

```
LAB_OBJECTS=<N> LAB_FRAMES=800 ./lab_04          # 300 워밍업 + 500 측정 → results.csv
LAB_OBJECTS=128 LAB_FRAMES=401 LAB_CAPTURE=400 LAB_CAPTURE_FILE=lab04.png ./lab_04
imgdiff lab01.png lab04.png                      # 픽셀 동등성
```

CSV의 `condition` 컬럼은 `A1xB1`.

## 검증

- **픽셀 동등성** — N=128, 프레임 400, 1280×720에서 lab_01과 **bit-exact**
  (불일치 0/921600, maxChannelDelta=0). 네 조건이 서로 모두 일치한다.
- **배치 수** — 드로우 224개 → 배치 14개(= 메시 수), N과 무관하게 고정.
- **SPIR-V 해시** — 위 표대로.

---

<a id="english"></a>

# English

Full design in [`../indirect-rendering-experiment.md`](../indirect-rendering-experiment.md).

- **A1** — multi-buffer + **per-buffer** `vkCmdDrawIndexedIndirect` — **identical to lab_03**
- **B1** — bindless, set bound once + index reference — **identical to lab_02**

Almost no new code. This condition is lab_03's draw path joined to lab_02's
material path, which is exactly what **closes the 2×2 grid**.

---

## Position in the 2×2 grid

| | B0 (set per material) | B1 (bindless) |
|---|---|---|
| **A0** (draw per object) | lab_01 | lab_02 |
| **A1** (per-buffer indirect) | lab_03 | **lab_04** |

With lab_04 in place **all four comparisons work**:

| Comparison | What it isolates |
|---|---|
| lab_01 ↔ lab_02 | B0 vs B1 (under A0) |
| lab_03 ↔ lab_04 | B0 vs B1 (under A1) |
| lab_01 ↔ lab_03 | A0 vs A1 (under B0) |
| lab_02 ↔ lab_04 | A0 vs A1 (under B1) |

The two comparisons along an axis check each other. If the B0 vs B1 effect
differs sharply between A0 and A1, the factors interact — which must be checked
before writing any standalone conclusion.

---

## Implementation

Taken wholesale from lab_03 and lab_02. Nothing here is unique to it.

| | From | What |
|---|---|---|
| Geometry / draw | lab_03 | VkBuffer per mesh, per-mesh `MeshBatch` → `vkCmdDrawIndexedIndirect` |
| Indirect buffer | lab_03 | one command per draw, `firstInstance = i`, built once at setup |
| Material | lab_02 | `materials[]` SSBO + `texture2D textures[36]` + one shared sampler |
| Vertex shader | series-wide | `objects[gl_BaseInstanceARB]` |
| Fragment shader | lab_02 | `materials[inMaterial]`, `nonuniformEXT` indexing |

Binding **disappears from the draw loop** entirely (except the buffer binds):

```cpp
// set 0 (frame + objects) + set 1 (bindless), once per frame
vkCmdBindDescriptorSets(handle, ..., 0, 2, sets.data(), 0, nullptr);

for (batch : m_batches) {                    // 14 iterations (mesh count)
    vkCmdBindVertexBuffers(...);             // A1 is still multi-buffer
    vkCmdBindIndexBuffer(...);
    vkCmdDrawIndexedIndirect(buf, batch.firstDraw*stride, batch.drawCount, stride);
}
```

With no material rebinds the per-frame command count is fixed at `2 + 14×3` —
**completely independent of N**. Only the mesh buffer binds remain, and removing
those is what the single-buffer conditions (A2/A3) are for.

### The sort order stays

B1 does not need material grouping, but the sort is kept. Two reasons:

1. **A1 requires it.** A mesh's draws must be contiguous to form a batch.
2. **Render order is a fixed control** (design §3). A per-condition order would
   change the overdraw pattern and break both pixel equivalence and fragment-count
   comparisons.

## SPIR-V hashes (measured)

| | mesh.vert | mesh.frag |
|---|---|---|
| lab_01 (A0×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| lab_02 (A0×B1) | `69ea042b2fa47005` | `d8aa2b932838c51d` |
| lab_03 (A1×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| **lab_04 (A1×B1)** | `69ea042b2fa47005` | `d8aa2b932838c51d` |

The vertex shader is identical in all four, and the fragment shader depends on
the **B level only**. That the A axis changes no shader — so no shader difference
leaks into an A comparison — is what this table proves.

Required features: `multiDrawIndirect`, `drawIndirectFirstInstance`,
`shaderDrawParameters` (A1) plus `descriptorIndexing` (B1). The constructor throws
if any is missing.

## Descriptor layout

```
set 0 (bound once per frame, one per frame-in-flight)
  binding 0  UBO       Frame { viewProj, camPos, lightDir, lightColor, ambient }
  binding 1  SSBO      Object[] { model(mat4), material(uvec4) }        vert

set 1 (bound once for the whole run; there is exactly one) — identical to lab_02
  binding 0  SSBO      Material[] { baseColorFactor, mr, tex(uvec4) }   frag
  binding 1  texture2D textures[3 × material count]                     frag
  binding 2  sampler   one shared sampler                               frag
```

---

## Known issues

Same as lab_03 — **MoltenVK** has no native multi-draw-indirect, so `drawCount > 1`
is emulated as a loop and A1's submission saving may not appear on Apple hardware.
Record the platform alongside any A0 vs A1 number.

(lab_03's validation-layer warning does not occur here: the B1 fragment shader
actually reads the vertex output at location 4.)

## Running

```
LAB_OBJECTS=<N> LAB_FRAMES=800 ./lab_04          # 300 warm-up + 500 measured -> results.csv
LAB_OBJECTS=128 LAB_FRAMES=401 LAB_CAPTURE=400 LAB_CAPTURE_FILE=lab04.png ./lab_04
imgdiff lab01.png lab04.png                      # pixel equivalence
```

The CSV `condition` column reads `A1xB1`.

## Verification

- **Pixel equivalence** — **bit-exact** against lab_01 at N=128, frame 400,
  1280×720 (0/921600 mismatched, maxChannelDelta=0). All four conditions match
  each other.
- **Batch count** — 224 draws → 14 batches (= mesh count), fixed regardless of N.
- **SPIR-V hashes** — per the table above.
