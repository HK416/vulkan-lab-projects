# imgdiff — 픽셀 동등성 검사 / Pixel-equivalence check

**[한국어](#한국어) · [English](#english)**

<a id="한국어"></a>

# 한국어

Indirect 렌더링 실험의 **픽셀 동등성 검증** 도구. 랩이 캡처한 PNG 두 장을
설계문서가 정한 허용오차로 비교하고 exit code로 판정한다.
전체 설계는 [`../indirect-rendering-experiment.md`](../indirect-rendering-experiment.md) §5 참조.

GPU를 쓰지 않는 **순수 호스트 도구**다. `project_deps`만 링크하며(stb include 때문),
Vulkan·SDL·`template`에 의존하지 않는다.

---

## 왜 필요한가

조건마다 드로우 경로가 다르면 래스터 순서가 미세하게 갈려 부동소수점 결과가
달라진다. 그래서 설계는 exact match가 아니라 **임계값**을 요구한다:

> 채널당 ≤ 1 LSB, 불일치 픽셀 < 0.01%

이건 실제 계산이 필요한 판정 로직이다. ImageMagick `compare`나 Python PIL은
시스템에 있다는 보장이 없고, 의존성 없는 순수 python3로 PNG를 디코딩하면
C++판보다 코드가 길어진다. `stb_image`는 이미 `project_deps`에 있으므로
로드는 공짜고 남는 건 비교 루프뿐이라 여기에 직접 넣었다.

---

## 사용법

```
imgdiff a.png b.png [maxDelta=1] [maxMismatchRatio=0.0001]
imgdiff --selfcheck
```

| exit | 의미 |
|------|------|
| 0 | 허용오차 내 (PASS) |
| 1 | 허용오차 초과 (FAIL) |
| 2 | 사용법 오류 / 파일 읽기 실패 / 크기 불일치 |

스윕 스크립트에 그대로 꽂을 수 있게 exit code로 판정한다.

출력 예:

```
PASS  1280x720  mismatched=0/921600 (0.000000%)  maxChannelDelta=0  [tolerance: delta<=1, mismatch<=0.010000%]
```

- `mismatched` — **어느 한 채널이라도** `maxDelta`를 넘은 픽셀 수.
  설계가 정한 기준이 채널당 경계이므로, 네 채널이 모두 흔들린 픽셀도 불일치 1개다.
- `maxChannelDelta` — 관측된 최악의 단일 채널 차이. 허용오차를 통과해도
  이 값이 커지고 있으면 경로가 갈리기 시작했다는 신호다.

`--selfcheck`는 비교 로직 자체를 검증한다(동일 / 전 채널 1 LSB / 한 채널 2 LSB /
`maxDelta=0` exact match, 4케이스 assert). 비교기가 고장 나면 모든 조건이
조용히 PASS하므로, 판정 결과를 신뢰하기 전에 한 번 돌릴 것.

---

## 비교할 PNG 만들기

캡처는 `template`의 `App`이 담당한다(환경변수, 랩 코드 변경 없음).

```
LAB_FRAMES=401 LAB_CAPTURE=400 LAB_CAPTURE_FILE=lab01.png ./lab_01
LAB_FRAMES=401 LAB_CAPTURE=400 LAB_CAPTURE_FILE=lab02.png ./lab_02
imgdiff lab01.png lab02.png
```

| 환경변수 | 뜻 |
|---|---|
| `LAB_FRAMES=N` | N 프레임 렌더 후 종료 (미설정/0 = 창 닫을 때까지) |
| `LAB_CAPTURE=K` | 프레임 K를 PNG로 덤프 (랩이 세는 프레임 인덱스와 동일) |
| `LAB_CAPTURE_FILE=p` | 출력 경로 (기본 `capture.png`) |

- **두 실행의 `LAB_CAPTURE`가 같아야 비교가 성립한다.** 카메라가 프레임 인덱스로
  샘플링되므로 프레임 번호가 다르면 애초에 다른 뷰다.
- 워밍업 300프레임 이후를 쓸 것(위 예시의 400).
- 캡처는 랩의 커맨드 버퍼 **끝에 append**된다. 별도 렌더 패스가 없으므로
  측정되는 경로와 캡처되는 경로가 갈릴 여지가 없다.
- 창 크기가 다르면 크기 불일치로 exit 2. 고해상도 디스플레이에서는 실제 픽셀
  크기가 요청값과 다를 수 있으니 두 실행을 같은 디스플레이에서 돌릴 것.

---

## 알려진 결과

| 비교 | 결과 |
|---|---|
| lab_01 (A0×B0) vs lab_02 (A0×B1) @ frame 400, 1280×720 | **PASS, 0/921600, maxChannelDelta=0** (bit-exact) |

허용오차를 쓸 필요조차 없었다. A축이 동일해 드로우 순서·지오메트리가 같고,
B1은 같은 값을 다른 경로로 읽어올 뿐이기 때문. A2/A3처럼 드로우가 병합되는
조건에서는 1 LSB 수준의 차이가 나타날 수 있고, 그때 허용오차가 실제로 쓰인다.

---

<a id="english"></a>

# English

The **pixel-equivalence check** for the indirect-rendering experiment. Compares
two PNGs captured by the labs against the tolerance the design fixes, and reports
the verdict as an exit code. Full design in
[`../indirect-rendering-experiment.md`](../indirect-rendering-experiment.md) §5.

A **pure host tool** — no GPU. It links `project_deps` only (for the stb include
dir) and depends on neither Vulkan, SDL, nor `template`.

---

## Why it exists

Different draw paths shift raster order, so floating-point results differ
slightly between conditions. The design therefore specifies a **threshold**, not
an exact match:

> ≤ 1 LSB per channel, mismatched pixels < 0.01%

That is real decision logic that has to be computed. ImageMagick's `compare` and
Python's PIL are not guaranteed to be installed, and decoding PNG in
dependency-free python3 is more code than the C++ version. `stb_image` is already
in `project_deps`, so loading is free and only the comparison loop remains — hence
this tool.

---

## Usage

```
imgdiff a.png b.png [maxDelta=1] [maxMismatchRatio=0.0001]
imgdiff --selfcheck
```

| exit | meaning |
|------|---------|
| 0 | within tolerance (PASS) |
| 1 | outside tolerance (FAIL) |
| 2 | usage error / unreadable file / size mismatch |

The verdict is an exit code so it drops straight into a sweep script.

Example output:

```
PASS  1280x720  mismatched=0/921600 (0.000000%)  maxChannelDelta=0  [tolerance: delta<=1, mismatch<=0.010000%]
```

- `mismatched` — pixels where **any** channel exceeds `maxDelta`. The design's
  bound is per channel, so a pixel that drifts on all four channels is still one
  mismatch.
- `maxChannelDelta` — the worst single-channel difference seen. Even on a PASS, a
  growing value signals that the paths are starting to diverge.

`--selfcheck` verifies the comparison logic itself (identical / every channel 1
LSB off / one channel 2 LSB off / `maxDelta=0` exact match — four asserts). A
broken comparator would silently PASS every condition, so run it once before
trusting a verdict.

---

## Producing the PNGs

Capture lives in `template`'s `App` and is driven by the environment — no lab
code changes.

```
LAB_FRAMES=401 LAB_CAPTURE=400 LAB_CAPTURE_FILE=lab01.png ./lab_01
LAB_FRAMES=401 LAB_CAPTURE=400 LAB_CAPTURE_FILE=lab02.png ./lab_02
imgdiff lab01.png lab02.png
```

| env var | meaning |
|---|---|
| `LAB_FRAMES=N` | quit after N rendered frames (unset/0 = until the window closes) |
| `LAB_CAPTURE=K` | dump frame K as PNG (the same frame index the labs count) |
| `LAB_CAPTURE_FILE=p` | output path (default `capture.png`) |

- **Both runs must use the same `LAB_CAPTURE`.** The camera is sampled by frame
  index, so a different frame number is simply a different view.
- Pick a frame past the 300-frame warm-up (400 above).
- The capture is **appended to the lab's own command buffer**. There is no
  separate render path that could diverge from the measured one.
- Different window sizes exit 2 as a size mismatch. On a high-DPI display the
  actual pixel size may differ from the requested one, so run both on the same
  display.

---

## Known results

| Comparison | Result |
|---|---|
| lab_01 (A0×B0) vs lab_02 (A0×B1) @ frame 400, 1280×720 | **PASS, 0/921600, maxChannelDelta=0** (bit-exact) |

The tolerance was not even needed: the A axis is identical, so draw order and
geometry match, and B1 only fetches the same values over a different path.
Conditions that merge draws (A2/A3) may produce 1 LSB differences — that is when
the tolerance starts doing real work.
