// SPDX-License-Identifier: GPL-3.0-or-later
// Graphics memory census, EDEN_GFX_CENSUS=1 in env.txt (off unless set).
//
// Every VMA allocation the emulator makes is charged to an owner (render target, depth, texture,
// buffer cache, staging, ...; gfx_census_ledger.h) through the VMA entry points, which the PS5 link
// wraps (tools/link-headless-native.sh, --wrap=vma*). The owner rides in the allocation's VMA user
// data, which nothing else in this tree uses, so a free finds it again without a table of its own.
// A thread writes one EDEN_GFX_CENSUS line every 5 s, and within a second of a refused allocation:
// per owner the live MiB, live count and peak MiB, then VMA's own totals (blocks held, bytes in
// allocations, unused space inside blocks, the largest hole). Each refusal also gets one
// EDEN_GFX_CENSUS_FAIL line with what was asked (at most 64). tools/eden-mem-report.py reads them
// with RADV_PS5_MEMORY, the pool ledger and the heap lines.
//
// Cost when off: one load of a static bool per VMA call. When on: a few atomics per allocation and
// free, one vmaGetAllocationInfo per free, and vmaCalculateStatistics every 5 s on its own thread.
//
// The allocator is created VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED: VMA takes no lock of its
// own and its callers keep to one thread at a time. The census thread is not one of them, so with
// the census on every wrapped call (all of VMA's allocations and frees) and the thread's
// vmaCalculateStatistics hold `vma_mutex`: the walk over VMA's blocks never meets one being
// changed (Vampinity's review of 0912, issue 21). Off, the mutex is never touched.
// Not counted: allocations made before the census saw its first one (none: it starts with the
// first allocation), and memory the driver takes for itself (RADV_PS5_MEMORY shows that).
#ifdef PS5_NATIVE
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "video_core/vulkan_common/vma.h"
#include "gfx_census_ledger.h"

