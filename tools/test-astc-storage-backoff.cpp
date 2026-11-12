// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for ASTC decode storage image failure backoff (patch 0923).
// Tests exponential backoff, allocation storm prevention, free pool reuse, and recovery.
//   c++ -std=c++20 -O2 tools/test-astc-storage-backoff.cpp -o /tmp/test-astc-storage-backoff && /tmp/test-astc-storage-backoff
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

struct Shape {
    uint32_t width;
    uint32_t height;
    uint32_t levels;
    uint32_t layers;
    bool operator==(const Shape& o) const = default;
};

struct MockPendingTranscode {
    Shape shape;
};

class MockTextureCacheRuntime {
public:
    uint64_t astc_decode_retry_frame = 0;
    uint32_t astc_decode_fail_count = 0;
    uint64_t runtime_frame_index = 0;
    bool upload_refused = false;

    // Simulation metrics
    uint32_t alloc_attempts = 0;
    bool force_alloc_failure = false;

    std::vector<MockPendingTranscode> free_transcodes;

    void TickFrame() {
        ++runtime_frame_index;
    }

    [[nodiscard]] bool CanAllocateAstcDecodeStorage() const noexcept {
        return runtime_frame_index >= astc_decode_retry_frame;
    }

    void NoteAstcDecodeStorageRefusal() noexcept {
        ++astc_decode_fail_count;
        const uint64_t backoff_frames =
            std::min<uint64_t>(uint64_t{1} << std::min<uint32_t>(astc_decode_fail_count + 2, 6), 64);
        astc_decode_retry_frame = runtime_frame_index + backoff_frames;
        upload_refused = true;
    }

    void NoteAstcDecodeStorageSuccess() noexcept {
        astc_decode_fail_count = 0;
        astc_decode_retry_frame = 0;
    }

    bool TryTranscode(const Shape& shape) {
        // 1. Reusable decode image of the same shape
        auto reusable = std::find_if(free_transcodes.begin(), free_transcodes.end(),
                                     [&](const MockPendingTranscode& t) { return t.shape == shape; });
        if (reusable != free_transcodes.end()) {
            free_transcodes.erase(reusable);
            upload_refused = false;
            return true;
        }

        // 2. Backoff check: do not allocate while in backoff
        if (!CanAllocateAstcDecodeStorage()) {
            upload_refused = true;
            return false;
        }

        // 3. Attempt allocation
        ++alloc_attempts;
        if (force_alloc_failure) {
            NoteAstcDecodeStorageRefusal();
            return false;
        }

        NoteAstcDecodeStorageSuccess();
        upload_refused = false;
        return true;
    }
};

static void TestNormalOperation() {
    MockTextureCacheRuntime runtime;
    Shape s1{1536, 1536, 11, 1};

    CHECK(runtime.TryTranscode(s1));
    CHECK(runtime.alloc_attempts == 1);
    CHECK(runtime.astc_decode_fail_count == 0);
    CHECK(runtime.astc_decode_retry_frame == 0);
    CHECK(!runtime.upload_refused);
}

static void TestExponentialBackoffAndStormPrevention() {
    MockTextureCacheRuntime runtime;
    runtime.force_alloc_failure = true;
    Shape s1{1536, 1536, 11, 1};
    Shape s2{768, 768, 10, 1};
    Shape s3{1024, 1024, 11, 1};

    // Frame 0: First failure
    CHECK(!runtime.TryTranscode(s1));
    CHECK(runtime.alloc_attempts == 1);
    CHECK(runtime.astc_decode_fail_count == 1);
    // 1 << (1 + 2) = 8 frames -> retry at frame 8
    CHECK(runtime.astc_decode_retry_frame == 8);
    CHECK(runtime.upload_refused);

    // Frame 0: Consecutive textures in the SAME frame (simulating scene with dozens of ASTC textures)
    // Must NOT attempt allocation, must NOT spam the allocator!
    CHECK(!runtime.TryTranscode(s2));
    CHECK(!runtime.TryTranscode(s3));
    CHECK(runtime.alloc_attempts == 1); // Still 1! Zero new allocation calls!
    CHECK(runtime.astc_decode_fail_count == 1);
    CHECK(runtime.astc_decode_retry_frame == 8);

    // Frames 1..7: Advance frame ticks during cooldown
    for (uint64_t f = 1; f < 8; ++f) {
        runtime.TickFrame();
        CHECK(!runtime.CanAllocateAstcDecodeStorage());
        // Draws during cooldown are rejected without touching allocator
        CHECK(!runtime.TryTranscode(s1));
        CHECK(!runtime.TryTranscode(s2));
        CHECK(runtime.alloc_attempts == 1);
    }

    // Frame 8: Cooldown expired
    runtime.TickFrame();
    CHECK(runtime.runtime_frame_index == 8);
    CHECK(runtime.CanAllocateAstcDecodeStorage());

    // 2nd failure at frame 8: backoff = 1 << (2 + 2) = 16 frames -> retry at 8 + 16 = 24
    CHECK(!runtime.TryTranscode(s1));
    CHECK(runtime.alloc_attempts == 2);
    CHECK(runtime.astc_decode_fail_count == 2);
    CHECK(runtime.astc_decode_retry_frame == 24);

    // Skip to frame 23 (during cooldown frames 9..23)
    while (runtime.runtime_frame_index < 23) {
        runtime.TickFrame();
        CHECK(!runtime.TryTranscode(s1));
    }
    CHECK(runtime.alloc_attempts == 2); // No attempts during frames 9..23

    // Advance to frame 24: Cooldown expired
    runtime.TickFrame();
    CHECK(runtime.runtime_frame_index == 24);

    // 3rd failure at frame 24: backoff = 1 << (3 + 2) = 32 frames -> retry at 24 + 32 = 56
    CHECK(!runtime.TryTranscode(s1));
    CHECK(runtime.alloc_attempts == 3);
    CHECK(runtime.astc_decode_fail_count == 3);
    CHECK(runtime.astc_decode_retry_frame == 56);

    // Skip to frame 55
    while (runtime.runtime_frame_index < 55) {
        runtime.TickFrame();
        CHECK(!runtime.TryTranscode(s1));
    }
    CHECK(runtime.alloc_attempts == 3);

    // Advance to frame 56
    runtime.TickFrame();
    CHECK(runtime.runtime_frame_index == 56);

    // 4th failure at frame 56: backoff = 1 << (4 + 2) = 64 frames -> retry at 56 + 64 = 120
    CHECK(!runtime.TryTranscode(s1));
    CHECK(runtime.alloc_attempts == 4);
    CHECK(runtime.astc_decode_fail_count == 4);
    CHECK(runtime.astc_decode_retry_frame == 120);

    // Skip to frame 119
    while (runtime.runtime_frame_index < 119) {
        runtime.TickFrame();
        CHECK(!runtime.TryTranscode(s1));
    }
    CHECK(runtime.alloc_attempts == 4);

    // Advance to frame 120
    runtime.TickFrame();
    CHECK(runtime.runtime_frame_index == 120);

    // 5th failure at frame 120: backoff capped at 64 frames -> retry at 120 + 64 = 184
    CHECK(!runtime.TryTranscode(s1));
    CHECK(runtime.alloc_attempts == 5);
    CHECK(runtime.astc_decode_fail_count == 5);
    CHECK(runtime.astc_decode_retry_frame == 184);

    // Across 184 frames with hundreds of textures, only 5 allocation calls occurred instead of ~10,000!
}

