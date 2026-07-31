# Lab 03 — 버퍼당 Indirect (A1 × B0)

**[한국어](#한국어) · [English](#english)**

<a id="한국어"></a>

# 한국어

전체 설계는 [`../indirect-rendering-experiment.md`](../indirect-rendering-experiment.md) 참조.

- **A1** — 다중 버퍼(메시당 VkBuffer, A0과 동일) + **버퍼당** `vkCmdDrawIndexedIndirect`
- **B0** — 머티리얼마다 `vkCmdBindDescriptorSets` — **lab_01과 동일**

B축이 lab_01과 같으므로 **lab_01 vs lab_03 = A0 vs A1 단독 비교**다.
설계문서 §1이 지정한 유효 비교쌍 중 하나 — "버퍼 동일(다중), 직접 드로우 vs indirect".

---

## lab_01 대비 실제 변경점

| | lab_01 (A0) | lab_03 (A1) |
|---|---|---|
| 지오메트리 | 메시당 VkBuffer 쌍 | **동일** |
| 드로우 단위 | 오브젝트마다 `vkCmdDrawIndexed` | **메시 버퍼마다** `vkCmdDrawIndexedIndirect` |
| 드로우 명령 수 (N=128) | 224회 | **14회** (메시 수) |
| indirect 버퍼 | 없음 | 드로우당 커맨드 1개, setup에서 1회 빌드 |
| 셰이더 | — | **완전히 동일** (vert/frag SPIR-V 해시 일치) |
| 머티리얼 바인딩 | 머티리얼당 1회 | **동일** |

드로우 루프를 제외한 모든 것이 같다. 씬(seed 42, k=7)·카메라·정렬 순서·워밍업·
라이팅·텍스처가 동일하고, **셰이더는 바이트 단위로 같은 파일**이다.

---

## A1의 배칭 구조

```
정렬된 드로우 리스트 (머티리얼 → 메시)
   ├─ mesh 0 의 드로우들 ─┐
   ├─ mesh 1 의 드로우들 ─┼→ 메시별 연속 구간 = MeshBatch
   └─ ...                 ┘

프레임마다:
for batch in batches:                       # N이 아니라 메시 수만큼
    if 머티리얼 바뀜: vkCmdBindDescriptorSets   # B0
    vkCmdBindVertexBuffers / IndexBuffer       # A1도 메시당 바인딩은 남는다
    vkCmdDrawIndexedIndirect(buf, batch.firstDraw*stride, batch.drawCount, stride)
```

- **정렬이 A1의 전제다.** 머티리얼→메시 정렬이 없으면 한 메시의 드로우가 흩어져
  연속 구간을 만들 수 없고, 배칭 자체가 불가능하다. 다른 조건에서는 정렬이
  "B0 리바인드를 줄이는 최적화"였지만 A1에서는 **필수 조건**이다.
- **버퍼 바인딩은 줄지 않는다.** A1은 여전히 다중 버퍼이므로 메시마다
  `vkCmdBindVertexBuffers`/`vkCmdBindIndexBuffer`가 필요하다. 이 바인딩까지
  없애는 것이 단일 버퍼 조건(A2/A3)의 몫이다.
- **indirect 버퍼는 정적이다.** 컬링 off(1차 실험)이므로 setup에서 1회 빌드하고
  프레임당 CPU 비용이 0이다. 프레임당 재빌드 비용은 2차 실험(컬링 on)에서 발생한다.

## 인덱스 경로 (시리즈 공통)

```
드로우 i → 인덱스 커맨드 i 의 firstInstance = i
        → 정점 셰이더 objects[gl_BaseInstanceARB] → { model, materialIndex }
```

lab_02가 확정한 경로를 그대로 쓴다. `objects[]` SSBO는 **set 0, binding 1**에
있고 — 머티리얼 세트가 아니라 프레임 세트 옆에 — 이 배치 덕분에 **A0~A3 × B0/B1
전 조건이 하나의 정점 셰이더를 공유**한다. 실측 SPIR-V 해시:

| | mesh.vert | mesh.frag |
|---|---|---|
| lab_01 (A0×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| lab_02 (A0×B1) | `69ea042b2fa47005` | `d8aa2b932838c51d` |
| lab_03 (A1×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |

정점 셰이더는 3조건 모두 동일하고, 프래그먼트는 B 수준에만 의존한다.
즉 lab_01 vs lab_03은 **드로우 호출 방식만** 다르다.

필요 feature: `multiDrawIndirect`(drawCount > 1), `drawIndirectFirstInstance`
(indirect 커맨드의 `firstInstance ≠ 0`), `shaderDrawParameters`. 하나라도
미지원이면 생성자가 즉시 throw한다. `descriptorIndexing`은 **요청하지 않는다** —
B0는 bindless를 쓰지 않으므로(설계문서 §7의 "조건별 필요한 것만 opt-in").

## Descriptor 레이아웃

```
set 0 (프레임당 1회 바인딩, frame-in-flight마다 1개)
  binding 0  UBO   Frame { viewProj, camPos, lightDir, lightColor, ambient }
  binding 1  SSBO  Object[] { model(mat4), material(uvec4) }        vert

set 1 (머티리얼마다 1개, 머티리얼 바뀔 때 바인딩) — lab_01과 동일
  binding 0/1/2  combined image sampler  baseColor / metalRough / normal
  binding 3      UBO  Material { baseColorFactor, mr }
```

---

## 알려진 이슈

- **검증 레이어 경고** — 정점 셰이더가 location 4(머티리얼 인덱스)를 출력하지만
  B0 프래그먼트는 그걸 읽지 않는다. Vulkan 스펙상 합법이며(사용되지 않는 정점 출력),
  하나의 정점 셰이더를 B0/B1이 공유하는 데서 오는 필연적 결과다. `#define`으로
  분기하면 경고는 사라지지만 정점 SPIR-V 해시가 B 수준마다 갈려 위 표의 성질을
  잃는다. **경고를 받아들이는 쪽을 선택했다.**
- **MoltenVK** — Metal에 네이티브 multi-draw-indirect가 없어 `drawCount > 1`은
  커맨드를 루프로 인코딩해 에뮬레이션된다. API 레벨 호출 수는 224 → 14로 줄지만
  드라이버가 다시 펼치므로 **Apple 하드웨어에서는 A1의 제출 절감이 나타나지 않을
  수 있다.** A0 vs A1 수치에는 반드시 플랫폼을 함께 기록할 것.

## 실행

```
LAB_OBJECTS=<N> LAB_FRAMES=800 ./lab_03          # 300 워밍업 + 500 측정 → results.csv
LAB_OBJECTS=128 LAB_FRAMES=401 LAB_CAPTURE=400 LAB_CAPTURE_FILE=lab03.png ./lab_03
imgdiff lab01.png lab03.png                      # 픽셀 동등성
```

CSV의 `condition` 컬럼은 `A1xB0`.

## 검증

- **픽셀 동등성** — N=128, 프레임 400, 1280×720에서 lab_01·lab_02와 모두
  **bit-exact** (불일치 0/921600, maxChannelDelta=0).
- **배치 수** — N=128에서 드로우 224개가 배치 14개(= 메시 수)로 접힘. 배치 수는
  N과 무관하게 메시 수로 고정되어야 한다.
- **SPIR-V 해시** — 위 표대로 정점 셰이더가 3조건 동일해야 한다.

---

<a id="english"></a>

# English

Full design in [`../indirect-rendering-experiment.md`](../indirect-rendering-experiment.md).

- **A1** — multi-buffer (one VkBuffer per mesh, same as A0) + **per-buffer** `vkCmdDrawIndexedIndirect`
- **B0** — `vkCmdBindDescriptorSets` per material — **identical to lab_01**

Because the B axis matches lab_01, **lab_01 vs lab_03 is a standalone A0 vs A1
comparison** — one of the valid pairs §1 names: "same buffer (multi), direct draw
vs indirect".

---

## What actually changed vs lab_01

| | lab_01 (A0) | lab_03 (A1) |
|---|---|---|
| Geometry | one VkBuffer pair per mesh | **same** |
| Draw unit | `vkCmdDrawIndexed` per object | **`vkCmdDrawIndexedIndirect` per mesh buffer** |
| Draw commands (N=128) | 224 | **14** (one per mesh) |
| Indirect buffer | none | one command per draw, built once at setup |
| Shaders | — | **byte-identical** (matching vert/frag SPIR-V hashes) |
| Material binding | once per material | **same** |

Everything but the draw loop is the same: scene (seed 42, k=7), camera, sort
order, warm-up, lighting and textures, and **the shader files are literally the
same bytes**.

---

## A1's batching structure

```
sorted draw list (material -> mesh)
   |- draws of mesh 0 -|
   |- draws of mesh 1 -|--> a contiguous run per mesh = MeshBatch
   |- ...              -|

per frame:
for batch in batches:                        # mesh count, not N
    if material changed: vkCmdBindDescriptorSets   # B0
    vkCmdBindVertexBuffers / IndexBuffer           # A1 still binds per mesh
    vkCmdDrawIndexedIndirect(buf, batch.firstDraw*stride, batch.drawCount, stride)
```

- **The sort is a precondition for A1.** Without material→mesh ordering a mesh's
  draws are scattered and cannot form a contiguous run, so batching is impossible.
  In the other conditions sorting was an optimization for B0's rebinds; in A1 it
  is a **requirement**.
- **Buffer binds do not go away.** A1 is still multi-buffer, so every mesh still
  needs `vkCmdBindVertexBuffers`/`vkCmdBindIndexBuffer`. Removing those binds is
  what the single-buffer conditions (A2/A3) are for.
- **The indirect buffer is static.** Culling is off in experiment 1, so it is
  built once and costs 0 per frame. The per-frame rebuild arrives in experiment 2.

## Index path (shared by the series)

```
draw i -> indirect command i has firstInstance = i
       -> vertex: objects[gl_BaseInstanceARB] -> { model, materialIndex }
```

This reuses the path lab_02 fixed. The `objects[]` SSBO sits at **set 0, binding
1** — next to the frame data, not in the material set — and that placement is what
lets **every condition (A0–A3 × B0/B1) share one vertex shader**. Measured SPIR-V
hashes:

| | mesh.vert | mesh.frag |
|---|---|---|
| lab_01 (A0×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| lab_02 (A0×B1) | `69ea042b2fa47005` | `d8aa2b932838c51d` |
| lab_03 (A1×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |

The vertex shader is identical in all three; the fragment shader depends only on
the B level. So lab_01 vs lab_03 differ **only in how draws are issued**.

Required features: `multiDrawIndirect` (drawCount > 1),
`drawIndirectFirstInstance` (`firstInstance != 0` in an indirect command), and
`shaderDrawParameters`. The constructor throws if any is missing.
`descriptorIndexing` is **not requested** — B0 does not use bindless (§7's "opt in
per condition to only what it needs").

## Descriptor layout

```
set 0 (bound once per frame, one per frame-in-flight)
  binding 0  UBO   Frame { viewProj, camPos, lightDir, lightColor, ambient }
  binding 1  SSBO  Object[] { model(mat4), material(uvec4) }        vert

set 1 (one per material, bound on change) — identical to lab_01
  binding 0/1/2  combined image sampler  baseColor / metalRough / normal
  binding 3      UBO  Material { baseColorFactor, mr }
```

---

## Known issues

- **Validation-layer warning** — the vertex shader writes location 4 (the material
  index) but the B0 fragment shader does not read it. This is legal Vulkan (an
  unused vertex output) and is the unavoidable consequence of B0 and B1 sharing
  one vertex shader. Branching with `#define` would silence it but would split the
  vertex SPIR-V hash per B level, losing the property in the table above. **Taking
  the warning was the deliberate choice.**
- **MoltenVK** — Metal has no native multi-draw-indirect, so `drawCount > 1` is
  emulated by encoding the commands in a loop. The API-level call count drops from
  224 to 14, but the driver expands it again, so **A1's submission saving may not
  appear on Apple hardware.** Always record the platform alongside an A0 vs A1
  number.

## Running

```
LAB_OBJECTS=<N> LAB_FRAMES=800 ./lab_03          # 300 warm-up + 500 measured -> results.csv
LAB_OBJECTS=128 LAB_FRAMES=401 LAB_CAPTURE=400 LAB_CAPTURE_FILE=lab03.png ./lab_03
imgdiff lab01.png lab03.png                      # pixel equivalence
```

The CSV `condition` column reads `A1xB0`.

## Verification

- **Pixel equivalence** — **bit-exact** against both lab_01 and lab_02 at N=128,
  frame 400, 1280×720 (0/921600 mismatched, maxChannelDelta=0).
- **Batch count** — 224 draws collapse into 14 batches (= mesh count) at N=128.
  The batch count must stay pinned to the mesh count regardless of N.
- **SPIR-V hashes** — the vertex shader must match across all three labs, per the
  table above.
