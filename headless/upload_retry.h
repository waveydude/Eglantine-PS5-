// When a texture upload that found no memory is tried again (the texture cache's UploadLater).
//
// Wave Ocarina 03 gave an image up for good at its 8th refusal: it cleared the image's "the game
// wrote this" mark and drew with whatever the image held, until the game wrote it again. Under a
// long shortage that is every waiting image, about 630 frames after the shortage began, and with
// Wave Ocarina 04's shared ASTC cooldown every ASTC upload is refused while the cooldown runs. An
// image that was never uploaded is black. So an image is now tried again for as long as it is
// used, 128 frames apart at most, which costs one refused attempt each time and nothing else.
// EDEN_NX2_UPLOAD_MAX_RETRIES=<n> (1 or more) brings the old limit back; unset or 0: no limit.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace Eden::UploadRetry {

constexpr std::uint64_t MaxBackoffFrames = 128;

// Frames to wait after the `fail_count`-th refusal in a row (1 for the first): 8, 16, 32, 64,
// then 128 for every one after.
constexpr std::uint64_t BackoffFrames(std::uint32_t fail_count) {
    const std::uint32_t shift = std::min<std::uint32_t>(fail_count, 5) + 2;
    return std::min<std::uint64_t>(std::uint64_t{1} << shift, MaxBackoffFrames);
}

// EDEN_NX2_UPLOAD_MAX_RETRIES as text: a number of refusals after which the image is given up,
// or 0 (also for no text, or text that is not a number): never.
inline std::uint32_t ReadMaxRetries(const char* text) {
    if (text == nullptr || text[0] < '0' || text[0] > '9') {
        return 0;
    }
    const unsigned long value = std::strtoul(text, nullptr, 10);
    return value > 0xffffffffUL ? 0xffffffffU : static_cast<std::uint32_t>(value);
}

// Whether the image is given up at its `fail_count`-th refusal in a row.
constexpr bool GiveUp(std::uint32_t fail_count, std::uint32_t max_retries) {
    return max_retries != 0 && fail_count >= max_retries;
}

// The count after one more refusal: it stops at its top instead of wrapping to 0, which would
// start the short waits again.
constexpr std::uint32_t NextFailCount(std::uint32_t fail_count) {
    return fail_count == 0xffffffffU ? fail_count : fail_count + 1;
}

} // namespace Eden::UploadRetry