static void TestFreePoolBypassesBackoff() {
    MockTextureCacheRuntime runtime;
    runtime.force_alloc_failure = true;
    Shape s1{1536, 1536, 11, 1};
    Shape cached_shape{512, 512, 10, 1};

    // Put a shape in free_transcodes
    runtime.free_transcodes.push_back({cached_shape});

    // Cause a failure on s1 to enter backoff
    CHECK(!runtime.TryTranscode(s1));
    CHECK(runtime.astc_decode_retry_frame == 8);

    // The cached shape in free_transcodes can still be used without allocating!
    CHECK(runtime.TryTranscode(cached_shape));
    CHECK(runtime.alloc_attempts == 1); // No new allocation!
    CHECK(runtime.free_transcodes.empty());
}

static void TestRecoveryOnSuccess() {
    MockTextureCacheRuntime runtime;
    runtime.force_alloc_failure = true;
    Shape s1{1536, 1536, 11, 1};

    // Fail 3 times
    CHECK(!runtime.TryTranscode(s1));
    while (runtime.runtime_frame_index < runtime.astc_decode_retry_frame) runtime.TickFrame();
    CHECK(!runtime.TryTranscode(s1));
    while (runtime.runtime_frame_index < runtime.astc_decode_retry_frame) runtime.TickFrame();
    CHECK(!runtime.TryTranscode(s1));
    CHECK(runtime.astc_decode_fail_count == 3);

    // Wait until retry frame
    while (runtime.runtime_frame_index < runtime.astc_decode_retry_frame) runtime.TickFrame();

    // Memory recovers
    runtime.force_alloc_failure = false;
    CHECK(runtime.TryTranscode(s1));
    CHECK(runtime.astc_decode_fail_count == 0);
    CHECK(runtime.astc_decode_retry_frame == 0);
    CHECK(!runtime.upload_refused);
}

static void TestCensusNotificationRateLimiting() {
    std::atomic<uint32_t> notify_count{0};
    std::atomic<uint64_t> last_wake_ms{0};

    auto RefusedNotify = [&](uint64_t fake_now_ms) {
        auto prev_ms = last_wake_ms.load(std::memory_order_relaxed);
        if (fake_now_ms >= prev_ms + 1000 &&
            last_wake_ms.compare_exchange_strong(prev_ms, fake_now_ms, std::memory_order_relaxed)) {
            notify_count.fetch_add(1, std::memory_order_relaxed);
        }
    };

    // Simulate 1,000 rapid refusal events within 500 ms at t=1000ms
    for (int i = 0; i < 1000; ++i) {
        RefusedNotify(1000 + (i % 500));
    }
    // Exactly 1 notification should be delivered
    CHECK(notify_count.load() == 1);

    // At t=2001ms (1001ms later), next notification is allowed
    RefusedNotify(2001);
    CHECK(notify_count.load() == 2);

    // More rapid events before t=3000ms are ignored
    for (int i = 0; i < 500; ++i) {
        RefusedNotify(2050 + i);
    }
    CHECK(notify_count.load() == 2);
}

int main() {
    TestNormalOperation();
    TestExponentialBackoffAndStormPrevention();
    TestFreePoolBypassesBackoff();
    TestRecoveryOnSuccess();
    TestCensusNotificationRateLimiting();

    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("astc storage backoff: storm prevention, exponential backoff, free pool reuse, and census rate limiting PASS");
    return 0;
}
