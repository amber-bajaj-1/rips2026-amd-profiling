#include "bellman_ford.hpp"

#include "../sssp/roctx_ranges.hpp"

#include <hip/hip_runtime.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bellman_ford_detail {

using Offset = minplus_sparse::Offset;
using Index = minplus_sparse::Index;
using DeviceEdge = std::uint32_t;

constexpr int kBlockSize = 256;
constexpr unsigned int kGridXLimit = 65535;
constexpr DeviceEdge kNoPredecessor =
    std::numeric_limits<DeviceEdge>::max();
constexpr unsigned int kInfinityBits = 0x7f800000u;

static_assert(sizeof(float) == sizeof(unsigned int) &&
                  std::numeric_limits<float>::is_iec559,
              "Bellman-Ford requires IEEE-754 float32 distances");
static_assert(sizeof(unsigned long long) == 2 * sizeof(unsigned int),
              "Bellman-Ford requires a 64-bit packed atomic state");

struct DeviceGraph {
  Offset rows = 0;
  Offset nnz = 0;
  const Offset* rowptr = nullptr;
  const Index* colind = nullptr;
  const float* values = nullptr;
};

struct IterationStatus {
  int next_count = 0;
  int error_status = 0;
  int reached_target_count = 0;
  unsigned int min_next_distance_bits = kInfinityBits;
  unsigned int max_target_distance_bits = 0;
};

enum TargetPathStatus : int {
  kTargetUnreachable = 0,
  kTargetPathValid = 1,
  kTargetPathInvalid = 2,
};

struct TargetSummary {
  unsigned long long state = 0;
  unsigned long long node_count = 0;
  unsigned long long edge_count = 0;
  int root = -1;
  int status = kTargetUnreachable;
};

void check_hip(hipError_t status, const char* what) {
  if (status != hipSuccess) {
    throw std::runtime_error(std::string(what) + ": " +
                             hipGetErrorString(status));
  }
}

int current_device() {
  int device = -1;
  check_hip(hipGetDevice(&device), "get Bellman-Ford HIP device");
  return device;
}

class ScopedOwningDevice {
 public:
  explicit ScopedOwningDevice(int owner) noexcept {
    if (hipGetDevice(&previous_) != hipSuccess) return;
    if (previous_ == owner) {
      active_ = true;
      return;
    }
    if (hipSetDevice(owner) == hipSuccess) {
      active_ = true;
      restore_ = true;
    }
  }

  ~ScopedOwningDevice() noexcept {
    if (restore_) (void)hipSetDevice(previous_);
  }

  bool active() const noexcept { return active_; }

 private:
  int previous_ = -1;
  bool active_ = false;
  bool restore_ = false;
};

class DrainStreamOnException {
 public:
  explicit DrainStreamOnException(hipStream_t stream)
      : stream_(stream), exceptions_(std::uncaught_exceptions()) {}

  ~DrainStreamOnException() noexcept {
    if (std::uncaught_exceptions() > exceptions_) {
      (void)hipStreamSynchronize(stream_);
    }
  }

 private:
  hipStream_t stream_ = nullptr;
  int exceptions_ = 0;
};

template <typename T>
T* device_allocate(std::size_t count, const char* what) {
  if (count == 0) return nullptr;
  T* result = nullptr;
  check_hip(hipMalloc(reinterpret_cast<void**>(&result),
                      sssp_capacity::checked_bytes<T>(count)),
            what);
  return result;
}

dim3 grid_for_items(Offset items) {
  if (items <= 0) return dim3(1);
  const Offset blocks = (items + kBlockSize - 1) / kBlockSize;
  const unsigned int grid_x = static_cast<unsigned int>(
      std::min<Offset>(blocks, static_cast<Offset>(kGridXLimit)));
  const unsigned int grid_y = static_cast<unsigned int>(
      (blocks + grid_x - 1) / grid_x);
  return dim3(grid_x, grid_y);
}

__device__ __forceinline__ Offset logical_thread_id() {
  return (static_cast<Offset>(blockIdx.y) *
              static_cast<Offset>(gridDim.x) +
          static_cast<Offset>(blockIdx.x)) *
             static_cast<Offset>(blockDim.x) +
         static_cast<Offset>(threadIdx.x);
}

__host__ __device__ __forceinline__ unsigned long long pack_state(
    unsigned int distance_bits,
    DeviceEdge predecessor) {
  return (static_cast<unsigned long long>(distance_bits) << 32) |
         static_cast<unsigned long long>(predecessor);
}

__host__ __device__ __forceinline__ unsigned int state_distance_bits(
    unsigned long long state) {
  return static_cast<unsigned int>(state >> 32);
}

__host__ __device__ __forceinline__ DeviceEdge state_predecessor(
    unsigned long long state) {
  return static_cast<DeviceEdge>(state);
}

