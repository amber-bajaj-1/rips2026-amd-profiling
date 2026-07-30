#pragma once

// Compile PathFinder with -DPATHFINDER_ENABLE_ROCTX and link against
// either current or legacy ROCTx to emit these ranges. Normal builds remain
// independent of the profiling SDK and optimize the scopes away.
#if defined(PATHFINDER_ENABLE_ROCTX)
#if defined(PATHFINDER_USE_LEGACY_ROCTX)
#include <roctx.h>
#else
#include <rocprofiler-sdk-roctx/roctx.h>
#endif
#endif

namespace pathfinder_profile {

class ScopedRange {
 public:
  explicit ScopedRange(const char* name) noexcept {
#if defined(PATHFINDER_ENABLE_ROCTX)
    roctxRangePush(name);
#else
    (void)name;
#endif
  }

  ~ScopedRange() noexcept {
#if defined(PATHFINDER_ENABLE_ROCTX)
    roctxRangePop();
#endif
  }

  ScopedRange(const ScopedRange&) = delete;
  ScopedRange& operator=(const ScopedRange&) = delete;
};

}  // namespace pathfinder_profile

#define PATHFINDER_PROFILE_CONCAT_IMPL(left, right) left##right
#define PATHFINDER_PROFILE_CONCAT(left, right) \
  PATHFINDER_PROFILE_CONCAT_IMPL(left, right)
#define PATHFINDER_PROFILE_RANGE(name)                                  \
  ::pathfinder_profile::ScopedRange PATHFINDER_PROFILE_CONCAT(          \
      pathfinder_profile_range_, __LINE__)(name)
