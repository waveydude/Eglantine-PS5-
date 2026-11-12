// Host test for headless/upload_retry.h (the texture cache's UploadLater, patch 0924).
//   c++ -std=c++20 -O1 -Iheadless tools/test-upload-retry.cpp -o /tmp/tur && /tmp/tur
#include <cassert>
#include <cstdio>

#include "upload_retry.h"

using namespace Eden::UploadRetry;

int main() {
    // The waits grow 8, 16, 32, 64 and stay at 128.
    static_assert(BackoffFrames(1) == 8 && BackoffFrames(2) == 16 && BackoffFrames(3) == 32);
    static_assert(BackoffFrames(4) == 64 && BackoffFrames(5) == 128 && BackoffFrames(6) == 128);
    static_assert(BackoffFrames(1000) == 128 && BackoffFrames(0xffffffffU) == 128);
    static_assert(BackoffFrames(0) == 4); // never asked (the count is 1 at the first refusal)

    // No limit unless one is asked for: unset, empty, 0 and text that is no number never give up.
    assert(ReadMaxRetries(nullptr) == 0 && ReadMaxRetries("") == 0 && ReadMaxRetries("0") == 0);
    assert(ReadMaxRetries("off") == 0 && ReadMaxRetries("-1") == 0);
    assert(ReadMaxRetries("8") == 8 && ReadMaxRetries("1000000") == 1000000);
    assert(ReadMaxRetries("99999999999999999999") == 0xffffffffU);
    static_assert(!GiveUp(1, 0) && !GiveUp(8, 0) && !GiveUp(0xffffffffU, 0));
    // With a limit: at that refusal, not before.
    static_assert(!GiveUp(7, 8) && GiveUp(8, 8) && GiveUp(9, 8) && GiveUp(1, 1));

    // A long shortage (the Kakariko crash runs: minutes at 1 MiB free) keeps one image alive:
    // 100,000 frames of refusals, never given up, a try every 128 frames once the waits are long.
    {
        const std::uint32_t max_retries = ReadMaxRetries(nullptr);
        std::uint32_t fails = 0;
        std::uint64_t retry_frame = 0, attempts = 0;
        for (std::uint64_t frame = 1; frame <= 100'000; ++frame) {
            if (frame < retry_frame) continue;
            ++attempts;
            fails = NextFailCount(fails);
            assert(!GiveUp(fails, max_retries));
            retry_frame = frame + BackoffFrames(fails);
        }
        assert(attempts > 700 && attempts < 800); // about 100,000 / 128, not 100,000
        // Memory comes back: the next try uploads, and the count starts again.
        fails = 0;
        assert(BackoffFrames(NextFailCount(fails)) == 8);
    }
    // The old limit, asked for: given up at the 8th refusal, about 630 frames in.
    {
        std::uint32_t fails = 0;
        std::uint64_t frame = 1, retry_frame = 0;
        for (;; ++frame) {
            if (frame < retry_frame) continue;
            fails = NextFailCount(fails);
            if (GiveUp(fails, 8)) break;
            retry_frame = frame + BackoffFrames(fails);
        }
        assert(fails == 8 && frame > 400 && frame < 700);
    }
    // The count does not wrap back to short waits.
    static_assert(NextFailCount(0) == 1 && NextFailCount(0xfffffffeU) == 0xffffffffU);
    static_assert(NextFailCount(0xffffffffU) == 0xffffffffU);
    std::puts("upload retry: capped waits, no limit by default, old limit on request PASS");
    return 0;
}
