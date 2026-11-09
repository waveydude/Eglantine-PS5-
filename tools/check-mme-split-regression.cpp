// SPDX-License-Identifier: GPL-3.0-or-later
// Regression test suite for Ocarina of Time / Switch 2 pushbuffer split and MME parked macro state.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>
#include <array>

#include "../Eglantine/src/video_core/split_macro_state.h"

using Tegra::ReadsAsCommands;
using Tegra::SplitReadsAsContinuation;

static void TestEntry42FreshStreamSplit() {
    printf("[TEST 1] Ampere pushbuffer split: Entry 42 fresh stream detection vs continuation...\n");
    // Exact 64 words captured from Entry 42 in eden_log.txt line 4481-4500
    const std::array<u32, 64> entry42_words = {
        0x2001055b, 0xbf000000, 0x2001056f, 0xc1200000, 0x2001061f, 0x00000000, 0x2001064f, 0x0000181d,
        0x80000e3a, 0xa0050e08, 0x00000001, 0x0000174f, 0x1155a50f, 0x1155a518, 0x0100121a, 0xa0050e08,
        0x00000005, 0x0000174f, 0x1155a9a8, 0x1155aa44, 0x4500805f, 0x800003d0, 0x80000084, 0x800003c7,
        0x20020e5c, 0x20164010, 0x00018020, 0x80000489, 0x8000065b, 0x800101d5, 0x20010ed6, 0x00030002,
        0x80200820, 0x20020825, 0x00000021, 0x0622e900, 0x80220e3a, 0x80330e3a, 0x80440e3a, 0x80000e28,
        0x80180e3a, 0x80370e3a, 0x80490e3a, 0x80160e3a, 0x20010ed6, 0x03fc0000, 0xa0040e02, 0x00000008,
        0x00000000, 0x34dce500, 0x03500022, 0xa0040e02, 0x00000004, 0x00000000, 0x34dce500, 0x03500022,
        0xa0040e02, 0x00000004, 0x00000004, 0x34d91400, 0x00100022, 0x81480e36, 0x200308e4, 0x362b6040,
    };

    // Macro 0x311 from Entry 41 wanted 12 more words: method_count = 12
    const u32 carried_count = 12;
    const bool as_fresh = ReadsAsCommands(entry42_words, 0x9e4, 0);
    assert(as_fresh && "Entry 42 must be recognized as valid fresh command stream at offset 0");

    const bool as_continuation = ReadsAsCommands(entry42_words, 0x9e4, carried_count);
    assert(!as_continuation && "Entry 42 offset 12 must NOT be recognized as commands");

    const bool continues = SplitReadsAsContinuation(entry42_words, 0x9e4, carried_count);
    assert(!continues && "Entry 42 must NOT be treated as a continuation; it MUST be parked");
    printf("  -> PASS: Entry 42 correctly identified as fresh stream; macro 0x311 will be safely parked.\n");
}

static void TestSubroutineContinuationNotParked() {
    printf("[TEST 2] Ampere pushbuffer split: Subroutine data continuation (preserving run 165 fix)...\n");
    // Pure data words (floats, coordinates, integers) passed in subroutines
    const std::array<u32, 5> subroutine_data = {
        0x3f800000, // 1.0f (header >> 29 is 1, count is 0x1f80 = 8064 words, exceeds entry_words 5 -> invalid command!)
        0x40000000, // 2.0f
        0x00000000, // 0
        0x00000001, // 1
        0x00000002  // 2
    };

    const bool as_fresh = ReadsAsCommands(subroutine_data, 5, 0);
    assert(!as_fresh && "Raw data must NOT be recognized as fresh commands");

    const bool continues = SplitReadsAsContinuation(subroutine_data, 5, 5);
    assert(continues && "Pure data subroutine MUST continue method, not be parked");
    printf("  -> PASS: Subroutine data correctly recognized as continuation; avoids hang.\n");
}

static void TestMacroParkingAndResume() {
    u32 active = 0xe00;
    std::vector<u32> params{0x10, 0x20, 0x30};
    Tegra::ParkedMacro::Segments reads{{0x2000, 2}, {0, 1}};
    bool dirty = true;
    Tegra::ParkedMacro parked;

    parked.Take(active, params, reads, dirty);
    assert(active == 0 && params.empty() && reads.empty() && !dirty);

    // A fresh entry can write another macro while the original is parked.
    active = 0xe02;
    params = {0xaa};
    reads = {{0x8000, 1}};
    std::move(parked).Restore(active, params, reads, dirty);
    assert(active == 0xe00 && dirty);
    assert((params == std::vector<u32>{0x10, 0x20, 0x30}));
    assert((reads == Tegra::ParkedMacro::Segments{{0x2000, 2}, {0, 1}}));

    // High GPU accuracy refreshes from the recorded segments in order. A segment
    // without a guest address still consumes its words in the parameter vector.
    reads.emplace_back(0x3000, 1);
    params.push_back(0x40);
    std::size_t index = 0;
    for (const auto& [address, count] : reads) {
        if (address != 0) {
            for (std::size_t i = 0; i < count; ++i) {
                params[index + i] = static_cast<u32>(address + i * sizeof(u32));
            }
        }
        index += count;
    }
    assert(index == params.size());
    assert((params == std::vector<u32>{0x2000, 0x2004, 0x30, 0x3000}));
    std::puts("  -> PASS: production macro state preserves guest addresses and dirty state");
}

int main() {
    TestEntry42FreshStreamSplit();
    TestSubroutineContinuationNotParked();
    TestMacroParkingAndResume();
}