float host_state_distance(unsigned long long state) {
  const unsigned int bits = state_distance_bits(state);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

__device__ __forceinline__ float device_state_distance(
    unsigned long long state) {
  return __uint_as_float(state_distance_bits(state));
}

__device__ __forceinline__ bool finite_device(float value) {
  return value == value && value != INFINITY && value != -INFINITY;
}

__device__ __forceinline__ unsigned long long coherent_atomic_load(
    unsigned long long* address) {
  return atomicCAS(address, 0ULL, 0ULL);
}

__device__ __forceinline__ bool atomic_relax_strict(
    unsigned long long* address,
    float candidate,
    DeviceEdge predecessor) {
  const unsigned int candidate_bits = __float_as_uint(candidate);
  unsigned long long observed = coherent_atomic_load(address);
  while (candidate_bits < state_distance_bits(observed)) {
    const unsigned long long desired =
        pack_state(candidate_bits, predecessor);
    const unsigned long long assumed = observed;
    observed = atomicCAS(address, assumed, desired);
    if (observed == assumed) return true;
  }
  return false;
}

__device__ __forceinline__ Index source_for_edge(
    const Offset* rowptr,
    Offset rows,
    DeviceEdge edge) {
  Offset low = 0;
  Offset high = rows;
  while (low + 1 < high) {
    const Offset middle = low + (high - low) / 2;
    if (rowptr[middle] <= static_cast<Offset>(edge)) {
      low = middle;
    } else {
      high = middle;
    }
  }
  return rowptr[low] <= static_cast<Offset>(edge) &&
                 static_cast<Offset>(edge) < rowptr[low + 1]
             ? static_cast<Index>(low)
             : -1;
}

__global__ void clear_workspace_kernel(Offset rows,
                                       unsigned long long* best_state,
                                       int* next_marks,
                                       unsigned char* source_mask) {
  const Offset node = logical_thread_id();
  if (node >= rows) return;
  best_state[node] = pack_state(kInfinityBits, kNoPredecessor);
  next_marks[node] = 0;
  source_mask[node] = 0;
}

__global__ void seed_sources_kernel(const Index* sources,
                                    int source_count,
                                    unsigned long long* best_state,
                                    Index* frontier,
                                    unsigned char* source_mask) {
  const Offset item = logical_thread_id();
  if (item >= static_cast<Offset>(source_count)) return;
  const Index source = sources[item];
  best_state[source] = pack_state(0u, kNoPredecessor);
  source_mask[source] = 1;
  frontier[item] = source;
}

__global__ void reset_iteration_status_kernel(IterationStatus* status) {
  if (logical_thread_id() != 0) return;
  status->next_count = 0;
  status->error_status = 0;
  status->reached_target_count = 0;
  status->min_next_distance_bits = kInfinityBits;
  status->max_target_distance_bits = 0;
}

__global__ void relax_frontier_kernel(DeviceGraph graph,
                                      const Index* frontier,
                                      int frontier_count,
                                      int mark_token,
                                      unsigned long long* best_state,
                                      Index* next_frontier,
                                      int* next_marks,
                                      const unsigned char* source_mask,
                                      IterationStatus* status) {
  const Offset item = logical_thread_id();
  if (item >= static_cast<Offset>(frontier_count)) return;
  const Index from = frontier[item];
  if (from < 0 || static_cast<Offset>(from) >= graph.rows) {
    atomicExch(&status->error_status, 1);
    return;
  }
  const float from_distance =
      device_state_distance(coherent_atomic_load(&best_state[from]));
  if (!finite_device(from_distance)) return;
  const Offset begin = graph.rowptr[from];
  const Offset end = graph.rowptr[from + 1];
  if (begin < 0 || end < begin || end > graph.nnz) {
    atomicExch(&status->error_status, 2);
    return;
  }

  unsigned int local_min = kInfinityBits;
  for (Offset edge = begin; edge < end; ++edge) {
    const Index to = graph.colind[edge];
    if (to < 0 || static_cast<Offset>(to) >= graph.rows) {
      atomicExch(&status->error_status, 3);
      continue;
    }
    if (source_mask[to] != 0) continue;
    const float weight = graph.values[edge];
    if (!finite_device(weight) || weight < 0.0f) {
      atomicExch(&status->error_status, 4);
      continue;
    }
    const float candidate = from_distance + weight;
    if (!finite_device(candidate)) continue;
    if (!atomic_relax_strict(&best_state[to], candidate,
                             static_cast<DeviceEdge>(edge))) {
      continue;
    }
    const unsigned int candidate_bits = __float_as_uint(candidate);
    local_min = candidate_bits < local_min ? candidate_bits : local_min;
    if (atomicExch(&next_marks[to], mark_token) != mark_token) {
      const int slot = atomicAdd(&status->next_count, 1);
      if (slot < 0 || static_cast<Offset>(slot) >= graph.rows) {
        atomicExch(&status->error_status, 5);
      } else {
        next_frontier[slot] = to;
      }
    }
  }
  if (local_min != kInfinityBits) {
    atomicMin(&status->min_next_distance_bits, local_min);
  }
}

__global__ void update_target_status_kernel(
    const unsigned long long* best_state,
    const Index* targets,
    int target_count,
    IterationStatus* status) {
  const Offset item = logical_thread_id();
  if (item >= static_cast<Offset>(target_count)) return;
  const unsigned int bits = state_distance_bits(best_state[targets[item]]);
  if (bits == kInfinityBits) return;
  atomicAdd(&status->reached_target_count, 1);
  atomicMax(&status->max_target_distance_bits, bits);
}

__global__ void summarize_target_paths_kernel(
    DeviceGraph graph,
    const unsigned long long* best_state,
    const unsigned char* source_mask,
    const Index* targets,
    int target_count,
    TargetSummary* summaries) {
  const Offset item = logical_thread_id();
  if (item >= static_cast<Offset>(target_count)) return;
  const Index target = targets[item];
  TargetSummary summary{};
  summary.state = best_state[target];
  if (state_distance_bits(summary.state) == kInfinityBits) {
    summaries[item] = summary;
    return;
  }

  Index current = target;
  unsigned long long edge_count = 0;
  for (Offset step = 0; step <= graph.rows; ++step) {
    if (current < 0 || static_cast<Offset>(current) >= graph.rows) {
      summary.status = kTargetPathInvalid;
      summaries[item] = summary;
      return;
    }
    if (source_mask[current] != 0) {
      summary.node_count = edge_count + 1;
      summary.edge_count = edge_count;
      summary.root = current;
      summary.status = kTargetPathValid;
      summaries[item] = summary;
      return;
    }
    const DeviceEdge predecessor = state_predecessor(best_state[current]);
    if (predecessor == kNoPredecessor ||
        static_cast<Offset>(predecessor) >= graph.nnz ||
        graph.colind[predecessor] != current) {
      summary.status = kTargetPathInvalid;
      summaries[item] = summary;
      return;
    }
    current = source_for_edge(graph.rowptr, graph.rows, predecessor);
    ++edge_count;
  }
  summary.status = kTargetPathInvalid;
  summaries[item] = summary;
}

__global__ void materialize_target_paths_kernel(
    DeviceGraph graph,
    const unsigned long long* best_state,
    const Index* targets,
    const TargetSummary* summaries,
    const unsigned long long* node_offsets,
    const unsigned long long* edge_offsets,
    int target_count,
    Index* compact_nodes,
    DeviceEdge* compact_edges) {
  const Offset item = logical_thread_id();
  if (item >= static_cast<Offset>(target_count) ||
      summaries[item].status != kTargetPathValid) {
    return;
  }
  const TargetSummary summary = summaries[item];
  const unsigned long long node_base = node_offsets[item];
  const unsigned long long edge_base = edge_offsets[item];
  Index current = targets[item];
  compact_nodes[node_base + summary.edge_count] = current;
  for (unsigned long long remaining = summary.edge_count; remaining > 0;
       --remaining) {
    const DeviceEdge predecessor = state_predecessor(best_state[current]);
    const Index source = source_for_edge(graph.rowptr, graph.rows, predecessor);
    compact_edges[edge_base + remaining - 1] = predecessor;
    compact_nodes[node_base + remaining - 1] = source;
    current = source;
  }
}

std::vector<int> deduplicate_nodes(const std::vector<int>& nodes,
                                   Offset rows,
                                   const char* label) {
  std::vector<int> unique;
  unique.reserve(nodes.size());
  std::unordered_set<int> seen;
  seen.reserve(nodes.size() * 2 + 1);
  for (const int node : nodes) {
    if (node < 0 || static_cast<Offset>(node) >= rows) {
      throw std::out_of_range(std::string("Bellman-Ford ") + label +
                              " is outside the graph");
    }
    if (seen.insert(node).second) unique.push_back(node);
  }
  if (unique.empty()) {
    throw std::invalid_argument(std::string("Bellman-Ford requires at least one ") +
                                label);
  }
  return unique;
}

void validate_host_csr(const HostCsrF32& graph) {
  if (graph.rows <= 0 || graph.rows != graph.cols ||
      graph.rows > std::numeric_limits<int>::max()) {
    throw std::invalid_argument(
        "Bellman-Ford CSR must be a nonempty square graph with int-sized rows");
  }
  if (graph.nnz < 0 ||
      static_cast<std::uint64_t>(graph.nnz) >=
          static_cast<std::uint64_t>(kNoPredecessor)) {
    throw std::invalid_argument(
        "Bellman-Ford CSR edge count exceeds compact predecessor capacity");
  }
  const std::size_t rows = static_cast<std::size_t>(graph.rows);
  const std::size_t nnz = static_cast<std::size_t>(graph.nnz);
  if (graph.rowptr.size() != rows + 1 || graph.colind.size() != nnz ||
      graph.values.size() != nnz || graph.rowptr.front() != 0 ||
      graph.rowptr.back() != graph.nnz) {
    throw std::invalid_argument("Bellman-Ford CSR arrays do not match its shape");
  }
  for (Offset row = 0; row < graph.rows; ++row) {
    const Offset begin = graph.rowptr[static_cast<std::size_t>(row)];
    const Offset end = graph.rowptr[static_cast<std::size_t>(row + 1)];
    if (begin < 0 || end < begin || end > graph.nnz) {
      throw std::invalid_argument("Bellman-Ford CSR row offsets are invalid");
    }
  }
  for (std::size_t edge = 0; edge < nnz; ++edge) {
    if (graph.colind[edge] < 0 ||
        static_cast<Offset>(graph.colind[edge]) >= graph.rows) {
      throw std::invalid_argument(
          "Bellman-Ford CSR contains an out-of-range destination");
    }
    if (!std::isfinite(graph.values[edge]) || graph.values[edge] < 0.0f) {
      throw std::invalid_argument(
          "Bellman-Ford requires finite nonnegative edge weights");
    }
  }
}

}  // namespace bellman_ford_detail

