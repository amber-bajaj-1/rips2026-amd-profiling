# RIPS AMD PathFinder Profiling

This repository runs the PathFinder wrapper with GPU Delta-Stepping on the AMD
University Program (AUP) cloud. Generic bucketed Delta-Stepping is always used;
there is no Unit-BFS or Bellman-Ford selection.

## 1. Set up the AUP workspace

Start in the AUP home directory. The setup script must be present in this
directory before running these commands.

```bash
cd /home/jovyan
chmod +x setup-tpe.sh
./setup-tpe.sh
```

The script is named `setup-tpe.sh`, so run `./setup-tpe.sh`, not `./setup.sh`.

Setup performs the following work:

- installs or validates Java 21, Cap'n Proto, HIP/ROCm, ROCprofiler-SDK, and
  ROCTx;
- creates `/home/jovyan/fpga24_routing_contest`;
- downloads all FPGA'24 routing benchmarks and prepares `xcvu3p.device`;
- places this repository at
  `/home/jovyan/fpga24_routing_contest/rips2026-amd-profiling`;
- generates the FPGA Interchange C++ schemas and the device routing graph; and
- compiles the wrapper, preprocessing tools, GPU PathFinder/Delta-Stepping
  executable, and post-processing tool.

Setup does not start a routing run. It stops with an error if a dependency,
benchmark, device file, or required executable cannot be prepared.

After setup completes, enter the profiling repository:

```bash
cd /home/jovyan/fpga24_routing_contest/rips2026-amd-profiling
```

Optional verification:

```bash
ls -lh PathFinderFile pathfinder interchange_to_csr device_to_routing_graph routes_to_phys
ls -lh xcvu3p.full-poc-base-wire.devicegraph
rocprofv3 --version
```

## 2. Choose a benchmark

Set `BENCHMARK` to any downloaded contest benchmark:

```bash
BENCHMARK=boom_med_pb
```

Available names are:

```text
logicnets_jscl
boom_med_pb
vtr_mcml
rosetta_fd
corundum_25g
finn_radioml
vtr_lu64peeng
corescore_500
corescore_500_pb
mlcad_d181_lefttwo3rds
koios_dla_like_large
boom_soc
ispd16_example2
```

## 3. Run without GPU profiling

Run PathFinder with an explicit Delta-Stepping bucket width:

```bash
make run BENCHMARK="$BENCHMARK" PATHFINDER_ARGS='--delta 1'
```

The routed physical netlist is written to:

```text
/home/jovyan/fpga24_routing_contest/<benchmark>_PathFinderFile.phys
```

To let PathFinder choose the Delta-Stepping width automatically:

```bash
make run BENCHMARK="$BENCHMARK" PATHFINDER_ARGS='--delta auto'
```

## 4. Run with GPU profiling

Use the same benchmark and Delta setting with the `profile` target:

```bash
make profile BENCHMARK="$BENCHMARK" PATHFINDER_ARGS='--delta 1'
```

For automatic Delta selection:

```bash
make profile BENCHMARK="$BENCHMARK" PATHFINDER_ARGS='--delta auto'
```

The profile target runs `rocprofv3` around the inner GPU PathFinder executable.
It records HIP runtime activity, kernel dispatches, memory operations, and the
ROCTx ranges built into PathFinder and Delta-Stepping. Delta-Stepping telemetry
is also enabled automatically for profiled runs.

Every profiled run gets a timestamped directory:

```text
/home/jovyan/fpga24_routing_contest/rips2026-amd-profiling/
  profiling/<benchmark>/<YYYYMMDD-HHMMSS>/
```

That directory contains:

- ROCprofiler CSV trace files, possibly inside hostname/process subdirectories;
- `pathfinder-wrapper.log`, including the Delta-Stepping telemetry record; and
- `<benchmark>_PathFinderFile.phys`, the routed result from the profiled run.

List the generated results with:

```bash
find "profiling/$BENCHMARK" -type f | sort
```

## 5. Run an input outside the standard benchmark names

For a non-profiled run, provide all three netlist paths:

```bash
make run \
  INPUT_PHYS=/absolute/path/example_unrouted.phys \
  LOGICAL_NETLIST=/absolute/path/example.netlist \
  OUTPUT_PHYS=/absolute/path/example_PathFinderFile.phys \
  PATHFINDER_ARGS='--delta 1'
```

For a profiled run, provide the input paths and a short label:

```bash
make profile \
  INPUT_PHYS=/absolute/path/example_unrouted.phys \
  LOGICAL_NETLIST=/absolute/path/example.netlist \
  PROFILE_LABEL=example \
  PATHFINDER_ARGS='--delta 1'
```

The custom profiled run is written under `profiling/example/<timestamp>/`.

## Useful commands

Display the Makefile's current examples:

```bash
make help
```

Recompile all pipeline executables using the AUP paths detected during setup:

```bash
make clean
make pipeline
```

`make clean` removes compiled executables only. It does not remove benchmarks,
the device graph, routed netlists, or profiling results.
