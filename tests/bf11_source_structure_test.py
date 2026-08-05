#!/usr/bin/env python3
"""CPU-only source guardrails for the HIP-only BF11 implementation."""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "bellman_ford/bf11.cpp"
HEADER = ROOT / "bellman_ford/bf11.hpp"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def function_body(source: str, signature: str, next_signature: str) -> str:
    begin = source.index(signature)
    end = source.index(next_signature, begin)
    return source[begin:end]


def main() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")

    require(
        "using BellmanFord11BoundingBox = routing::RoutingQueryBounds" in header
        and "routing::route_node_admitted" in source
        and "routing::derive_query_bounds" in source
        and "/*allow_missing_bounded_sources=*/true" in source,
        "BF11 is no longer using the shared routing-bounds contract",
    )
    require(
        "using DeviceOffset = std::uint32_t" in source
        and "std::vector<DeviceOffset> compact_rowptr" in source,
        "BF11 no longer compacts 64-bit host row offsets for device storage",
    )

    require(
        "workspace.stream == nullptr ? cooperative_block_count(workspace) : 0"
        in source,
        "explicit BF11 streams can still enter the full-residency cooperative controller",
    )
    require(
        "Index* touched_nodes" in source and "int* touched_count" in source,
        "BF11 workspace lost its sparse touched-state storage",
    )
    require(
        "relaxation.first_discovery" in source
        and "touched_nodes[touched_slot] = dst" in source,
        "first finite BF11 labels are no longer recorded for sparse reset",
    )
    atomic_relax = function_body(
        source,
        "__device__ __forceinline__ AtomicRelaxResult atomic_relax_strict(",
        "__device__ __forceinline__ float effective_edge_weight(",
    )
    require(
        "unsigned long long old_state = coherent_atomic_load(address);"
        in atomic_relax,
        "BF11 first-discovery checks bypass the coherent atomic-load helper",
    )
    require(
        "BF11_FORCE_CAS_ATOMIC_LOAD" in source,
        "BF11 lost the compile-time CAS compatibility override",
    )
    require(
        re.search(
            r"__hip_atomic_load\s*\(\s*address\s*,\s*__ATOMIC_RELAXED\s*,"
            r"\s*__HIP_MEMORY_SCOPE_AGENT\s*\)",
            source,
        )
        is not None,
        "BF11 primary state observation is not a relaxed agent-scope HIP atomic load",
    )
    require(
        "atomicCAS(address, 0ULL, 0ULL)" in source,
        "BF11 lost the proven CAS compatibility load",
    )
    coherent_load = function_body(
        source,
        "__device__ __forceinline__ unsigned long long coherent_atomic_load(",
        "__device__ __forceinline__ AtomicRelaxResult atomic_relax_strict(",
    )
    require(
        "BF11_FORCE_CAS_ATOMIC_LOAD" in coherent_load
        and "__hip_atomic_load" in coherent_load
        and "atomicCAS(address, 0ULL, 0ULL)" in coherent_load
        and "#else" in coherent_load,
        "BF11 coherent load does not keep guarded HIP-load and CAS paths",
    )
    require(
        re.search(r"(?:return\s+|=\s*)\*\s*address\b", coherent_load) is None
        and re.search(r"\baddress\s*\[\s*0\s*\]", coherent_load) is None,
        "BF11 coherent state observation regressed to an ordinary cached load",
    )

    telemetry_initialization = function_body(
        source,
        "void initialize_workspace_telemetry(",
        "void begin_telemetry_event(",
    )
    require(
        "hipDeviceAttributeWallClockRate" in telemetry_initialization,
        "BF11 cooperative telemetry does not query the fixed wall-clock rate",
    )
    cooperative_controller = function_body(
        source,
        "__global__ void frontier_controller_kernel(",
        "__global__ void summarize_target_paths_kernel(",
    )
    require(
        "wall_clock64()" in cooperative_controller,
        "BF11 cooperative telemetry does not use the GFX11-safe wall clock",
    )
    require(
        re.search(r"(?<!wall_)clock64\s*\(", source) is None,
        "BF11 telemetry uses bare clock64, which is unreliable on GFX11",
    )
    require(
        re.search(r"\b\w+\s*\.\s*clockRate\b", source) is None,
        "BF11 telemetry converts timer ticks with multiprocessor clockRate",
    )

    host_controller = function_body(
        source, "SsspStatus run_host_controller(", "SsspStatus run_sssp("
    )
    require(
        "clear_touched_state_kernel" in host_controller
        and "clear_state_kernel" not in host_controller,
        "the normal BF11 host controller regressed to a full graph clear",
    )
    host_initialization = host_controller.split("int frontier_count", 1)[0]
    require(
        "hipStreamSynchronize" not in host_initialization,
        "BF11 reintroduced a host round trip between reset, seed, and round 1",
    )
    gpu_controller = function_body(
        source, "__global__ void frontier_controller_kernel(",
        "__global__ void summarize_target_paths_kernel(",
    )
    require(
        "prior_touched_count" in gpu_controller
        and "for (Offset row = thread; row < graph.rows" not in gpu_controller,
        "the persistent BF11 controller regressed to a per-query full graph clear",
    )
    require(
        "needs_full_state_reset = true" in source
        and "fully_reset_workspace_state(workspace)" in source,
        "BF11 lost its defensive dense reset after an exceptional query",
    )
    require(
        "result.target_path_edge_costs.push_back(compact_edge_costs[edge])"
        in source,
        "BF11 no longer returns effective path-edge costs",
    )
    require(
        "g_bf11_auto_unbounded_retries.fetch_add" in source
        and "bounded.target_reached" in source,
        "BF11 lost the single unbounded retry after a bounded miss",
    )

    print("BF11 source-structure policy test passed")


if __name__ == "__main__":
    main()
