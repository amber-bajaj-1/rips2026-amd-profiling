/* Focused BF11/Delta-Stepping routing-bounds parity test (AMD HIP GPU).
   Build from the repository root:
   hipcc -std=c++17 -O2 -x hip -DBF11_NO_MAIN -I. \
     tests/routing_engines_bounds_hip_test.cpp \
     delta_stepping/delta_stepping.cpp bellman_ford/bf11.cpp \
     -pthread -o /tmp/rips-routing-engines-bounds-hip-test
   Run on a ROCm host with a visible AMD GPU:
   /tmp/rips-routing-engines-bounds-hip-test
*/

#include "../bellman_ford/bf11.hpp"
#include "../delta_stepping/delta_stepping.hpp"
#include "../routing/route_policy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ri = routing::interchange;
using Offset = minplus_sparse::Offset;

namespace {

constexpr float kInfinity = std::numeric_limits<float>::infinity();

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void check_hip(hipError_t status, const char* operation) {
  if (status != hipSuccess) {
    throw std::runtime_error(std::string(operation) + ": " +
                             hipGetErrorString(status));
  }
}

class HipStream {
 public:
  HipStream() {
    check_hip(hipStreamCreateWithFlags(&stream_, hipStreamNonBlocking),
              "create test stream");
  }

  ~HipStream() {
    if (stream_ != nullptr) (void)hipStreamDestroy(stream_);
  }

  HipStream(const HipStream&) = delete;
  HipStream& operator=(const HipStream&) = delete;

  hipStream_t get() const noexcept { return stream_; }

