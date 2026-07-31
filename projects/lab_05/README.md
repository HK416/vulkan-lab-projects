# Lab 05 — 단일 버퍼 + 오브젝트당 Indirect (A2 × B0)

**[한국어](#한국어) · [English](#english)**

<a id="한국어"></a>

# 한국어

전체 설계는 [`../indirect-rendering-experiment.md`](../indirect-rendering-experiment.md) 참조.

- **A2** — **단일 버퍼**(전 메시를 하나의 정점/인덱스 버퍼에) + **오브젝트당** `vkCmdDrawIndexedIndirect`
- **B0** — 머티리얼마다 `vkCmdBindDescriptorSets` — lab_01/lab_03과 동일

A2의 핵심은 **메시 선택이 버퍼 바인딩에서 indirect 커맨드의 오프셋으로 이동**한다는
것이다. 버퍼는 프레임당 1회만 바인딩된다.

---

## A2 = 단일 버퍼

A0/A1은 메시마다 `VkBuffer` 쌍을 갖고, 드로우 전에 그걸 바인딩해야 했다.
A2는 모든 메시를 이어붙인 버퍼 2개만 만들고, 메시를 **범위**로 참조한다.

```cpp
struct GpuMesh {           // A0/A1: GpuBuffer vertex; GpuBuffer index;
    uint32_t indexCount;
    uint32_t firstIndex;   // 공유 인덱스 버퍼 내 시작 위치
    int32_t  vertexOffset; // 모든 인덱스에 더해지는 값
    int      material;
};
```

이 세 숫자가 그대로 indirect 커맨드로 들어간다:

```cpp
cmd.indexCount    = mesh.indexCount;
cmd.instanceCount = 1;
cmd.firstIndex    = mesh.firstIndex;     // A0/A1은 항상 0이었다
cmd.vertexOffset  = mesh.vertexOffset;   // A0/A1은 항상 0이었다
cmd.firstInstance = i;                   // 인덱스 경로 (시리즈 공통)
```

**인덱스는 메시 로컬(0-based) 그대로 둔다.** `vertexOffset`이 리베이스를 담당하는데,
그게 바로 이 필드의 존재 이유다. 인덱스를 다시 쓰지 않으므로 인덱스 데이터가
A0/A1 랩과 바이트 단위로 같고, 조건 간 정점/삼각형 수가 어긋날 여지가 없다.

측정값: 모델 세트 7개 = **정점 92,128개, 인덱스 435,360개**, 할당 **2개**.
(A0/A1은 메시당 2개 = 28개.) 이 할당 개수 차이가 설계문서 §5의 메모리 지표다.

### 업로드는 2패스

버퍼 크기는 모든 모델을 읽어야 정해지므로:

1. CPU 데이터 로드 + 머티리얼 이미지 생성 + 메시별 오프셋 기록 (크기 누적)
2. 버퍼 2개 생성 후 각 메시를 자기 오프셋으로 복사 (`StagingUploader`의 `dstOffset`)

## 드로우 루프

```cpp
vkCmdBindVertexBuffers(...);   // 프레임당 1회
vkCmdBindIndexBuffer(...);     // 프레임당 1회

for (i, draw : m_draws) {                       // N번 (224 @ N=128)
    if (머티리얼 바뀜) vkCmdBindDescriptorSets(...);   // B0, 12회
    vkCmdDrawIndexedIndirect(buf, i*stride, /*drawCount=*/1, stride);
}
```

A0/A1이 내던 **메시당 바인딩 쌍이 사라졌다.** 남은 것은 오브젝트당 indirect 호출
N개의 순수한 제출 비용이며, **그걸 하나로 접는 것이 A3(lab_07)의 몫**이다.

### `multiDrawIndirect`가 필요 없다

A2는 `drawCount = 1`로만 호출하므로 이 feature 없이 동작한다. 스펙상
`multiDrawIndirect`가 비활성이면 `drawCount`는 0 또는 1이어야 한다. 즉 **A2 vs A3
비교는 이 feature 하나의 유무와 정확히 겹친다** — 그래서 §7의 "조건별 필요한 것만
opt-in" 원칙에 따라 여기서는 요청하지 않는다.

요청 feature: `drawIndirectFirstInstance`, `shaderDrawParameters`.

---

## ⚠ 비교쌍 주의

**lab_03(A1) vs lab_05(A2)로 단독 결론을 내지 말 것.** 버퍼 전략(다중→단일)과
드로우 granularity(버퍼당→오브젝트당)가 **동시에** 바뀐다. 설계문서 §1이 명시적으로
금지한 조합이다.

유효한 쌍:

| 쌍 | 격리되는 것 |
|---|---|
| lab_01 ↔ lab_03 | A0 vs A1 — 직접 드로우 vs indirect (버퍼 동일: 다중) |
| **lab_05 ↔ lab_07** | **A2 vs A3 — multi-draw 배칭 이득 (버퍼 동일: 단일)** |
| lab_05 ↔ lab_06 | B0 vs B1 (A2에서) |

lab_05는 A2 vs A3 쌍의 한쪽이며, 그 비교가 이 랩의 존재 이유다.

## SPIR-V 해시 (실측)

| | mesh.vert | mesh.frag |
|---|---|---|
| lab_01 (A0×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| lab_03 (A1×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| **lab_05 (A2×B0)** | `69ea042b2fa47005` | `ee009e8460448a9f` |

A0·A1·A2가 셰이더를 공유한다. 버퍼 전략이 바뀌어도 셰이더는 안 바뀐다 —
정점 데이터 레이아웃과 인덱스 경로가 고정 제어이기 때문이다.

## Descriptor 레이아웃

lab_01/lab_03과 동일.

```
set 0  binding 0  UBO   Frame { viewProj, camPos, lightDir, lightColor, ambient }
       binding 1  SSBO  Object[] { model(mat4), material(uvec4) }        vert
set 1  binding 0/1/2  combined image sampler  baseColor / metalRough / normal
       binding 3      UBO  Material { baseColorFactor, mr }
```

---

## 알려진 이슈

- **검증 레이어 경고** — 공용 정점 셰이더가 location 4(머티리얼 인덱스)를 출력하지만
  B0 프래그먼트는 읽지 않는다. lab_01/lab_03과 같은 사유이며 스펙상 합법이다.

## 실행

```
LAB_OBJECTS=<N> LAB_FRAMES=800 ./lab_05          # 300 워밍업 + 500 측정 → results.csv
LAB_OBJECTS=128 LAB_FRAMES=401 LAB_CAPTURE=400 LAB_CAPTURE_FILE=lab05.png ./lab_05
imgdiff lab01.png lab05.png                      # 픽셀 동등성
```

CSV의 `condition` 컬럼은 `A2xB0`.

## 검증

- **픽셀 동등성** — N=128, 프레임 400, 1280×720에서 lab_01과 **bit-exact**.
  단일 버퍼 오프셋 계산이 틀리면 즉시 깨지므로 이 검사가 A2 구현의 핵심 검증이다.
- **단일 버퍼 크기** — 시작 로그의 `A2 single buffer: 92128 vertices, 435360 indices`.
  이 수치는 모델 세트에만 의존하므로 N과 무관하게 고정되어야 한다.
- **SPIR-V 해시** — 위 표대로.

---

<a id="english"></a>

# English

Full design in [`../indirect-rendering-experiment.md`](../indirect-rendering-experiment.md).

- **A2** — **single buffer** (every mesh in one vertex/index buffer) + **per-object** `vkCmdDrawIndexedIndirect`
- **B0** — `vkCmdBindDescriptorSets` per material — identical to lab_01/lab_03

A2's point is that **mesh selection moves out of buffer binding and into the
indirect command's offsets**. The buffers are bound once per frame.

---

## A2 = single buffer

A0/A1 gave every mesh its own `VkBuffer` pair, which had to be bound before its
draws. A2 concatenates everything into two buffers and refers to a mesh as a
**range**.

```cpp
struct GpuMesh {           // A0/A1: GpuBuffer vertex; GpuBuffer index;
    uint32_t indexCount;
    uint32_t firstIndex;   // where this mesh starts in the shared index buffer
    int32_t  vertexOffset; // added to every one of its indices
    int      material;
};
```

Those three numbers go straight into the indirect command:

```cpp
cmd.indexCount    = mesh.indexCount;
cmd.instanceCount = 1;
cmd.firstIndex    = mesh.firstIndex;     // always 0 in A0/A1
cmd.vertexOffset  = mesh.vertexOffset;   // always 0 in A0/A1
cmd.firstInstance = i;                   // the series-wide index path
```

**Indices stay mesh-local (0-based).** `vertexOffset` rebases them, which is
exactly what that field is for. No index rewriting, so the index data is
byte-identical to the A0/A1 labs' and vertex/triangle counts cannot drift between
conditions.

Measured: the 7-model set is **92,128 vertices and 435,360 indices** in **2**
allocations. (A0/A1 use 2 per mesh = 28.) That allocation-count difference is the
memory metric from design §5.

### Upload is two passes

The buffer sizes are only known once every model is read, so:

1. load the CPU data, create the material images, record each mesh's offsets
   (accumulating the totals)
2. create the two buffers and copy each mesh to its offset (`StagingUploader`'s
   `dstOffset`)

## Draw loop

```cpp
vkCmdBindVertexBuffers(...);   // once per frame
vkCmdBindIndexBuffer(...);     // once per frame

for (i, draw : m_draws) {                          // N times (224 at N=128)
    if (material changed) vkCmdBindDescriptorSets(...);   // B0, 12 times
    vkCmdDrawIndexedIndirect(buf, i*stride, /*drawCount=*/1, stride);
}
```

The **per-mesh bind pair A0/A1 paid is gone**. What remains is the raw submission
cost of N per-object indirect calls — and **folding those into one is what A3
(lab_07) does**.

### `multiDrawIndirect` is not needed

A2 only ever calls with `drawCount = 1`, which is allowed without the feature: the
spec requires `drawCount` to be 0 or 1 when `multiDrawIndirect` is disabled. So
**the A2 vs A3 comparison lines up exactly with the presence of that one
feature** — and per §7's "opt in per condition to only what it needs", this lab
does not request it.

Requested features: `drawIndirectFirstInstance`, `shaderDrawParameters`.

---

## ⚠ Mind the comparison pair

**Do not draw a standalone conclusion from lab_03 (A1) vs lab_05 (A2).** Buffer
strategy (multi → single) and draw granularity (per-buffer → per-object) change
**together**. Design §1 explicitly forbids that pairing.

Valid pairs:

| Pair | What it isolates |
|---|---|
| lab_01 ↔ lab_03 | A0 vs A1 — direct draw vs indirect (same buffer: multi) |
| **lab_05 ↔ lab_07** | **A2 vs A3 — multi-draw batching gain (same buffer: single)** |
| lab_05 ↔ lab_06 | B0 vs B1 (under A2) |

lab_05 is one half of the A2 vs A3 pair, and that comparison is why it exists.

## SPIR-V hashes (measured)

| | mesh.vert | mesh.frag |
|---|---|---|
| lab_01 (A0×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| lab_03 (A1×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| **lab_05 (A2×B0)** | `69ea042b2fa47005` | `ee009e8460448a9f` |

A0, A1 and A2 share their shaders. Changing the buffer strategy changes no shader,
because the vertex data layout and the index path are both fixed controls.

## Descriptor layout

Identical to lab_01/lab_03.

```
set 0  binding 0  UBO   Frame { viewProj, camPos, lightDir, lightColor, ambient }
       binding 1  SSBO  Object[] { model(mat4), material(uvec4) }        vert
set 1  binding 0/1/2  combined image sampler  baseColor / metalRough / normal
       binding 3      UBO  Material { baseColorFactor, mr }
```

---

## Known issues

- **Validation-layer warning** — the shared vertex shader writes location 4 (the
  material index) which the B0 fragment shader does not read. Same cause as
  lab_01/lab_03, and legal Vulkan.

## Running

```
LAB_OBJECTS=<N> LAB_FRAMES=800 ./lab_05          # 300 warm-up + 500 measured -> results.csv
LAB_OBJECTS=128 LAB_FRAMES=401 LAB_CAPTURE=400 LAB_CAPTURE_FILE=lab05.png ./lab_05
imgdiff lab01.png lab05.png                      # pixel equivalence
```

The CSV `condition` column reads `A2xB0`.

## Verification

- **Pixel equivalence** — **bit-exact** against lab_01 at N=128, frame 400,
  1280×720. A wrong single-buffer offset breaks it immediately, which makes this
  the key check on the A2 implementation.
- **Single-buffer size** — the startup log line
  `A2 single buffer: 92128 vertices, 435360 indices`. It depends only on the model
  set, so it must stay fixed regardless of N.
- **SPIR-V hashes** — per the table above.