struct BellmanFordCsrGraph::Impl {
  using Offset = bellman_ford_detail::Offset;
  using Index = bellman_ford_detail::Index;

  explicit Impl(const HostCsrF32& graph, hipStream_t stream)
      : rows(graph.rows), nnz(graph.nnz), device(bellman_ford_detail::current_device()) {
    PATHFINDER_PROFILE_RANGE("bellman_ford.upload_graph");
    bellman_ford_detail::validate_host_csr(graph);
    try {
      rowptr = bellman_ford_detail::device_allocate<Offset>(
          static_cast<std::size_t>(rows) + 1,
          "allocate Bellman-Ford row offsets");
      colind = bellman_ford_detail::device_allocate<Index>(
          static_cast<std::size_t>(nnz),
          "allocate Bellman-Ford column indices");
      values = bellman_ford_detail::device_allocate<float>(
          static_cast<std::size_t>(nnz),
          "allocate Bellman-Ford edge weights");
      bellman_ford_detail::check_hip(
          hipMemcpyAsync(rowptr, graph.rowptr.data(),
                         graph.rowptr.size() * sizeof(Offset),
                         hipMemcpyHostToDevice, stream),
          "copy Bellman-Ford row offsets");
      if (nnz != 0) {
        bellman_ford_detail::check_hip(
            hipMemcpyAsync(colind, graph.colind.data(),
                           graph.colind.size() * sizeof(Index),
                           hipMemcpyHostToDevice, stream),
            "copy Bellman-Ford column indices");
        bellman_ford_detail::check_hip(
            hipMemcpyAsync(values, graph.values.data(),
                           graph.values.size() * sizeof(float),
                           hipMemcpyHostToDevice, stream),
            "copy Bellman-Ford edge weights");
      }
      bellman_ford_detail::check_hip(
          hipStreamSynchronize(stream), "synchronize Bellman-Ford graph upload");
    } catch (...) {
      (void)hipStreamSynchronize(stream);
      if (rowptr) (void)hipFree(rowptr);
      if (colind) (void)hipFree(colind);
      if (values) (void)hipFree(values);
      rowptr = nullptr;
      colind = nullptr;
      values = nullptr;
      throw;
    }
  }

