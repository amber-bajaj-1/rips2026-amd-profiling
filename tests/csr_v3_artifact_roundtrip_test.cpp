#include "../routing/csr_artifact.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace ri = routing::interchange;

namespace {

constexpr char kCsrMagic[8] = {'R', 'I', 'P', 'S', 'C', 'S', 'R', '1'};
constexpr std::uint64_t kLegacyVersion = 1;
constexpr std::uint64_t kPairedLegacyVersion = 2;
constexpr std::uint64_t kRoutingSidecarVersion = 3;
constexpr std::uint64_t kOutgoingOrientation = 2;

struct Artifact {
  std::optional<ri::InterchangeArtifactPairId> pair_id;
  HostCsrF32 graph;
  ri::RoutingCsrSidecars sidecars;
};

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void require_rejected(const std::string& label,
                      const std::string& expected_message,
                      Function&& function) {
  try {
    function();
  } catch (const std::exception& error) {
    require(std::string(error.what()).find(expected_message) !=
                std::string::npos,
            label + " produced an unhelpful error: " + error.what());
    return;
  }
  throw std::runtime_error(label + " was accepted");
}

template <typename T>
void write_scalar(std::ostream& out, T value) {
  static_assert(std::is_trivially_copyable<T>::value);
  out.write(reinterpret_cast<const char*>(&value), sizeof(value));
  if (!out) throw std::runtime_error("failed to write CSR fixture scalar");
}

template <typename T>
void write_array(std::ostream& out, const std::vector<T>& values) {
  static_assert(std::is_trivially_copyable<T>::value);
  if (!values.empty()) {
    out.write(reinterpret_cast<const char*>(values.data()),
              static_cast<std::streamsize>(values.size() * sizeof(T)));
  }
  if (!out) throw std::runtime_error("failed to write CSR fixture array");
}

void write_fixture(const std::filesystem::path& path,
                   const Artifact& artifact,
                   std::uint64_t version) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("could not create CSR fixture");
  out.write(kCsrMagic, sizeof(kCsrMagic));
  write_scalar(out, version);
  write_scalar(out, kOutgoingOrientation);
  if (version >= 2) {
    const ri::InterchangeArtifactPairId pair =
        artifact.pair_id.value_or(ri::InterchangeArtifactPairId{});
    write_scalar(out, pair.high);
    write_scalar(out, pair.low);
  }

  const HostCsrF32& graph = artifact.graph;
  write_scalar(out, static_cast<std::uint64_t>(graph.rows));
  write_scalar(out, static_cast<std::uint64_t>(graph.cols));
  write_scalar(out, static_cast<std::uint64_t>(graph.nnz));
  write_scalar(out, static_cast<std::uint64_t>(graph.nnz));
  write_scalar(out, static_cast<std::uint64_t>(graph.nnz));
  write_scalar(out, static_cast<std::uint64_t>(graph.rowptr.size()));
  write_scalar(out, static_cast<std::uint64_t>(graph.colind.size()));
  write_scalar(out, static_cast<std::uint64_t>(graph.values.size()));

  if (version >= kRoutingSidecarVersion) {
    const ri::RoutingCsrSidecars& sidecars = artifact.sidecars;
    const ri::SpatialEdgeShards& shards = sidecars.spatial_edges;
    write_scalar(out,
                 static_cast<std::uint64_t>(sidecars.route_end_x.size()));
    write_scalar(out,
                 static_cast<std::uint64_t>(sidecars.route_end_y.size()));
    write_scalar(out,
                 static_cast<std::uint64_t>(sidecars.base_vertex_cost.size()));
    write_scalar(out, static_cast<std::int64_t>(shards.min_x));
    write_scalar(out, static_cast<std::int64_t>(shards.min_y));
    write_scalar(out, shards.width);
    write_scalar(out, shards.height);
    write_scalar(out, static_cast<std::uint64_t>(shards.offsets.size()));
    write_scalar(out, static_cast<std::uint64_t>(shards.edge_ids.size()));
  }

