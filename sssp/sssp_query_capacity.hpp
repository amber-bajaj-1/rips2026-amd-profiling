#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <type_traits>

struct SsspQueryCapacityHints {
  std::size_t max_sources = 0;
  std::size_t max_targets = 0;
};

namespace sssp_capacity {

constexpr std::size_t checked_add(std::size_t left, std::size_t right) {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    throw std::overflow_error("SSSP capacity addition overflow");
  }
  return left + right;
}

constexpr std::size_t checked_multiply(std::size_t left,
                                       std::size_t right) {
  if (left != 0 &&
      right > std::numeric_limits<std::size_t>::max() / left) {
    throw std::overflow_error("SSSP capacity multiplication overflow");
  }
  return left * right;
}

template <typename T>
constexpr std::size_t checked_bytes(std::size_t count) {
  static_assert(!std::is_void<T>::value,
                "SSSP byte counts require a complete object type");
  return checked_multiply(count, sizeof(T));
}

constexpr std::size_t checked_device_count(std::size_t count) {
  if (count > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::overflow_error("SSSP query count exceeds device int range");
  }
  return count;
}

constexpr std::size_t checked_target_offset_count(std::size_t target_count) {
  return checked_add(checked_device_count(target_count), 1);
}

constexpr void validate_reservation(const SsspQueryCapacityHints& hints) {
  const std::size_t source_count = checked_device_count(hints.max_sources);
  const std::size_t target_count = checked_device_count(hints.max_targets);
  const std::size_t target_offset_count =
      checked_target_offset_count(target_count);
  (void)checked_bytes<int>(source_count);
  (void)checked_bytes<int>(target_count);
  (void)checked_bytes<int>(target_offset_count);
}

constexpr void accumulate_query_counts(SsspQueryCapacityHints& hints,
                                       std::size_t raw_source_count,
                                       std::size_t raw_target_count) {
  validate_reservation(hints);
  SsspQueryCapacityHints candidate = hints;
  candidate.max_sources =
      std::max(candidate.max_sources, checked_device_count(raw_source_count));
  candidate.max_targets =
      std::max(candidate.max_targets, checked_device_count(raw_target_count));
  validate_reservation(candidate);
  hints = candidate;
}

constexpr std::size_t geometric_capacity(std::size_t current,
                                         std::size_t required) {
  if (current >= required) return current;
  if (current == 0) return required;
  const std::size_t growth = checked_add(current / 2, 1);
  return std::max(required, checked_add(current, growth));
}

}  // namespace sssp_capacity