  ~Impl() {
    bellman_ford_detail::ScopedOwningDevice guard(device);
    if (!guard.active()) return;
    if (rowptr) (void)hipFree(rowptr);
    if (colind) (void)hipFree(colind);
    if (values) (void)hipFree(values);
  }

  bellman_ford_detail::DeviceGraph view() const noexcept {
    return {rows, nnz, rowptr, colind, values};
  }

  Offset rows = 0;
  Offset nnz = 0;
  Offset* rowptr = nullptr;
  Index* colind = nullptr;
  float* values = nullptr;
  int device = -1;
};

BellmanFordCsrGraph::BellmanFordCsrGraph(const HostCsrF32& adjacency,
                                         hipStream_t stream)
    : impl_(std::make_shared<Impl>(adjacency, stream)) {}

BellmanFordCsrGraph::~BellmanFordCsrGraph() = default;
BellmanFordCsrGraph::BellmanFordCsrGraph(BellmanFordCsrGraph&&) noexcept =
    default;
BellmanFordCsrGraph& BellmanFordCsrGraph::operator=(
    BellmanFordCsrGraph&&) noexcept = default;

struct BellmanFordCsrWorkspace::Impl {
  using Offset = bellman_ford_detail::Offset;
  using Index = bellman_ford_detail::Index;
  using DeviceEdge = bellman_ford_detail::DeviceEdge;
  using TargetSummary = bellman_ford_detail::TargetSummary;
  using IterationStatus = bellman_ford_detail::IterationStatus;

  static std::shared_ptr<const BellmanFordCsrGraph::Impl> require_graph(
      const std::shared_ptr<const BellmanFordCsrGraph>& graph) {
    if (!graph || !graph->impl_) {
      throw std::invalid_argument("Bellman-Ford shared graph must not be null");
    }
    return graph->impl_;
  }

  Impl(std::shared_ptr<const BellmanFordCsrGraph::Impl> graph_in,
       hipStream_t stream_in,
       BellmanFordCsrWorkspaceOptions options)
      : graph(std::move(graph_in)), stream(stream_in), device(graph->device) {
    PATHFINDER_PROFILE_RANGE("bellman_ford.create_workspace");
    if (bellman_ford_detail::current_device() != device) {
      throw std::invalid_argument(
          "Bellman-Ford graph belongs to a different HIP device");
    }
    sssp_capacity::validate_reservation(options.capacity_hints);
    try {
      const std::size_t rows = static_cast<std::size_t>(graph->rows);
      best_state = bellman_ford_detail::device_allocate<unsigned long long>(
          rows, "allocate Bellman-Ford distance state");
      frontier_a = bellman_ford_detail::device_allocate<Index>(
          rows, "allocate Bellman-Ford frontier A");
      frontier_b = bellman_ford_detail::device_allocate<Index>(
          rows, "allocate Bellman-Ford frontier B");
      next_marks = bellman_ford_detail::device_allocate<int>(
          rows, "allocate Bellman-Ford frontier marks");
      source_mask = bellman_ford_detail::device_allocate<unsigned char>(
          rows, "allocate Bellman-Ford source mask");
      iteration_status =
          bellman_ford_detail::device_allocate<IterationStatus>(
              1, "allocate Bellman-Ford iteration status");
      ensure_source_capacity(std::min(rows, options.capacity_hints.max_sources));
      ensure_target_capacity(std::min(rows, options.capacity_hints.max_targets));
    } catch (...) {
      release();
      throw;
    }
  }

