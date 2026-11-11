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

static void Shapes() {
    // Tags: the owner survives with a shape packed above it; pointers and null are not tags.
    for (std::size_t i = 0; i < OwnerCount; ++i) {
        for (const std::uint32_t shape : {0u, 1u, 255u, 256u, 1024u, 0xffffu}) {
            Owner owner{};
            void* const tag = TagOf(static_cast<Owner>(i), shape);
            CHECK(OwnerOfTag(tag, &owner) && owner == static_cast<Owner>(i));
            CHECK(ShapeOfTag(tag) == shape);
        }
    }
    Owner owner{};
    CHECK(!OwnerOfTag(nullptr, &owner));
    CHECK(!OwnerOfTag(reinterpret_cast<void*>(std::uintptr_t{0x7f12345678}), &owner)); // a real pointer
    CHECK(ShapeOfTag(reinterpret_cast<void*>(std::uintptr_t{0x7f12345678})) == 0);
    CHECK(!OwnerOfTag(reinterpret_cast<void*>(std::uintptr_t{OwnerCount + 1}), &owner)); // past the last owner
    CHECK(!OwnerOfTag(reinterpret_cast<void*>(std::uintptr_t{0x100}), &owner));        // a shape, no owner

    ShapeTable table;
    const Shape shadow{12288, 12288, 1, 1, 126, 1, Owner::RtDepth};
    const Shape depth{1920, 1080, 1, 1, 130, 1, Owner::RtDepth};
    const Shape colour{1920, 1080, 1, 1, 37, 1, Owner::RtColor};
    const std::uint32_t shadow_id = table.Add(shadow, 288u << 20);
    CHECK(shadow_id == 1 && table.Add(shadow, 288u << 20) == shadow_id); // same shape, same id
    const std::uint32_t depth_id = table.Add(depth, 8u << 20);
    const std::uint32_t colour_id = table.Add(colour, 8u << 20);
    CHECK(depth_id == 2 && colour_id == 3); // same size and extent, another owner: its own row
    for (int i = 0; i < 99; ++i) CHECK(table.Add(depth, 8u << 20) == depth_id);

    ShapeTable::Row rows[6];
    CHECK(table.Top(Owner::RtDepth, rows, 6) == 2);
    CHECK(rows[0].shape == depth && rows[0].count == 100 && rows[0].bytes == (std::uint64_t{800} << 20));
    CHECK(rows[1].shape == shadow && rows[1].count == 2 && rows[1].bytes == (std::uint64_t{576} << 20));
    CHECK(table.Top(Owner::RtDepth, rows, 1) == 1 && rows[0].shape == depth); // only the largest fits
    CHECK(table.Top(Owner::RtColor, rows, 6) == 1 && rows[0].shape == colour);
    CHECK(table.Top(Owner::TexBlock, rows, 6) == 0);
    CHECK(table.Top(Owner::RtDepth, rows, 0) == 0);

    // Frees: counted down, never below zero, id 0 and unknown ids ignored, emptied rows not listed.
    table.Free(shadow_id, 288u << 20);
    table.Free(0, 1u << 30);
    table.Free(999, 1u << 30);
    CHECK(table.Top(Owner::RtDepth, rows, 6) == 2 && rows[1].count == 1 && rows[1].bytes == (std::uint64_t{288} << 20));
    table.Free(shadow_id, 288u << 20);
    table.Free(shadow_id, 288u << 20); // one free too many
    CHECK(table.Top(Owner::RtDepth, rows, 6) == 1 && rows[0].shape == depth);
    CHECK(table.Add(shadow, 288u << 20) == shadow_id); // an emptied shape keeps its id

    // More than six: the six largest, in order, whatever order they came in.
    ShapeTable many;
    for (std::uint32_t i = 1; i <= 20; ++i) {
        const std::uint32_t size = (i * 7) % 20 + 1; // a permutation of 1..20
        CHECK(many.Add(Shape{size, size, 1, 1, 37, 1, Owner::RtColor}, std::uint64_t{size} << 20) == i);
    }
    CHECK(many.Top(Owner::RtColor, rows, 6) == 6);
    for (std::uint32_t i = 0; i < 6; ++i) CHECK(rows[i].shape.width == 20 - i);

    // Full: the 1025th shape is not counted and gets id 0; known shapes still count.
    ShapeTable full;
    for (std::uint32_t i = 0; i < ShapeTable::Capacity; ++i) {
        CHECK(full.Add(Shape{i + 1, 1, 1, 1, 37, 1, Owner::RtColor}, 4096) == i + 1);
    }
    CHECK(full.Add(Shape{5000, 1, 1, 1, 37, 1, Owner::RtColor}, 4096) == 0);
    CHECK(full.Add(Shape{7, 1, 1, 1, 37, 1, Owner::RtColor}, 4096) == 7);
    CHECK(ShapeOfTag(TagOf(Owner::RtColor, ShapeTable::Capacity)) == ShapeTable::Capacity);
}

int main() {
    Shapes();
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
