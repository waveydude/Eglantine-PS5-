// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// How hard the texture collector works, from the shared memory pool (the texture cache's
// PoolPressure, tools/prepare-vulkan-port.py). 0: the cache's own marks decide; 1: images unused
// for 25 frames go (as past "expected"); 2: images unused for 10 frames go (as past "critical").
//
// The fixed marks (1 GiB short, 256 MiB critical) let two crashes through:
// - crash-20261108-025613 (door): the images held 3420 MiB, 1897 MiB of them unused for 10+
//   frames, and the pool refused with 1 GiB of margin long gone. A cache that size needs a margin
//   that size: an area load makes images in proportion to what the scene draws.
// - crash-20261108-222452 (85 s): the pool fell from 5534 to 984 MiB in about 4 s at the Kokiri
//   load. Collection runs once a frame and only takes what is already unused, so it has to start
//   while the fall is under way, not once the mark is crossed.
// So the margins scale with the images held, and a fast fall counts as short early.

#include <algorithm>
#include <cstdint>

namespace Eden::TexturePool {

struct Inputs {
    std::uint64_t free_bytes;     // largest free block of the pool (~0: not known)
    std::uint64_t images_bytes;   // what the texture cache's images hold
    std::uint64_t short_bytes;    // EDEN_NX2_TC_POOL_SHORT_MIB (1024 MiB)
    std::uint64_t critical_bytes; // EDEN_NX2_TC_POOL_CRITICAL_MIB (256 MiB)
    bool recent_refusal;          // an allocation was refused in the last 120 frames
    std::uint64_t falling_bytes_per_s; // how fast the pool's free block shrinks (0: not falling)
};

// The marks as a cache of this size needs them.
constexpr std::uint64_t ShortMark(const Inputs& in) {
    return (std::max)(in.short_bytes, in.images_bytes / 2);
}
constexpr std::uint64_t CriticalMark(const Inputs& in) {
    return (std::max)(in.critical_bytes, in.images_bytes / 8);
}

constexpr int Pressure(const Inputs& in) {
    if (in.recent_refusal) {
        return 2;
    }
    if (in.free_bytes < CriticalMark(in)) {
        return 2;
    }
    const std::uint64_t short_mark = ShortMark(in);
    if (in.free_bytes < short_mark) {
        return 1;
    }
    // Falling fast enough to cross the short mark within 2 s: collect now.
    if (in.falling_bytes_per_s != 0 && in.free_bytes - short_mark < 2 * in.falling_bytes_per_s) {
        return 1;
    }
    return 0;
}

// The fixed-mark rule this replaces (EDEN_NX2_TC_ADAPTIVE=0 keeps it).
constexpr int FixedPressure(const Inputs& in) {
    if (in.recent_refusal || in.free_bytes < in.critical_bytes) {
        return 2;
    }
    return in.free_bytes < in.short_bytes ? 1 : 0;
}

} // namespace Eden::TexturePool