  ~Impl() {
    bellman_ford_detail::ScopedOwningDevice guard(device);
    if (guard.active()) release();
  }

  void release() noexcept {
    if (best_state) (void)hipFree(best_state);
    if (frontier_a) (void)hipFree(frontier_a);
    if (frontier_b) (void)hipFree(frontier_b);
    if (next_marks) (void)hipFree(next_marks);
    if (source_mask) (void)hipFree(source_mask);
    if (iteration_status) (void)hipFree(iteration_status);
    if (source_nodes) (void)hipFree(source_nodes);
    if (target_nodes) (void)hipFree(target_nodes);
    if (target_summaries) (void)hipFree(target_summaries);
    if (node_offsets) (void)hipFree(node_offsets);
    if (edge_offsets) (void)hipFree(edge_offsets);
    if (compact_nodes) (void)hipFree(compact_nodes);
    if (compact_edges) (void)hipFree(compact_edges);
    best_state = nullptr;
    frontier_a = nullptr;
    frontier_b = nullptr;
    next_marks = nullptr;
    source_mask = nullptr;
    iteration_status = nullptr;
    source_nodes = nullptr;
    target_nodes = nullptr;
    target_summaries = nullptr;
    node_offsets = nullptr;
    edge_offsets = nullptr;
    compact_nodes = nullptr;
    compact_edges = nullptr;
  }

  void require_stream(hipStream_t candidate) const {
    if (candidate != stream) {
      throw std::invalid_argument(
          "Bellman-Ford workspace is stream-affine");
    }
    if (bellman_ford_detail::current_device() != device) {
      throw std::invalid_argument(
          "Bellman-Ford workspace belongs to a different HIP device");
    }
  }

  void ensure_source_capacity(std::size_t required) {
    if (required <= source_capacity) return;
    const std::size_t capacity = sssp_capacity::geometric_capacity(
        source_capacity, required);
    Index* replacement = bellman_ford_detail::device_allocate<Index>(
        capacity, "grow Bellman-Ford source storage");
    if (source_nodes) (void)hipFree(source_nodes);
    source_nodes = replacement;
    source_capacity = capacity;
  }

  void ensure_target_capacity(std::size_t required) {
    if (required <= target_capacity) return;
    const std::size_t capacity = sssp_capacity::geometric_capacity(
        target_capacity, required);
    Index* new_targets = nullptr;
    TargetSummary* new_summaries = nullptr;
    unsigned long long* new_node_offsets = nullptr;
    unsigned long long* new_edge_offsets = nullptr;
    try {
      new_targets = bellman_ford_detail::device_allocate<Index>(
          capacity, "grow Bellman-Ford target storage");
      new_summaries = bellman_ford_detail::device_allocate<TargetSummary>(
          capacity, "grow Bellman-Ford target summaries");
      new_node_offsets =
          bellman_ford_detail::device_allocate<unsigned long long>(
              capacity, "grow Bellman-Ford node offsets");
      new_edge_offsets =
          bellman_ford_detail::device_allocate<unsigned long long>(
              capacity, "grow Bellman-Ford edge offsets");
    } catch (...) {
      if (new_targets) (void)hipFree(new_targets);
      if (new_summaries) (void)hipFree(new_summaries);
      if (new_node_offsets) (void)hipFree(new_node_offsets);
      if (new_edge_offsets) (void)hipFree(new_edge_offsets);
      throw;
    }
    if (target_nodes) (void)hipFree(target_nodes);
    if (target_summaries) (void)hipFree(target_summaries);
    if (node_offsets) (void)hipFree(node_offsets);
    if (edge_offsets) (void)hipFree(edge_offsets);
    target_nodes = new_targets;
    target_summaries = new_summaries;
    node_offsets = new_node_offsets;
    edge_offsets = new_edge_offsets;
    target_capacity = capacity;
  }

  void ensure_compact_capacity(std::size_t required_nodes,
                               std::size_t required_edges) {
    if (required_nodes > compact_node_capacity) {
      const std::size_t capacity = sssp_capacity::geometric_capacity(
          compact_node_capacity, required_nodes);
      Index* replacement = bellman_ford_detail::device_allocate<Index>(
          capacity, "grow Bellman-Ford compact nodes");
      if (compact_nodes) (void)hipFree(compact_nodes);
      compact_nodes = replacement;
      compact_node_capacity = capacity;
    }
    if (required_edges > compact_edge_capacity) {
      const std::size_t capacity = sssp_capacity::geometric_capacity(
          compact_edge_capacity, required_edges);
      DeviceEdge* replacement =
          bellman_ford_detail::device_allocate<DeviceEdge>(
              capacity, "grow Bellman-Ford compact edges");
      if (compact_edges) (void)hipFree(compact_edges);
      compact_edges = replacement;
      compact_edge_capacity = capacity;
    }
  }