namespace Eden::GfxCensus {
static_assert(VK_IMAGE_USAGE_STORAGE_BIT == ImageStorage && VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT == ImageColor &&
              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT == ImageDepth);
static_assert(VK_BUFFER_USAGE_TRANSFER_SRC_BIT == BufferTransferSrc &&
              VK_BUFFER_USAGE_TRANSFER_DST_BIT == BufferTransferDst);
static_assert(VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT == BufferResourceDescriptor &&
              VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT == BufferSamplerDescriptor);
static_assert(VK_FORMAT_BC1_RGB_UNORM_BLOCK == 131 && VK_FORMAT_ASTC_12x12_SRGB_BLOCK == 184);

namespace {
Ledger ledger;
ShapeTable shapes;
std::mutex allocator_mutex; // guards `allocator` against vmaDestroyAllocator
VmaAllocator allocator{};
std::mutex vma_mutex;       // census on: one of {a wrapped VMA call, the census thread's statistics} at a time.
                            // Taken after allocator_mutex where both are held, never before it.

std::atomic<std::uint64_t> fails_logged{0};
std::mutex wake_mutex;
std::condition_variable wake;

bool Enabled() noexcept {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_GFX_CENSUS");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

// Holds vma_mutex for a wrapped call, with the census on only.
class VmaCall {
public:
    VmaCall() {
        if (Enabled()) lock = std::unique_lock{vma_mutex};
    }

private:
    std::unique_lock<std::mutex> lock;
};

bool HostAccess(const VmaAllocationCreateInfo* aci) noexcept {
    if (aci == nullptr) return false;
    constexpr VmaAllocationCreateFlags host = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                              VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                              VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    return (aci->flags & host) != 0 || aci->usage == VMA_MEMORY_USAGE_CPU_ONLY ||
           aci->usage == VMA_MEMORY_USAGE_CPU_TO_GPU || aci->usage == VMA_MEMORY_USAGE_GPU_TO_CPU ||
           aci->usage == VMA_MEMORY_USAGE_CPU_COPY || (aci->requiredFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
}

bool Readback(const VmaAllocationCreateInfo* aci) noexcept {
    if (aci == nullptr) return false;
    return (aci->flags & VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT) != 0 ||
           aci->usage == VMA_MEMORY_USAGE_GPU_TO_CPU ||
           ((aci->requiredFlags | aci->preferredFlags) & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
}

void WriteLine(bool on_refusal);

void StartThread() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::fputs("EDEN_GFX_CENSUS_LEGEND per owner: live MiB/live count/peak MiB; vma: MiB in blocks, "
                   "MiB in allocations, MiB unused inside blocks, largest hole MiB, blocks\n",
                   stderr);
        std::thread([] {
            using namespace std::chrono_literals;
            auto last = std::chrono::steady_clock::now();
            auto last_refusal = std::chrono::steady_clock::time_point{};
            std::uint64_t seen_failures = 0;
            for (;;) {
                {
                    std::unique_lock lock{wake_mutex};
                    wake.wait_for(lock, 1s);
                }
                const std::uint64_t failures = ledger.TotalFailures();
                const auto now = std::chrono::steady_clock::now();
                const bool refused = failures != seen_failures;
                const bool refusal_due = refused && (now - last_refusal >= 1s);
                if (refusal_due || now - last >= 5s) {
                    WriteLine(refusal_due);
                    last = now;
                    if (refusal_due) {
                        last_refusal = now;
                    }
                    seen_failures = failures;
                }
            }
        }).detach();
    });
}

std::chrono::steady_clock::time_point Start() {
    static const auto start = std::chrono::steady_clock::now();
    return start;
}

void WriteLine(bool on_refusal) {
    char line[2048];
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - Start()).count();
    int length = std::snprintf(line, sizeof(line), "EDEN_GFX_CENSUS t=%lld%s", static_cast<long long>(seconds),
                               on_refusal ? " refusal" : "");
    std::uint64_t live_total = 0;
    for (std::size_t i = 0; i < OwnerCount && length > 0 && length < static_cast<int>(sizeof(line)); ++i) {
        const OwnerSnapshot s = ledger.Read(static_cast<Owner>(i));
        live_total += s.live_bytes;
        if (s.made == 0 && s.failures == 0) continue;
        length += std::snprintf(line + length, sizeof(line) - static_cast<std::size_t>(length), " %s=%llu/%llu/%llu",
                                OwnerNames[i], static_cast<unsigned long long>(s.live_bytes >> 20),
                                static_cast<unsigned long long>(s.live_count),
                                static_cast<unsigned long long>(s.peak_bytes >> 20));
        if (s.failures != 0 && length > 0 && length < static_cast<int>(sizeof(line)))
            length += std::snprintf(line + length, sizeof(line) - static_cast<std::size_t>(length), "/fail%llu",
                                    static_cast<unsigned long long>(s.failures));
    }
    if (length > 0 && length < static_cast<int>(sizeof(line)))
        length += std::snprintf(line + length, sizeof(line) - static_cast<std::size_t>(length), " live_mib=%llu",
                                static_cast<unsigned long long>(live_total >> 20));
    {
        std::scoped_lock lock{allocator_mutex};
        if (allocator != nullptr && length > 0 && length < static_cast<int>(sizeof(line))) {
            VmaTotalStatistics stats{};
            {
                std::scoped_lock vma_lock{vma_mutex};
                vmaCalculateStatistics(allocator, &stats);
            }
            const VmaDetailedStatistics& total = stats.total;
            const VkDeviceSize blocks = total.statistics.blockBytes, used = total.statistics.allocationBytes;
            length += std::snprintf(line + length, sizeof(line) - static_cast<std::size_t>(length),
                                    " vma=%llu/%llu/%llu/%llu/%u",
                                    static_cast<unsigned long long>(blocks >> 20),
                                    static_cast<unsigned long long>(used >> 20),
                                    static_cast<unsigned long long>((blocks > used ? blocks - used : 0) >> 20),
                                    static_cast<unsigned long long>(total.unusedRangeSizeMax >> 20),
                                    total.statistics.blockCount);
        }
    }
    if (length > 0) {
        if (length >= static_cast<int>(sizeof(line))) length = sizeof(line) - 2;
        line[length++] = '\n';
        std::fwrite(line, 1, static_cast<std::size_t>(length), stderr);
    }
    // Which images hold the render targets' and textures' memory: the six shapes with the most
    // bytes per owner, as width x height x layers, mip levels, VkFormat, samples, count and MiB.
    // With every 5 s line, and with refusal lines at most once a second.
    static long long shapes_at = -1;
    if (on_refusal && seconds == shapes_at) return;
    shapes_at = seconds;
    for (const Owner owner : {Owner::RtDepth, Owner::RtColor, Owner::TexBlock}) {
        ShapeTable::Row rows[6];
        const std::size_t count = shapes.Top(owner, rows, 6);
        if (count == 0) continue;
        length = std::snprintf(line, sizeof(line), "EDEN_GFX_CENSUS_SHAPES t=%lld %s:", static_cast<long long>(seconds),
                               OwnerNames[static_cast<std::size_t>(owner)]);
        for (std::size_t i = 0; i < count && length > 0 && length < static_cast<int>(sizeof(line)); ++i) {
            const Shape& s = rows[i].shape;
            length += std::snprintf(line + length, sizeof(line) - static_cast<std::size_t>(length),
                                    " %ux%ux%u/L%u/f%u/s%u n=%llu %lluMiB;", s.width, s.height, s.layers, s.levels,
                                    s.format, s.samples, static_cast<unsigned long long>(rows[i].count),
                                    static_cast<unsigned long long>(rows[i].bytes >> 20));
        }
        if (length <= 0) continue;
        if (length >= static_cast<int>(sizeof(line))) length = sizeof(line) - 2;
        line[length++] = '\n';
        std::fwrite(line, 1, static_cast<std::size_t>(length), stderr);
    }
}

// Returns the tag the allocation now carries (null: left uncounted). `shape` is for images.
void* Charged(VmaAllocator vma, VmaAllocation allocation, Owner owner, const Shape* shape = nullptr) {
    if (allocation == nullptr) return nullptr;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(vma, allocation, &info);
    if (info.pUserData != nullptr) return nullptr; // someone else's data: leave it, and the allocation uncounted
    void* const tag = TagOf(owner, shape != nullptr ? shapes.Add(*shape, info.size) : 0);
    vmaSetAllocationUserData(vma, allocation, tag);
    ledger.Add(owner, info.size);
    Start();
    StartThread();
    return tag;
}

void Released(VmaAllocator vma, VmaAllocation allocation) {
    if (allocation == nullptr) return;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(vma, allocation, &info);
    Owner owner;
    if (OwnerOfTag(info.pUserData, &owner)) {
        ledger.Free(owner, info.size);
        shapes.Free(ShapeOfTag(info.pUserData), info.size);
    }
}

void Refused(Owner owner, VkResult result, const char* what, unsigned long long a, unsigned long long b,
             unsigned long long c, unsigned format) {
    ledger.Fail(owner);
    if (fails_logged.fetch_add(1, std::memory_order_relaxed) < 64)
        std::fprintf(stderr, "EDEN_GFX_CENSUS_FAIL owner=%s result=%d %s %llux%llux%llu format=%u\n",
                     OwnerNames[static_cast<std::size_t>(owner)], static_cast<int>(result), what, a, b, c, format);
    static std::atomic<std::uint64_t> last_wake_ms{0};
    const auto now_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    auto prev_ms = last_wake_ms.load(std::memory_order_relaxed);
    if (now_ms >= prev_ms + 1000 && last_wake_ms.compare_exchange_strong(prev_ms, now_ms, std::memory_order_relaxed)) {
        wake.notify_one();
    }
}
} // namespace
} // namespace Eden::GfxCensus

