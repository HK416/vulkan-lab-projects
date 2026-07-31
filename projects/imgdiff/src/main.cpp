// imgdiff — pixel-equivalence check for the indirect-rendering experiment.
//
// Different draw paths shift raster order, so conditions are NOT expected to
// match exactly; the design fixes a tolerance instead (see
// ../indirect-rendering-experiment.md section 5): at most 1 LSB per channel,
// and fewer than 0.01% of pixels differing at all.
//
//   imgdiff a.png b.png [maxDelta=1] [maxMismatchRatio=0.0001]
//   imgdiff --selfcheck
//
// Exit code 0 = within tolerance, 1 = differs, 2 = usage/IO error, so it drops
// straight into a sweep script.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace {

struct Diff {
    uint64_t mismatched = 0; // pixels with any channel beyond maxDelta
    int maxChannelDelta = 0; // worst single-channel difference seen
};

// RGBA8 in, per-pixel verdict out. A pixel counts as mismatched when ANY channel
// differs by more than maxDelta — the per-channel bound is what the design
// specifies, so a pixel that drifts on every channel is still one mismatch.
Diff compare(const uint8_t* a, const uint8_t* b, size_t pixels, int maxDelta) {
    Diff d;
    for (size_t p = 0; p < pixels; ++p) {
        int worst = 0;
        for (size_t c = 0; c < 4; ++c) {
            const int delta = std::abs(static_cast<int>(a[p * 4 + c]) - static_cast<int>(b[p * 4 + c]));
            worst = std::max(worst, delta);
        }
        d.maxChannelDelta = std::max(d.maxChannelDelta, worst);
        if (worst > maxDelta) {
            ++d.mismatched;
        }
    }
    return d;
}

int selfCheck() {
    const uint8_t base[12] = {10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255};

    uint8_t same[12];
    std::memcpy(same, base, sizeof(base));
    Diff d = compare(base, same, 3, 1);
    assert(d.mismatched == 0 && d.maxChannelDelta == 0);

    uint8_t off1[12]; // every channel 1 LSB off — within tolerance
    for (size_t i = 0; i < 12; ++i) {
        off1[i] = static_cast<uint8_t>(base[i] - 1);
    }
    d = compare(base, off1, 3, 1);
    assert(d.mismatched == 0 && d.maxChannelDelta == 1);

    uint8_t off2[12]; // one pixel, one channel, 2 LSB off — out of tolerance
    std::memcpy(off2, base, sizeof(base));
    off2[5] = static_cast<uint8_t>(off2[5] + 2);
    d = compare(base, off2, 3, 1);
    assert(d.mismatched == 1 && d.maxChannelDelta == 2);

    // maxDelta = 0 means exact match required.
    d = compare(base, off1, 3, 0);
    assert(d.mismatched == 3);

    std::printf("selfcheck ok\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--selfcheck") {
        return selfCheck();
    }
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: imgdiff a.png b.png [maxDelta=1] [maxMismatchRatio=0.0001]\n"
                     "       imgdiff --selfcheck\n");
        return 2;
    }
    const int maxDelta = argc > 3 ? std::atoi(argv[3]) : 1;
    const double maxRatio = argc > 4 ? std::atof(argv[4]) : 0.0001;

    int wa = 0, ha = 0, wb = 0, hb = 0, channels = 0;
    uint8_t* a = stbi_load(argv[1], &wa, &ha, &channels, 4);
    if (a == nullptr) {
        std::fprintf(stderr, "cannot read %s: %s\n", argv[1], stbi_failure_reason());
        return 2;
    }
    uint8_t* b = stbi_load(argv[2], &wb, &hb, &channels, 4);
    if (b == nullptr) {
        std::fprintf(stderr, "cannot read %s: %s\n", argv[2], stbi_failure_reason());
        stbi_image_free(a);
        return 2;
    }
    if (wa != wb || ha != hb) {
        std::fprintf(stderr, "size mismatch: %dx%d vs %dx%d\n", wa, ha, wb, hb);
        stbi_image_free(a);
        stbi_image_free(b);
        return 2;
    }

    const size_t pixels = static_cast<size_t>(wa) * ha;
    const Diff d = compare(a, b, pixels, maxDelta);
    stbi_image_free(a);
    stbi_image_free(b);

    const double ratio = static_cast<double>(d.mismatched) / static_cast<double>(pixels);
    const bool pass = ratio <= maxRatio;
    std::printf("%s  %dx%d  mismatched=%llu/%zu (%.6f%%)  maxChannelDelta=%d"
                "  [tolerance: delta<=%d, mismatch<=%.6f%%]\n",
                pass ? "PASS" : "FAIL",
                wa,
                ha,
                static_cast<unsigned long long>(d.mismatched),
                pixels,
                ratio * 100.0,
                d.maxChannelDelta,
                maxDelta,
                maxRatio * 100.0);
    return pass ? 0 : 1;
}
