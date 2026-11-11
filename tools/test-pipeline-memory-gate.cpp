// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for Eglantine's src/video_core/renderer_vulkan/pipeline_memory_gate.h (patch 0915).
//   c++ -std=c++20 -O1 -pthread -IEglantine/src/video_core/renderer_vulkan tools/test-pipeline-memory-gate.cpp -o /tmp/pmg && /tmp/pmg
// (Eglantine patched through 0915; the header is that patch's new file.)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "pipeline_memory_gate.h"

using namespace Vulkan::PipelineMemoryGate;
using namespace std::chrono_literals;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static std::atomic<std::uint64_t> pool_free{0};
static std::uint64_t Free() { return pool_free.load(); }

// Eight builders, each inside the gate for `hold`; returns the most that were inside at once.
static unsigned Run(Counters& counters, std::uint64_t mark, std::chrono::microseconds limit,
                    std::chrono::milliseconds hold, unsigned threads = 8) {
    std::atomic<unsigned> inside{0}, most{0};
    std::vector<std::thread> builders;
    for (unsigned t = 0; t < threads; ++t) {
        builders.emplace_back([&] {
            Gate gate{counters, &Free, mark, limit, 200us};
            const unsigned now = inside.fetch_add(1) + 1;
            unsigned seen = most.load();
            while (now > seen && !most.compare_exchange_weak(seen, now)) {}
            std::this_thread::sleep_for(hold);
            inside.fetch_sub(1);
        });
    }
    for (auto& b : builders) b.join();
    return most.load();
}

int main() {
    constexpr std::uint64_t MiB = 1 << 20;
    {   // Off (mark 0): never waits, never counts.
        Counters c; pool_free = 1;
        CHECK(Run(c, 0, 2s, 20ms) > 1);
        CHECK(c.builds == 0 && c.waited == 0 && c.running == 0);
    }
    {   // Plenty free: builds run side by side, nobody waits.
        Counters c; pool_free = 900 * MiB;
        CHECK(Run(c, 512 * MiB, 2s, 20ms) > 1);
        CHECK(c.builds == 8 && c.waited == 0 && c.timeouts == 0 && c.running == 0);
    }
    {   // Pool unknown (0): no waiting either.
        Counters c; pool_free = 0;
        CHECK(Run(c, 512 * MiB, 2s, 20ms) > 1);
        CHECK(c.waited == 0 && c.running == 0);
    }
    {   // Short: one at a time, all of them done, none timed out.
        Counters c; pool_free = 100 * MiB;
        CHECK(Run(c, 512 * MiB, 5s, 10ms) == 1);
        CHECK(c.builds == 8 && c.waited >= 1 && c.timeouts == 0 && c.running == 0);
    }
    {   // A waiter goes ahead at its limit while another build holds the gate.
        Counters c; pool_free = 100 * MiB;
        std::atomic<bool> held{false}, release{false};
        std::thread holder([&] {
            Gate gate{c, &Free, 512 * MiB, 10s, 200us};
            held = true;
            while (!release) std::this_thread::sleep_for(1ms);
        });
        while (!held) std::this_thread::sleep_for(1ms);
        const auto start = std::chrono::steady_clock::now();
        { Gate gate{c, &Free, 512 * MiB, 30ms, 200us}; CHECK(c.running == 2); }
        const auto waited = std::chrono::steady_clock::now() - start;
        CHECK(waited >= 30ms && waited < 2s);
        CHECK(c.timeouts == 1);
        release = true;
        holder.join();
        CHECK(c.running == 0);
    }
    {   // The pool recovers while a build waits: it goes in before its limit.
        Counters c; pool_free = 100 * MiB;
        std::atomic<bool> held{false}, release{false};
        std::thread holder([&] {
            Gate gate{c, &Free, 512 * MiB, 10s, 200us};
            held = true;
            while (!release) std::this_thread::sleep_for(1ms);
        });
        while (!held) std::this_thread::sleep_for(1ms);
        std::thread freer([] { std::this_thread::sleep_for(20ms); pool_free = 900 * MiB; });
        const auto start = std::chrono::steady_clock::now();
        { Gate gate{c, &Free, 512 * MiB, 5s, 200us}; }
        CHECK(std::chrono::steady_clock::now() - start < 1s);
        CHECK(c.timeouts == 0 && c.waited == 1);
        freer.join();
        release = true;
        holder.join();
        CHECK(c.running == 0);
    }
    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::puts("pipeline memory gate: all checks passed");
    return 0;
}
