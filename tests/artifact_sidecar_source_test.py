#!/usr/bin/env python3
"""Guards the source-level CSR-v3/device-graph-v4 sidecar contract."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def source(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


class ArtifactSidecarSourceTest(unittest.TestCase):
    def test_csr_v3_writes_complete_sidecar_payload(self) -> None:
        text = source("pre-process/interchange_to_csr.cpp")
        self.assertRegex(
            text, r"constexpr\s+std::uint64_t\s+CSR_FORMAT_VERSION\s*=\s*3\s*;"
        )
        required_in_order = (
            "write_u64(out, CSR_FORMAT_VERSION",
            '"route-end X count"',
            '"route-end Y count"',
            '"base vertex cost count"',
            '"spatial shard offset count"',
            '"spatial shard edge ID count"',
            '"route-end X values"',
            '"route-end Y values"',
            '"base vertex costs"',
            '"spatial shard offsets"',
            '"spatial shard edge IDs"',
        )
        cursor = -1
        for token in required_in_order:
            next_cursor = text.find(token, cursor + 1)
            self.assertGreater(next_cursor, cursor, f"missing/out-of-order token: {token}")
            cursor = next_cursor
        self.assertIn("read_device_routing_graph_for_routing", text)
        self.assertIn("build_destination_spatial_edge_shards", text)
        self.assertIn("validate_destination_spatial_edge_shards", text)

    def test_device_graph_v4_preserves_and_legacy_v3_synthesizes_sidecars(self) -> None:
        text = source("pre-process/device_routing_graph.cpp")
        self.assertRegex(
            text,
            r"constexpr\s+std::uint64_t\s+MIN_DEVICE_GRAPH_VERSION\s*=\s*3\s*;",
        )
        self.assertRegex(
            text, r"constexpr\s+std::uint64_t\s+DEVICE_GRAPH_VERSION\s*=\s*4\s*;"
        )
        for field in (
            "node_route_end_x",
            "node_route_end_y",
            "node_base_vertex_cost",
        ):
            self.assertIn(f"write_array(out, graph.{field}", text)
            self.assertIn(f"read_array(in, graph.{field}", text)
        self.assertIn("if (version >= 4)", text)
        self.assertIn("synthesize_route_sidecars_from_extents(graph)", text)
        self.assertIn("read_legacy_route_sidecars_projection", text)
        self.assertIn("read_device_routing_graph_for_routing", text)
        self.assertIn("return csr;", text)
        self.assertTrue(
            text.rstrip().endswith("}  // namespace routing::interchange"),
            "device-routing graph implementation is truncated",
        )

    def test_pathfinder_v3_loader_and_legacy_bounds_policy(self) -> None:
        loader = source("routing/csr_artifact.cpp")
        pathfinder = source("routing/pathfinder.cpp")
        header = source("routing/pathfinder.hpp")
        self.assertRegex(
            loader,
            r"constexpr\s+std::uint64_t\s+kCurrentCsrVersion\s*=\s*3\s*;",
        )
        required_loader_tokens = (
            'read_u64(in, "CSR route-end x count")',
            'read_u64(in, "CSR route-end y count")',
            'read_u64(in, "CSR base vertex cost count")',
            'read_i64(in, "CSR spatial shard minimum x")',
            'read_i64(in, "CSR spatial shard minimum y")',
            '"CSR routing sidecar counts are inconsistent"',
            '"CSR route-end x coordinates"',
            '"CSR route-end y coordinates"',
            '"CSR base vertex costs"',
            '"CSR spatial shard offsets"',
            '"CSR spatial shard edge IDs"',
            "validate_destination_spatial_edge_shards",
            "validate_routing_csr_sidecars",
        )
        cursor = -1
        for token in required_loader_tokens:
            next_cursor = loader.find(token, cursor + 1)
            self.assertGreater(next_cursor, cursor, f"missing/out-of-order token: {token}")
            cursor = next_cursor

        self.assertIn(
            "if (options.bounds.enabled && !has_any_routing_sidecars)",
            pathfinder,
        )
        self.assertIn(
            "bounded routing requires a CSR v3 artifact with route-end ",
            pathfinder,
        )
        self.assertIn("regenerate the CSR or select ", pathfinder)
        self.assertIn("--unbounded/--bf11-unbounded", pathfinder)
        self.assertIn('#include "csr_artifact.hpp"', header)
        self.assertNotIn("HostCsrF32 load_csrbin(", pathfinder)


if __name__ == "__main__":
    unittest.main()
