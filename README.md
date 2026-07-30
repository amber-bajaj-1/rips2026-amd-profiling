# RIPS AMD PathFinder Profiling

Run the PathFinder wrapper with GPU Delta-Stepping on the AMD University
Program (AUP) cloud.

## 1. Set up the AUP workspace

Choose a writable root directory, then run:

```bash
RIPS_ROOT=/path/to/your/workspace
chmod +x setup-tpe.sh
./setup-tpe.sh "$RIPS_ROOT"
source "$RIPS_ROOT/rips2026-amd-profiling/environment.sh"
cd "$RIPS_ROOT/rips2026-amd-profiling"
```

Setup downloads the `benchmarks-v1` release asset, extracts it into
`benchmarks/`, compiles the pipeline, and generates the routing device graph
there. An interrupted benchmark download resumes when setup is run again.

## 2. Choose a benchmark

```bash
BENCHMARK=boom_med_pb
```

Available benchmarks:

```text
logicnets_jscl
boom_med_pb
vtr_mcml
rosetta_fd
```

## 3. Run without profiling

```bash
make run BENCHMARK="$BENCHMARK"
```

The routed physical netlist is written to:

```text
benchmarks/<benchmark>_PathFinderFile.phys
```

## 4. Run with profiling

```bash
make profile BENCHMARK="$BENCHMARK"
```

Each run writes its ROCprofiler traces, wrapper log, telemetry, and routed
physical netlist to:

```text
profiling/<benchmark>/<YYYYMMDD-HHMMSS>/
```

List the generated files with:

```bash
find "profiling/$BENCHMARK" -type f | sort
```

## 5. Runtime options

Delta defaults to `1`. Select automatic Delta or another numeric value with:

```bash
make run BENCHMARK="$BENCHMARK" DELTA=auto
make profile BENCHMARK="$BENCHMARK" DELTA=2
```

Set the number of independent routing workers with
`--parallel-net-workers`. The default is `0`, which selects the worker count
automatically.

```bash
make run \
  BENCHMARK="$BENCHMARK" \
  PATHFINDER_ARGS='--parallel-net-workers 4'
```

The same option works for profiling:

```bash
make profile \
  BENCHMARK="$BENCHMARK" \
  DELTA=auto \
  PATHFINDER_ARGS='--parallel-net-workers 4'
```

Other useful `PATHFINDER_ARGS` include:

| Option | Purpose |
|---|---|
| `--net-limit N` | Route only the first `N` requests |
| `--max-sssp-iters N` | Limit Delta-Stepping rounds |
| `--delta-telemetry` | Print Delta-Stepping telemetry during a regular run |
| `--strict-routing` | Fail instead of writing partial routes |
| `--keep-work-dir` | Preserve intermediate CSR, metadata, and route files |
| `--work-dir PATH` | Choose the intermediate-file directory |

Multiple options can be placed in the same string:

```bash
make profile \
  BENCHMARK="$BENCHMARK" \
  PATHFINDER_ARGS='--parallel-net-workers 4 --net-limit 100 --keep-work-dir'
```

Profiled runs enable Delta-Stepping telemetry automatically. Use `DELTA=...`
for the bucket width instead of placing `--delta` in `PATHFINDER_ARGS`.

Display all wrapper options with:

```bash
./PathFinderFile --help
```

## 6. Run a custom benchmark

Without profiling:

```bash
make run \
  INPUT_PHYS=/absolute/path/example_unrouted.phys \
  LOGICAL_NETLIST=/absolute/path/example.netlist \
  OUTPUT_PHYS=/absolute/path/example_PathFinderFile.phys
```

With profiling:

```bash
make profile \
  INPUT_PHYS=/absolute/path/example_unrouted.phys \
  LOGICAL_NETLIST=/absolute/path/example.netlist \
  PROFILE_LABEL=example
```
