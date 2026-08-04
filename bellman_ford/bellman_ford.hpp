#pragma once

#include "../sssp/sssp_query_capacity.hpp"
#include "../sssp/sssp_types.hpp"

#include <hip/hip_runtime.h>

#include <memory>
#include <vector>

using BellmanFordCsrProgress = SsspCsrProgress;
using BellmanFordCsrProgressCallback = SsspCsrProgressCallback;
using BellmanFordCsrResult = SsspCsrResult;

struct BellmanFordCsrWorkspaceOptions {
  SsspQueryCapacityHints capacity_hints{};
};

// Immutable outgoing CSR uploaded once and shared by independent worker
// workspaces. Runs are unbounded and use each CSR edge value directly (the
// compatibility path's vertex-cost multiplier is 1). The graph and its
// workspaces are affine to the HIP device current at construction time.
class BellmanFordCsrGraph {
 public:
  struct Impl;

  explicit BellmanFordCsrGraph(const HostCsrF32& adjacency,
                               hipStream_t stream = nullptr);
  ~BellmanFordCsrGraph();

  BellmanFordCsrGraph(const BellmanFordCsrGraph&) = delete;
  BellmanFordCsrGraph& operator=(const BellmanFordCsrGraph&) = delete;
  BellmanFordCsrGraph(BellmanFordCsrGraph&&) noexcept;
  BellmanFordCsrGraph& operator=(BellmanFordCsrGraph&&) noexcept;

 private:
  std::shared_ptr<const Impl> impl_;
  friend class BellmanFordCsrWorkspace;
};

// Reusable, stream-affine active-frontier Bellman-Ford workspace. Vector
// target runs return compact target paths and original CSR edge IDs without
// copying full graph-sized predecessor arrays back to the host.
class BellmanFordCsrWorkspace {
 public:
  struct Impl;

  explicit BellmanFordCsrWorkspace(
      const HostCsrF32& adjacency,
      hipStream_t stream = nullptr,
      BellmanFordCsrWorkspaceOptions options = {});
  explicit BellmanFordCsrWorkspace(
      std::shared_ptr<const BellmanFordCsrGraph> adjacency,
      hipStream_t stream = nullptr,
      BellmanFordCsrWorkspaceOptions options = {});
  ~BellmanFordCsrWorkspace();

  BellmanFordCsrWorkspace(const BellmanFordCsrWorkspace&) = delete;
  BellmanFordCsrWorkspace& operator=(const BellmanFordCsrWorkspace&) = delete;
  BellmanFordCsrWorkspace(BellmanFordCsrWorkspace&&) noexcept;
  BellmanFordCsrWorkspace& operator=(BellmanFordCsrWorkspace&&) noexcept;

  BellmanFordCsrResult run(
      const std::vector<int>& sources,
      const std::vector<int>& targets,
      int max_iters = -1,
      hipStream_t stream = nullptr,
      BellmanFordCsrProgressCallback progress_callback = nullptr,
      void* progress_user_data = nullptr);

  BellmanFordCsrResult run(
      const std::vector<int>& sources,
      int target,
      int max_iters = -1,
      hipStream_t stream = nullptr,
      BellmanFordCsrProgressCallback progress_callback = nullptr,
      void* progress_user_data = nullptr);

  BellmanFordCsrResult run(
      int source,
      int target,
      int max_iters = -1,
      hipStream_t stream = nullptr,
      BellmanFordCsrProgressCallback progress_callback = nullptr,
      void* progress_user_data = nullptr);

 private:
  std::unique_ptr<Impl> impl_;
};
