// Host tests for video_core/ahead_guard.h (the real header, stubbed logging/types).
// Run once per configuration: the guard reads its switches once per process.
//   EDEN_UP_AHEAD_GUARD=3 EDEN_UP_AHEAD_GUARD_SLOTS=2 ./t slots2
//   EDEN_UP_AHEAD_GUARD=2 ./t slots1
//   EDEN_UP_AHEAD_GUARD=2 EDEN_UP_AHEAD_GUARD_SLOTS=2 EDEN_UP_AHEAD_GUARD_COPY_PAGES=64 ./t pool
//   EDEN_UP_AHEAD_GUARD=2 EDEN_UP_AHEAD_GUARD_SLOTS=<1|2> ./t stress
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "video_core/ahead_guard.h"

namespace G = VideoCommon::AheadGuard;

static int failures = 0;
#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
            ++failures;                                                                        \
        }                                                                                      \
    } while (0)

constexpr u64 Base = 0x100; // first device page the tests use
constexpr u32 Pages = 256;

struct Memory {
    std::vector<std::atomic<u32>> words = std::vector<std::atomic<u32>>(Pages * 1024);
    std::vector<std::atomic<int>> cached = std::vector<std::atomic<int>>(Pages);
    std::atomic<int> outside{};

    static u64 Addr(u32 page) { return (Base + page) << 12; }
    std::atomic<int>* Count(u64 page) {
        return page >= Base && page < Base + Pages ? &cached[page - Base] : nullptr;
    }
    void ReadBlockUnsafe(u64 addr, void* out, size_t size) {
        auto* bytes = static_cast<u8*>(out);
        for (size_t i = 0; i < size; i += 4) {
            const u64 at = addr + i;
            const u64 page = at >> 12;
            u32 value = 0;
            if (page >= Base && page < Base + Pages) {
                value = words[(page - Base) * 1024 + (at & 4095) / 4].load(std::memory_order_relaxed);
            }
            std::memcpy(bytes + i, &value, (std::min<size_t>)(4, size - i));
        }
    }
    void UpdatePagesCachedCount(u64 addr, size_t size, int delta) {
        for (u64 page = addr >> 12; page < (addr + size + 4095) >> 12; ++page) {
            if (auto* c = Count(page)) {
                c->fetch_add(delta);
            } else {
                outside.fetch_add(delta);
            }
        }
    }
    void UpdatePagesCachedBatch(const std::vector<std::pair<u64, size_t>>& runs, int delta) {
        for (const auto& [addr, size] : runs) {
            UpdatePagesCachedCount(addr, size, delta);
        }
    }
    // A guest thread's write of word 0 of `page`, through the guard as memory.cpp does.
    // Returns false when the write would have to wait (pool full).
    bool GuestWrite(u32 page, u32 value) {
        const u64 addr = Addr(page);
        if (G::Active()) {
            if (G::Guarded(addr, 4)) {
                if (!G::CopyBeforeWrite(addr, 4, *this)) {
                    return false;
                }
            } else if (G::AllWrites()) {
                G::CopyOtherBeforeWrite(addr, 4, *this);
            }
        }
        words[page * 1024].store(value, std::memory_order_relaxed);
        return true;
    }
    u32 GpuRead(u32 page) {
        const u64 addr = Addr(page);
        G::NoteRead(addr, 4);
        u32 value = 0;
        G::Read(addr, reinterpret_cast<u8*>(&value), 4, true,
                [this](u64 a, u8* out, u64 size) { ReadBlockUnsafe(a, out, size); });
        return value;
    }
    bool Balanced() {
        for (auto& c : cached) {
            if (c.load() != 0) {
                return false;
            }
        }
        return outside.load() == 0;
    }
};

