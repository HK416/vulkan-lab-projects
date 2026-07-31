# Lab 07 — Multi-draw Indirect (A3 × B1)

**[한국어](#한국어) · [English](#english)**

<a id="한국어"></a>

# 한국어

전체 설계는 [`../indirect-rendering-experiment.md`](../indirect-rendering-experiment.md) 참조.
**시리즈의 마지막 조건**이며, 이로써 유효 조건 7개가 모두 구현되었다.

- **A3** — A2의 단일 버퍼 + **multi-draw indirect**: 프레임 전체 지오메트리가
  `vkCmdDrawIndexedIndirect` **1회**
- **B1** — Bindless. **선택이 아니라 강제다** (아래).

---

## A3 × B0가 존재하지 않는 이유

multi-draw는 드로우 사이에 커맨드를 끼워 넣을 지점이 없다. `vkCmdDrawIndexedIndirect`
한 번을 호출하면 GPU가 N개 드로우를 연속 실행하며, 그 사이에서 CPU가
`vkCmdBindDescriptorSets`를 부를 방법이 없다. 따라서 **머티리얼별 descriptor set 교체
(B0)가 원리적으로 불가능**하고, 머티리얼은 반드시 인덱스로 참조되어야 한다(B1).

이것이 조건 수가 4×2=8이 아니라 **7개**인 이유다.

---

## lab_06 대비 변경점 — 드로우 루프가 사라진다

```cpp
// lab_06 (A2×B1) — N번
for (uint32_t i = 0; i < m_draws.size(); ++i) {
    vkCmdDrawIndexedIndirect(buf, i * stride, /*drawCount=*/1, stride);
}

// lab_07 (A3×B1) — 1번
vkCmdDrawIndexedIndirect(buf, 0, /*drawCount=*/총 드로우 수, stride);
```

**indirect 버퍼는 바이트 단위로 동일하다.** A2와 A3의 차이는 버퍼 내용이 아니라
그것을 소비하는 호출 횟수뿐이다. 그래서 lab_06 ↔ lab_07이 **multi-draw 배칭 이득을
격리하는 유일한 쌍**이 된다 — 설계문서 §1이 지정한 "A2 vs A3 — 버퍼 동일(단일),
multi-draw 배칭 이득".

프레임당 커맨드 수:

| 랩 | 조건 | 프레임당 커맨드 |
|---|---|---|
| lab_05 | A2×B0 | `3 + 12 + N` |
| lab_06 | A2×B1 | `4 + N` |
| **lab_07** | **A3×B1** | **`4 + 1` — N과 완전히 무관** |

N=32768(드로우 65,223개)에서도 드로우 커맨드는 1개다.

### `maxDrawIndirectCount` 가드

한 번의 호출이 소비할 수 있는 커맨드 수에는 장치 한계가 있다. 데스크톱에서는 매우
크지만 보장된 값은 아니고, **한계를 넘겨 장면 일부만 그리면 "빠른 조건"과 구별이
안 된다**. 그래서 시작 시 한계를 읽고, 드로우 리스트가 넘치면 청크로 나눈 뒤
경고를 남긴다.

```
maxDrawIndirectCount=1073741824
scene: N=32768 ... draws=65223 indirectCalls=1
```

`indirectCalls`가 1이 아니면 그 실행은 A3가 아니므로 측정과 함께 기록해야 한다.
(M1 Pro/MoltenVK에서는 한계가 10억이라 발동하지 않는다.)

### `vkCmdDrawIndexedIndirectCount`를 쓰지 않는 이유

설계문서 §7이 A3 항목에 `drawIndirectCount`를 함께 적어둔 것은 **2차 실험(컬링 on)**
때문이다. 1차 실험은 컬링이 꺼져 있어 드로우 개수가 CPU에서 확정되므로 평범한
multi-draw로 충분하다. 컬링이 켜지면 개수를 GPU가 정하게 되고, 그때
`vkCmdDrawIndexedIndirectCount`(+`drawIndirectCount` feature, MoltenVK 미지원)로
넘어간다.

필요 feature: **`multiDrawIndirect`**(= A2와의 차이 그 자체), `drawIndirectFirstInstance`,
`shaderDrawParameters`, `descriptorIndexing`.

---

## SPIR-V 해시 (실측, 7조건 전부)

| | mesh.vert | mesh.frag |
|---|---|---|
| lab_01 (A0×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| lab_02 (A0×B1) | `69ea042b2fa47005` | `d8aa2b932838c51d` |
| lab_03 (A1×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| lab_04 (A1×B1) | `69ea042b2fa47005` | `d8aa2b932838c51d` |
| lab_05 (A2×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| lab_06 (A2×B1) | `69ea042b2fa47005` | `d8aa2b932838c51d` |
| **lab_07 (A3×B1)** | `69ea042b2fa47005` | `d8aa2b932838c51d` |

**7개 조건이 정점 셰이더 하나를 공유하고, 프래그먼트 셰이더는 2종뿐이다.**
설계문서 §3이 요구한 "인덱스 경로 고정 → SPIR-V 해시 고정"이 시리즈 전체에서
실측으로 성립한다. A축 네 수준 전부가 셰이더를 건드리지 않는다.

## Descriptor 레이아웃

lab_02/04/06과 동일.

```
set 0  binding 0  UBO       Frame { viewProj, camPos, lightDir, lightColor, ambient }
       binding 1  SSBO      Object[] { model(mat4), material(uvec4) }        vert
set 1  binding 0  SSBO      Material[] { baseColorFactor, mr, tex(uvec4) }   frag
       binding 1  texture2D textures[3 × 머티리얼 수]                         frag
       binding 2  sampler   공유 샘플러 1개                                   frag
```

---

## ⚠ MoltenVK에서는 A3가 의미를 잃을 수 있다

Metal에는 네이티브 multi-draw-indirect가 없다. MoltenVK는 `drawCount > 1`을 받으면
**커맨드를 루프로 펼쳐** Metal 드로우를 하나씩 인코딩한다. A3의 전제가 "N개 호출을
1개로 줄인다"인데 드라이버가 다시 N개로 되돌리므로, **Apple 하드웨어에서 측정한
A2 vs A3는 배칭 이득이 아니라 에뮬레이션 오버헤드일 수 있다.**

이 조건은 네이티브 multi-draw를 지원하는 플랫폼에서 측정해야 의미가 있다.
어느 쪽이든 플랫폼을 수치와 함께 기록할 것.

## 실행

```
LAB_OBJECTS=<N> LAB_FRAMES=800 ./lab_07          # 300 워밍업 + 500 측정 → results.csv
LAB_OBJECTS=128 LAB_FRAMES=401 LAB_CAPTURE=400 LAB_CAPTURE_FILE=lab07.png ./lab_07
imgdiff lab06.png lab07.png                      # A2 vs A3 픽셀 동등성
```

CSV의 `condition` 컬럼은 `A3xB1`.

## 검증

- **픽셀 동등성** — lab_01·lab_06과 **bit-exact**. N=128(224 드로우)과
  N=32768(65,223 드로우) 양쪽에서 확인.
- **`indirectCalls=1`** — 시작 로그. N과 무관하게 1이어야 한다.
- **SPIR-V 해시** — 위 표대로.

---

<a id="english"></a>

# English

Full design in [`../indirect-rendering-experiment.md`](../indirect-rendering-experiment.md).
**The last condition of the series** — all 7 valid conditions are now implemented.

- **A3** — A2's single buffer + **multi-draw indirect**: the whole frame's geometry
  in **one** `vkCmdDrawIndexedIndirect`
- **B1** — bindless. **Not a choice, a requirement** (below).

---

## Why A3 × B0 does not exist

Multi-draw offers no point to insert a command between draws. One
`vkCmdDrawIndexedIndirect` call makes the GPU run N draws back to back, and the
CPU has no way to slip a `vkCmdBindDescriptorSets` in between. **Per-material
descriptor set swaps (B0) are therefore impossible**, and materials must be
referenced by index (B1).

That is why the series has **7** conditions rather than 4×2 = 8.

---

## What changed vs lab_06 — the draw loop disappears

```cpp
// lab_06 (A2xB1) — N times
for (uint32_t i = 0; i < m_draws.size(); ++i) {
    vkCmdDrawIndexedIndirect(buf, i * stride, /*drawCount=*/1, stride);
}

// lab_07 (A3xB1) — once
vkCmdDrawIndexedIndirect(buf, 0, /*drawCount=*/total draws, stride);
```

**The indirect buffer is byte-identical.** What differs between A2 and A3 is not
its contents but the number of calls consuming it — which is what makes lab_06 ↔
lab_07 **the pair that isolates multi-draw batching**, exactly as §1 specifies:
"A2 vs A3 — same buffer (single), multi-draw batching gain".

Per-frame command counts:

| Lab | Condition | Commands per frame |
|---|---|---|
| lab_05 | A2×B0 | `3 + 12 + N` |
| lab_06 | A2×B1 | `4 + N` |
| **lab_07** | **A3×B1** | **`4 + 1` — independent of N** |

At N=32768 (65,223 draws) there is still exactly one draw command.

### The `maxDrawIndirectCount` guard

A device caps how many commands one call may consume. It is enormous on desktop
but not guaranteed, and **exceeding it would draw only part of the scene, which is
indistinguishable from a fast condition**. So the limit is read at startup, the
draw list is split into chunks if it overflows, and the run warns.

```
maxDrawIndirectCount=1073741824
scene: N=32768 ... draws=65223 indirectCalls=1
```

If `indirectCalls` is not 1, that run is not A3 and it must be recorded with the
measurement. (On M1 Pro / MoltenVK the limit is a billion, so it never triggers.)

### Why not `vkCmdDrawIndexedIndirectCount`

§7 lists `drawIndirectCount` next to A3 because of **experiment 2 (culling on)**.
Experiment 1 has culling off, so the draw count is fixed on the CPU and plain
multi-draw is sufficient. Once culling decides the count on the GPU, A3 moves to
`vkCmdDrawIndexedIndirectCount` (and the `drawIndirectCount` feature, absent on
MoltenVK).

Required features: **`multiDrawIndirect`** (the difference from A2, in one flag),
`drawIndirectFirstInstance`, `shaderDrawParameters`, `descriptorIndexing`.

---

## SPIR-V hashes (measured, all 7 conditions)

| | mesh.vert | mesh.frag |
|---|---|---|
| lab_01 (A0×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| lab_02 (A0×B1) | `69ea042b2fa47005` | `d8aa2b932838c51d` |
| lab_03 (A1×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| lab_04 (A1×B1) | `69ea042b2fa47005` | `d8aa2b932838c51d` |
| lab_05 (A2×B0) | `69ea042b2fa47005` | `ee009e8460448a9f` |
| lab_06 (A2×B1) | `69ea042b2fa47005` | `d8aa2b932838c51d` |
| **lab_07 (A3×B1)** | `69ea042b2fa47005` | `d8aa2b932838c51d` |

**Seven conditions share one vertex shader, and there are only two fragment
shaders.** §3's requirement — fix the index path, and the SPIR-V hash stays fixed
— holds across the whole series by measurement. None of the four A levels touches
a shader.

## Descriptor layout

Identical to lab_02/04/06.

```
set 0  binding 0  UBO       Frame { viewProj, camPos, lightDir, lightColor, ambient }
       binding 1  SSBO      Object[] { model(mat4), material(uvec4) }        vert
set 1  binding 0  SSBO      Material[] { baseColorFactor, mr, tex(uvec4) }   frag
       binding 1  texture2D textures[3 × material count]                     frag
       binding 2  sampler   one shared sampler                               frag
```

---

## ⚠ A3 may be meaningless on MoltenVK

Metal has no native multi-draw-indirect. Given `drawCount > 1`, MoltenVK **expands
the commands in a loop** and encodes Metal draws one at a time. A3's whole premise
is replacing N calls with one, and the driver puts the N back — so **an A2 vs A3
number measured on Apple hardware may be emulation overhead rather than batching
gain.**

This condition is only meaningful on a platform with native multi-draw. Either
way, record the platform alongside the number.

## Running

```
LAB_OBJECTS=<N> LAB_FRAMES=800 ./lab_07          # 300 warm-up + 500 measured -> results.csv
LAB_OBJECTS=128 LAB_FRAMES=401 LAB_CAPTURE=400 LAB_CAPTURE_FILE=lab07.png ./lab_07
imgdiff lab06.png lab07.png                      # A2 vs A3 pixel equivalence
```

The CSV `condition` column reads `A3xB1`.

## Verification

- **Pixel equivalence** — **bit-exact** against lab_01 and lab_06, at both N=128
  (224 draws) and N=32768 (65,223 draws).
- **`indirectCalls=1`** in the startup log — must stay 1 regardless of N.
- **SPIR-V hashes** — per the table above.
