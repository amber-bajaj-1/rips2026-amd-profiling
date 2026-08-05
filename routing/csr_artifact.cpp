#include "csr_artifact.hpp"

#include "../sssp/sssp_query_capacity.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace routing {
namespace {

constexpr char kCsrMagic[8] = {'R', 'I', 'P', 'S', 'C', 'S', 'R', '1'};
constexpr std::uint64_t kLegacyCsrVersion = 1;
constexpr std::uint64_t kPairedCsrVersion = 2;
constexpr std::uint64_t kCurrentCsrVersion = 3;
constexpr std::uint64_t kOutgoingEdgeOrientation = 2;

std::uint64_t read_u64(std::ifstream& in, const char* name) {
  std::uint64_t value = 0;
  in.read(reinterpret_cast<char*>(&value), sizeof(value));
  if (!in) {
    throw std::runtime_error(std::string("failed while reading ") + name);
  }
  return value;
}

std::int64_t read_i64(std::ifstream& in, const char* name) {
  std::int64_t value = 0;
  in.read(reinterpret_cast<char*>(&value), sizeof(value));
  if (!in) {
    throw std::runtime_error(std::string("failed while reading ") + name);
  }
  return value;
}

template <typename T>
std::size_t checked_vector_count(std::uint64_t count, const char* name) {
  if (count >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    throw std::overflow_error(std::string(name) +
                              " count is too large for this host");
  }
  const std::size_t host_count = static_cast<std::size_t>(count);
  try {
    (void)sssp_capacity::checked_bytes<T>(host_count);
  } catch (const std::overflow_error&) {
    throw std::overflow_error(std::string(name) + " byte count overflows");
  }
  return host_count;
}

template <typename T>
void read_array(std::ifstream& in,
                std::vector<T>& values,
                std::uint64_t count,
                const char* name) {
  const std::size_t host_count = checked_vector_count<T>(count, name);
  const std::size_t bytes = sssp_capacity::checked_bytes<T>(host_count);
  if (bytes > static_cast<std::size_t>(
                  std::numeric_limits<std::streamsize>::max())) {
    throw std::overflow_error(std::string(name) +
                              " byte count exceeds stream range");
  }
  values.resize(host_count);
  if (values.empty()) return;
  in.read(reinterpret_cast<char*>(values.data()),
          static_cast<std::streamsize>(bytes));
  if (!in) {
    throw std::runtime_error(std::string("failed while reading ") + name);
  }
}

void skip_bytes(std::ifstream& in, std::size_t bytes, const char* name) {
  if (bytes == 0) return;
  if (bytes > static_cast<std::size_t>(
                  std::numeric_limits<std::streamoff>::max())) {
    throw std::overflow_error(std::string(name) +
                              " byte count exceeds stream offset range");
  }
  in.seekg(static_cast<std::streamoff>(bytes), std::ios::cur);
  if (!in) {
    throw std::runtime_error(std::string("failed while skipping ") + name);
  }
}

template <typename T>
void skip_array(std::ifstream& in, std::uint64_t count, const char* name) {
  const std::size_t host_count = checked_vector_count<T>(count, name);
  skip_bytes(in, sssp_capacity::checked_bytes<T>(host_count), name);
}

void require_position_within_file(std::ifstream& in, const char* name) {
  const std::ifstream::pos_type position = in.tellg();
  if (position == std::ifstream::pos_type(-1)) {
    throw std::runtime_error(std::string("failed while checking ") + name);
  }
  in.seekg(0, std::ios::end);
  if (!in) {
    throw std::runtime_error(std::string("failed while checking ") + name);
  }
  const std::ifstream::pos_type end = in.tellg();
  if (end == std::ifstream::pos_type(-1) || position > end) {
    throw std::runtime_error(std::string(name) + " is truncated");
  }
}

}  // namespace

