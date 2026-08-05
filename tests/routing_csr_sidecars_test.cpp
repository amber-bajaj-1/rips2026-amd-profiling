#include "../pre-process/routing_csr_sidecars.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ri = routing::interchange;

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void require_rejected(const std::string& label, Function&& function) {
  bool rejected = false;
  try {
    function();
  } catch (const std::exception&) {
    rejected = true;
  }
  require(rejected, label + " was accepted");
}

const std::vector<std::int32_t>& destinations() {
  static const std::vector<std::int32_t> value = {0, 1, 4, 2, 3, 4, 1};
  return value;
}

ri::RoutingCsrSidecars make_valid_sidecars() {
  ri::RoutingCsrSidecars sidecars;
  sidecars.route_end_x = {10, 11, 10, 12, ri::kMissingRouteCoordinate};
  sidecars.route_end_y = {20, 20, 21, 22, ri::kMissingRouteCoordinate};
  sidecars.base_vertex_cost = {1.0f, 1.25f, 2.0f, 3.5f, 7.0f};
  sidecars.spatial_edges = ri::build_destination_spatial_edge_shards(
      sidecars.route_end_x, sidecars.route_end_y, destinations());
  return sidecars;
}

void test_valid_sidecars_and_sentinel() {
  static_assert(ri::kMissingRouteCoordinate ==
                routing::kMissingRouteCoordinate);
  const ri::RoutingCsrSidecars sidecars = make_valid_sidecars();
  ri::validate_routing_csr_sidecars(sidecars, 5, destinations().size());
  ri::validate_destination_spatial_edge_shards(sidecars, 5, destinations());

  require(ri::has_route_coordinate(0, 0),
          "origin should be a valid route coordinate");
  require(!ri::has_route_coordinate(ri::kMissingRouteCoordinate,
                                    ri::kMissingRouteCoordinate),
          "paired sentinel should represent a missing coordinate");
  require(sidecars.spatial_edges.spill_shard() == 9,
          "known coordinate grid or spill shard changed");

  const auto selected = ri::spatial_shards_for_box(
      sidecars.spatial_edges, {10, 11, 20, 20});
  require(selected == std::vector<std::uint64_t>({0, 1, 9}),
          "inclusive box did not select regular boundary cells plus spill");
}

void test_malformed_node_column_lengths() {
  const ri::RoutingCsrSidecars valid = make_valid_sidecars();
  require_rejected("short route-end X column", [&] {
    auto broken = valid;
    broken.route_end_x.pop_back();
    ri::validate_routing_csr_sidecars(broken, 5, destinations().size());
  });
  require_rejected("long route-end Y column", [&] {
    auto broken = valid;
    broken.route_end_y.push_back(0);
    ri::validate_routing_csr_sidecars(broken, 5, destinations().size());
  });
  require_rejected("short base-cost column", [&] {
    auto broken = valid;
    broken.base_vertex_cost.pop_back();
    ri::validate_routing_csr_sidecars(broken, 5, destinations().size());
  });

  require_rejected("mismatched builder coordinate columns", [&] {
    (void)ri::build_destination_spatial_edge_shards(
        std::vector<std::int32_t>{0}, std::vector<std::int32_t>{}, {});
  });
}

void test_malformed_coordinate_sentinels() {
  const ri::RoutingCsrSidecars valid = make_valid_sidecars();
  require_rejected("missing X with known Y", [&] {
    auto broken = valid;
    broken.route_end_x[0] = ri::kMissingRouteCoordinate;
    ri::validate_routing_csr_sidecars(broken, 5, destinations().size());
  });
  require_rejected("known X with missing Y", [&] {
    auto broken = valid;
    broken.route_end_y[0] = ri::kMissingRouteCoordinate;
    ri::validate_routing_csr_sidecars(broken, 5, destinations().size());
  });
  require_rejected("negative X that is not the sentinel", [&] {
    auto broken = valid;
    broken.route_end_x[0] = -2;
    ri::validate_routing_csr_sidecars(broken, 5, destinations().size());
  });
  require_rejected("negative Y that is not the sentinel", [&] {
    auto broken = valid;
    broken.route_end_y[0] = -2;
    ri::validate_routing_csr_sidecars(broken, 5, destinations().size());
  });

  require_rejected("half-missing builder coordinate", [&] {
    (void)ri::build_destination_spatial_edge_shards(
        std::vector<std::int32_t>{ri::kMissingRouteCoordinate},
        std::vector<std::int32_t>{0}, {});
  });
}

