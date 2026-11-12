// SPDX-License-Identifier: GPL-3.0-or-later
// ---- EDEN_UP_STALL_PROBE=1: what the guest's memory service was doing during a stall ----
//
// W-10 and W-11 in Jabu-Jabu's Belly: stretches of frames of about 960 ms (sometimes two in a row),
// one every 2 s or so, for tens of seconds. In those windows the six guest cores sat idle more than
// in smooth play (3-4.7 s of every 5 s against 1.4-2.3 s), the GPU front's single reads of guest
// memory took 400-920 ms (DMA snapshot miss, list writers, macro DMA read) against a few ms in smooth
// play, the game decoded no assets, and a host thread that sleeps 100 ms at a time came up to 6 s
// late over 15 s. Everything that touches guest memory stopped, not one thread.
//
// The service locks a 2 MiB slot of guest memory to change it (LockSlot, a spin with sched_yield),
// and a write that needs a block may wait for one while it holds the lock (WaitForBlock, up to
// EDEN_GUEST_WRITE_WAIT_MS). Nothing measured how long either took, and WaitForBlock's own line goes
// to the kernel log only. With the switch on:
//   - every slot lock records how long the thread waited for it and, once released, how long it was
//     held, by who took it (a guest or GPU thread taking a fault, the scanner, the swapper, the
//     reserve keeper);
//   - every WaitForBlock records its length;
//   - a probe thread sleeps 10 ms at a time; a wake 100 ms or more late is a stall, and it writes one
//     EDEN_STALL line: seconds since the service started, how late, and the waits and holds of 50 ms
//     or more that ended since the last EDEN_STALL line (their count and the longest, by role);
//   - each EDEN_CRAM report adds an EDEN_SLOT_LOCK line of the totals.
// Off: one relaxed load per slot lock and unlock, no clock read, no thread.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <time.h>

