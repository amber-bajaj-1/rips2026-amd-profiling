#define PATHFINDER_ROUTER_NO_MAIN
#include "../routing/pathfinder_router.cpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

Options parse(std::vector<std::string> arguments) {
  std::vector<char*> argv;
  argv.reserve(arguments.size());
  for (std::string& argument : arguments) argv.push_back(argument.data());
  return parse_args(static_cast<int>(argv.size()), argv.data());
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

void require_forwarded(const Options& options,
                       const std::vector<std::string>& expected,
                       const std::string& label) {
  require(options.pathfinder_args == expected,
          label + " changed the forwarded PathFinder argument sequence");
}

void test_defaults_and_inference() {
  const Options options =
      parse({"PathFinderFile", "work/design_unrouted.phys", "out.phys"});
  require(options.logical_netlist == "work/design.netlist",
          "default logical-netlist inference changed");
  require(options.allow_unrouted,
          "launcher should preserve partial-route compatibility by default");
  require_forwarded(options, {},
                    "default bounded Delta-Stepping selection");
}

void test_engine_neutral_bounds_forwarding() {
  const Options options = parse(
      {"PathFinderFile", "in.phys", "out.phys", "--sssp-engine",
       "delta_stepping", "--bbox-margin-x", "5", "--bbox-margin-y", "17",
       "--no-unbounded-fallback", "--delta-telemetry",
       "--delta-telemetry"});
  require_forwarded(
      options,
      {"--sssp-engine", "delta_stepping", "--bbox-margin-x", "5",
       "--bbox-margin-y", "17", "--no-unbounded-fallback",
       "--delta-telemetry"},
      "engine-neutral bounds controls");
}

void test_bf11_compatibility_aliases() {
  const Options options = parse(
      {"PathFinderFile", "in.phys", "out.phys", "--sssp-engine",
       "bellman_ford_11", "--bf11-unbounded", "--bf11-bounds",
       "--bf11-bbox-margin-x", "0", "--bf11-bbox-margin-y", "14",
       "--bf11-no-unbounded-fallback", "--bf11-target-check-interval", "2",
       "--bellman-ford-telemetry", "--bf11-telemetry"});
  require_forwarded(
      options,
      {"--sssp-engine", "bellman_ford_11", "--bf11-unbounded",
       "--bf11-bounds", "--bf11-bbox-margin-x", "0",
       "--bf11-bbox-margin-y", "14", "--bf11-no-unbounded-fallback",
       "--bf11-target-check-interval", "2", "--bellman-ford-telemetry"},
      "BF11 compatibility aliases");
}

void test_delta_aliases_and_controls() {
  const Options options = parse(
      {"PathFinderFile", "in.phys", "out.phys", "--sssp-engine", "delta",
       "--use-delta-step", "--delta-force-generic",
       "--delta-force-legacy-parent", "--delta", "auto",
       "--delta-multiplier", "1.25", "--delta-controller",
       "reduced-round-trip", "--delta-controller-batch-size", "8",
       "--delta-benchmark-weights", "mixed",
       "--delta-benchmark-weight-seed", "42"});
  require_forwarded(
      options,
      {"--sssp-engine", "delta", "--use-delta-step",
       "--delta-force-generic", "--delta-force-legacy-parent", "--delta",
       "auto", "--delta-multiplier", "1.25", "--delta-controller",
       "reduced-round-trip", "--delta-controller-batch-size", "8",
       "--delta-benchmark-weights", "mixed",
       "--delta-benchmark-weight-seed", "42"},
      "Delta-Stepping aliases and controls");
}

void test_invalid_values_are_rejected() {
  require_rejected("invalid engine", "invalid sssp-engine", [] {
    (void)parse({"PathFinderFile", "in.phys", "out.phys", "--sssp-engine",
                 "bf10"});
  });
  require_rejected("missing margin", "--bbox-margin-x requires a value", [] {
    (void)parse({"PathFinderFile", "in.phys", "out.phys",
                 "--bbox-margin-x"});
  });
  require_rejected("negative neutral margin", "nonnegative integer", [] {
    (void)parse({"PathFinderFile", "in.phys", "out.phys",
                 "--bbox-margin-y", "-1"});
  });
  require_rejected("negative BF11 margin", "nonnegative integer", [] {
    (void)parse({"PathFinderFile", "in.phys", "out.phys",
                 "--bf11-bbox-margin-x", "-2"});
  });
  require_rejected("non-integer margin", "requires an integer", [] {
    (void)parse({"PathFinderFile", "in.phys", "out.phys",
                 "--bbox-margin-x", "2.5"});
  });
  require_rejected("zero target interval", "positive integer", [] {
    (void)parse({"PathFinderFile", "in.phys", "out.phys",
                 "--target-check-interval", "0"});
  });
  require_rejected("BF11 target interval with Delta", "cannot be used", [] {
    (void)parse({"PathFinderFile", "in.phys", "out.phys",
                 "--target-check-interval", "1"});
  });
  require_rejected("BF11 telemetry with Delta", "cannot be used", [] {
    (void)parse({"PathFinderFile", "in.phys", "out.phys",
                 "--bf11-telemetry"});
  });
  require_rejected("seed without mixed weights", "requires", [] {
    (void)parse({"PathFinderFile", "in.phys", "out.phys",
                 "--delta-benchmark-weights", "unit",
                 "--delta-benchmark-weight-seed", "7"});
  });
  require_rejected("Delta option with BF11", "cannot be used", [] {
    (void)parse({"PathFinderFile", "in.phys", "out.phys", "--sssp-engine",
                 "bf11", "--delta", "1"});
  });
  require_rejected("unknown option", "unknown option", [] {
    (void)parse({"PathFinderFile", "in.phys", "out.phys", "--unit-bfs"});
  });
}

}  // namespace

int main() {
  try {
    // Suppressing the production main leaves these orchestration helpers
    // intentionally uncalled in this parser-only test translation unit.
    (void)&print_progress;
    (void)&run_command;
    (void)&make_work_dir;
    (void)&require_file;
    test_defaults_and_inference();
    test_engine_neutral_bounds_forwarding();
    test_bf11_compatibility_aliases();
    test_delta_aliases_and_controls();
    test_invalid_values_are_rejected();
    std::cout << "PathFinder router argument tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "PathFinder router argument test failed: " << error.what()
              << '\n';
    return 1;
  }
}