void validate_csr(const HostCsrF32& graph) {
  if (graph.rows <= 0 || graph.rows != graph.cols) {
    throw std::runtime_error("CSR graph must be nonempty and square");
  }
  if (graph.nnz < 0) {
    throw std::runtime_error("CSR nnz must be nonnegative");
  }
  const std::uint64_t unsigned_rows =
      static_cast<std::uint64_t>(graph.rows);
  const std::uint64_t unsigned_nnz =
      static_cast<std::uint64_t>(graph.nnz);
  if (unsigned_rows >=
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max()) ||
      unsigned_rows >
          static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
      unsigned_nnz >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    throw std::runtime_error("CSR graph is too large for PathFinder");
  }
  const std::size_t row_count = static_cast<std::size_t>(unsigned_rows);
  const std::size_t edge_count = static_cast<std::size_t>(unsigned_nnz);
  if (graph.rowptr.size() != row_count + 1 ||
      graph.colind.size() != edge_count ||
      graph.values.size() != edge_count) {
    throw std::runtime_error("CSR array sizes do not match header counts");
  }
  if (graph.rowptr.front() != 0 || graph.rowptr.back() != graph.nnz) {
    throw std::runtime_error("CSR rowptr must start at 0 and end at nnz");
  }
  for (minplus_sparse::Offset row = 0; row < graph.rows; ++row) {
    const minplus_sparse::Offset begin =
        graph.rowptr[static_cast<std::size_t>(row)];
    const minplus_sparse::Offset end =
        graph.rowptr[static_cast<std::size_t>(row + 1)];
    if (begin < 0 || end < begin || end > graph.nnz) {
      throw std::runtime_error("CSR rowptr is not monotone");
    }
  }
  for (std::size_t edge = 0; edge < graph.colind.size(); ++edge) {
    if (graph.colind[edge] < 0 ||
        static_cast<minplus_sparse::Offset>(graph.colind[edge]) >= graph.cols) {
      throw std::runtime_error("CSR colind contains an out-of-range vertex");
    }
    if (!std::isfinite(graph.values[edge]) || graph.values[edge] < 0.0f) {
      throw std::runtime_error("CSR values must be finite nonnegative costs");
    }
  }
}

