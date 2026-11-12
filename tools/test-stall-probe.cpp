// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for src/stall_probe.h (EDEN_UP_STALL_PROBE).
//   c++ -std=c++20 -O1 -fsanitize=address,undefined -Isrc tools/test-stall-probe.cpp -o /tmp/t && /tmp/t
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "stall_probe.h"

namespace SP = StallProbe;
static int failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

static bool Has(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

// Off: the hooks record nothing and read no clock.
static void Off() {
    SP::enabled.store(false);
    SP::SlotWaited(1);
    SP::SlotTaken();
    SP::SlotReleased();
    SP::BlockWaited(1);
    CHECK(SP::Get().slot_wait.count[SP::Fault].load() == 0);
    CHECK(SP::Get().slot_hold.count[SP::Fault].load() == 0);
    CHECK(SP::Get().block_wait.count[SP::Fault].load() == 0);
    CHECK(SP::held_depth == 0);
}

// Records go to the caller's role; only 50 ms and over reach the window; the window empties when
// taken; totals stay.
static void Windows() {
    SP::enabled.store(true);
    auto& s = SP::Get();
    SP::Record(s.slot_wait, SP::Fault, 10 * SP::Ms);   // under the window floor
    SP::Record(s.slot_wait, SP::Fault, 60 * SP::Ms);
    SP::Record(s.slot_wait, SP::Fault, 900 * SP::Ms);
    SP::Record(s.slot_hold, SP::Swapper, 950 * SP::Ms);
    SP::Record(s.slot_hold, 9, 70 * SP::Ms); // an unknown role counts as a fault
    char line[512];
    const int n = SP::StallLine(line, sizeof(line), s.start_ns.load() + 12'340'000'000ull, 960 * SP::Ms);
    const std::string text(line, n);
    std::printf("  %s", text.c_str());
    CHECK(Has(text, "EDEN_STALL t=12.34s late=960ms;"));
    CHECK(Has(text, "slot_wait fault=2/900ms"));
    CHECK(Has(text, "slot_hold fault=1/70ms swapper=1/950ms"));
    CHECK(Has(text, "block_wait none"));
    CHECK(text.back() == '\n');
    // Taken: the next line has nothing.
    const std::string again(line, SP::StallLine(line, sizeof(line), s.start_ns.load(), 100 * SP::Ms));
    CHECK(Has(again, "slot_wait none") && Has(again, "slot_hold none"));
    // Totals keep everything, the 10 ms wait too.
    CHECK(s.slot_wait.count[SP::Fault].load() == 3);
    CHECK(s.slot_wait.over_100ms[SP::Fault].load() == 1);
    CHECK(s.slot_wait.longest_ns[SP::Fault].load() == 900 * SP::Ms);
    const std::string totals(line, SP::TotalsLine(line, sizeof(line)));
    std::printf("  %s", totals.c_str());
    CHECK(Has(totals, "EDEN_SLOT_LOCK"));
    CHECK(Has(totals, "slot_wait fault=3/970ms/900ms/>100ms:1"));
    CHECK(Has(totals, "slot_hold fault=1/70ms/70ms/>100ms:0 swapper=1/950ms/950ms/>100ms:1"));
}

// A small buffer truncates and stays terminated.
static void Truncation() {
    SP::Record(SP::Get().block_wait, SP::Scanner, 500 * SP::Ms);
    char small[24];
    std::memset(small, 'x', sizeof(small));
    const int n = SP::StallLine(small, sizeof(small), SP::Get().start_ns.load(), 200 * SP::Ms);
    CHECK(n >= 0 && n < static_cast<int>(sizeof(small)));
    CHECK(std::memchr(small, '\0', sizeof(small)) != nullptr);
    char tiny[8];
    CHECK(SP::TotalsLine(tiny, sizeof(tiny)) < static_cast<int>(sizeof(tiny)));
    CHECK(std::memchr(tiny, '\0', sizeof(tiny)) != nullptr);
}

// Nested holds count once, from the outermost take to the outermost release; a release with
// nothing held does nothing.
static void Nesting() {
    auto& hold = SP::Get().slot_hold;
    const auto before = hold.count[SP::Keeper].load();
    std::thread([&] {
        SP::role = SP::Keeper;
        SP::SlotReleased(); // nothing held
        SP::SlotTaken();
        SP::SlotTaken();
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        SP::SlotReleased();
        CHECK(SP::held_depth == 1);
        SP::SlotReleased();
        CHECK(SP::held_depth == 0);
    }).join();
    CHECK(hold.count[SP::Keeper].load() == before + 1);
    CHECK(hold.longest_ns[SP::Keeper].load() >= 60 * SP::Ms);
}

// The loop: a 10 ms sleep that took 970 ms is a stall of 960 ms; one that took 105 ms is not.
// The extra 50 ms sleep after a stall comes before the line.
static void Loop() {
    std::vector<std::uint64_t> sleeps;
    std::vector<std::string> lines;
    std::uint64_t clock = SP::Get().start_ns.load() + 1'000'000'000ull;
    const std::uint64_t durations[] = {10 * SP::Ms, 105 * SP::Ms, 970 * SP::Ms, 50 * SP::Ms, 10 * SP::Ms};
    std::size_t step = 0;
    int rounds = 0;
    const auto stalls_before = SP::Get().stalls.load();
    SP::ProbeLoop([&] { return rounds++ >= 3; },
                  [&](std::uint64_t ns) {
                      sleeps.push_back(ns);
                      clock += step < std::size(durations) ? durations[step++] : ns;
                  },
                  [&] { return clock; }, [&](const char* line) { lines.emplace_back(line); });
    CHECK(SP::Get().stalls.load() == stalls_before + 1);
    CHECK(lines.size() == 1);
    if (!lines.empty()) CHECK(Has(lines[0], "late=960ms"));
    CHECK(sleeps.size() == 4 && sleeps[3] == SP::WindowFloorNs);
    CHECK(SP::Get().longest_stall_ns.load() >= 960 * SP::Ms);
}

// The real case: a holder keeps a slot 300 ms while a fault spins for it, and the probe thread
// misses its wake because this test pauses it (a stand-in for the console's stall). The stall line
// names both: the fault's wait and the swapper's hold.
static void Threads() {
    std::atomic<bool> locked{false}, stop{false}, release{false};
    std::vector<std::string> lines;
    std::mutex lines_mutex;
    std::atomic<bool> pause_probe{false};
    std::thread probe([&] {
        SP::ProbeLoop([&] { return stop.load(); },
                      [&](std::uint64_t ns) {
                          std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
                          while (pause_probe.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                      },
                      SP::NowNs,
                      [&](const char* line) {
                          std::lock_guard lock{lines_mutex};
                          lines.emplace_back(line);
                      });
    });
    std::thread holder([&] {
        SP::role = SP::Swapper;
        bool expected = false;
        while (!locked.compare_exchange_weak(expected, true)) expected = false;
        SP::SlotTaken();
        while (!release.load()) std::this_thread::yield();
        SP::SlotReleased();
        locked.store(false);
    });
    while (!locked.load()) std::this_thread::yield();
    std::thread fault([&] {
        SP::role = SP::Fault;
        std::uint64_t since = 0;
        bool waited = false;
        bool expected = false;
        while (!locked.compare_exchange_weak(expected, true)) {
            expected = false;
            if (!waited) since = SP::NowNs();
            waited = true;
            std::this_thread::yield();
        }
        if (waited) SP::SlotWaited(since);
        SP::SlotTaken();
        SP::SlotReleased();
        locked.store(false);
    });
    pause_probe.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    release.store(true);
    holder.join();
    fault.join();
    pause_probe.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    stop.store(true);
    probe.join();
    std::lock_guard lock{lines_mutex};
    CHECK(!lines.empty());
    bool named = false;
    for (const auto& line : lines) {
        std::printf("  %s", line.c_str());
        named |= Has(line, "slot_wait fault=1/") && Has(line, "slot_hold swapper=1/");
    }
    CHECK(named);
}

int main() {
    SP::Get().start_ns.store(SP::NowNs());
    Off();
    Windows();
    Truncation();
    Nesting();
    Loop();
    Threads();
    // The switch: exact "1" only.
    setenv("EDEN_UP_STALL_PROBE", "1", 1);
    CHECK(SP::ReadSwitch());
    setenv("EDEN_UP_STALL_PROBE", "10", 1);
    CHECK(!SP::ReadSwitch());
    unsetenv("EDEN_UP_STALL_PROBE");
    CHECK(!SP::ReadSwitch());
    std::printf("stall probe: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
