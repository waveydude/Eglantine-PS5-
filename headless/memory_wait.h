#pragma once
// Waits for memory where a refusal would otherwise end the session.
//
// mihawk-99/PS5_Hookshot (a native port of the same game) measured on 10 Nov 2026 that leaving Hyrule
// Castle Town, memory comes back within seconds while one area's pages and textures go and the next
// area's arrive: its game pages gave up after 3 s and survived at 10 s (9100911), and an operator new
// that threw std::bad_alloc on the submit thread was gone once it waited (aaf80e3). Here three places
// end the session on the first refusal or after a fixed 2 s:
//   - operator new (headless/main.cpp's new handler): EDEN_UP_NEW_WAIT_MS, unset or 0: no wait;
//   - the JIT code cache's next 2 MiB (headless/jit-allocator.h): EDEN_JIT_COMMIT_WAIT_MS, unset 2000;
//   - a guest write that needs a block (src/guest_memory_service.inc, WaitForBlock):
//     EDEN_GUEST_WRITE_WAIT_MS, unset 2000.
// All three are read once the game's own env file is in. A wait holds the waiting thread, never memory.
#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace Eden::MemoryWait {

// A wait in ms from text: unset or not starting with a digit gives `fallback`, a number is held
// within [low, high]. Trailing characters after the digits are ignored, as the other switches do.
inline unsigned ParseMs(const char* text, unsigned fallback, unsigned low, unsigned high) {
    if (text == nullptr || text[0] < '0' || text[0] > '9') return fallback;
    const unsigned long long value = std::strtoull(text, nullptr, 10);
    if (value < low) return low;
    if (value > high) return high;
    return static_cast<unsigned>(value);
}

inline unsigned ReadMs(const char* name, unsigned fallback, unsigned low, unsigned high) {
    return ParseMs(std::getenv(name), fallback, low, high);
}

// operator new calls its handler after each failed try and tries again when the handler returns; a
// try that succeeds calls nothing. So the handler cannot see a wait end. It tells one wait from the
// next by the request: a call for the same number of bytes within SameRequestGapMs of the last one
// is the same wait going on, and so is any call within EpisodeGapMs.
//
// The first version went by EpisodeGapMs alone, and hung the game (11 Nov 2026, 17:52 UTC, 16 minutes
// in): with the pool empty one try of a 32 MiB request took about 2 s inside the heap (its own
// EDEN_UP_HEAP_WAIT_MS), every call came later than 250 ms after the last, each counted as a new
// wait, and the 5 s limit was never reached. The picture stood still for good.
constexpr std::uint64_t EpisodeGapMs = 250;
// A try can take as long as the heap's own wait, which is at most 60 s (heap_pages.cpp).
constexpr std::uint64_t SameRequestGapMs = 60'000;
// After a wait runs out, this thread's next failures give up at once for this long: memory is not
// coming back, and a caller that copes with the refusal and asks again every frame would otherwise
// stand still for the whole limit each time.
constexpr std::uint64_t CooldownMs = 30'000;
// The heap's empty spaces go back at a wait's start and every TrimEveryMs after.
constexpr std::uint64_t TrimEveryMs = 500;

struct Episode {
    std::uint64_t start_ms = 0;
    std::uint64_t last_ms = 0;
    std::uint64_t last_trim_ms = 0;
    std::uint64_t bytes = 0;
    std::uint64_t cooldown_until_ms = 0;
    bool active = false;
};

enum class Step {
    Start,  // a new wait: log it, trim, wait a step
    Wait,   // the same wait goes on: wait a step
    GiveUp, // no wait (limit 0), the limit is spent, or a wait just ran out here: throw as before
};

// `bytes`: what the failed try asked for.
inline Step Next(Episode& episode, std::uint64_t now_ms, unsigned limit_ms, std::uint64_t bytes = 0) {
    if (limit_ms == 0) {
        episode.active = false;
        return Step::GiveUp;
    }
    if (now_ms < episode.cooldown_until_ms) {
        return Step::GiveUp;
    }
    const std::uint64_t gap = now_ms - episode.last_ms;
    const bool same_wait = episode.active &&
                           (gap <= EpisodeGapMs || (bytes == episode.bytes && gap <= SameRequestGapMs));
    Step step = Step::Wait;
    if (!same_wait) {
        episode.start_ms = now_ms;
        episode.last_trim_ms = now_ms;
        episode.bytes = bytes;
        episode.active = true;
        step = Step::Start;
    }
    episode.last_ms = now_ms;
    if (now_ms - episode.start_ms >= limit_ms) {
        episode.active = false;
        episode.cooldown_until_ms = now_ms + CooldownMs;
        return Step::GiveUp;
    }
    return step;
}

// Whether this step trims the heap: at the start and every TrimEveryMs of the same wait.
inline bool TrimNow(Episode& episode, Step step, std::uint64_t now_ms) {
    if (step == Step::Start) return true;
    if (step == Step::Wait && now_ms - episode.last_trim_ms >= TrimEveryMs) {
        episode.last_trim_ms = now_ms;
        return true;
    }
    return false;
}

// A request no wait can serve gives up at once: the first hybrid build asked operator new for
// 2 TiB out of a damaged table (crash-20261110-214902), and ten seconds of waiting would only have
// delayed that report. The largest honest single request seen is a 341 MiB upload buffer.
constexpr std::uint64_t LargestWaitedRequest = std::uint64_t{1} << 30;
inline bool WorthWaitingFor(std::uint64_t bytes) {
    return bytes <= LargestWaitedRequest;
}

// The handler's limit for one refused request. No wait of its own for a request no wait can serve,
// nor on a thread that builds pipelines: the heap already waits for those threads
// (EDEN_UP_HEAP_WAIT_MS, EDEN_UP_HEAP_WAIT_GPU_MS), and a build that finds no memory fails its
// pipeline and the picture goes on, so five more seconds there are five seconds of a still picture.
inline unsigned HandlerLimitMs(std::uint64_t bytes, unsigned configured_ms, bool builds_pipelines) {
    return WorthWaitingFor(bytes) && !builds_pipelines ? configured_ms : 0;
}

// Number of 20 ms steps in a wait of `ms` (at least one).
inline unsigned Steps(unsigned ms) {
    return ms < 20 ? 1u : ms / 20u;
}

// EDEN_UP_NEW_WAIT_MS, set by main once the game's env file is in (0 until then: no wait).
inline std::atomic<unsigned> new_wait_ms{0};

} // namespace Eden::MemoryWait