  write_array(out, graph.rowptr);
  write_array(out, graph.colind);
  write_array(out, graph.values);
  if (version >= kRoutingSidecarVersion) {
    write_array(out, artifact.sidecars.route_end_x);
    write_array(out, artifact.sidecars.route_end_y);
    write_array(out, artifact.sidecars.base_vertex_cost);
    write_array(out, artifact.sidecars.spatial_edges.offsets);
    write_array(out, artifact.sidecars.spatial_edges.edge_ids);
  }
}

Artifact make_artifact() {
  Artifact artifact;
  artifact.pair_id = ri::InterchangeArtifactPairId{
      0x123456789abcdef0ULL, 0x0fedcba987654321ULL};
  artifact.graph.rows = 4;
  artifact.graph.cols = 4;
  artifact.graph.nnz = 4;
  artifact.graph.rowptr = {0, 2, 3, 4, 4};
  artifact.graph.colind = {1, 2, 3, 3};
  artifact.graph.values = {1.0f, 4.0f, 1.5f, 2.0f};
  artifact.sidecars.route_end_x = {10, 11, ri::kMissingRouteCoordinate, 12};
  artifact.sidecars.route_end_y = {20, 20, ri::kMissingRouteCoordinate, 21};
  artifact.sidecars.base_vertex_cost = {1.0f, 1.25f, 2.0f, 3.0f};
  artifact.sidecars.spatial_edges =
      ri::build_destination_spatial_edge_shards(
          artifact.sidecars.route_end_x, artifact.sidecars.route_end_y,
          std::vector<std::int32_t>{1, 2, 3, 3});
  return artifact;
}

bool has_node_sidecars(const ri::RoutingCsrSidecars& sidecars) {
  return !sidecars.route_end_x.empty() || !sidecars.route_end_y.empty() ||
         !sidecars.base_vertex_cost.empty();
}

void require_bounds_compatible_artifact(
    const ri::RoutingCsrSidecars& sidecars,
    bool bounds_enabled) {
  if (bounds_enabled && !has_node_sidecars(sidecars)) {
    throw std::runtime_error(
        "bounded routing requires a CSR v3 artifact with route-end "
        "coordinates; regenerate the CSR or select --unbounded");
  }
}

void test_v3_round_trip_and_loading_modes(const std::filesystem::path& path) {
  const Artifact expected = make_artifact();
  write_fixture(path, expected, kRoutingSidecarVersion);

  std::optional<ri::InterchangeArtifactPairId> pair_id;
  ri::RoutingCsrSidecars full_sidecars;
  const HostCsrF32 full =
      routing::load_csrbin(path, &pair_id, &full_sidecars, true);
  require(pair_id == expected.pair_id &&
              full.rowptr == expected.graph.rowptr &&
              full.colind == expected.graph.colind &&
              full.values == expected.graph.values &&
              full_sidecars.route_end_x == expected.sidecars.route_end_x &&
              full_sidecars.route_end_y == expected.sidecars.route_end_y &&
              full_sidecars.base_vertex_cost ==
                  expected.sidecars.base_vertex_cost &&
              full_sidecars.spatial_edges.offsets ==
                  expected.sidecars.spatial_edges.offsets &&
              full_sidecars.spatial_edges.edge_ids ==
                  expected.sidecars.spatial_edges.edge_ids,
          "production CSR v3 full load changed graph or sidecar data");
  require_bounds_compatible_artifact(full_sidecars, true);

  ri::RoutingCsrSidecars node_sidecars;
  (void)routing::load_csrbin(path, &pair_id, &node_sidecars, false);
  require(node_sidecars.route_end_x == expected.sidecars.route_end_x &&
              node_sidecars.route_end_y == expected.sidecars.route_end_y &&
              node_sidecars.base_vertex_cost ==
                  expected.sidecars.base_vertex_cost &&
              node_sidecars.spatial_edges.offsets.empty() &&
              node_sidecars.spatial_edges.edge_ids.empty(),
          "production node-only load retained or lost the wrong sidecars");
  require_bounds_compatible_artifact(node_sidecars, true);

  pair_id.reset();
  const HostCsrF32 graph_only = routing::load_csrbin(path, &pair_id);
  require(pair_id == expected.pair_id &&
              graph_only.colind == expected.graph.colind,
          "production graph-only load changed graph or pair identity");
}

