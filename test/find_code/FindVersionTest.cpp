#include <gtest/gtest.h>

#include "lib/FindMode/FindVersion.h"

TEST(FindVersion, AnUpstreamVersionHasBuildZero) {
  const int build = find_mode::buildNumber("1.6.5");

  EXPECT_EQ(build, 0);
}

TEST(FindVersion, ReadsTheFirstBuild) {
  const int build = find_mode::buildNumber("1.6.5-findmode.1");

  EXPECT_EQ(build, 1);
}

TEST(FindVersion, ReadsABuildPastNine) {
  const int build = find_mode::buildNumber("1.6.5-findmode.12");

  EXPECT_EQ(build, 12);
}

TEST(FindVersion, ReadsTheBuildOfAReleaseCandidate) {
  const int build = find_mode::buildNumber("1.6.5-findmode.2-rc+1a2b3c4");

  EXPECT_EQ(build, 2);
}