using namespace Eden::GfxCensus;

extern "C" {
VkResult __real_vmaCreateAllocator(const VmaAllocatorCreateInfo*, VmaAllocator*);
void __real_vmaDestroyAllocator(VmaAllocator);
VkResult __real_vmaCreateImage(VmaAllocator, const VkImageCreateInfo*, const VmaAllocationCreateInfo*, VkImage*,
                               VmaAllocation*, VmaAllocationInfo*);
VkResult __real_vmaCreateBuffer(VmaAllocator, const VkBufferCreateInfo*, const VmaAllocationCreateInfo*, VkBuffer*,
                                VmaAllocation*, VmaAllocationInfo*);
VkResult __real_vmaCreateBufferWithAlignment(VmaAllocator, const VkBufferCreateInfo*, const VmaAllocationCreateInfo*,
                                             VkDeviceSize, VkBuffer*, VmaAllocation*, VmaAllocationInfo*);
VkResult __real_vmaAllocateMemory(VmaAllocator, const VkMemoryRequirements*, const VmaAllocationCreateInfo*,
                                  VmaAllocation*, VmaAllocationInfo*);
VkResult __real_vmaAllocateMemoryForBuffer(VmaAllocator, VkBuffer, const VmaAllocationCreateInfo*, VmaAllocation*,
                                           VmaAllocationInfo*);
VkResult __real_vmaAllocateMemoryForImage(VmaAllocator, VkImage, const VmaAllocationCreateInfo*, VmaAllocation*,
                                          VmaAllocationInfo*);
void __real_vmaDestroyImage(VmaAllocator, VkImage, VmaAllocation);
void __real_vmaDestroyBuffer(VmaAllocator, VkBuffer, VmaAllocation);
void __real_vmaFreeMemory(VmaAllocator, VmaAllocation);

VkResult __wrap_vmaCreateAllocator(const VmaAllocatorCreateInfo* info, VmaAllocator* out) {
    const VkResult result = __real_vmaCreateAllocator(info, out);
    if (result == VK_SUCCESS && out != nullptr && Enabled()) {
        std::scoped_lock lock{allocator_mutex};
        if (allocator == nullptr) allocator = *out;
    }
    return result;
}

void __wrap_vmaDestroyAllocator(VmaAllocator vma) {
    if (Enabled()) {
        std::scoped_lock lock{allocator_mutex};
        if (allocator == vma) allocator = nullptr;
    }
    __real_vmaDestroyAllocator(vma);
}

VkResult __wrap_vmaCreateImage(VmaAllocator vma, const VkImageCreateInfo* ci, const VmaAllocationCreateInfo* aci,
                               VkImage* image, VmaAllocation* allocation, VmaAllocationInfo* info) {
    const VmaCall vma_call;
    const VkResult result = __real_vmaCreateImage(vma, ci, aci, image, allocation, info);
    if (!Enabled() || ci == nullptr) return result;
    const Owner owner = ClassifyImage(ci->usage, static_cast<std::uint32_t>(ci->format));
    if (result == VK_SUCCESS) {
        const Shape shape{ci->extent.width, ci->extent.height, ci->arrayLayers * ci->extent.depth, ci->mipLevels,
                          static_cast<std::uint32_t>(ci->format), static_cast<std::uint32_t>(ci->samples), owner};
        void* const tag = Charged(vma, *allocation, owner, &shape);
        if (info != nullptr && tag != nullptr) info->pUserData = tag;
    } else {
        Refused(owner, result, "image", ci->extent.width, ci->extent.height, ci->arrayLayers,
                static_cast<unsigned>(ci->format));
    }
    return result;
}

static void BufferResult(VmaAllocator vma, const VkBufferCreateInfo* ci, const VmaAllocationCreateInfo* aci,
                         VkResult result, VmaAllocation* allocation, VmaAllocationInfo* info) {
    if (!Enabled() || ci == nullptr) return;
    const Owner owner = ClassifyBuffer(static_cast<std::uint32_t>(ci->usage), HostAccess(aci), Readback(aci));
    if (result == VK_SUCCESS) {
        Charged(vma, *allocation, owner);
        if (info != nullptr) info->pUserData = TagOf(owner);
    } else {
        Refused(owner, result, "buffer KiB", ci->size >> 10, 1, 1, 0);
    }
}

VkResult __wrap_vmaCreateBuffer(VmaAllocator vma, const VkBufferCreateInfo* ci, const VmaAllocationCreateInfo* aci,
                                VkBuffer* buffer, VmaAllocation* allocation, VmaAllocationInfo* info) {
    const VmaCall vma_call;
    const VkResult result = __real_vmaCreateBuffer(vma, ci, aci, buffer, allocation, info);
    BufferResult(vma, ci, aci, result, allocation, info);
    return result;
}

VkResult __wrap_vmaCreateBufferWithAlignment(VmaAllocator vma, const VkBufferCreateInfo* ci,
                                             const VmaAllocationCreateInfo* aci, VkDeviceSize alignment,
                                             VkBuffer* buffer, VmaAllocation* allocation, VmaAllocationInfo* info) {
    const VmaCall vma_call;
    const VkResult result = __real_vmaCreateBufferWithAlignment(vma, ci, aci, alignment, buffer, allocation, info);
    BufferResult(vma, ci, aci, result, allocation, info);
    return result;
}

static void MemoryResult(VmaAllocator vma, Owner owner, VkResult result, VkDeviceSize size,
                         VmaAllocation* allocation, VmaAllocationInfo* info) {
    if (!Enabled()) return;
    if (result == VK_SUCCESS) {
        Charged(vma, *allocation, owner);
        if (info != nullptr) info->pUserData = TagOf(owner);
    } else {
        Refused(owner, result, "memory KiB", size >> 10, 1, 1, 0);
    }
}

VkResult __wrap_vmaAllocateMemory(VmaAllocator vma, const VkMemoryRequirements* reqs, const VmaAllocationCreateInfo* aci,
                                  VmaAllocation* allocation, VmaAllocationInfo* info) {
    const VmaCall vma_call;
    const VkResult result = __real_vmaAllocateMemory(vma, reqs, aci, allocation, info);
    MemoryResult(vma, Owner::MemRaw, result, reqs != nullptr ? reqs->size : 0, allocation, info);
    return result;
}

VkResult __wrap_vmaAllocateMemoryForBuffer(VmaAllocator vma, VkBuffer buffer, const VmaAllocationCreateInfo* aci,
                                           VmaAllocation* allocation, VmaAllocationInfo* info) {
    const VmaCall vma_call;
    const VkResult result = __real_vmaAllocateMemoryForBuffer(vma, buffer, aci, allocation, info);
    MemoryResult(vma, Owner::MemBuffer, result, 0, allocation, info);
    return result;
}

VkResult __wrap_vmaAllocateMemoryForImage(VmaAllocator vma, VkImage image, const VmaAllocationCreateInfo* aci,
                                          VmaAllocation* allocation, VmaAllocationInfo* info) {
    const VmaCall vma_call;
    const VkResult result = __real_vmaAllocateMemoryForImage(vma, image, aci, allocation, info);
    MemoryResult(vma, Owner::MemImage, result, 0, allocation, info);
    return result;
}

void __wrap_vmaDestroyImage(VmaAllocator vma, VkImage image, VmaAllocation allocation) {
    const VmaCall vma_call;
    if (Enabled()) Released(vma, allocation);
    __real_vmaDestroyImage(vma, image, allocation);
}

void __wrap_vmaDestroyBuffer(VmaAllocator vma, VkBuffer buffer, VmaAllocation allocation) {
    const VmaCall vma_call;
    if (Enabled()) Released(vma, allocation);
    __real_vmaDestroyBuffer(vma, buffer, allocation);
}

void __wrap_vmaFreeMemory(VmaAllocator vma, VmaAllocation allocation) {
    const VmaCall vma_call;
    if (Enabled()) Released(vma, allocation);
    __real_vmaFreeMemory(vma, allocation);
}
} // extern "C"
#endif
