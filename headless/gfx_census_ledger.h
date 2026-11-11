// SPDX-License-Identifier: GPL-3.0-or-later
// Graphics memory census (EDEN_GFX_CENSUS=1): who owns the Vulkan memory the emulator allocates.
//
// This header is the part with no Vulkan or VMA in it, so tools/test-gfx-census.cpp can test it on
// any host: which owner an allocation is charged to, and the per-owner ledger (live bytes and
// count, peak, totals, failures). headless/gfx_census.cpp feeds it from the VMA entry points.
// Flag values are Vulkan's and VMA's own (checked against the headers there with static_asserts).
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <cstddef>
#include <cstdint>

namespace Eden::GfxCensus {

enum class Owner : std::uint8_t {
    RtColor,       // image with COLOR_ATTACHMENT: render targets
    RtDepth,       // image with DEPTH_STENCIL_ATTACHMENT: depth and shadow maps
    TexBlock,      // sampled image in a block-compressed format (BC, ASTC, ETC/EAC)
    TexStorage,    // image with STORAGE and no attachment use (decode and compute scratch)
    TexOther,      // any other image
    BufDevice,     // buffer the CPU does not map: the buffer cache, GPU-only scratch
    BufStream,     // mapped buffer the GPU reads directly: stream ring, uniforms, inline uploads
    BufStaging,    // mapped transfer-only buffer: upload staging
    BufDownload,   // buffer read back by the CPU: download staging
    BufDescriptor, // descriptor buffer
    MemImage,      // memory bound to an image created elsewhere (vmaAllocateMemoryForImage)
    MemBuffer,     // memory bound to a buffer created elsewhere (vmaAllocateMemoryForBuffer)
    MemRaw,        // vmaAllocateMemory: sparse binds and other raw commits
    Count,
};
inline constexpr std::size_t OwnerCount = static_cast<std::size_t>(Owner::Count);

inline constexpr std::array<const char*, OwnerCount> OwnerNames{
    "rt_color", "rt_depth",   "tex_block",  "tex_storage", "tex_other", "buf_device", "buf_stream",
    "buf_staging", "buf_download", "buf_descriptor", "mem_image", "mem_buffer", "mem_raw"};

// Vulkan usage bits used below (VkImageUsageFlagBits, VkBufferUsageFlagBits).
inline constexpr std::uint32_t ImageStorage = 0x8, ImageColor = 0x10, ImageDepth = 0x20;
inline constexpr std::uint32_t BufferTransferSrc = 0x1, BufferTransferDst = 0x2;
inline constexpr std::uint32_t BufferSamplerDescriptor = 0x200000, BufferResourceDescriptor = 0x400000;

// Whether a VkFormat is block compressed (BC 131-146, ETC2/EAC 147-156, ASTC LDR 157-184,
// ASTC HDR 1000066000-1000066013).
[[nodiscard]] constexpr bool IsBlockFormat(std::uint32_t format) noexcept {
    return (format >= 131 && format <= 184) || (format >= 1000066000u && format <= 1000066013u);
}

[[nodiscard]] constexpr Owner ClassifyImage(std::uint32_t usage, std::uint32_t format) noexcept {
    if (usage & ImageColor) return Owner::RtColor;
    if (usage & ImageDepth) return Owner::RtDepth;
    if (IsBlockFormat(format)) return Owner::TexBlock;
    if (usage & ImageStorage) return Owner::TexStorage;
    return Owner::TexOther;
}

// host_access: the caller asked for memory the CPU maps (VMA's MAPPED or HOST_ACCESS flags, a
// CPU usage, or HOST_VISIBLE required). readback: it asked for memory the CPU reads (random host
// access, GPU_TO_CPU, or HOST_CACHED required). The caller's request, not the memory type VMA
// picked: on a console every type may be host visible.
[[nodiscard]] constexpr Owner ClassifyBuffer(std::uint32_t usage, bool host_access, bool readback) noexcept {
    if (usage & (BufferResourceDescriptor | BufferSamplerDescriptor)) return Owner::BufDescriptor;
    if (readback) return Owner::BufDownload;
    if (!host_access) return Owner::BufDevice;
    if ((usage & ~(BufferTransferSrc | BufferTransferDst)) == 0) return Owner::BufStaging;
    return Owner::BufStream;
}

// The tag an allocation carries in its VMA user data: owner + 1 in the low byte, so null means
// untagged, and for an image its shape's id (ShapeTable, 0: none) in the 16 bits above.
[[nodiscard]] inline void* TagOf(Owner owner, std::uint32_t shape = 0) noexcept {
    return reinterpret_cast<void*>((static_cast<std::uintptr_t>(owner) + 1) |
                                   (static_cast<std::uintptr_t>(shape & 0xffff) << 8));
}
[[nodiscard]] inline bool OwnerOfTag(const void* tag, Owner* owner) noexcept {
    const auto value = reinterpret_cast<std::uintptr_t>(tag);
    const auto low = value & 0xff;
    if ((value >> 24) != 0 || low == 0 || low > OwnerCount) return false; // not a tag: a pointer, or null
    *owner = static_cast<Owner>(low - 1);
    return true;
}
[[nodiscard]] inline std::uint32_t ShapeOfTag(const void* tag) noexcept {
    const auto value = reinterpret_cast<std::uintptr_t>(tag);
    return (value >> 24) != 0 ? 0 : static_cast<std::uint32_t>((value >> 8) & 0xffff);
}

// What an image is, for the census's "which images hold an owner's memory" lines.
struct Shape {
    std::uint32_t width, height, layers, levels, format, samples;
    Owner owner;
    [[nodiscard]] constexpr bool operator==(const Shape&) const noexcept = default;
};

// Live images by shape. Images are made and destroyed a few thousand times a session, so one
// mutex and a linear search of at most Capacity shapes are enough. An id is the shape's index + 1
// and is never reused; with the table full a new shape gets id 0 and is not counted here (the
// ledger still counts its bytes under its owner).
class ShapeTable {
public:
    static constexpr std::size_t Capacity = 1024;
    struct Row {
        Shape shape;
        std::uint64_t count, bytes;
    };

