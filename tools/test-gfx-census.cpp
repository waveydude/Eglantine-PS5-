// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for headless/gfx_census_ledger.h (the graphics memory census's owner rules and ledger).
//   c++ -std=c++20 -O1 -pthread -Iheadless tools/test-gfx-census.cpp -o /tmp/test-gfx-census && /tmp/test-gfx-census
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "gfx_census_ledger.h"

using namespace Eden::GfxCensus;

static int failures = 0;
#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);        \
            ++failures;                                                                     \
        }                                                                                   \
    } while (0)

static void Classification() {
    // A render target that is also sampled is a render target; depth wins over texture formats.
    CHECK(ClassifyImage(0x4 | ImageColor, 37) == Owner::RtColor);
    CHECK(ClassifyImage(0x4 | ImageDepth, 124) == Owner::RtDepth);
    CHECK(ClassifyImage(0x4 | 0x2, 131) == Owner::TexBlock);           // BC1 sampled, upload target
    CHECK(ClassifyImage(0x4 | 0x2, 184) == Owner::TexBlock);           // ASTC 12x12 sRGB
    CHECK(ClassifyImage(0x4, 1000066005u) == Owner::TexBlock);         // ASTC HDR
    CHECK(ClassifyImage(0x4 | ImageStorage, 37) == Owner::TexStorage); // decode scratch
    CHECK(ClassifyImage(0x4, 37) == Owner::TexOther);
    CHECK(!IsBlockFormat(130) && !IsBlockFormat(185) && IsBlockFormat(147));
    // Buffers: descriptor beats everything, readback beats host access, transfer-only mapped is staging.
    CHECK(ClassifyBuffer(BufferResourceDescriptor | 0x20, true, false) == Owner::BufDescriptor);
    CHECK(ClassifyBuffer(BufferTransferDst, true, true) == Owner::BufDownload);
    CHECK(ClassifyBuffer(BufferTransferSrc, true, false) == Owner::BufStaging);
    CHECK(ClassifyBuffer(BufferTransferSrc | BufferTransferDst, true, false) == Owner::BufStaging);
    CHECK(ClassifyBuffer(0x10 | 0x80, true, false) == Owner::BufStream); // mapped uniform + vertex
    CHECK(ClassifyBuffer(0x20 | 0x80 | BufferTransferDst, false, false) == Owner::BufDevice);
}

static void Tags() {
    Owner owner;
    CHECK(!OwnerOfTag(nullptr, &owner));
    for (std::size_t i = 0; i < OwnerCount; ++i) {
        CHECK(OwnerOfTag(TagOf(static_cast<Owner>(i)), &owner) && owner == static_cast<Owner>(i));
    }
    // Someone else's user data (any pointer past the tag range) is not ours.
    int unrelated = 0;
    CHECK(!OwnerOfTag(&unrelated, &owner));
    CHECK(!OwnerOfTag(reinterpret_cast<void*>(OwnerCount + 1), &owner));
}

static void Accounting() {
    Ledger ledger;
    ledger.Add(Owner::RtDepth, 297u << 20);
    ledger.Add(Owner::RtDepth, 15u << 20);
    OwnerSnapshot s = ledger.Read(Owner::RtDepth);
    CHECK(s.live_bytes == 312u << 20 && s.live_count == 2 && s.peak_bytes == 312u << 20 && s.made == 2);
    ledger.Free(Owner::RtDepth, 297u << 20);
    s = ledger.Read(Owner::RtDepth);
    CHECK(s.live_bytes == 15u << 20 && s.live_count == 1 && s.peak_bytes == 312u << 20 && s.freed == 1);
    // A free larger than what is live (a size read back differently) clamps at zero, never wraps.
    ledger.Free(Owner::RtDepth, 100u << 20);
    ledger.Free(Owner::RtDepth, 1);
    s = ledger.Read(Owner::RtDepth);
    CHECK(s.live_bytes == 0 && s.live_count == 0);
    // Owners are independent; failures are counted per owner and in total.
    ledger.Fail(Owner::BufStaging);
    ledger.Fail(Owner::BufStaging);
    ledger.Fail(Owner::MemRaw);
    CHECK(ledger.Read(Owner::BufStaging).failures == 2 && ledger.TotalFailures() == 3);
    CHECK(ledger.Read(Owner::TexBlock).made == 0);
}

static void Threads() {
    Ledger ledger;
    constexpr int ThreadCount = 8, Rounds = 20000;
    std::vector<std::thread> threads;
    for (int t = 0; t < ThreadCount; ++t) {
        threads.emplace_back([&ledger, t] {
            const Owner owner = static_cast<Owner>(t % 3);
            for (int i = 0; i < Rounds; ++i) {
                ledger.Add(owner, 4096);
                if (i % 2 == 0) ledger.Free(owner, 4096);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    std::uint64_t live = 0, count = 0, made = 0, freed = 0;
    for (int o = 0; o < 3; ++o) {
        const OwnerSnapshot s = ledger.Read(static_cast<Owner>(o));
        live += s.live_bytes;
        count += s.live_count;
        made += s.made;
        freed += s.freed;
        CHECK(s.peak_bytes >= s.live_bytes);
    }
    CHECK(made == std::uint64_t{ThreadCount} * Rounds);
    CHECK(freed == std::uint64_t{ThreadCount} * Rounds / 2);
    CHECK(count == made - freed);
    CHECK(live == count * 4096);
}

int main() {
    Classification();
    Tags();
    Accounting();
    Threads();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("gfx census ledger: all checks passed");
    return 0;
}
