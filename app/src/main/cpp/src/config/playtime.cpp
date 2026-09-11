#include "playtime.h"
#include <cstdio>

namespace config {

void PlaytimeTracker::format(char* buf, usize bufSize) const {
    if (!buf || bufSize < 2) return;

    u32 total = seconds();
    u32 h = total / 3600;
    u32 m = (total % 3600) / 60;
    u32 s = total % 60;

    if (h > 0) {
        std::snprintf(buf, bufSize, "%uh %um %us",
                      (unsigned)h, (unsigned)m, (unsigned)s);
    } else if (m > 0) {
        std::snprintf(buf, bufSize, "%um %us",
                      (unsigned)m, (unsigned)s);
    } else {
        std::snprintf(buf, bufSize, "%us", (unsigned)s);
    }
}

} // namespace config