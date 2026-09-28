#pragma once

// Find mode release builds carry a build number after the upstream version:
// 1.6.5-findmode.2 is the second find mode release on upstream 1.6.5. OTA
// compares the three upstream numbers first; when they match, a higher build
// number is the newer release.

#include <cstdlib>
#include <cstring>

namespace find_mode {

static constexpr char BUILD_MARKER[] = "-findmode.";

// The number after "-findmode.", 0 for a version without one.
inline int buildNumber(const char* version) {
  const char* marker = std::strstr(version, BUILD_MARKER);
  return marker ? std::atoi(marker + sizeof(BUILD_MARKER) - 1) : 0;
}

}  // namespace find_mode
