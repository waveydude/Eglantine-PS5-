// SPDX-License-Identifier: GPL-3.0-or-later
// The texture collector's pool rule (headless/texture_pool_pressure.h) on numbers from the console.
#include <cassert>
#include <cstdint>
#include <cstdio>

#include "../headless/texture_pool_pressure.h"

using Eden::TexturePool::FixedPressure;
using Eden::TexturePool::Inputs;
using Eden::TexturePool::Pressure;

constexpr std::uint64_t MiB = 1ULL << 20;

static Inputs At(std::uint64_t free_mib, std::uint64_t images_mib, std::uint64_t falling_mib_s = 0,
                 bool refusal = false) {
    return Inputs{free_mib * MiB, images_mib * MiB, 1024 * MiB, 256 * MiB, refusal, falling_mib_s * MiB};
}

int main() {
    // crash-20261108-222452, t=56 s: title screen, 133 MiB of images, 5534 MiB free, steady.
    assert(Pressure(At(5534, 133)) == 0 && FixedPressure(At(5534, 133)) == 0);

    // The same run while it loads Kokiri: 5534 -> 984 MiB in ~4 s (~1100 MiB/s). Halfway down
    // (3000 MiB free) the fixed rule still says nothing; the new one starts collecting.
    assert(FixedPressure(At(3000, 133, 1100)) == 0);
    assert(Pressure(At(3000, 133, 1100)) == 1);

    // crash-20261108-025613 (door), just before the refusal: 3420 MiB of images. With 1400 MiB
    // free the fixed rule is calm; a cache this size needs ~1710 MiB of margin.
    assert(FixedPressure(At(1400, 3420)) == 0);
    assert(Pressure(At(1400, 3420)) == 1);
    // ... and 400 MiB free is critical for it (3420 / 8 = 427 MiB), merely short for the old rule.
    assert(FixedPressure(At(400, 3420)) == 1);
    assert(Pressure(At(400, 3420)) == 2);

    // A refusal always means the hardest collection, both rules.
    assert(Pressure(At(5000, 100, 0, true)) == 2 && FixedPressure(At(5000, 100, 0, true)) == 2);

    // Normal play must stay calm: a 1.5 GiB working set with 4 GiB free, slow drift.
    assert(Pressure(At(4096, 1536, 50)) == 0);
    // Small caches keep the old marks exactly (no extra eviction where nothing went wrong).
    for (std::uint64_t free_mib : {100ULL, 255ULL, 256ULL, 900ULL, 1023ULL, 1024ULL, 3000ULL}) {
        assert(Pressure(At(free_mib, 400)) == FixedPressure(At(free_mib, 400)));
    }
    // Unknown pool (~0 free) is never pressure.
    assert(Pressure(Inputs{~0ULL, 3000 * MiB, 1024 * MiB, 256 * MiB, false, 0}) == 0);
    std::puts("texture pool pressure: PASS (crash-20261108-222452 load, crash-20261108-025613 door, steady play)");
}
