#!/usr/bin/env python3

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT / "delta_stepping" / "delta_stepping.cpp").read_text()
HEADER = (ROOT / "delta_stepping" / "delta_stepping.hpp").read_text()


def function_body(source: str, name: str) -> str:
    signature = source.index(name)
    begin = source.index("{", signature)
    depth = 0
    for position in range(begin, len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[begin : position + 1]
    raise AssertionError(f"unterminated function body for {name}")


class DeltaRoutingBoundsSourceTest(unittest.TestCase):
    def test_every_generic_relaxation_path_checks_before_cost(self) -> None:
        functions = (
            "relax_light_edges_kernel",
            "relax_heavy_edges_kernel",
            "cooperative_relax_light_range",
            "cooperative_relax_heavy_range",
        )
        for name in functions:
            with self.subTest(name=name):
                body = function_body(CPP, name)
                destination = body.index("const int v")
                admission = body.index("routing::route_node_admitted")
                effective_cost = body.index("const float effective_w")
                self.assertLess(destination, admission)
                self.assertLess(admission, effective_cost)
                self.assertEqual(body.count("routing::route_node_admitted"), 1)

    def test_shared_graph_owns_host_and_device_coordinate_columns(self) -> None:
        graph_impl = function_body(CPP, "struct DeltaSteppingCsrGraph::Impl")
        self.assertIn("host_route_end_x", graph_impl)
        self.assertIn("host_route_end_y", graph_impl)
        self.assertIn("device_route_end_x", graph_impl)
        self.assertIn("device_route_end_y", graph_impl)
        self.assertIn("routing::validate_coordinate_columns", graph_impl)
        self.assertEqual(graph_impl.count("hipMemcpyAsync"), 2)
        self.assertIn("hipStreamSynchronize", graph_impl)

    def test_bounds_are_per_run_and_default_to_unbounded(self) -> None:
        self.assertIn(
            "routing::RoutingQueryBounds routing_bounds{};", HEADER
        )
        self.assertIn("active_routing_bounds_", HEADER)
        self.assertIn("validate_routing_query", CPP)
        self.assertIn(
            "bounded Delta-Stepping requires routing coordinate sidecars",
            CPP,
        )

    def test_cooperative_controller_receives_same_descriptor(self) -> None:
        args = function_body(CPP, "struct CooperativeDeltaControllerArgs")
        self.assertIn("const std::int32_t* route_end_x", args)
        self.assertIn("const std::int32_t* route_end_y", args)
        self.assertIn("routing::RoutingQueryBounds routing_bounds", args)
        self.assertIn("args.routing_bounds = routing_bounds", CPP)


if __name__ == "__main__":
    unittest.main()
