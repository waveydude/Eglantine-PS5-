// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for headless/memory_wait.h (how long operator new, the JIT code cache and guest writes
// wait for memory before the session ends).
//   c++ -std=c++20 -O1 -Iheadless tools/test-memory-wait.cpp -o /tmp/test-memory-wait && /tmp/test-memory-wait
#include <cassert>
#include <cstdio>

#include "memory_wait.h"

using namespace Eden::MemoryWait;

static int failures = 0;
#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
            ++failures;                                                                     \
        }                                                                                   \
    } while (0)

static void Parse() {
    // Unset and junk keep the old behaviour.
    CHECK(ParseMs(nullptr, 2000, 20, 60000) == 2000);
    CHECK(ParseMs("", 2000, 20, 60000) == 2000);
    CHECK(ParseMs("abc", 2000, 20, 60000) == 2000);
    CHECK(ParseMs("-5", 2000, 20, 60000) == 2000);
    // Numbers are held within the bounds.
    CHECK(ParseMs("10000", 2000, 20, 60000) == 10000);
    CHECK(ParseMs("0", 2000, 20, 60000) == 20);
    CHECK(ParseMs("1", 0, 0, 60000) == 1);
    CHECK(ParseMs("0", 0, 0, 60000) == 0);
    CHECK(ParseMs("999999999999999999999", 0, 0, 60000) == 60000);
    CHECK(ParseMs("60001", 0, 0, 60000) == 60000);
    // A stray carriage return from an env file written on Windows.
    CHECK(ParseMs("5000\r", 0, 0, 60000) == 5000);
    CHECK(Steps(2000) == 100);
    CHECK(Steps(10000) == 500);
    CHECK(Steps(20) == 1);
    CHECK(Steps(5) == 1);
}

static void Off() {
    Episode e;
    // Limit 0 (unset): the first failure throws, as before the port.
    CHECK(Next(e, 1000, 0) == Step::GiveUp);
    CHECK(!e.active);
}

static void WaitsThenGivesUp() {
    Episode e;
    const unsigned limit = 5000;
    std::uint64_t now = 100000;
    CHECK(Next(e, now, limit) == Step::Start);
    CHECK(TrimNow(e, Step::Start, now));
    unsigned steps = 1;
    Step step;
    // Calls every 21 ms (20 ms sleep plus the retried allocation) stay in one wait.
    while ((step = Next(e, now += 21, limit)) == Step::Wait) ++steps;
    CHECK(step == Step::GiveUp);
    CHECK(now - 100000 >= limit);
    CHECK(now - 100000 < limit + 21);
    CHECK(steps == 5000 / 21 + 1);
    // The next failure right after giving up throws at once (CooldownAfterGivingUp has the rest).
    CHECK(Next(e, now + 1, limit) == Step::GiveUp);
}

static void ServedWaitEndsBetweenFailures() {
    Episode e;
    const unsigned limit = 5000;
    CHECK(Next(e, 0, limit, 100) == Step::Start);
    CHECK(Next(e, 20, limit, 100) == Step::Wait);
    // The retry succeeded; the next failure, for something else, comes 4.9 s later. Counting it in
    // the old wait would leave it 100 ms: it must start its own.
    CHECK(Next(e, 4920, limit, 200) == Step::Start);
    CHECK(Next(e, 4940, limit, 200) == Step::Wait);
    // Within the gap a call joins whatever it asks for; past it, another size starts again.
    CHECK(Next(e, 4940 + EpisodeGapMs, limit, 300) == Step::Wait);
    CHECK(Next(e, 4940 + 2 * EpisodeGapMs + 1, limit, 400) == Step::Start);
}

// The hang of 11 Nov 2026 17:52 UTC: with the pool empty each try of one 32 MiB request took about
// 2 s inside the heap, so the handler was called every ~2060 ms. Going by the 250 ms gap alone every
// call was a new wait and the limit was never reached (the old rule: 19 "start" lines in 35 s, no
// "gave up"). It must end within the limit plus one try.
static void SlowTriesStillReachTheLimit() {
    Episode e;
    const unsigned limit = 5000;
    const std::uint64_t bytes = 32435200;
    std::uint64_t now = 927200;
    CHECK(Next(e, now, limit, bytes) == Step::Start);
    unsigned starts = 1, calls = 1;
    Step step;
    while ((step = Next(e, now += 2060, limit, bytes)) != Step::GiveUp && calls < 100) {
        starts += step == Step::Start ? 1 : 0;
        ++calls;
    }
    CHECK(step == Step::GiveUp);
    CHECK(starts == 1);
    CHECK(now - 927200 >= limit && now - 927200 < limit + 2060);
    // Tries as slow as the heap's own longest wait (60 s) are still the same request: one more
    // call ends it. One ms slower than that and it counts as new.
    Episode slow;
    CHECK(Next(slow, 0, limit, bytes) == Step::Start);
    CHECK(Next(slow, SameRequestGapMs, limit, bytes) == Step::GiveUp);
    Episode slower;
    CHECK(Next(slower, 0, limit, bytes) == Step::Start);
    CHECK(Next(slower, SameRequestGapMs + 1, limit, bytes) == Step::Start);
}