void test_legacy_bounds_policy(const std::filesystem::path& path) {
  const Artifact source = make_artifact();
  write_fixture(path, source, kLegacyVersion);
  std::optional<ri::InterchangeArtifactPairId> pair_id = source.pair_id;
  ri::RoutingCsrSidecars sidecars;
  const HostCsrF32 graph =
      routing::load_csrbin(path, &pair_id, &sidecars, true);
  require(graph.rows == source.graph.rows && !pair_id.has_value() &&
              !has_node_sidecars(sidecars),
          "legacy CSR load fabricated v3 routing metadata");
  require_rejected("bounded legacy CSR", "regenerate the CSR", [&] {
    require_bounds_compatible_artifact(sidecars, true);
  });
  require_bounds_compatible_artifact(sidecars, false);

  write_fixture(path, source, kPairedLegacyVersion);
  pair_id.reset();
  sidecars = {};
  (void)routing::load_csrbin(path, &pair_id, &sidecars, false);
  require(pair_id == source.pair_id && !has_node_sidecars(sidecars),
          "paired legacy CSR lost its identity or fabricated v3 sidecars");
  require_rejected("bounded paired legacy CSR", "regenerate the CSR", [&] {
    require_bounds_compatible_artifact(sidecars, true);
  });
  require_bounds_compatible_artifact(sidecars, false);
}

void test_malformed_sidecars_and_truncation(
    const std::filesystem::path& path) {
  const Artifact valid = make_artifact();

  require_rejected("short route-end X column", "counts are inconsistent", [&] {
    Artifact broken = valid;
    broken.sidecars.route_end_x.pop_back();
    write_fixture(path, broken, kRoutingSidecarVersion);
    ri::RoutingCsrSidecars loaded;
    (void)routing::load_csrbin(path, nullptr, &loaded, true);
  });
  require_rejected("half-missing coordinate", "invalid route-end", [&] {
    Artifact broken = valid;
    broken.sidecars.route_end_x[0] = ri::kMissingRouteCoordinate;
    write_fixture(path, broken, kRoutingSidecarVersion);
    ri::RoutingCsrSidecars loaded;
    (void)routing::load_csrbin(path, nullptr, &loaded, true);
  });
  require_rejected("zero base cost", "finite and positive", [&] {
    Artifact broken = valid;
    broken.sidecars.base_vertex_cost[0] = 0.0f;
    write_fixture(path, broken, kRoutingSidecarVersion);
    ri::RoutingCsrSidecars loaded;
    (void)routing::load_csrbin(path, nullptr, &loaded, false);
  });
  require_rejected("duplicate spatial edge ID", "duplicate edge ID", [&] {
    Artifact broken = valid;
    broken.sidecars.spatial_edges.edge_ids[0] =
        broken.sidecars.spatial_edges.edge_ids[1];
    write_fixture(path, broken, kRoutingSidecarVersion);
    ri::RoutingCsrSidecars loaded;
    (void)routing::load_csrbin(path, nullptr, &loaded, true);
  });
  require_rejected("zero pair ID", "must not be zero", [&] {
    Artifact broken = valid;
    broken.pair_id.reset();
    write_fixture(path, broken, kRoutingSidecarVersion);
    (void)routing::load_csrbin(path);
  });

  write_fixture(path, valid, kRoutingSidecarVersion);
  std::filesystem::resize_file(path, std::filesystem::file_size(path) - 1);
  require_rejected("truncated full v3 payload", "failed while reading", [&] {
    ri::RoutingCsrSidecars loaded;
    (void)routing::load_csrbin(path, nullptr, &loaded, true);
  });
  require_rejected("truncated graph-only v3 payload", "truncated",
                   [&] { (void)routing::load_csrbin(path); });
}

}  // namespace

int main() {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("rips_csr_v3_roundtrip_" +
       std::to_string(std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count()));
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  } cleanup{directory};

  try {
    std::filesystem::create_directory(directory);
    const std::filesystem::path path = directory / "graph.csrbin";
    test_v3_round_trip_and_loading_modes(path);
    test_legacy_bounds_policy(path);
    test_malformed_sidecars_and_truncation(path);
    std::cout << "production CSR v3 artifact round-trip tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "production CSR v3 artifact round-trip test failed: "
              << error.what() << '\n';
    return 1;
  }
}