  BellmanFordCsrResult run(const std::vector<int>& sources,
                           const std::vector<int>& targets,
                           int requested_max_iters,
                           BellmanFordCsrProgressCallback progress_callback,
                           void* progress_user_data) {
    PATHFINDER_PROFILE_RANGE("bellman_ford.run");
    if (requested_max_iters < -1) {
      throw std::invalid_argument(
          "Bellman-Ford max iterations must be -1 or nonnegative");
    }
    bellman_ford_detail::DrainStreamOnException drain(stream);
    const std::vector<int> unique_sources =
        bellman_ford_detail::deduplicate_nodes(sources, graph->rows, "source");
    const std::vector<int> unique_targets =
        bellman_ford_detail::deduplicate_nodes(targets, graph->rows, "target");
    const int max_iters = requested_max_iters < 0
                              ? static_cast<int>(graph->rows)
                              : requested_max_iters;
    ensure_source_capacity(unique_sources.size());
    ensure_target_capacity(unique_targets.size());

    bellman_ford_detail::check_hip(
        hipMemcpyAsync(source_nodes, unique_sources.data(),
                       unique_sources.size() * sizeof(Index),
                       hipMemcpyHostToDevice, stream),
        "copy Bellman-Ford sources");
    bellman_ford_detail::check_hip(
        hipMemcpyAsync(target_nodes, unique_targets.data(),
                       unique_targets.size() * sizeof(Index),
                       hipMemcpyHostToDevice, stream),
        "copy Bellman-Ford targets");
    {
      PATHFINDER_PROFILE_RANGE("bellman_ford.reset_seed");
      hipLaunchKernelGGL(
          bellman_ford_detail::clear_workspace_kernel,
          bellman_ford_detail::grid_for_items(graph->rows),
          dim3(bellman_ford_detail::kBlockSize), 0, stream, graph->rows,
          best_state, next_marks, source_mask);
      bellman_ford_detail::check_hip(
          hipGetLastError(), "clear Bellman-Ford workspace");
      hipLaunchKernelGGL(
          bellman_ford_detail::seed_sources_kernel,
          bellman_ford_detail::grid_for_items(
              static_cast<Offset>(unique_sources.size())),
          dim3(bellman_ford_detail::kBlockSize), 0, stream, source_nodes,
          static_cast<int>(unique_sources.size()), best_state, frontier_a,
          source_mask);
      bellman_ford_detail::check_hip(
          hipGetLastError(), "seed Bellman-Ford sources");
    }

    int iterations_used = 0;
    int frontier_count = static_cast<int>(unique_sources.size());
    Index* current_frontier = frontier_a;
    Index* next_frontier = frontier_b;
    bool converged = false;
    bool stopped_on_target = std::all_of(
        unique_targets.begin(), unique_targets.end(), [&](int target) {
          return std::find(unique_sources.begin(), unique_sources.end(), target) !=
                 unique_sources.end();
        });

    while (!stopped_on_target && iterations_used < max_iters) {
      hipLaunchKernelGGL(
          bellman_ford_detail::reset_iteration_status_kernel, dim3(1), dim3(1),
          0, stream, iteration_status);
      bellman_ford_detail::check_hip(
          hipGetLastError(), "reset Bellman-Ford iteration status");
      {
        PATHFINDER_PROFILE_RANGE("bellman_ford.relax");
        hipLaunchKernelGGL(
            bellman_ford_detail::relax_frontier_kernel,
            bellman_ford_detail::grid_for_items(frontier_count),
            dim3(bellman_ford_detail::kBlockSize), 0, stream, graph->view(),
            current_frontier, frontier_count, iterations_used + 1, best_state,
            next_frontier, next_marks, source_mask, iteration_status);
        bellman_ford_detail::check_hip(
            hipGetLastError(), "relax Bellman-Ford frontier");
      }
      {
        PATHFINDER_PROFILE_RANGE("bellman_ford.target_check");
        hipLaunchKernelGGL(
            bellman_ford_detail::update_target_status_kernel,
            bellman_ford_detail::grid_for_items(
                static_cast<Offset>(unique_targets.size())),
            dim3(bellman_ford_detail::kBlockSize), 0, stream, best_state,
            target_nodes, static_cast<int>(unique_targets.size()),
            iteration_status);
        bellman_ford_detail::check_hip(
            hipGetLastError(), "check Bellman-Ford targets");
      }
      IterationStatus host_status{};
      bellman_ford_detail::check_hip(
          hipMemcpyAsync(&host_status, iteration_status, sizeof(host_status),
                         hipMemcpyDeviceToHost, stream),
          "copy Bellman-Ford iteration status");
      bellman_ford_detail::check_hip(
          hipStreamSynchronize(stream),
          "synchronize Bellman-Ford iteration");
      ++iterations_used;
      if (host_status.error_status != 0) {
        throw std::runtime_error(
            "Bellman-Ford device traversal reported invalid graph state " +
            std::to_string(host_status.error_status));
      }
      if (progress_callback) {
        const BellmanFordCsrProgress progress{
            iterations_used, max_iters, true, host_status.next_count != 0};
        progress_callback(progress, progress_user_data);
      }
      if (host_status.next_count == 0) {
        converged = true;
        break;
      }
      if (host_status.reached_target_count ==
              static_cast<int>(unique_targets.size()) &&
          host_status.min_next_distance_bits >=
              host_status.max_target_distance_bits) {
        stopped_on_target = true;
        break;
      }
      frontier_count = host_status.next_count;
      std::swap(current_frontier, next_frontier);
    }

    std::vector<TargetSummary> summaries(unique_targets.size());
    {
      PATHFINDER_PROFILE_RANGE("bellman_ford.summarize_paths");
      hipLaunchKernelGGL(
          bellman_ford_detail::summarize_target_paths_kernel,
          bellman_ford_detail::grid_for_items(
              static_cast<Offset>(unique_targets.size())),
          dim3(bellman_ford_detail::kBlockSize), 0, stream, graph->view(),
          best_state, source_mask, target_nodes,
          static_cast<int>(unique_targets.size()), target_summaries);
      bellman_ford_detail::check_hip(
          hipGetLastError(), "summarize Bellman-Ford target paths");
      bellman_ford_detail::check_hip(
          hipMemcpyAsync(summaries.data(), target_summaries,
                         summaries.size() * sizeof(TargetSummary),
                         hipMemcpyDeviceToHost, stream),
          "copy Bellman-Ford target summaries");
      bellman_ford_detail::check_hip(
          hipStreamSynchronize(stream),
          "synchronize Bellman-Ford target summaries");
    }

    std::vector<unsigned long long> host_node_offsets(unique_targets.size());
    std::vector<unsigned long long> host_edge_offsets(unique_targets.size());
    unsigned long long total_nodes = 0;
    unsigned long long total_edges = 0;
    for (std::size_t i = 0; i < summaries.size(); ++i) {
      host_node_offsets[i] = total_nodes;
      host_edge_offsets[i] = total_edges;
      const TargetSummary& summary = summaries[i];
      if (summary.status == bellman_ford_detail::kTargetPathInvalid) {
        throw std::runtime_error(
            "Bellman-Ford produced an invalid or cyclic predecessor chain");
      }
      if (summary.status != bellman_ford_detail::kTargetPathValid) continue;
      if (summary.node_count == 0 ||
          summary.node_count != summary.edge_count + 1 ||
          summary.node_count >
              std::numeric_limits<unsigned long long>::max() - total_nodes ||
          summary.edge_count >
              std::numeric_limits<unsigned long long>::max() - total_edges) {
        throw std::overflow_error("Bellman-Ford compact path size overflow");
      }
      total_nodes += summary.node_count;
      total_edges += summary.edge_count;
    }
    if (total_nodes > static_cast<unsigned long long>(
                          std::numeric_limits<int>::max()) ||
        total_edges > static_cast<unsigned long long>(
                          std::numeric_limits<int>::max())) {
      throw std::overflow_error(
          "Bellman-Ford compact paths exceed result offset capacity");
    }
    const std::size_t compact_node_count =
        static_cast<std::size_t>(total_nodes);
    const std::size_t compact_edge_count =
        static_cast<std::size_t>(total_edges);
    ensure_compact_capacity(compact_node_count, compact_edge_count);

    std::vector<Index> host_compact_nodes(compact_node_count);
    std::vector<DeviceEdge> host_compact_edges(compact_edge_count);
    {
      PATHFINDER_PROFILE_RANGE("bellman_ford.materialize_paths");
      bellman_ford_detail::check_hip(
          hipMemcpyAsync(node_offsets, host_node_offsets.data(),
                         host_node_offsets.size() *
                             sizeof(unsigned long long),
                         hipMemcpyHostToDevice, stream),
          "copy Bellman-Ford target node offsets");
      bellman_ford_detail::check_hip(
          hipMemcpyAsync(edge_offsets, host_edge_offsets.data(),
                         host_edge_offsets.size() *
                             sizeof(unsigned long long),
                         hipMemcpyHostToDevice, stream),
          "copy Bellman-Ford target edge offsets");
      hipLaunchKernelGGL(
          bellman_ford_detail::materialize_target_paths_kernel,
          bellman_ford_detail::grid_for_items(
              static_cast<Offset>(unique_targets.size())),
          dim3(bellman_ford_detail::kBlockSize), 0, stream, graph->view(),
          best_state, target_nodes, target_summaries, node_offsets,
          edge_offsets, static_cast<int>(unique_targets.size()), compact_nodes,
          compact_edges);
      bellman_ford_detail::check_hip(
          hipGetLastError(), "materialize Bellman-Ford target paths");
      if (!host_compact_nodes.empty()) {
        bellman_ford_detail::check_hip(
            hipMemcpyAsync(host_compact_nodes.data(), compact_nodes,
                           host_compact_nodes.size() * sizeof(Index),
                           hipMemcpyDeviceToHost, stream),
            "copy Bellman-Ford compact nodes");
      }
      if (!host_compact_edges.empty()) {
        bellman_ford_detail::check_hip(
            hipMemcpyAsync(host_compact_edges.data(), compact_edges,
                           host_compact_edges.size() * sizeof(DeviceEdge),
                           hipMemcpyDeviceToHost, stream),
            "copy Bellman-Ford compact edges");
      }
      bellman_ford_detail::check_hip(
          hipStreamSynchronize(stream),
          "synchronize Bellman-Ford compact paths");
    }

    std::unordered_map<int, std::size_t> unique_target_index;
    unique_target_index.reserve(unique_targets.size() * 2 + 1);
    for (std::size_t i = 0; i < unique_targets.size(); ++i) {
      unique_target_index.emplace(unique_targets[i], i);
    }

    BellmanFordCsrResult result;
    result.iterations_used = iterations_used;
    result.converged = converged;
    result.stopped_on_target = stopped_on_target;
    result.target_reached = true;
    result.target_distances.reserve(targets.size());
    result.target_sources.reserve(targets.size());
    result.target_path_offsets.reserve(targets.size() + 1);
    result.target_edge_offsets.reserve(targets.size() + 1);
    result.target_path_offsets.push_back(0);
    result.target_edge_offsets.push_back(0);
    for (const int target : targets) {
      const std::size_t unique = unique_target_index.at(target);
      const TargetSummary& summary = summaries[unique];
      if (summary.status == bellman_ford_detail::kTargetUnreachable) {
        result.target_distances.push_back(
            std::numeric_limits<float>::infinity());
        result.target_sources.push_back(-1);
        result.target_reached = false;
      } else {
        const std::size_t node_begin =
            static_cast<std::size_t>(host_node_offsets[unique]);
        const std::size_t edge_begin =
            static_cast<std::size_t>(host_edge_offsets[unique]);
        const std::size_t node_end =
            node_begin + static_cast<std::size_t>(summary.node_count);
        const std::size_t edge_end =
            edge_begin + static_cast<std::size_t>(summary.edge_count);
        if (node_end > host_compact_nodes.size() ||
            edge_end > host_compact_edges.size() ||
            host_compact_nodes[node_begin] != summary.root ||
            host_compact_nodes[node_end - 1] != target) {
          throw std::runtime_error(
              "Bellman-Ford compact target path is malformed");
        }
        result.target_distances.push_back(
            bellman_ford_detail::host_state_distance(summary.state));
        result.target_sources.push_back(summary.root);
        result.target_path_nodes.insert(
            result.target_path_nodes.end(),
            host_compact_nodes.begin() +
                static_cast<std::ptrdiff_t>(node_begin),
            host_compact_nodes.begin() +
                static_cast<std::ptrdiff_t>(node_end));
        for (std::size_t edge = edge_begin; edge < edge_end; ++edge) {
          result.target_path_edges.push_back(
              static_cast<Offset>(host_compact_edges[edge]));
        }
      }
      result.target_path_offsets.push_back(
          static_cast<int>(result.target_path_nodes.size()));
      result.target_edge_offsets.push_back(
          static_cast<int>(result.target_path_edges.size()));
    }
    return result;
  }

