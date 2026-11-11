// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for core patch 0916 (EDEN_NX2_BC_PENDING_MIB): PendingDestroyBytes must hold exactly
// what a DelayedDestructionRing of the same length holds, through any order of Push, Tick, Clear.
//   c++ -std=c++20 -I Eglantine/src tools/test-pending-destroy.cpp -o /tmp/t && /tmp/t
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "video_core/buffer_cache/pending_destroy.h"
#include "video_core/delayed_destruction_ring.h"

namespace {
int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                                    \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

std::uint64_t live_bytes = 0;
std::uint64_t live_count = 0;
// Stands in for a buffer: counts itself as alive until the ring destroys it.
struct Fake {
    std::uint64_t bytes = 0;
    explicit Fake(std::uint64_t b) : bytes{b} {
        live_bytes += b;
        ++live_count;
    }
    Fake(Fake&& other) noexcept : bytes{other.bytes} {
        other.bytes = ~std::uint64_t{0};
    }
    Fake& operator=(Fake&& other) noexcept {
        Drop();
        bytes = other.bytes;
        other.bytes = ~std::uint64_t{0};
        return *this;
    }
    ~Fake() {
        Drop();
    }
    void Drop() {
        if (bytes != ~std::uint64_t{0}) {
            live_bytes -= bytes;
            --live_count;
            bytes = ~std::uint64_t{0};
        }
    }
};
} // namespace

int main() {
    constexpr std::size_t Ticks = 8;
    {
        VideoCommon::PendingDestroyBytes<Ticks> pending;
        CHECK(pending.Bytes() == 0 && pending.Buffers() == 0);
        CHECK(!pending.Over(0));           // no limit: never, even with nothing waiting
        CHECK(!pending.Over(1));
        pending.Push(100);
        CHECK(!pending.Over(0));           // no limit: never, with something waiting
        CHECK(!pending.Over(101));         // under the limit
        CHECK(pending.Over(100));          // exactly at the limit counts
        CHECK(pending.Over(99));
        // A buffer pushed now leaves after exactly Ticks ticks, not one earlier.
        for (std::size_t tick = 1; tick < Ticks; ++tick) {
            pending.Tick();
            CHECK(pending.Bytes() == 100 && pending.Buffers() == 1);
        }
        pending.Tick();
        CHECK(pending.Bytes() == 0 && pending.Buffers() == 0);
        // Nothing ticks during a long frame: everything pushed stays.
        for (int i = 0; i < 1000; ++i) {
            pending.Push(1 << 20);
        }
        CHECK(pending.Bytes() == (std::uint64_t{1000} << 20) && pending.Buffers() == 1000);
        pending.Clear();
        CHECK(pending.Bytes() == 0 && pending.Buffers() == 0 && !pending.Over(1));
        // After a Clear the slots are empty too: ticking must not take anything off twice.
        pending.Push(7);
        for (std::size_t tick = 0; tick < 3 * Ticks; ++tick) {
            pending.Tick();
        }
        CHECK(pending.Bytes() == 0 && pending.Buffers() == 0);
    }
    // Against the real ring, random operations.
    {
        VideoCommon::DelayedDestructionRing<Fake, Ticks> ring;
        VideoCommon::PendingDestroyBytes<Ticks> pending;
        std::mt19937_64 random{20261110};
        for (int step = 0; step < 200000; ++step) {
            const auto roll = random() % 100;
            if (roll < 70) {
                const std::uint64_t bytes = random() % (64u << 20);
                pending.Push(bytes);
                ring.Push(Fake{bytes});
            } else if (roll < 98) {
                ring.Tick();
                pending.Tick();
            } else {
                ring.Clear();
                pending.Clear();
            }
            if (pending.Bytes() != live_bytes || pending.Buffers() != live_count) {
                std::printf("FAIL step %d: counted %llu bytes in %llu, the ring holds %llu in %llu\n", step,
                            static_cast<unsigned long long>(pending.Bytes()),
                            static_cast<unsigned long long>(pending.Buffers()),
                            static_cast<unsigned long long>(live_bytes),
                            static_cast<unsigned long long>(live_count));
                ++failures;
                break;
            }
        }
    }
    CHECK(live_bytes == 0 && live_count == 0);
    std::printf(failures == 0 ? "pending-destroy: all checks passed\n" : "pending-destroy: %d FAILED\n", failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
