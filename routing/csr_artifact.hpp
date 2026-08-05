#pragma once

#include "../pre-process/import_policy.hpp"
#include "../pre-process/routing_csr_sidecars.hpp"
#include "../sssp/sssp_types.hpp"

#include <filesystem>
#include <optional>

namespace routing {

// Validate the complete host graph payload before it is exposed to routing or
// uploaded by a backend. This remains host-only so artifact tests do not need
// a HIP toolchain.
void validate_csr(const HostCsrF32& graph);

// Load RIPSCSR1 versions 1-3. V3 callers may select the complete spatial
// payload, node sidecars only, or graph-only loading without weakening
// truncation and header validation.
HostCsrF32 load_csrbin(
    const std::filesystem::path& path,
    std::optional<interchange::InterchangeArtifactPairId>* artifact_pair_id =
        nullptr,
    interchange::RoutingCsrSidecars* routing_sidecars = nullptr,
    bool load_spatial_edge_shards = false);

}  // namespace routing