  std::shared_ptr<const BellmanFordCsrGraph::Impl> graph;
  hipStream_t stream = nullptr;
  int device = -1;
  unsigned long long* best_state = nullptr;
  Index* frontier_a = nullptr;
  Index* frontier_b = nullptr;
  int* next_marks = nullptr;
  unsigned char* source_mask = nullptr;
  IterationStatus* iteration_status = nullptr;
  Index* source_nodes = nullptr;
  std::size_t source_capacity = 0;
  Index* target_nodes = nullptr;
  TargetSummary* target_summaries = nullptr;
  unsigned long long* node_offsets = nullptr;
  unsigned long long* edge_offsets = nullptr;
  std::size_t target_capacity = 0;
  Index* compact_nodes = nullptr;
  DeviceEdge* compact_edges = nullptr;
  std::size_t compact_node_capacity = 0;
  std::size_t compact_edge_capacity = 0;
  std::mutex operation_mutex;
};

BellmanFordCsrWorkspace::BellmanFordCsrWorkspace(
    const HostCsrF32& adjacency,
    hipStream_t stream,
    BellmanFordCsrWorkspaceOptions options)
    : impl_(std::make_unique<Impl>(
          std::make_shared<BellmanFordCsrGraph::Impl>(adjacency, stream),
          stream, options)) {}