HostCsrF32 load_csrbin(
    const std::filesystem::path& path,
    std::optional<interchange::InterchangeArtifactPairId>* artifact_pair_id,
    interchange::RoutingCsrSidecars* routing_sidecars,
    bool load_spatial_edge_shards) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("could not open CSR file: " + path.string());
  }

  char magic[sizeof(kCsrMagic)] = {};
  in.read(magic, sizeof(magic));
  if (!in || std::memcmp(magic, kCsrMagic, sizeof(kCsrMagic)) != 0) {
    throw std::runtime_error("input is not a recognized RIPS CSR file");
  }

  const std::uint64_t version = read_u64(in, "CSR format version");
  const std::uint64_t orientation = read_u64(in, "CSR orientation");
  if (version < kLegacyCsrVersion || version > kCurrentCsrVersion) {
    throw std::runtime_error("unsupported CSR format version");
  }
  if (orientation != kOutgoingEdgeOrientation) {
    throw std::runtime_error("unsupported CSR orientation");
  }

  std::optional<interchange::InterchangeArtifactPairId> parsed_pair_id;
  if (version >= kPairedCsrVersion) {
    interchange::InterchangeArtifactPairId id;
    id.high = read_u64(in, "CSR artifact pair id high");
    id.low = read_u64(in, "CSR artifact pair id low");
    if (id.is_zero()) {
      throw std::runtime_error("CSR artifact pair id must not be zero");
    }
    parsed_pair_id = id;
  }

  const std::uint64_t rows = read_u64(in, "CSR row count");
  const std::uint64_t cols = read_u64(in, "CSR column count");
  (void)read_u64(in, "declared edge count");
  (void)read_u64(in, "loaded edge count");
  const std::uint64_t nnz = read_u64(in, "CSR nnz");
  const std::uint64_t rowptr_count = read_u64(in, "CSR rowptr count");
  const std::uint64_t colind_count = read_u64(in, "CSR colind count");
  const std::uint64_t values_count = read_u64(in, "CSR values count");

  std::uint64_t route_x_count = 0;
  std::uint64_t route_y_count = 0;
  std::uint64_t base_cost_count = 0;
  std::int64_t spatial_min_x = 0;
  std::int64_t spatial_min_y = 0;
  std::uint64_t spatial_width = 0;
  std::uint64_t spatial_height = 0;
  std::uint64_t spatial_offset_count = 0;
  std::uint64_t spatial_edge_id_count = 0;
  if (version >= kCurrentCsrVersion) {
    route_x_count = read_u64(in, "CSR route-end x count");
    route_y_count = read_u64(in, "CSR route-end y count");
    base_cost_count = read_u64(in, "CSR base vertex cost count");
    spatial_min_x = read_i64(in, "CSR spatial shard minimum x");
    spatial_min_y = read_i64(in, "CSR spatial shard minimum y");
    spatial_width = read_u64(in, "CSR spatial shard width");
    spatial_height = read_u64(in, "CSR spatial shard height");
    spatial_offset_count = read_u64(in, "CSR spatial shard offset count");
    spatial_edge_id_count = read_u64(in, "CSR spatial shard edge-id count");
  }

  if (rows == 0 || rows != cols) {
    throw std::runtime_error("CSR graph must be nonempty and square");
  }
  if (rows > static_cast<std::uint64_t>(
                 std::numeric_limits<minplus_sparse::Offset>::max()) ||
      rows > static_cast<std::uint64_t>(
                 std::numeric_limits<minplus_sparse::Index>::max()) ||
      nnz > static_cast<std::uint64_t>(
                std::numeric_limits<minplus_sparse::Offset>::max())) {
    throw std::runtime_error("CSR graph is too large for this API");
  }
  if (rowptr_count != rows + 1 || colind_count != nnz ||
      values_count != nnz) {
    throw std::runtime_error("CSR header counts are inconsistent");
  }
  if (version >= kCurrentCsrVersion) {
    if (route_x_count != rows || route_y_count != rows ||
        base_cost_count != rows || spatial_edge_id_count != nnz) {
      throw std::runtime_error(
          "CSR routing sidecar counts are inconsistent");
    }
    if (spatial_min_x < std::numeric_limits<std::int32_t>::min() ||
        spatial_min_x > std::numeric_limits<std::int32_t>::max() ||
        spatial_min_y < std::numeric_limits<std::int32_t>::min() ||
        spatial_min_y > std::numeric_limits<std::int32_t>::max()) {
      throw std::runtime_error(
          "CSR spatial shard origin exceeds int32 range");
    }
    if ((spatial_width == 0) != (spatial_height == 0) ||
        (spatial_width != 0 &&
         spatial_height >
             std::numeric_limits<std::uint64_t>::max() / spatial_width)) {
      throw std::runtime_error(
          "CSR spatial shard grid dimensions are invalid");
    }
    const std::uint64_t regular_shards = spatial_width * spatial_height;
    if (regular_shards >
            interchange::maximum_dense_spatial_cells(
                static_cast<std::size_t>(rows)) ||
        regular_shards > std::numeric_limits<std::uint64_t>::max() - 2 ||
        spatial_offset_count != regular_shards + 2) {
      throw std::runtime_error(
          "CSR spatial shard offset count is inconsistent");
    }
  }

  HostCsrF32 graph;
  graph.rows = static_cast<minplus_sparse::Offset>(rows);
  graph.cols = static_cast<minplus_sparse::Offset>(cols);
  graph.nnz = static_cast<minplus_sparse::Offset>(nnz);
  read_array(in, graph.rowptr, rowptr_count, "CSR rowptr");
  read_array(in, graph.colind, colind_count, "CSR colind");
  read_array(in, graph.values, values_count, "CSR values");

  interchange::RoutingCsrSidecars parsed_sidecars;
  if (version >= kCurrentCsrVersion) {
    if (routing_sidecars != nullptr) {
      read_array(in, parsed_sidecars.route_end_x, route_x_count,
                 "CSR route-end x coordinates");
      read_array(in, parsed_sidecars.route_end_y, route_y_count,
                 "CSR route-end y coordinates");
      read_array(in, parsed_sidecars.base_vertex_cost, base_cost_count,
                 "CSR base vertex costs");
      if (load_spatial_edge_shards) {
        parsed_sidecars.spatial_edges.min_x =
            static_cast<std::int32_t>(spatial_min_x);
        parsed_sidecars.spatial_edges.min_y =
            static_cast<std::int32_t>(spatial_min_y);
        parsed_sidecars.spatial_edges.width = spatial_width;
        parsed_sidecars.spatial_edges.height = spatial_height;
        read_array(in, parsed_sidecars.spatial_edges.offsets,
                   spatial_offset_count, "CSR spatial shard offsets");
        read_array(in, parsed_sidecars.spatial_edges.edge_ids,
                   spatial_edge_id_count, "CSR spatial shard edge IDs");
      } else {
        skip_array<std::uint64_t>(in, spatial_offset_count,
                                  "CSR spatial shard offsets");
        skip_array<std::uint32_t>(in, spatial_edge_id_count,
                                  "CSR spatial shard edge IDs");
      }
      if (load_spatial_edge_shards) {
        interchange::validate_destination_spatial_edge_shards(
            parsed_sidecars, static_cast<std::size_t>(rows), graph.colind);
      } else {
        interchange::validate_routing_csr_sidecars(
            parsed_sidecars, static_cast<std::size_t>(rows),
            static_cast<std::size_t>(nnz), false);
      }
    } else {
      skip_array<std::int32_t>(in, route_x_count,
                               "CSR route-end x coordinates");
      skip_array<std::int32_t>(in, route_y_count,
                               "CSR route-end y coordinates");
      skip_array<float>(in, base_cost_count, "CSR base vertex costs");
      skip_array<std::uint64_t>(in, spatial_offset_count,
                                "CSR spatial shard offsets");
      skip_array<std::uint32_t>(in, spatial_edge_id_count,
                                "CSR spatial shard edge IDs");
    }
  }
  require_position_within_file(in, "CSR payload");
  validate_csr(graph);
  if (artifact_pair_id != nullptr) {
    *artifact_pair_id = parsed_pair_id;
  }
  if (routing_sidecars != nullptr) {
    *routing_sidecars = std::move(parsed_sidecars);
  }
  return graph;
}

}  // namespace routing
