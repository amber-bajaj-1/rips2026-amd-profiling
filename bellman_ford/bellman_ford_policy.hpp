#pragma once

#include "../sssp/sssp_query_capacity.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace bellman_ford_policy {

inline constexpr std::size_t kMaximumAutomaticWorkers = 4;
inline constexpr std::size_t kWorkspaceBytesPerVertex = 24;
inline constexpr std::size_t kPerWorkerReserveBytes =
    32ULL * 1024ULL * 1024ULL;

constexpr std::size_t saturating_add(std::size_t left, std::size_t right) {
  return right > std::numeric_limits<std::size_t>::max() - left
             ? std::numeric_limits<std::size_t>::max()
             : left + right;
}

constexpr std::size_t saturating_multiply(std::size_t left,
                                          std::size_t right) {
  return left != 0 &&
                 right > std::numeric_limits<std::size_t>::max() / left
             ? std::numeric_limits<std::size_t>::max()
             : left * right;
}

constexpr std::size_t estimated_workspace_bytes(std::size_t vertex_count,
                                                std::size_t source_capacity,
                                                std::size_t target_capacity) {
  std::size_t bytes = sssp_capacity::checked_multiply(
      vertex_count, kWorkspaceBytesPerVertex);
  bytes = sssp_capacity::checked_add(bytes, kPerWorkerReserveBytes);
  bytes = sssp_capacity::checked_add(
      bytes, sssp_capacity::checked_bytes<int>(source_capacity));
  bytes = sssp_capacity::checked_add(
      bytes, sssp_capacity::checked_bytes<int>(target_capacity));
  // A successful compact path can contain V nodes and V-1 edges per target.
  // Retained geometric buffers temporarily overlap their replacements, so a
  // three-times ceiling keeps automatic worker selection conservative.
  const std::size_t retained_targets =
      std::min(vertex_count, target_capacity);
  const std::size_t maximum_path_items =
      saturating_multiply(vertex_count, retained_targets);
  const std::size_t compact_path_bytes = saturating_multiply(
      maximum_path_items, 2 * sizeof(std::uint32_t));
  return saturating_add(
      bytes, saturating_multiply(compact_path_bytes, 3));
}

struct Inputs {
  std::size_t route_request_count = 0;
  std::size_t cpu_hardware_concurrency = 0;
  std::size_t free_device_bytes = 0;
  std::size_t workspace_bytes = 0;
};

constexpr std::size_t recommend_worker_count(const Inputs& inputs) {
  if (inputs.route_request_count <= 1 || inputs.workspace_bytes == 0) return 1;
  const std::size_t cpu_limit =
      std::max<std::size_t>(1, inputs.cpu_hardware_concurrency);
  const std::size_t memory_budget =
      inputs.free_device_bytes - inputs.free_device_bytes / 4;
  const std::size_t memory_limit =
      std::max<std::size_t>(1, memory_budget / inputs.workspace_bytes);
  return std::max<std::size_t>(
      1, std::min({kMaximumAutomaticWorkers,
                   inputs.route_request_count,
                   cpu_limit,
                   memory_limit}));
}

}  // namespace bellman_ford_policy
