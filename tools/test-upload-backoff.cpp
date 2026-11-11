// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for texture upload failure backoff and fallback (patch 0919).
// Tests chunked upload staging refusal, fallback at 8th failure, and KeepContentsFallback safety.
//   c++ -std=c++20 -O2 tools/test-upload-backoff.cpp -o /tmp/test-upload-backoff && /tmp/test-upload-backoff
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

enum ImageFlags : uint32_t {
    None = 0,
    CpuModified = 1 << 0,
};

struct MockImage {
    uint64_t gpu_addr = 0x8000000;
    uint64_t upload_retry_frame = 0;
    uint32_t upload_fail_count = 0;
    uint32_t flags = CpuModified;
    bool kept_contents = false;
};

struct MockRuntime {
    bool staging_should_fail = false;
    bool upload_refused = false;
};

// Replicate KeepContentsFallback from patch 0919
void KeepContentsFallback(MockImage& image) {
    image.kept_contents = true;
    image.flags &= ~CpuModified;
}

// Replicate UploadLater from patch 0919
void UploadLater(MockImage& image, uint64_t frame_tick) {
    ++image.upload_fail_count;
    constexpr uint32_t max_retries = 8;
    if (image.upload_fail_count >= max_retries) {
        KeepContentsFallback(image);
        image.upload_fail_count = 0;
        image.upload_retry_frame = 0;
        return;
    }
    const uint64_t backoff_frames = std::min<uint64_t>(uint64_t{1} << std::min<uint32_t>(image.upload_fail_count + 2, 7), 128);
    image.upload_retry_frame = frame_tick + backoff_frames;
    image.flags |= CpuModified;
}

// Replicate UploadInChunks from patch 0919
bool UploadInChunks(MockImage& image, MockRuntime& runtime, uint64_t min_size = 32 * 1024 * 1024) {
    (void)image;
    (void)min_size;
    if (runtime.staging_should_fail) {
        // Staging failed: returns false without calling UploadLater or returning true
        return false;
    }
    return true;
}

// Replicate RefreshContents upload decision from patch 0919
void RefreshContents(MockImage& image, MockRuntime& runtime, uint64_t frame_tick) {
    if ((image.flags & CpuModified) == 0) {
        return;
    }
    if (image.upload_retry_frame > frame_tick) {
        return;
    }

    bool uploaded = false;
    if (UploadInChunks(image, runtime)) {
        uploaded = true;
    } else if (!runtime.staging_should_fail) {
        if (runtime.upload_refused) {
            UploadLater(image, frame_tick);
        } else {
            uploaded = true;
        }
    } else if (UploadInChunks(image, runtime, 0)) {
        uploaded = true;
    } else {
        UploadLater(image, frame_tick);
    }

    if (uploaded) {
        image.upload_fail_count = 0;
        image.upload_retry_frame = 0;
    }
}

void NotifyCpuWrite(MockImage& image) {
    image.flags |= CpuModified;
    image.upload_fail_count = 0;
    image.upload_retry_frame = 0;
}

int main() {
    MockImage img;
    MockRuntime runtime;

    // Test 1: Successful upload resets retry state
    img.upload_fail_count = 3;
    img.upload_retry_frame = 50;
    RefreshContents(img, runtime, 100);
    CHECK(img.upload_fail_count == 0);
    CHECK(img.upload_retry_frame == 0);
    CHECK(img.flags & CpuModified);

    // Test 2: Forced staging failure in chunked upload
    // When staging fails, UploadInChunks returns false, RefreshContents falls back to UploadLater.
    // It must NOT report success, must NOT reset fail_count, and must back off properly.
    runtime.staging_should_fail = true;
    uint64_t current_frame = 200;

    // 1st failure: backoff = 1 << (1 + 2) = 8 frames -> retry at 208
    RefreshContents(img, runtime, current_frame);
    CHECK(img.upload_fail_count == 1);
    CHECK(img.upload_retry_frame == 208);
    CHECK(!img.kept_contents);

    // During backoff frames 201..207, RefreshContents must skip without modifying fail count
    for (uint64_t f = 201; f <= 207; ++f) {
        RefreshContents(img, runtime, f);
        CHECK(img.upload_fail_count == 1);
        CHECK(img.upload_retry_frame == 208);
    }

    // 2nd failure at frame 208: backoff = 1 << (2 + 2) = 16 frames -> retry at 224
    RefreshContents(img, runtime, 208);
    CHECK(img.upload_fail_count == 2);
    CHECK(img.upload_retry_frame == 224);

    // Failures 3 through 7
    current_frame = 224;
    for (uint32_t fail = 3; fail <= 7; ++fail) {
        RefreshContents(img, runtime, current_frame);
        CHECK(img.upload_fail_count == fail);
        uint64_t expected_backoff = std::min<uint64_t>(uint64_t{1} << (fail + 2), 128ULL);
        CHECK(img.upload_retry_frame == current_frame + expected_backoff);
        current_frame += expected_backoff;
    }
    CHECK(!img.kept_contents);

    // 8th failure at current_frame: triggers fallback immediately (>= max_retries)
    RefreshContents(img, runtime, current_frame);
    CHECK(img.kept_contents);
    CHECK(img.upload_fail_count == 0);
    CHECK(img.upload_retry_frame == 0);
    // Crucial: CpuModified must be cleared so future draws do not retry
    CHECK((img.flags & CpuModified) == 0);

    // Subsequent frame: since CpuModified is cleared, RefreshContents does nothing
    img.kept_contents = false;
    RefreshContents(img, runtime, current_frame + 1);
    CHECK(!img.kept_contents); // Not called again

    // Test 3: CPU write re-enables upload
    NotifyCpuWrite(img);
    CHECK(img.flags & CpuModified);
    CHECK(img.upload_fail_count == 0);
    CHECK(img.upload_retry_frame == 0);

    // Now if memory recovered, upload succeeds
    runtime.staging_should_fail = false;
    RefreshContents(img, runtime, current_frame + 2);
    CHECK(img.upload_fail_count == 0);
    CHECK(img.upload_retry_frame == 0);

    if (failures != 0) {
        std::fprintf(stderr, "test-upload-backoff failed with %d errors\n", failures);
        return 1;
    }

    std::puts("texture upload backoff: chunked failure backoff, 8th failure fallback, CpuModified clearing PASS");
    return 0;
}
