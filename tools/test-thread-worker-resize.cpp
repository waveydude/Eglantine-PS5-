// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for Common::StatefulThreadWorker scaling and resizing (patch 0920).
//   g++ -std=c++20 -O2 -pthread -IEglantine/src tools/test-thread-worker-resize.cpp -o /tmp/test-thread-worker-resize && /tmp/test-thread-worker-resize
#include <atomic>
#include <chrono>
#include <cstdio>
#include <vector>

#include "common/thread_worker.h"

// Stubs for thread affinity / naming helpers in standalone test
namespace Common {
void SetCurrentThreadName(const char*) {}
void SetCurrentThreadPriority(ThreadPriority) {}
void SetCurrentThreadToEfficiencyCores() {}
void SetCurrentThreadToBackgroundWork() {}
void SetCurrentThreadToAllCores() {}
}

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

using namespace std::chrono_literals;

int main() {
    // 1. Initial pool with 8 workers
    Common::ThreadWorker workers(8, "TestWorker", {}, Common::ThreadPlacement::Background);
    CHECK(workers.WorkerCount() == 8);

    std::atomic<int> counter{0};
    for (int i = 0; i < 64; ++i) {
        workers.QueueWork([&counter] {
            std::this_thread::sleep_for(1ms);
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }
    workers.WaitForRequests();
    CHECK(counter.load() == 64);

    // 2. Scale down from 8 to 2 workers
    workers.SetWorkerCount(2);
    CHECK(workers.WorkerCount() == 2);

    for (int i = 0; i < 32; ++i) {
        workers.QueueWork([&counter] {
            std::this_thread::sleep_for(1ms);
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }
    workers.WaitForRequests();
    CHECK(counter.load() == 96);

    // 3. Scale up from 2 to 4 workers
    workers.SetWorkerCount(4);
    CHECK(workers.WorkerCount() == 4);

    for (int i = 0; i < 32; ++i) {
        workers.QueueWork([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }
    workers.WaitForRequests();
    CHECK(counter.load() == 128);

    // 4. Clamping to at least 1 worker
    workers.SetWorkerCount(0);
    CHECK(workers.WorkerCount() == 1);

    if (failures != 0) {
        std::fprintf(stderr, "test-thread-worker-resize failed with %d errors\n", failures);
        return 1;
    }

    std::puts("thread worker resize: scale down, join, execute, scale up, clamp PASS");
    return 0;
}