void test_invalid_costs_and_optional_spatial_index() {
  const ri::RoutingCsrSidecars valid = make_valid_sidecars();
  for (const float cost :
       {0.0f, -1.0f, std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN()}) {
    require_rejected("non-positive or non-finite base cost", [&, cost] {
      auto broken = valid;
      broken.base_vertex_cost[0] = cost;
      ri::validate_routing_csr_sidecars(broken, 5, destinations().size());
    });
  }

  auto node_only = valid;
  node_only.spatial_edges = {};
  ri::validate_routing_csr_sidecars(node_only, 5, destinations().size(), false);
  require_rejected("missing required spatial index", [&] {
    ri::validate_routing_csr_sidecars(node_only, 5, destinations().size(), true);
  });
}

void test_malformed_spatial_arrays() {
  const ri::RoutingCsrSidecars valid = make_valid_sidecars();
  require_rejected("inconsistent grid dimensions", [&] {
    auto broken = valid;
    broken.spatial_edges.width = 0;
    ri::validate_routing_csr_sidecars(broken, 5, destinations().size());
  });
  require_rejected("wrong offset count", [&] {
    auto broken = valid;
    broken.spatial_edges.offsets.pop_back();
    ri::validate_routing_csr_sidecars(broken, 5, destinations().size());
  });
  require_rejected("nonzero first offset", [&] {
    auto broken = valid;
    broken.spatial_edges.offsets.front() = 1;
    ri::validate_routing_csr_sidecars(broken, 5, destinations().size());
  });
  require_rejected("nonmonotone offsets", [&] {
    auto broken = valid;
    broken.spatial_edges.offsets[4] = 0;
    broken.spatial_edges.offsets[5] = 0;
    broken.spatial_edges.offsets[6] = 0;
    broken.spatial_edges.offsets[7] = 0;
    broken.spatial_edges.offsets[8] = 0;
    ri::validate_routing_csr_sidecars(broken, 5, destinations().size());
  });
  require_rejected("terminal offset mismatch", [&] {
    auto broken = valid;
    broken.spatial_edges.offsets.back() = destinations().size() - 1;
    ri::validate_routing_csr_sidecars(broken, 5, destinations().size());
  });
  require_rejected("edge-count mismatch", [&] {
    auto broken = valid;
    broken.spatial_edges.edge_ids.pop_back();
    broken.spatial_edges.offsets.back() = broken.spatial_edges.edge_ids.size();
    ri::validate_routing_csr_sidecars(broken, 5, destinations().size());
  });
  require_rejected("out-of-range edge ID", [&] {
    auto broken = valid;
    broken.spatial_edges.edge_ids[0] =
        static_cast<std::uint32_t>(destinations().size());
    ri::validate_routing_csr_sidecars(broken, 5, destinations().size());
  });
  require_rejected("duplicate edge ID", [&] {
    auto broken = valid;
    broken.spatial_edges.edge_ids[0] = broken.spatial_edges.edge_ids[1];
    ri::validate_routing_csr_sidecars(broken, 5, destinations().size());
  });

  require_rejected("semantically misplaced edge", [&] {
    auto broken = valid;
    std::swap(broken.spatial_edges.edge_ids[0],
              broken.spatial_edges.edge_ids[1]);
    ri::validate_destination_spatial_edge_shards(broken, 5, destinations());
  });
}

}  // namespace

int main() {
  try {
    test_valid_sidecars_and_sentinel();
    test_malformed_node_column_lengths();
    test_malformed_coordinate_sentinels();
    test_invalid_costs_and_optional_spatial_index();
    test_malformed_spatial_arrays();
    std::cout << "routing CSR sidecar policy tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "routing CSR sidecar policy test failed: " << error.what()
              << '\n';
    return 1;
  }
}