struct Syncpoints {
    std::mutex m;
    std::map<u32, u32> reached;
    std::vector<std::tuple<u32, u32, std::function<void()>>> actions;
    bool IsReadyGuest(u32 id, u32 value) {
        std::scoped_lock lk{m};
        return reached[id] >= value;
    }
    void RegisterGuestAction(u32 id, u32 value, std::function<void()> action) {
        {
            std::scoped_lock lk{m};
            if (reached[id] < value) {
                actions.emplace_back(id, value, std::move(action));
                return;
            }
        }
        action();
    }
    void Signal(u32 id, u32 value) {
        std::vector<std::function<void()>> run;
        {
            std::scoped_lock lk{m};
            reached[id] = (std::max)(reached[id], value);
            for (auto it = actions.begin(); it != actions.end();) {
                if (std::get<0>(*it) == id && std::get<1>(*it) <= value) {
                    run.push_back(std::move(std::get<2>(*it)));
                    it = actions.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& f : run) {
            f();
        }
    }
};

static size_t Redirtied() {
    size_t n = 0;
    G::TakeRedirty([&](u64, size_t) { ++n; });
    return n;
}

static void Slots2() {
    CHECK(G::SlotCount() == 2 && G::Mode() == 3);
    Memory mem;
    Syncpoints sp;
    auto& s = G::Get();
    const u32 P = 1, Q = 2;

    // Frame 0 reads P and Q; present 0 returns before its fence: slot A guards both.
    CHECK(mem.GpuRead(P) == 0);
    CHECK(mem.GpuRead(Q) == 0);
    G::AtPresent(0, 1, 1, false, sp, mem);
    CHECK(G::Active());
    CHECK(mem.cached[P].load() == 1 && mem.cached[Q].load() == 1);
    // Frame 1's write to P: copied for A.
    CHECK(mem.GuestWrite(P, 1));
    CHECK(mem.cached[P].load() == 0);
    CHECK(mem.GpuRead(P) == 0); // the back still records frame 0
    // Present 1 with frame 0 still in flight: slot B guards P and Q too.
    G::AtPresent(1, 1, 2, false, sp, mem);
    CHECK(s.stats.second_arms == 1 && s.stats.overlaps == 0);
    CHECK(mem.cached[P].load() == 1);
    CHECK(mem.cached[Q].load() == 2); // guarded by both
    // HoldWrites/HoldTag: the newest guarded frame is 1, the back records 0.
    CHECK(G::HoldWrites() && G::HoldTag() == 1 && G::StillHeld(1));
    // Frame 2's writes: P copied for B (as frame 1 left it), Q once for both.
    CHECK(mem.GuestWrite(P, 2));
    CHECK(mem.GuestWrite(Q, 2));
    CHECK(s.stats.shared_copies == 1);
    CHECK(mem.cached[P].load() == 0 && mem.cached[Q].load() == 0);
    CHECK(s.copies_used == 3);
    // The back records frame 0: A's copies.
    CHECK(mem.GpuRead(P) == 0);
    CHECK(mem.GpuRead(Q) == 0);
    // A present with both slots up finds no slot.
    G::AtPresent(2, 1, 3, false, sp, mem);
    CHECK(s.stats.overlaps == 1);
    // The back passes frame 0: B's copies, A's reads go back as guest writes.
    G::BackPassed(0);
    CHECK(Redirtied() == 3); // A served P twice and Q once
    CHECK(mem.GpuRead(P) == 1);
    CHECK(mem.GpuRead(Q) == 0);
    CHECK(G::StillHeld(1));
    // Frame 0's fence: A lifted, its own copy goes, the shared one stays for B.
    sp.Signal(1, 1);
    CHECK(s.copies_used == 2);
    CHECK(mem.GpuRead(P) == 1);
    CHECK(mem.GpuRead(Q) == 0);
    // The back passes frame 1: memory (nothing guards frame 2), writes held for 1 go.
    G::BackPassed(1);
    CHECK(!G::StillHeld(1));
    CHECK(!G::HoldWrites()); // B is still armed, but the back is past it
    CHECK(Redirtied() == 4); // B served P and Q twice each
    CHECK(mem.GpuRead(P) == 2);
    CHECK(mem.GpuRead(Q) == 2);
    sp.Signal(1, 3);
    CHECK(!G::Active());
    CHECK(s.copies_used == 0);
    CHECK(mem.Balanced());

    // A newer frame's copy serves the older frame when the older one has none.
    const u32 R = 3, S = 4;
    G::AtPresent(9, 2, 1, false, sp, mem); // nothing read since the last frame end: not armed
    CHECK(!G::Active());
    CHECK(mem.GpuRead(S) == 0);
    G::AtPresent(10, 2, 2, false, sp, mem); // frame 10 guards S
    CHECK(mem.GpuRead(R) == 0);
    G::AtPresent(11, 2, 3, false, sp, mem); // frame 11 guards S and R
    CHECK(G::Active() && s.stats.second_arms >= 2);
    CHECK(mem.GuestWrite(R, 7)); // copied for frame 11 only
    const u64 newer_before = s.stats.newer_copy_reads;
    CHECK(mem.GpuRead(R) == 0); // the back records frame 10: frame 11's copy, not memory
    CHECK(s.stats.newer_copy_reads == newer_before + 1);
    sp.Signal(2, 3);
    CHECK(!G::Active() && s.copies_used == 0 && mem.Balanced());
    CHECK(mem.GpuRead(R) == 7);

    // A writer that gives up waiting lifts the slots it waited for.
    G::AtPresent(20, 3, 1, false, sp, mem);
    CHECK(G::Active());
    int stops = 0;
    G::HoldWriter(Memory::Addr(R), 4, 0, 1, mem, [&] { return ++stops > 2 ? 1 : 0; });
    CHECK(!G::Active() && mem.Balanced());
}

static void Slots1() {
    CHECK(G::SlotCount() == 1 && G::Mode() == 2);
    Memory mem;
    Syncpoints sp;
    auto& s = G::Get();
    const u32 P = 1;
    CHECK(mem.GpuRead(P) == 0);
    G::AtPresent(0, 1, 1, false, sp, mem);
    CHECK(G::Active() && mem.cached[P].load() == 1);
    CHECK(mem.GuestWrite(P, 1));
    CHECK(mem.GpuRead(P) == 0);
    // One slot: the next present finds it up and guards nothing (the flicker).
    G::AtPresent(1, 1, 2, false, sp, mem);
    CHECK(s.stats.overlaps == 1 && s.stats.second_arms == 0);
    CHECK(mem.GuestWrite(P, 2)); // not guarded for frame 1: no copy
    CHECK(s.copies_used == 1);
    CHECK(mem.GpuRead(P) == 0); // the copy, wherever the back is (as before)
    G::BackPassed(0);           // no effect with one slot
    CHECK(mem.GpuRead(P) == 0);
    sp.Signal(1, 1);
    CHECK(!G::Active() && s.copies_used == 0 && mem.Balanced());
    CHECK(mem.GpuRead(P) == 2);
    CHECK(Redirtied() == 3);
}

static void Pool() {
    CHECK(G::CopyCapacity() == 64 && G::SlotCount() == 2);
    Memory mem;
    Syncpoints sp;
    for (u32 p = 0; p < 65; ++p) {
        mem.GpuRead(p);
    }
    G::AtPresent(0, 1, 1, false, sp, mem);
    for (u32 p = 0; p < 64; ++p) {
        CHECK(mem.GuestWrite(p, 9));
    }
    CHECK(!mem.GuestWrite(64, 9)); // the pool is full: the writer would wait
    CHECK(mem.cached[64].load() == 1);
    sp.Signal(1, 1);
    CHECK(G::Get().copies_used == 0 && mem.Balanced());
    CHECK(mem.GuestWrite(64, 9));
}

// The game writes every page with the number of the frame it prepares, presents it, and waits
// for the frame two back. The back reads every page for each frame in turn and expects that
// frame's number; a fence thread signals each frame a little after the back has passed it.
static void Stress() {
    Memory mem;
    Syncpoints sp;
    constexpr u32 Frames = 400, Used = 64;
    std::mutex m;
    std::condition_variable cv;
    std::deque<u32> to_back, to_fence;
    bool done = false;
    std::atomic<u32> wrong{}, checked{};
    std::thread back([&] {
        std::mt19937 rng{1};
        while (true) {
            u32 f;
            {
                std::unique_lock lk{m};
                cv.wait(lk, [&] { return done || !to_back.empty(); });
                if (to_back.empty()) {
                    return;
                }
                f = to_back.front();
                to_back.pop_front();
            }
            for (u32 p = 0; p < Used; ++p) {
                const u32 v = mem.GpuRead(p);
                if (f >= 4) { // the first frames have no guess yet
                    checked.fetch_add(1);
                    if (v != f) {
                        wrong.fetch_add(1);
                        static std::atomic<int> shown{};
                        if (shown.fetch_add(1) < 12) {
                            auto& st = G::Get();
                            std::scoped_lock lk2{st.mutex};
                            const u64 page = Base + p;
                            std::printf("  wrong: frame %u page %u saw %u; back_rec %llu; slots", f, p, v,
                                        (unsigned long long)st.back_recording.load());
                            for (u32 sl = 0; sl < G::SlotCount(); ++sl) {
                                std::printf(" [%s fr %lld g%d c%d]", st.slots[sl].armed ? "armed" : "free",
                                            (long long)st.slots[sl].frame,
                                            (int)G::Detail::GuardedBit(st, sl, page),
                                            (int)G::Detail::CopiedBit(st, sl, page));
                            }
                            std::printf("\n");
                        }
                    }
                }
                if (rng() % 8 == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds{20});
                }
            }
            G::BackPassed(f);
            {
                std::scoped_lock lk{m};
                to_fence.push_back(f);
            }
            cv.notify_all();
        }
    });
    std::thread fence([&] {
        std::mt19937 rng{2};
        while (true) {
            u32 f;
            {
                std::unique_lock lk{m};
                cv.wait(lk, [&] { return done || !to_fence.empty(); });
                if (to_fence.empty()) {
                    return;
                }
                f = to_fence.front();
                to_fence.pop_front();
            }
            std::this_thread::sleep_for(std::chrono::microseconds{rng() % 400});
            sp.Signal(1, f + 1);
        }
    });
    u32 stalls = 0;
    for (u32 f = 0; f < Frames; ++f) {
        for (u32 p = 0; p < Used; ++p) {
            while (!mem.GuestWrite(p, f)) {
                ++stalls;
                std::this_thread::yield();
            }
        }
        // Frame f's lists are submitted before its present; the back may start on them now.
        {
            std::scoped_lock lk{m};
            to_back.push_back(f); // BackPassed(f) runs on the back after frame f, as RunInOrder
        }
        cv.notify_all();
        // Present f waits for frame f-2 (EDEN_UP_AHEAD_TWO; frame n's fence is value n + 1),
        // then the guard arms (nx2_gsv: the waits, then AtPresent): frames f-1 and f may both
        // be in flight while frame f+1 is written.
        if (f >= 2) {
            while (!sp.IsReadyGuest(1, f - 1)) {
                std::this_thread::sleep_for(std::chrono::microseconds{50});
            }
        }
        const bool previous_reached = f == 0 || sp.IsReadyGuest(1, f);
        G::AtPresent(f, 1, f + 1, previous_reached, sp, mem);
    }
    {
        std::scoped_lock lk{m};
        done = true;
    }
    cv.notify_all();
    back.join();
    fence.join();
    sp.Signal(1, Frames + 1);
    auto& s = G::Get();
    std::printf("stress slots=%u: %u of %u reads wrong, %u writer stalls, %u second arms, %u "
                "overlaps (last window)\n",
                G::SlotCount(), wrong.load(), checked.load(), stalls, s.stats.second_arms,
                s.stats.overlaps);
    CHECK(!G::Active() && s.copies_used == 0 && mem.Balanced());
    if (G::SlotCount() == 2) {
        CHECK(wrong.load() == 0);
    } else {
        // The control: one slot leaves the second frame unguarded, which this test must see.
        CHECK(wrong.load() != 0);
    }
}

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "";
    if (which == "slots2") {
        Slots2();
    } else if (which == "slots1") {
        Slots1();
    } else if (which == "pool") {
        Pool();
    } else if (which == "stress") {
        Stress();
    } else {
        std::printf("usage: %s slots2|slots1|pool|stress\n", argv[0]);
        return 2;
    }
    std::printf("%s: %s (%d failures)\n", which.c_str(), failures ? "FAILED" : "passed",
                failures);
    return failures ? 1 : 0;
}