 private:
  hipStream_t stream_ = nullptr;
};

template <typename First, typename Second>
void run_concurrently_on_device(int device, First&& first, Second&& second) {
  std::exception_ptr first_error;
  std::exception_ptr second_error;
  std::thread first_thread([&] {
    try {
      check_hip(hipSetDevice(device), "select first worker HIP device");
      first();
    } catch (...) {
      first_error = std::current_exception();
    }
  });
  std::thread second_thread([&] {
    try {
      check_hip(hipSetDevice(device), "select second worker HIP device");
      second();
    } catch (...) {
      second_error = std::current_exception();
    }
  });
  first_thread.join();
  second_thread.join();
  if (first_error) std::rethrow_exception(first_error);
  if (second_error) std::rethrow_exception(second_error);
}

bool close_enough(float expected, float actual) {
  if (std::isinf(expected) || std::isinf(actual)) {
    return std::isinf(expected) && std::isinf(actual) &&
           std::signbit(expected) == std::signbit(actual);
  }
  if (!std::isfinite(expected) || !std::isfinite(actual)) return false;
  const float scale =
      std::max({1.0f, std::fabs(expected), std::fabs(actual)});
  return std::fabs(expected - actual) <= 1e-5f * scale;
}

struct EdgeSpec {
  int from = -1;
  int to = -1;
  float cost = 0.0f;
};

HostCsrF32 make_graph(int vertex_count, const std::vector<EdgeSpec>& edges) {
  require(vertex_count > 0, "test graph must contain a vertex");
  HostCsrF32 graph;
  graph.rows = vertex_count;
  graph.cols = vertex_count;
  graph.nnz = static_cast<Offset>(edges.size());
  graph.rowptr.assign(static_cast<std::size_t>(vertex_count) + 1, 0);
  for (const EdgeSpec& edge : edges) {
    require(edge.from >= 0 && edge.from < vertex_count && edge.to >= 0 &&
                edge.to < vertex_count,
            "test edge endpoint lies outside its graph");
    require(std::isfinite(edge.cost) && edge.cost >= 0.0f,
            "test edge cost must be finite and nonnegative");
    ++graph.rowptr[static_cast<std::size_t>(edge.from) + 1];
  }
  for (int vertex = 0; vertex < vertex_count; ++vertex) {
    graph.rowptr[static_cast<std::size_t>(vertex + 1)] +=
        graph.rowptr[static_cast<std::size_t>(vertex)];
  }
  graph.colind.resize(edges.size());
  graph.values.resize(edges.size());
  std::vector<Offset> cursor = graph.rowptr;
  for (const EdgeSpec& edge : edges) {
    const std::size_t row = static_cast<std::size_t>(edge.from);
    const std::size_t position = static_cast<std::size_t>(cursor[row]++);
    graph.colind[position] = edge.to;
    graph.values[position] = edge.cost;
  }
  return graph;
}

ri::RoutingCsrSidecars make_sidecars(
    std::vector<std::int32_t> x,
    std::vector<std::int32_t> y,
    std::vector<float> base_vertex_cost = {}) {
  require(x.size() == y.size(),
          "test coordinate columns must have equal lengths");
  if (base_vertex_cost.empty()) base_vertex_cost.assign(x.size(), 1.0f);
  require(base_vertex_cost.size() == x.size(),
          "test base-cost column must match its coordinates");
  ri::RoutingCsrSidecars sidecars;
  sidecars.route_end_x = std::move(x);
  sidecars.route_end_y = std::move(y);
  sidecars.base_vertex_cost = std::move(base_vertex_cost);
  return sidecars;
}

bool node_admitted(const ri::RoutingCsrSidecars& sidecars,
                   int node,
                   const routing::RoutingQueryBounds& bounds) {
  return routing::route_node_admitted(sidecars.route_end_x.data(),
                                      sidecars.route_end_y.data(), node,
                                      bounds);
}

std::vector<float> cpu_distances(
    const HostCsrF32& graph,
    const ri::RoutingCsrSidecars& sidecars,
    const std::vector<int>& sources,
    const routing::RoutingQueryBounds& bounds) {
  std::vector<float> distance(static_cast<std::size_t>(graph.rows),
                              kInfinity);
  using QueueItem = std::pair<float, int>;
  std::priority_queue<QueueItem, std::vector<QueueItem>,
                      std::greater<QueueItem>>
      queue;
  for (const int source : sources) {
    if (distance[static_cast<std::size_t>(source)] != 0.0f) {
      distance[static_cast<std::size_t>(source)] = 0.0f;
      queue.push({0.0f, source});
    }
  }
  while (!queue.empty()) {
    const auto [from_distance, from] = queue.top();
    queue.pop();
    if (from_distance != distance[static_cast<std::size_t>(from)]) continue;
    for (Offset edge = graph.rowptr[static_cast<std::size_t>(from)];
         edge < graph.rowptr[static_cast<std::size_t>(from + 1)]; ++edge) {
      const int to = graph.colind[static_cast<std::size_t>(edge)];
      if (!node_admitted(sidecars, to, bounds)) continue;
      const float candidate =
          from_distance + graph.values[static_cast<std::size_t>(edge)];
      float& current = distance[static_cast<std::size_t>(to)];
      if (candidate < current) {
        current = candidate;
        queue.push({candidate, to});
      }
    }
  }
  return distance;
}

bool contains(const std::vector<int>& values, int value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

void validate_compact_paths(
    const std::string& label,
    const HostCsrF32& graph,
    const ri::RoutingCsrSidecars& sidecars,
    const std::vector<int>& sources,
    const std::vector<int>& targets,
    const routing::RoutingQueryBounds& bounds,
    const SsspCsrResult& result) {
  const std::vector<float> expected =
      cpu_distances(graph, sidecars, sources, bounds);
  require(result.target_distances.size() == targets.size() &&
              result.target_sources.size() == targets.size() &&
              result.target_path_offsets.size() == targets.size() + 1 &&
              result.target_edge_offsets.size() == targets.size() + 1,
          label + ": compact target arrays have inconsistent sizes");
  require(result.target_path_offsets.front() == 0 &&
              result.target_edge_offsets.front() == 0,
          label + ": compact target offsets do not begin at zero");
  require(result.target_path_edge_costs.empty() ||
              result.target_path_edge_costs.size() ==
                  result.target_path_edges.size(),
          label + ": effective edge costs do not align with path edges");

  bool all_reached = true;
  for (std::size_t target_index = 0; target_index < targets.size();
       ++target_index) {
    const int target = targets[target_index];
    const float expected_distance =
        expected[static_cast<std::size_t>(target)];
    const float actual_distance = result.target_distances[target_index];
    const int node_begin = result.target_path_offsets[target_index];
    const int node_end = result.target_path_offsets[target_index + 1];
    const int edge_begin = result.target_edge_offsets[target_index];
    const int edge_end = result.target_edge_offsets[target_index + 1];
    require(node_begin >= 0 && node_end >= node_begin && edge_begin >= 0 &&
                edge_end >= edge_begin &&
                static_cast<std::size_t>(node_end) <=
                    result.target_path_nodes.size() &&
                static_cast<std::size_t>(edge_end) <=
                    result.target_path_edges.size(),
            label + ": compact target slice is invalid");

    if (!std::isfinite(expected_distance)) {
      all_reached = false;
      require(std::isinf(actual_distance) &&
                  result.target_sources[target_index] == -1 &&
                  node_begin == node_end && edge_begin == edge_end,
              label + ": unreachable target contains finite path data");
      continue;
    }

    require(close_enough(expected_distance, actual_distance),
            label + ": target distance differs from bounded CPU Dijkstra");
    require(node_end - node_begin == edge_end - edge_begin + 1 &&
                node_end > node_begin,
            label + ": reached path has inconsistent node/edge counts");
    const int root = result.target_sources[target_index];
    require(contains(sources, root) &&
                result.target_path_nodes[static_cast<std::size_t>(node_begin)] ==
                    root &&
                result.target_path_nodes[
                    static_cast<std::size_t>(node_end - 1)] == target,
            label + ": compact path has the wrong root or target");

    float path_cost = 0.0f;
    for (int position = edge_begin; position < edge_end; ++position) {
      const int path_offset = position - edge_begin;
      const int from = result.target_path_nodes[
          static_cast<std::size_t>(node_begin + path_offset)];
      const int to = result.target_path_nodes[
          static_cast<std::size_t>(node_begin + path_offset + 1)];
      const Offset edge =
          result.target_path_edges[static_cast<std::size_t>(position)];
      require(edge >= graph.rowptr[static_cast<std::size_t>(from)] &&
                  edge < graph.rowptr[static_cast<std::size_t>(from + 1)] &&
                  graph.colind[static_cast<std::size_t>(edge)] == to,
              label + ": returned path edge is not an original CSR edge");
      require(node_admitted(sidecars, to, bounds),
              label + ": path traverses a known node outside its box");
      const float edge_cost = graph.values[static_cast<std::size_t>(edge)];
      if (!result.target_path_edge_costs.empty()) {
        require(close_enough(
                    edge_cost,
                    result.target_path_edge_costs[
                        static_cast<std::size_t>(position)]),
                label + ": reported effective edge cost is incorrect");
      }
      path_cost += edge_cost;
    }
    require(close_enough(expected_distance, path_cost) &&
                close_enough(actual_distance, path_cost),
            label + ": path edges do not sum to the target distance");
  }

  require(static_cast<std::size_t>(result.target_path_offsets.back()) ==
                  result.target_path_nodes.size() &&
              static_cast<std::size_t>(result.target_edge_offsets.back()) ==
                  result.target_path_edges.size(),
          label + ": final compact offsets do not match storage");
  require(result.target_reached == all_reached,
          label + ": aggregate reachability is inconsistent");
}

void require_engine_agreement(const std::string& label,
                              const SsspCsrResult& bf11,
                              const SsspCsrResult& delta) {
  require(bf11.target_distances.size() == delta.target_distances.size(),
          label + ": target counts differ between engines");
  for (std::size_t i = 0; i < bf11.target_distances.size(); ++i) {
    require(close_enough(bf11.target_distances[i],
                         delta.target_distances[i]),
            label + ": target distances differ between engines");
  }
  require(bf11.target_sources == delta.target_sources &&
              bf11.target_path_nodes == delta.target_path_nodes,
          label + ": unique compact paths differ between engines");
}

void test_shared_bounds_and_workspace_reuse() {
  // Node 2 sits exactly on max_x, node 3 has no physical coordinate, and node
  // 4 is known to be outside max_y. The bounded route to node 5 therefore has
  // to prove inclusive-boundary and spill admission, while excluding the much
  // cheaper known-outside shortcut. Node 7 is reachable only through node 4,
  // providing a real bounded-miss/unbounded-retry case.
  const HostCsrF32 graph = make_graph(
      8, {{0, 2, 1.0f}, {0, 4, 0.1f}, {0, 6, 5.0f},
          {1, 6, 2.0f}, {2, 3, 1.0f}, {3, 5, 1.0f},
          {4, 5, 0.1f}, {4, 7, 0.2f}});
  const ri::RoutingCsrSidecars sidecars = make_sidecars(
      {0, 0, 3, ri::kMissingRouteCoordinate, 1, 3, 2, 3},
      {0, 1, 0, ri::kMissingRouteCoordinate, 9, 1, 1, 1});
  const routing::RoutingQueryBounds box{true, 0, 3, 0, 1};
  const std::vector<int> sources = {0, 1};
  const std::vector<int> targets = {5, 6, 5};

  auto bf_graph =
      std::make_shared<BellmanFord11CsrGraph>(graph, sidecars, nullptr);
  BellmanFord11CsrWorkspace bf_workspace(bf_graph, nullptr);
  auto delta_graph = std::make_shared<DeltaSteppingCsrGraph>(
      graph, sidecars.route_end_x, sidecars.route_end_y, nullptr);
  DeltaSteppingCsrWorkspace delta_workspace(delta_graph, nullptr);

  BellmanFord11RunOptions bf_bounded_options;
  bf_bounded_options.bounds = box;
  DeltaSteppingCsrRunOptions delta_bounded_options;
  delta_bounded_options.routing_bounds = box;

  const SsspCsrResult bf_bounded = bf_workspace.run(
      sources, targets, 1.0f, -1, bf_bounded_options,
      nullptr, nullptr, nullptr);
  const SsspCsrResult delta_bounded = delta_workspace.run(
      sources, targets, 1.0f, -1, delta_bounded_options,
      nullptr, nullptr, nullptr);
  validate_compact_paths("BF11 bounded parity", graph, sidecars, sources,
                         targets, box, bf_bounded);
  validate_compact_paths("Delta bounded parity", graph, sidecars, sources,
                         targets, box, delta_bounded);
  require(routing::sssp_result_certified(bf_bounded) &&
              routing::sssp_result_certified(delta_bounded),
          "bounded parity query was not certified by both engines");
  require_engine_agreement("bounded parity", bf_bounded, delta_bounded);
  require(close_enough(3.0f, bf_bounded.target_distances[0]) &&
              bf_bounded.target_path_nodes ==
                  std::vector<int>({0, 2, 3, 5, 1, 6, 0, 2, 3, 5}),
          "bounded route did not include its boundary/spill path or duplicate");
  require(!contains(bf_bounded.target_path_nodes, 4),
          "bounded route traversed the known-outside shortcut");

  const routing::RoutingQueryBounds unbounded;
  const SsspCsrResult bf_unbounded = bf_workspace.run(
      sources, targets, 1.0f, -1, BellmanFord11RunOptions{},
      nullptr, nullptr, nullptr);
  const SsspCsrResult delta_unbounded = delta_workspace.run(
      sources, targets, 1.0f, -1, DeltaSteppingCsrRunOptions{},
      nullptr, nullptr, nullptr);
  validate_compact_paths("BF11 unbounded reuse", graph, sidecars, sources,
                         targets, unbounded, bf_unbounded);
  validate_compact_paths("Delta unbounded reuse", graph, sidecars, sources,
                         targets, unbounded, delta_unbounded);
  require_engine_agreement("unbounded reuse", bf_unbounded,
                           delta_unbounded);
  require(close_enough(0.2f, bf_unbounded.target_distances[0]) &&
              contains(bf_unbounded.target_path_nodes, 4),
          "unbounded query did not recover the known-outside shortcut");

  const SsspCsrResult bf_bounded_again = bf_workspace.run(
      sources, targets, 1.0f, -1, bf_bounded_options,
      nullptr, nullptr, nullptr);
  const SsspCsrResult delta_bounded_again = delta_workspace.run(
      sources, targets, 1.0f, -1, delta_bounded_options,
      nullptr, nullptr, nullptr);
  require(bf_bounded_again.target_path_nodes == bf_bounded.target_path_nodes &&
              delta_bounded_again.target_path_nodes ==
                  delta_bounded.target_path_nodes,
          "unbounded query state leaked into later bounded workspace reuse");

  const std::vector<int> retry_targets = {7};
  int bf_calls = 0;
  const routing::CertifiedSsspOutcome bf_recovered =
      routing::run_with_optional_unbounded_fallback(
          box, true, retry_targets.size(),
          [&](const routing::RoutingQueryBounds& query_bounds) {
            ++bf_calls;
            BellmanFord11RunOptions options;
            options.bounds = query_bounds;
            return bf_workspace.run(sources, retry_targets, 1.0f, -1,
                                    options, nullptr, nullptr, nullptr);
          });
  int delta_calls = 0;
  const routing::CertifiedSsspOutcome delta_recovered =
      routing::run_with_optional_unbounded_fallback(
          box, true, retry_targets.size(),
          [&](const routing::RoutingQueryBounds& query_bounds) {
            ++delta_calls;
            DeltaSteppingCsrRunOptions options;
            options.routing_bounds = query_bounds;
            return delta_workspace.run(sources, retry_targets, 1.0f, -1,
                                       options, nullptr, nullptr, nullptr);
          });
  require(bf_calls == 2 && delta_calls == 2 &&
              bf_recovered.used_unbounded_retry &&
              delta_recovered.used_unbounded_retry,
          "bounded miss did not trigger exactly one retry for each engine");
  validate_compact_paths("BF11 recovered fallback", graph, sidecars, sources,
                         retry_targets, unbounded, bf_recovered.result);
  validate_compact_paths("Delta recovered fallback", graph, sidecars, sources,
                         retry_targets, unbounded, delta_recovered.result);
  require(routing::sssp_result_certified(bf_recovered.result) &&
              routing::sssp_result_certified(delta_recovered.result),
          "fallback result was not certified by both engines");
  require_engine_agreement("fallback recovery", bf_recovered.result,
                           delta_recovered.result);

  const SsspCsrResult bf_miss_again = bf_workspace.run(
      sources, retry_targets, 1.0f, -1, bf_bounded_options,
      nullptr, nullptr, nullptr);
  const SsspCsrResult delta_miss_again = delta_workspace.run(
      sources, retry_targets, 1.0f, -1, delta_bounded_options,
      nullptr, nullptr, nullptr);
  validate_compact_paths("BF11 bounded miss after fallback", graph, sidecars,
                         sources, retry_targets, box, bf_miss_again);
  validate_compact_paths("Delta bounded miss after fallback", graph, sidecars,
                         sources, retry_targets, box, delta_miss_again);
  require(!bf_miss_again.target_reached && !delta_miss_again.target_reached &&
              routing::sssp_result_certified(bf_miss_again) &&
              routing::sssp_result_certified(delta_miss_again),
          "fallback state leaked or an exhausted bounded miss was uncertified");
}

void test_non_unit_base_effective_costs() {
  // The current focused PathFinder preprocessor emits unit base costs. Keep a
  // low-level non-unit regression so the original BF11 formula remains
  // authoritative when resource-specific base costs are populated:
  //   effective(edge u->v) = raw_edge_cost * base_vertex_cost[v]
  //                           * dynamic_vertex_cost[v]
  // Dynamic BF11 costs stay at their default 1 here. Delta receives the same
  // base column as its destination-cost vector, making the traversals exactly
  // comparable without changing either engine's immutable raw CSR.
  const HostCsrF32 graph = make_graph(
      4, {{0, 1, 1.0f}, {0, 2, 1.0f}, {1, 3, 1.0f}, {2, 3, 2.0f}});
  const ri::RoutingCsrSidecars sidecars = make_sidecars(
      {0, 1, 1, 2}, {0, 0, 1, 0}, {1.0f, 3.0f, 0.5f, 2.0f});
  const routing::RoutingQueryBounds box{true, 0, 2, 0, 1};
  const std::vector<int> sources = {0};
  const std::vector<int> targets = {3};

  auto bf_graph =
      std::make_shared<BellmanFord11CsrGraph>(graph, sidecars, nullptr);
  BellmanFord11CsrWorkspace bf_workspace(bf_graph, nullptr);
  auto delta_graph = std::make_shared<DeltaSteppingCsrGraph>(
      graph, sidecars.route_end_x, sidecars.route_end_y, nullptr);
  DeltaSteppingCsrWorkspace delta_workspace(delta_graph, nullptr);
  delta_workspace.update_vertex_costs(sidecars.base_vertex_cost, nullptr);

  BellmanFord11RunOptions bf_options;
  bf_options.bounds = box;
  DeltaSteppingCsrRunOptions delta_options;
  delta_options.routing_bounds = box;
  const SsspCsrResult bf_result = bf_workspace.run(
      sources, targets, 1.0f, -1, bf_options, nullptr, nullptr, nullptr);
  const SsspCsrResult delta_result = delta_workspace.run(
      sources, targets, 1.0f, -1, delta_options,
      nullptr, nullptr, nullptr);

  require(routing::sssp_result_certified(bf_result) &&
              routing::sssp_result_certified(delta_result),
          "non-unit effective-cost result was not certified");
  require(bf_result.target_reached && delta_result.target_reached &&
              bf_result.target_distances.size() == 1 &&
              delta_result.target_distances.size() == 1 &&
              close_enough(4.5f, bf_result.target_distances[0]) &&
              close_enough(4.5f, delta_result.target_distances[0]),
          "destination multipliers produced the wrong target distance");
  require(bf_result.target_path_nodes == std::vector<int>({0, 2, 3}) &&
              delta_result.target_path_nodes ==
                  std::vector<int>({0, 2, 3}),
          "non-unit destination costs did not replace the raw shortest arm");
  require(bf_result.target_path_edges.size() == 2 &&
              bf_result.target_path_edge_costs.size() == 2 &&
              close_enough(0.5f, bf_result.target_path_edge_costs[0]) &&
              close_enough(4.0f, bf_result.target_path_edge_costs[1]),
          "BF11 target_path_edge_costs do not equal raw*base*dynamic");
  require(delta_result.target_path_edges.size() == 2,
          "Delta non-unit route has the wrong edge count");

  float delta_path_cost = 0.0f;
  for (const Offset edge : delta_result.target_path_edges) {
    require(edge >= 0 && static_cast<std::size_t>(edge) < graph.values.size(),
            "Delta effective-cost path contains an invalid edge ID");
    const int destination = graph.colind[static_cast<std::size_t>(edge)];
    delta_path_cost +=
        graph.values[static_cast<std::size_t>(edge)] *
        sidecars.base_vertex_cost[static_cast<std::size_t>(destination)];
  }
  require(close_enough(4.5f, delta_path_cost),
          "Delta destination-cost path does not sum to its target distance");
  if (!delta_result.target_path_edge_costs.empty()) {
    require(delta_result.target_path_edge_costs.size() == 2 &&
                close_enough(0.5f,
                             delta_result.target_path_edge_costs[0]) &&
                close_enough(4.0f,
                             delta_result.target_path_edge_costs[1]),
            "Delta optional effective edge costs differ from BF11");
  }
  require_engine_agreement("non-unit effective costs", bf_result,
                           delta_result);
}

void test_parallel_explicit_stream_workspaces() {
  // Two disjoint chains make the expected state of each workspace obvious.
  // Both engines share one immutable graph per engine, while every stream has
  // private mutable search storage. A second, swapped round detects state that
  // leaks either between peer workspaces or between sequential queries.
  const HostCsrF32 graph = make_graph(
      6, {{0, 1, 1.0f}, {1, 2, 1.0f}, {3, 4, 2.0f}, {4, 5, 2.0f}});
  const ri::RoutingCsrSidecars sidecars =
      make_sidecars({0, 1, 2, 3, 4, 5}, {0, 0, 0, 0, 0, 0});
  const routing::RoutingQueryBounds box{true, 0, 5, 0, 0};
  const std::vector<int> first_sources = {0};
  const std::vector<int> first_targets = {2};
  const std::vector<int> second_sources = {3};
  const std::vector<int> second_targets = {5};

  auto bf_graph =
      std::make_shared<BellmanFord11CsrGraph>(graph, sidecars, nullptr);
  auto delta_graph = std::make_shared<DeltaSteppingCsrGraph>(
      graph, sidecars.route_end_x, sidecars.route_end_y, nullptr);
  int device = -1;
  check_hip(hipGetDevice(&device), "get parallel-test HIP device");
  HipStream stream_a;
  HipStream stream_b;

  {
    BellmanFord11CsrWorkspace bf_a(bf_graph, stream_a.get());
    BellmanFord11CsrWorkspace bf_b(bf_graph, stream_b.get());
    DeltaSteppingCsrWorkspace delta_a(delta_graph, stream_a.get());
    DeltaSteppingCsrWorkspace delta_b(delta_graph, stream_b.get());
    BellmanFord11RunOptions bf_options;
    bf_options.bounds = box;
    DeltaSteppingCsrRunOptions delta_options;
    delta_options.routing_bounds = box;

    SsspCsrResult bf_first;
    SsspCsrResult bf_second;
    run_concurrently_on_device(
        device,
        [&] {
          bf_first = bf_a.run(first_sources, first_targets, 1.0f, -1,
                              bf_options, stream_a.get(), nullptr, nullptr);
        },
        [&] {
          bf_second = bf_b.run(second_sources, second_targets, 1.0f, -1,
                               bf_options, stream_b.get(), nullptr, nullptr);
        });
    check_hip(hipStreamSynchronize(stream_a.get()),
              "synchronize first BF11 test stream");
    check_hip(hipStreamSynchronize(stream_b.get()),
              "synchronize second BF11 test stream");
    validate_compact_paths("BF11 parallel stream A", graph, sidecars,
                           first_sources, first_targets, box, bf_first);
    validate_compact_paths("BF11 parallel stream B", graph, sidecars,
                           second_sources, second_targets, box, bf_second);
    require(routing::sssp_result_certified(bf_first) &&
                routing::sssp_result_certified(bf_second),
            "parallel BF11 results were not certified");

    SsspCsrResult delta_first;
    SsspCsrResult delta_second;
    run_concurrently_on_device(
        device,
        [&] {
          delta_first = delta_a.run(
              first_sources, first_targets, 1.0f, -1, delta_options,
              stream_a.get(), nullptr, nullptr);
        },
        [&] {
          delta_second = delta_b.run(
              second_sources, second_targets, 1.0f, -1, delta_options,
              stream_b.get(), nullptr, nullptr);
        });
    check_hip(hipStreamSynchronize(stream_a.get()),
              "synchronize first Delta test stream");
    check_hip(hipStreamSynchronize(stream_b.get()),
              "synchronize second Delta test stream");
    validate_compact_paths("Delta parallel stream A", graph, sidecars,
                           first_sources, first_targets, box, delta_first);
    validate_compact_paths("Delta parallel stream B", graph, sidecars,
                           second_sources, second_targets, box, delta_second);
    require(routing::sssp_result_certified(delta_first) &&
                routing::sssp_result_certified(delta_second),
            "parallel Delta results were not certified");
    require_engine_agreement("parallel stream A", bf_first, delta_first);
    require_engine_agreement("parallel stream B", bf_second, delta_second);

    SsspCsrResult bf_swapped_a;
    SsspCsrResult bf_swapped_b;
    run_concurrently_on_device(
        device,
        [&] {
          bf_swapped_a = bf_a.run(
              second_sources, second_targets, 1.0f, -1, bf_options,
              stream_a.get(), nullptr, nullptr);
        },
        [&] {
          bf_swapped_b = bf_b.run(first_sources, first_targets, 1.0f, -1,
                                  bf_options, stream_b.get(), nullptr,
                                  nullptr);
        });
    check_hip(hipStreamSynchronize(stream_a.get()),
              "synchronize swapped BF11 stream A");
    check_hip(hipStreamSynchronize(stream_b.get()),
              "synchronize swapped BF11 stream B");

    SsspCsrResult delta_swapped_a;
    SsspCsrResult delta_swapped_b;
    run_concurrently_on_device(
        device,
        [&] {
          delta_swapped_a = delta_a.run(
              second_sources, second_targets, 1.0f, -1, delta_options,
              stream_a.get(), nullptr, nullptr);
        },
        [&] {
          delta_swapped_b = delta_b.run(
              first_sources, first_targets, 1.0f, -1, delta_options,
              stream_b.get(), nullptr, nullptr);
        });
    check_hip(hipStreamSynchronize(stream_a.get()),
              "synchronize swapped Delta stream A");
    check_hip(hipStreamSynchronize(stream_b.get()),
              "synchronize swapped Delta stream B");

    validate_compact_paths("BF11 swapped stream A", graph, sidecars,
                           second_sources, second_targets, box, bf_swapped_a);
    validate_compact_paths("BF11 swapped stream B", graph, sidecars,
                           first_sources, first_targets, box, bf_swapped_b);
    validate_compact_paths("Delta swapped stream A", graph, sidecars,
                           second_sources, second_targets, box,
                           delta_swapped_a);
    validate_compact_paths("Delta swapped stream B", graph, sidecars,
                           first_sources, first_targets, box,
                           delta_swapped_b);
    require(routing::sssp_result_certified(bf_swapped_a) &&
                routing::sssp_result_certified(bf_swapped_b) &&
                routing::sssp_result_certified(delta_swapped_a) &&
                routing::sssp_result_certified(delta_swapped_b),
            "swapped parallel workspace reuse was not certified");
    require(bf_swapped_a.target_path_nodes == bf_second.target_path_nodes &&
                bf_swapped_b.target_path_nodes == bf_first.target_path_nodes &&
                delta_swapped_a.target_path_nodes ==
                    delta_second.target_path_nodes &&
                delta_swapped_b.target_path_nodes ==
                    delta_first.target_path_nodes,
            "parallel peer or prior-query state leaked between workspaces");
    require_engine_agreement("swapped stream A", bf_swapped_a,
                             delta_swapped_a);
    require_engine_agreement("swapped stream B", bf_swapped_b,
                             delta_swapped_b);
  }

  // Workspaces have been destroyed before their affine streams. Synchronize
  // once more so stream destruction cannot race any deferred cleanup.
  check_hip(hipStreamSynchronize(stream_a.get()),
            "final synchronize first test stream");
  check_hip(hipStreamSynchronize(stream_b.get()),
            "final synchronize second test stream");
}

void test_iteration_limit_certificate() {
  // One relaxation round exposes the direct cost-10 target in BF11, but the
  // cost-2 route has not propagated. It is a valid-looking finite path, not a
  // shortest-path certificate. Delta filters the analogous unsettled target.
  const HostCsrF32 graph =
      make_graph(3, {{0, 2, 10.0f}, {0, 1, 1.0f}, {1, 2, 1.0f}});
  const ri::RoutingCsrSidecars sidecars =
      make_sidecars({0, 1, 2}, {0, 0, 0});
  const routing::RoutingQueryBounds box{true, 0, 2, 0, 0};
  const std::vector<int> sources = {0};
  const std::vector<int> targets = {2};

  auto bf_graph =
      std::make_shared<BellmanFord11CsrGraph>(graph, sidecars, nullptr);
  BellmanFord11CsrWorkspace bf_workspace(bf_graph, nullptr);
  auto delta_graph = std::make_shared<DeltaSteppingCsrGraph>(
      graph, sidecars.route_end_x, sidecars.route_end_y, nullptr);
  DeltaSteppingCsrWorkspace delta_workspace(delta_graph, nullptr);
  BellmanFord11RunOptions bf_options;
  bf_options.bounds = box;
  DeltaSteppingCsrRunOptions delta_options;
  delta_options.routing_bounds = box;

  const SsspCsrResult bf_limited = bf_workspace.run(
      sources, targets, 1.0f, 1, bf_options, nullptr, nullptr, nullptr);
  const SsspCsrResult delta_limited = delta_workspace.run(
      sources, targets, 1.0f, 1, delta_options, nullptr, nullptr, nullptr);
  require(!routing::sssp_result_certified(bf_limited) &&
              !routing::sssp_result_certified(delta_limited),
          "iteration-capped result was incorrectly certified");
  require(bf_limited.target_distances.size() == 1 &&
              close_enough(10.0f, bf_limited.target_distances[0]) &&
              bf_limited.target_path_nodes == std::vector<int>({0, 2}) &&
              bf_limited.target_path_edges.size() == 1,
          "BF11 iteration cap no longer exposes the finite tentative-path regression");
  require(delta_limited.target_distances.size() == 1 &&
              !delta_limited.target_reached,
          "Delta iteration cap exposed an unsettled target as reached");

  const SsspCsrResult bf_complete = bf_workspace.run(
      sources, targets, 1.0f, -1, bf_options, nullptr, nullptr, nullptr);
  const SsspCsrResult delta_complete = delta_workspace.run(
      sources, targets, 1.0f, -1, delta_options, nullptr, nullptr, nullptr);
  validate_compact_paths("BF11 reuse after iteration cap", graph, sidecars,
                         sources, targets, box, bf_complete);
  validate_compact_paths("Delta reuse after iteration cap", graph, sidecars,
                         sources, targets, box, delta_complete);
  require(routing::sssp_result_certified(bf_complete) &&
              routing::sssp_result_certified(delta_complete) &&
              close_enough(2.0f, bf_complete.target_distances[0]),
          "full rerun did not recover a certified shortest path");
  require_engine_agreement("iteration-cap recovery", bf_complete,
                           delta_complete);
}

}  // namespace

int main() {
  try {
    test_shared_bounds_and_workspace_reuse();
    test_non_unit_base_effective_costs();
    test_parallel_explicit_stream_workspaces();
    test_iteration_limit_certificate();
    std::cout << "routing engine HIP bounds parity test passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "routing engine HIP bounds parity test failed: "
              << error.what() << '\n';
    return 1;
  }
}
