// The name of this build, shown at the end of the HUD line and on the first boot-trace lines, so
// a picture or a log says which build made it. Bump it for every build that goes on a console.
#pragma once

#ifndef EDEN_BUILD_LABEL
#define EDEN_BUILD_LABEL "W-04"
#endif

namespace Eden {
inline constexpr const char* kBuildLabel = EDEN_BUILD_LABEL;
}