// After a wait runs out the caller often copes and asks again at once (six times in that run, every
// frame in the worst case). Those throw at once for CooldownMs instead of standing still for the
// limit each time; after it, waits are back.
static void CooldownAfterGivingUp() {
    Episode e;
    const unsigned limit = 5000;
    std::uint64_t now = 1000;
    CHECK(Next(e, now, limit, 64) == Step::Start);
    while (Next(e, now += 20, limit, 64) == Step::Wait) {}
    const std::uint64_t gave_up = now;
    CHECK(e.cooldown_until_ms == gave_up + CooldownMs);
    // Same size, another size, straight away and just before the end: all refused, no new wait.
    CHECK(Next(e, gave_up + 1, limit, 64) == Step::GiveUp);
    CHECK(Next(e, gave_up + 16, limit, 4096) == Step::GiveUp);
    CHECK(Next(e, gave_up + CooldownMs - 1, limit, 64) == Step::GiveUp);
    CHECK(!e.active);
    // A refusal during the cooldown does not push its end back.
    CHECK(e.cooldown_until_ms == gave_up + CooldownMs);
    // Once it is over the thread waits again, the full limit.
    CHECK(Next(e, gave_up + CooldownMs, limit, 64) == Step::Start);
    CHECK(Next(e, gave_up + CooldownMs + 4980, limit, 64) == Step::Wait);
    CHECK(Next(e, gave_up + CooldownMs + 5000, limit, 64) == Step::GiveUp);
    // Switched off (limit 0) stays off whatever the state.
    CHECK(Next(e, gave_up + 2 * CooldownMs + 6000, 0, 64) == Step::GiveUp);
}

static void Trim() {
    Episode e;
    std::uint64_t now = 0;
    Step step = Next(e, now, 10000);
    CHECK(TrimNow(e, step, now));
    unsigned trims = 1;
    while ((step = Next(e, now += 20, 10000)) != Step::GiveUp)
        trims += TrimNow(e, step, now) ? 1 : 0;
    // One at the start and one every 500 ms of a 10 s wait.
    CHECK(trims == 20);
    CHECK(!TrimNow(e, Step::GiveUp, now));
}

static void HopelessRequests() {
    // Honest requests wait: the 32 MiB write-back scratch, the 341 MiB upload buffer, 1 GiB itself.
    CHECK(WorthWaitingFor(0));
    CHECK(WorthWaitingFor(std::uint64_t{32} << 20));
    CHECK(WorthWaitingFor(std::uint64_t{341} << 20));
    CHECK(WorthWaitingFor(LargestWaitedRequest));
    // The 2 TiB request out of a damaged table does not: it must fail at once, as before.
    CHECK(!WorthWaitingFor(LargestWaitedRequest + 1));
    CHECK(!WorthWaitingFor(std::uint64_t{1} << 41));
    // And with the limit forced to 0 for it, the handler's step is GiveUp on the first call, even
    // in the middle of another wait on the same thread.
    Episode e;
    CHECK(Next(e, 0, 5000) == Step::Start);
    CHECK(Next(e, 20, WorthWaitingFor(std::uint64_t{1} << 41) ? 5000u : 0u) == Step::GiveUp);
    CHECK(!e.active);
}

// The handler adds no wait of its own on a thread that builds pipelines (Wave Ocarina 05, Kakariko:
// two builds waited the whole 5 s for 16 MiB and 11.4 MiB, got nothing, and the picture stood still
// 3.98 s and 6.80 s), nor for a request no wait can serve; every other thread keeps its wait.
static void HandlerLimit() {
    using namespace Eden::MemoryWait;
    assert(HandlerLimitMs(16777216, 5000, true) == 0);
    assert(HandlerLimitMs(11960320, 5000, true) == 0);
    assert(HandlerLimitMs(16777216, 5000, false) == 5000);  // GPUBack, CPU cores: as before
    assert(HandlerLimitMs(32435200, 5000, false) == 5000);  // the Lost Woods request still waits
    assert(HandlerLimitMs(std::uint64_t{2} << 40, 5000, false) == 0);
    assert(HandlerLimitMs(std::uint64_t{2} << 40, 5000, true) == 0);
    assert(HandlerLimitMs(4096, 0, false) == 0);  // the switch off
    // A limit of 0 gives up at once and starts no cooldown, so the thread's next request is free
    // to wait again if it is asked on another kind of thread's behalf later.
    Episode episode;
    assert(Next(episode, 1000, HandlerLimitMs(16777216, 5000, true), 16777216) == Step::GiveUp);
    assert(!episode.active && episode.cooldown_until_ms == 0);
}

// A wait for a guest block trims the heap at its start and every 500 ms, not at every 20 ms try.
static void GuestWaitTrims() {
    using namespace Eden::MemoryWait;
    static_assert(TrimAtTry(0) && !TrimAtTry(1) && !TrimAtTry(24) && TrimAtTry(25) && TrimAtTry(50));
    unsigned trims = 0;
    const unsigned limit = Steps(10000); // the console's EDEN_GUEST_WRITE_WAIT_MS
    for (unsigned tries = 0; tries < limit; ++tries) trims += TrimAtTry(tries) ? 1 : 0;
    assert(limit == 500 && trims == 20);
    // The shortest wait still trims once, before its only try.
    assert(Steps(20) == 1 && TrimAtTry(0));
}

int main() {
    GuestWaitTrims();
    HandlerLimit();
    Parse();
    HopelessRequests();
    Off();
    WaitsThenGivesUp();
    ServedWaitEndsBetweenFailures();
    SlowTriesStillReachTheLimit();
    CooldownAfterGivingUp();
    Trim();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("memory wait tests passed");
    return 0;
}