BellmanFordCsrWorkspace::BellmanFordCsrWorkspace(
    std::shared_ptr<const BellmanFordCsrGraph> adjacency,
    hipStream_t stream,
    BellmanFordCsrWorkspaceOptions options)
    : impl_(std::make_unique<Impl>(Impl::require_graph(adjacency), stream,
                                   options)) {}

BellmanFordCsrWorkspace::~BellmanFordCsrWorkspace() = default;
BellmanFordCsrWorkspace::BellmanFordCsrWorkspace(
    BellmanFordCsrWorkspace&&) noexcept = default;
BellmanFordCsrWorkspace& BellmanFordCsrWorkspace::operator=(
    BellmanFordCsrWorkspace&&) noexcept = default;

BellmanFordCsrResult BellmanFordCsrWorkspace::run(
    const std::vector<int>& sources,
    const std::vector<int>& targets,
    int max_iters,
    hipStream_t stream,
    BellmanFordCsrProgressCallback progress_callback,
    void* progress_user_data) {
  if (!impl_) {
    throw std::runtime_error("Bellman-Ford workspace has no implementation");
  }
  std::lock_guard<std::mutex> lock(impl_->operation_mutex);
  impl_->require_stream(stream);
  return impl_->run(sources, targets, max_iters, progress_callback,
                    progress_user_data);
}

BellmanFordCsrResult BellmanFordCsrWorkspace::run(
    const std::vector<int>& sources,
    int target,
    int max_iters,
    hipStream_t stream,
    BellmanFordCsrProgressCallback progress_callback,
    void* progress_user_data) {
  BellmanFordCsrResult result =
      run(sources, std::vector<int>{target}, max_iters, stream,
          progress_callback, progress_user_data);
  result.target = target;
  result.target_distance = result.target_distances.front();
  result.target_reached = std::isfinite(result.target_distance);
  return result;
}

BellmanFordCsrResult BellmanFordCsrWorkspace::run(
    int source,
    int target,
    int max_iters,
    hipStream_t stream,
    BellmanFordCsrProgressCallback progress_callback,
    void* progress_user_data) {
  return run(std::vector<int>{source}, target, max_iters, stream,
             progress_callback, progress_user_data);
}
