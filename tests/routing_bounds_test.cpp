#include "../routing/bounds.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

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

void require_bounds(const routing::RoutingQueryBounds& actual,
                    std::int32_t min_x,
                    std::int32_t max_x,
                    std::int32_t min_y,
                    std::int32_t max_y,
                    const std::string& label) {
  require(actual.enabled && actual.min_x == min_x && actual.max_x == max_x &&
              actual.min_y == min_y && actual.max_y == max_y,
          label + " produced the wrong inclusive rectangle");
}

void test_defaults_and_custom_margins() {
  const routing::RoutingBoundsConfig defaults;
  require(defaults.enabled, "routing bounds should be enabled by default");
  require(defaults.margin_x == 2 && defaults.margin_y == 14,
          "routing bound defaults must remain 2 by 14");
  require(defaults.unbounded_fallback,
          "missing-target fallback should be enabled by default");

  const std::vector<std::int32_t> x = {10, 30};
  const std::vector<std::int32_t> y = {20, 40};
  const auto default_bounds =
      routing::derive_query_bounds(x, y, {0}, {1}, defaults);
  require(!default_bounds.target_missing_coordinates,
          "known terminals were reported as missing coordinates");
  require_bounds(default_bounds.bounds, 8, 32, 6, 54,
                 "default-margin derivation");

  routing::RoutingBoundsConfig custom;
  custom.margin_x = 3;
  custom.margin_y = 4;
  const auto custom_bounds =
      routing::derive_query_bounds(x, y, {0}, {1}, custom);
  require_bounds(custom_bounds.bounds, 7, 33, 16, 44,
                 "custom-margin derivation");

  custom.margin_x = -1;
  require_rejected("negative X margin", [&] {
    (void)routing::derive_query_bounds(x, y, {0}, {1}, custom);
  });
}

void test_inclusive_admission_and_missing_coordinates() {
  const routing::RoutingQueryBounds bounds{true, 10, 20, 30, 40};
  require(routing::coordinate_in_bounds(10, 30, bounds) &&
              routing::coordinate_in_bounds(20, 40, bounds),
          "inclusive coordinate boundaries were excluded");
  require(!routing::coordinate_in_bounds(9, 30, bounds) &&
              !routing::coordinate_in_bounds(20, 41, bounds),
          "coordinates outside the rectangle were admitted");

  const std::int32_t missing = routing::kMissingRouteCoordinate;
  const std::vector<std::int32_t> x = {10, 20, 9, missing};
  const std::vector<std::int32_t> y = {30, 40, 35, missing};
  require(routing::route_node_admitted(x.data(), y.data(), 0, bounds) &&
              routing::route_node_admitted(x.data(), y.data(), 1, bounds),
          "route-node admission excluded an inclusive boundary");
  require(!routing::route_node_admitted(x.data(), y.data(), 2, bounds),
          "known coordinate outside the query box was admitted");
  require(routing::route_node_admitted(x.data(), y.data(), 3, bounds),
          "paired missing coordinates must remain admissible");

  const routing::RoutingQueryBounds disabled;
  require(routing::route_node_admitted(nullptr, nullptr, 0, disabled),
          "unbounded admission should not dereference coordinate arrays");
}

void test_int32_saturation() {
  constexpr std::int32_t low = std::numeric_limits<std::int32_t>::min();
  constexpr std::int32_t high = std::numeric_limits<std::int32_t>::max();
  require(routing::saturating_coordinate_margin(low + 1, 14, true) == low,
          "lower coordinate margin did not saturate at int32 minimum");
  require(routing::saturating_coordinate_margin(high - 1, 14, false) == high,
          "upper coordinate margin did not saturate at int32 maximum");

  routing::RoutingBoundsConfig config;
  config.margin_x = 2;
  config.margin_y = 14;
  const auto derived = routing::derive_query_bounds(
      std::vector<std::int32_t>{high}, std::vector<std::int32_t>{high}, {0},
      {0}, config);
  require_bounds(derived.bounds, high - 2, high, high - 14, high,
                 "saturating terminal derivation");
}

void test_missing_target_fallback_policy() {
  const std::int32_t missing = routing::kMissingRouteCoordinate;
  const std::vector<std::int32_t> x = {5, missing};
  const std::vector<std::int32_t> y = {7, missing};

  routing::RoutingBoundsConfig fallback;
  fallback.unbounded_fallback = true;
  const auto derived =
      routing::derive_query_bounds(x, y, {0}, {1}, fallback);
  require(derived.target_missing_coordinates,
          "missing target did not select the fallback policy");
  require(!derived.bounds.enabled,
          "missing-target fallback must select an unbounded first run");

  fallback.unbounded_fallback = false;
  require_rejected("missing target without fallback", [&] {
    (void)routing::derive_query_bounds(x, y, {0}, {1}, fallback);
  });

  fallback.enabled = false;
  const auto unbounded =
      routing::derive_query_bounds(x, y, {0}, {1}, fallback);
  require(!unbounded.bounds.enabled &&
              !unbounded.target_missing_coordinates,
          "explicit unbounded mode should bypass coordinate fallback");
}

void test_multi_source_target_and_duplicates() {
  const std::int32_t missing = routing::kMissingRouteCoordinate;
  const std::vector<std::int32_t> x = {10, missing, 100, 20};
  const std::vector<std::int32_t> y = {50, missing, 5, 60};
  const std::vector<int> sources = {0, 1, 0};
  const std::vector<int> targets = {2, 3, 2, 3};

  routing::RoutingBoundsConfig config;
  const auto derived =
      routing::derive_query_bounds(x, y, sources, targets, config);
  require_bounds(derived.bounds, 8, 102, -9, 74,
                 "multi-terminal derivation");
  routing::validate_terminals_in_bounds(x, y, sources, targets,
                                        derived.bounds);

  const routing::RoutingQueryBounds explicit_box{true, 10, 100, 5, 60};
  routing::validate_terminals_in_bounds(x, y, sources, targets, explicit_box);

  const routing::RoutingQueryBounds source_excluding{true, 11, 100, 5, 60};
  require_rejected("known source outside explicit bounds", [&] {
    routing::validate_terminals_in_bounds(x, y, sources, targets,
                                          source_excluding);
  });
  const routing::RoutingQueryBounds target_excluding{true, 10, 99, 5, 60};
  require_rejected("known target outside explicit bounds", [&] {
    routing::validate_terminals_in_bounds(x, y, sources, targets,
                                          target_excluding);
  });

  require_rejected("out-of-range terminal", [&] {
    (void)routing::derive_query_bounds(x, y, sources, {4}, config);
  });
}

}  // namespace

int main() {
  try {
    test_defaults_and_custom_margins();
    test_inclusive_admission_and_missing_coordinates();
    test_int32_saturation();
    test_missing_target_fallback_policy();
    test_multi_source_target_and_duplicates();
    std::cout << "routing bounds policy tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "routing bounds policy test failed: " << error.what() << '\n';
    return 1;
  }
}