namespace StallProbe {

enum Role : std::uint8_t { Fault = 0, Scanner = 1, Swapper = 2, Keeper = 3, RoleCount = 4 };
inline constexpr const char* RoleNames[RoleCount]{"fault", "scanner", "swapper", "keeper"};

inline constexpr std::uint64_t Ms = 1'000'000;
/// A wait or hold this long or longer is counted in the windows (and the probe's lines).
inline constexpr std::uint64_t WindowFloorNs = 50 * Ms;
/// A probe wake this late is a stall.
inline constexpr std::uint64_t StallNs = 100 * Ms;
inline constexpr std::uint64_t ProbeSleepNs = 10 * Ms;

inline std::uint64_t NowNs() noexcept {
    timespec time{};
    clock_gettime(CLOCK_MONOTONIC, &time);
    return static_cast<std::uint64_t>(time.tv_sec) * 1'000'000'000ull + static_cast<std::uint64_t>(time.tv_nsec);
}

inline std::atomic<bool> enabled{false};
inline bool On() noexcept {
    return enabled.load(std::memory_order_relaxed);
}
/// Reads EDEN_UP_STALL_PROBE (exact "1") once; the service calls it at start.
inline bool ReadSwitch() noexcept {
    const char* value = std::getenv("EDEN_UP_STALL_PROBE");
    const bool on = value != nullptr && value[0] == '1' && value[1] == '\0';
    enabled.store(on, std::memory_order_relaxed);
    return on;
}

inline thread_local std::uint8_t role = Fault;
inline thread_local std::uint64_t held_since = 0;
inline thread_local unsigned held_depth = 0;

/// One kind of event (slot waits, slot holds, block waits) by role: totals since the start, and a
/// window the probe takes and clears.
struct Kind {
    std::array<std::atomic<std::uint64_t>, RoleCount> count{}, total_ns{}, longest_ns{};
    std::array<std::atomic<std::uint64_t>, RoleCount> over_100ms{};
    std::array<std::atomic<std::uint64_t>, RoleCount> window_count{}, window_longest_ns{};
};

struct Stats {
    Kind slot_wait, slot_hold, block_wait;
    std::atomic<std::uint64_t> start_ns{0};
    std::atomic<std::uint64_t> stalls{0}, stall_ns{0}, longest_stall_ns{0};
};

inline Stats& Get() noexcept {
    static Stats stats;
    return stats;
}

inline void Max(std::atomic<std::uint64_t>& to, std::uint64_t value) noexcept {
    std::uint64_t seen = to.load(std::memory_order_relaxed);
    while (value > seen && !to.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
}

inline void Record(Kind& kind, std::uint8_t who, std::uint64_t ns) noexcept {
    if (who >= RoleCount) who = Fault;
    kind.count[who].fetch_add(1, std::memory_order_relaxed);
    kind.total_ns[who].fetch_add(ns, std::memory_order_relaxed);
    Max(kind.longest_ns[who], ns);
    if (ns >= 100 * Ms) kind.over_100ms[who].fetch_add(1, std::memory_order_relaxed);
    if (ns >= WindowFloorNs) {
        kind.window_count[who].fetch_add(1, std::memory_order_relaxed);
        Max(kind.window_longest_ns[who], ns);
    }
}

// ---- hooks (only called with the switch on; each checks On() itself for the callers' sake) ----

/// A slot lock the calling thread spun for, from `since` (NowNs() at the first failed try) to now.
inline void SlotWaited(std::uint64_t since) noexcept {
    if (!On() || since == 0) return;
    Record(Get().slot_wait, role, NowNs() - since);
}
/// The calling thread took a slot lock. Holds nest (a thread may hold two): the outermost counts.
inline void SlotTaken() noexcept {
    if (!On()) return;
    if (held_depth++ == 0) held_since = NowNs();
}
/// The calling thread let a slot lock go.
inline void SlotReleased() noexcept {
    if (!On() || held_depth == 0) return;
    if (--held_depth == 0) Record(Get().slot_hold, role, NowNs() - held_since);
}
/// A WaitForBlock that began at `since`.
inline void BlockWaited(std::uint64_t since) noexcept {
    if (!On()) return;
    Record(Get().block_wait, role, NowNs() - since);
}

// ---- lines ----

/// "name: count/longest_ms by role" for the roles with any events in the window, which is taken
/// and cleared. Returns what was written (never more than `size` - 1).
inline int TakeWindow(char* out, std::size_t size, const char* name, Kind& kind) noexcept {
    int length = std::snprintf(out, size, " %s", name);
    bool any = false;
    for (unsigned who = 0; who < RoleCount; ++who) {
        const std::uint64_t count = kind.window_count[who].exchange(0, std::memory_order_relaxed);
        const std::uint64_t longest = kind.window_longest_ns[who].exchange(0, std::memory_order_relaxed);
        if (count == 0) continue;
        any = true;
        if (length < 0 || static_cast<std::size_t>(length) >= size) break;
        length += std::snprintf(out + length, size - length, " %s=%llu/%llums", RoleNames[who],
                                static_cast<unsigned long long>(count),
                                static_cast<unsigned long long>(longest / Ms));
    }
    if (!any && length >= 0 && static_cast<std::size_t>(length) < size)
        length += std::snprintf(out + length, size - length, " none");
    if (length < 0) return 0;
    return static_cast<std::size_t>(length) >= size ? static_cast<int>(size - 1) : length;
}

/// The EDEN_STALL line for one stall: seconds since start, how late the probe woke, and what ended
/// since the last stall line (waits and holds of 50 ms or more).
inline int StallLine(char* out, std::size_t size, std::uint64_t now, std::uint64_t late) noexcept {
    Stats& stats = Get();
    const std::uint64_t start = stats.start_ns.load(std::memory_order_relaxed);
    int length = std::snprintf(out, size, "EDEN_STALL t=%.2fs late=%llums;",
                               static_cast<double>(now - start) / 1e9,
                               static_cast<unsigned long long>(late / Ms));
    if (length < 0 || static_cast<std::size_t>(length) >= size) return length < 0 ? 0 : static_cast<int>(size - 1);
    length += TakeWindow(out + length, size - length, "slot_wait", stats.slot_wait);
    length += TakeWindow(out + length, size - length, "slot_hold", stats.slot_hold);
    length += TakeWindow(out + length, size - length, "block_wait", stats.block_wait);
    if (static_cast<std::size_t>(length) + 1 < size) {
        out[length++] = '\n';
        out[length] = '\0';
    }
    return length;
}

/// The totals line beside each EDEN_CRAM report.
inline int TotalsLine(char* out, std::size_t size) noexcept {
    Stats& stats = Get();
    int length = std::snprintf(out, size, "EDEN_SLOT_LOCK stalls=%llu (%llu ms, longest %llu ms);",
                               static_cast<unsigned long long>(stats.stalls.load(std::memory_order_relaxed)),
                               static_cast<unsigned long long>(stats.stall_ns.load(std::memory_order_relaxed) / Ms),
                               static_cast<unsigned long long>(stats.longest_stall_ns.load(std::memory_order_relaxed) / Ms));
    const struct {
        const char* name;
        Kind* kind;
    } kinds[]{{"slot_wait", &stats.slot_wait}, {"slot_hold", &stats.slot_hold}, {"block_wait", &stats.block_wait}};
    for (const auto& [name, kind] : kinds) {
        if (length < 0 || static_cast<std::size_t>(length) >= size) break;
        length += std::snprintf(out + length, size - length, " %s", name);
        for (unsigned who = 0; who < RoleCount; ++who) {
            const std::uint64_t count = kind->count[who].load(std::memory_order_relaxed);
            if (count == 0) continue;
            if (length < 0 || static_cast<std::size_t>(length) >= size) break;
            length += std::snprintf(out + length, size - length, " %s=%llu/%llums/%llums/>100ms:%llu", RoleNames[who],
                                    static_cast<unsigned long long>(count),
                                    static_cast<unsigned long long>(kind->total_ns[who].load(std::memory_order_relaxed) / Ms),
                                    static_cast<unsigned long long>(kind->longest_ns[who].load(std::memory_order_relaxed) / Ms),
                                    static_cast<unsigned long long>(kind->over_100ms[who].load(std::memory_order_relaxed)));
        }
    }
    if (length < 0) return 0;
    if (static_cast<std::size_t>(length) + 1 < size) {
        out[length++] = '\n';
        out[length] = '\0';
    }
    return static_cast<std::size_t>(length) >= size ? static_cast<int>(size - 1) : length;
}

/// The probe: sleeps ProbeSleepNs at a time until `stop` is set, and calls `report(line)` for each
/// stall. After a late wake it sleeps 50 ms more before it writes, so the waits that ended the
/// stall have been recorded. `sleep(ns)` is nanosleep on the console and a fake in the tests.
template <class Stop, class Sleep, class Now, class ReportLine>
void ProbeLoop(Stop&& stop, Sleep&& sleep, Now&& now, ReportLine&& report) {
    Stats& stats = Get();
    char line[512];
    while (!stop()) {
        const std::uint64_t before = now();
        sleep(ProbeSleepNs);
        const std::uint64_t after = now();
        const std::uint64_t slept = after - before;
        if (slept < ProbeSleepNs + StallNs) continue;
        const std::uint64_t late = slept - ProbeSleepNs;
        stats.stalls.fetch_add(1, std::memory_order_relaxed);
        stats.stall_ns.fetch_add(late, std::memory_order_relaxed);
        Max(stats.longest_stall_ns, late);
        sleep(WindowFloorNs);
        if (StallLine(line, sizeof(line), after, late) > 0) report(line);
    }
}

} // namespace StallProbe
