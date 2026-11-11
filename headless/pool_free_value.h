// SPDX-License-Identifier: GPL-3.0-or-later
// What eden_pool_largest_free (performance.cpp) reports for one kernel query. Header only and free
// of the PS5 SDK, so tools/test-pipeline-memory-gate.cpp tests it with the gate on any host.
#pragma once

namespace Eden::Performance {

// 0 means "unknown" to every caller (the pipeline gate then never waits), so it is kept for a
// failed query alone. A query that succeeds with no free block is the pool at its emptiest: it
// reports 1 byte, which is under any mark. (Vampinity's review of 0912, issue 21: returning 0 there
// opened the gate exactly when memory was gone.)
constexpr unsigned long long PoolLargestFreeValue(bool query_succeeded, unsigned long long largest) noexcept {
    if (!query_succeeded) return 0;
    return largest == 0 ? 1 : largest;
}

} // namespace Eden::Performance
