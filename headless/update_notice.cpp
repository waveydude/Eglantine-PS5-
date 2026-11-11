// SPDX-License-Identifier: GPL-3.0-or-later
#include "update_notice.h"

#include <string>
#include "storage_paths.h"

// The kit's paths, the app's own (update_check/eden_paths.h): after elevation /app0 and /download0
// are gone, so they follow storage_paths.h.
extern "C" const char* eden_self_update_path(int which) {
    static const std::string helper = Eden::AppFile("self-updater.elf");
    static const std::string param = Eden::AppFile("sce_sys/param.json");
    static const std::string sequence = Eden::ConfigFile("self-update-sequence");
    return which == 0 ? helper.c_str() : which == 1 ? param.c_str() : sequence.c_str();
}

namespace Eden::UpdateNotice {

void Start() {
    // Online update check disabled for standalone build.
}

bool Take(Offer* out) {
    (void)out;
    return false;
}

bool Begin() {
    return false;
}

Progress Poll() {
    return Progress{};
}

void Cancel() {}

bool Apply() {
    return false;
}

void Finish() {}

} // namespace Eden::UpdateNotice