    /// Counts one more image of this shape and returns its id (0: table full, not counted).
    [[nodiscard]] std::uint32_t Add(const Shape& shape, std::uint64_t bytes) noexcept {
        std::scoped_lock lock{mutex};
        std::size_t index = 0;
        while (index < used && !(rows[index].shape == shape)) ++index;
        if (index == used) {
            if (used == Capacity) return 0;
            rows[used++] = Row{shape, 0, 0};
        }
        ++rows[index].count;
        rows[index].bytes += bytes;
        return static_cast<std::uint32_t>(index + 1);
    }

    /// One image of the shape with this id is gone. Never below zero; id 0 or unknown: nothing.
    void Free(std::uint32_t id, std::uint64_t bytes) noexcept {
        std::scoped_lock lock{mutex};
        if (id == 0 || id > used) return;
        Row& row = rows[id - 1];
        if (row.count != 0) --row.count;
        row.bytes -= (std::min)(row.bytes, bytes);
    }

    /// The owner's live shapes holding the most bytes, largest first. Returns how many were written.
    [[nodiscard]] std::size_t Top(Owner owner, Row* out, std::size_t max) const noexcept {
        std::scoped_lock lock{mutex};
        std::size_t written = 0;
        for (std::size_t index = 0; index < used; ++index) {
            const Row& row = rows[index];
            if (row.shape.owner != owner || row.count == 0) continue;
            // Insertion into the sorted prefix; the smallest falls off the end when full.
            std::size_t at = written;
            while (at > 0 && out[at - 1].bytes < row.bytes) --at;
            if (at >= max) continue;
            const std::size_t last = written < max ? written : max - 1;
            for (std::size_t move = last; move > at; --move) out[move] = out[move - 1];
            out[at] = row;
            if (written < max) ++written;
        }
        return written;
    }

private:
    mutable std::mutex mutex;
    std::array<Row, Capacity> rows{};
    std::size_t used = 0;
};

struct OwnerSnapshot {
    std::uint64_t live_bytes, live_count, peak_bytes, made, freed, failures;
};

// Per-owner counters. All relaxed atomics: a snapshot read while others allocate is a few
// allocations off, never torn per counter. A free of an allocation the ledger never saw (made
// before the census started) is not given to Free: callers only free what carries a tag.
class Ledger {
public:
    void Add(Owner owner, std::uint64_t bytes) noexcept {
        Slot& s = slots[Index(owner)];
        const std::uint64_t now = s.live_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        s.live_count.fetch_add(1, std::memory_order_relaxed);
        s.made.fetch_add(1, std::memory_order_relaxed);
        std::uint64_t peak = s.peak_bytes.load(std::memory_order_relaxed);
        while (now > peak && !s.peak_bytes.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
        }
    }
    void Free(Owner owner, std::uint64_t bytes) noexcept {
        Slot& s = slots[Index(owner)];
        // Never below zero, even if a size read back differs from the one added.
        std::uint64_t live = s.live_bytes.load(std::memory_order_relaxed);
        while (!s.live_bytes.compare_exchange_weak(live, live > bytes ? live - bytes : 0,
                                                   std::memory_order_relaxed)) {
        }
        std::uint64_t count = s.live_count.load(std::memory_order_relaxed);
        while (count != 0 &&
               !s.live_count.compare_exchange_weak(count, count - 1, std::memory_order_relaxed)) {
        }
        s.freed.fetch_add(1, std::memory_order_relaxed);
    }
    void Fail(Owner owner) noexcept {
        slots[Index(owner)].failures.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] OwnerSnapshot Read(Owner owner) const noexcept {
        const Slot& s = slots[Index(owner)];
        return {s.live_bytes.load(std::memory_order_relaxed), s.live_count.load(std::memory_order_relaxed),
                s.peak_bytes.load(std::memory_order_relaxed), s.made.load(std::memory_order_relaxed),
                s.freed.load(std::memory_order_relaxed), s.failures.load(std::memory_order_relaxed)};
    }
    [[nodiscard]] std::uint64_t TotalFailures() const noexcept {
        std::uint64_t total = 0;
        for (const Slot& s : slots) total += s.failures.load(std::memory_order_relaxed);
        return total;
    }

private:
    struct Slot {
        std::atomic<std::uint64_t> live_bytes{0}, live_count{0}, peak_bytes{0}, made{0}, freed{0}, failures{0};
    };
    static constexpr std::size_t Index(Owner owner) noexcept {
        const auto index = static_cast<std::size_t>(owner);
        return index < OwnerCount ? index : OwnerCount - 1;
    }
    std::array<Slot, OwnerCount> slots{};
};

} // namespace Eden::GfxCensus
