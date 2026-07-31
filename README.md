# RIPS AMD Delta Stepping Profiling

Run GPU Delta-Stepping on AUP Cloud on FPGA24 Routing Contest Benchmarks (overlapping SSSPs). 

## 1. Set up the AUP workspace

Choose a writable root directory (e.g. /home/jovyan on TPE AUP Cloud), then run:

```bash
RIPS_ROOT=/path/to/your/workspace
chmod +x setup-tpe.sh
./setup-tpe.sh "$RIPS_ROOT"
source "$RIPS_ROOT/rips2026-amd-profiling/environment.sh"
cd "$RIPS_ROOT/rips2026-amd-profiling"
```

## 2. Choose a benchmark

```bash
BENCHMARK=logicnets_jscl
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

### Recommended 1000-net profile (L2 Hit, VALU, Wait Cycles, etc)

This run collects kernel time allocation and the available hot-kernel
diagnostic counters for the first 1000 nets with Delta `1` and two routing
workers:

```bash
make profile-all \
  BENCHMARK="$BENCHMARK" \
  DELTA=1 \
  PATHFINDER_ARGS='--net-limit 100 --parallel-net-workers 2'
```

Each command writes its wrapper log, telemetry, routed physical netlist, and
profiler results under:

```text
profiling/<benchmark>/<YYYYMMDD-HHMMSS>/
```

### General Profiling Guidelines
Collect the runtime trace and timing statistics used for kernel time
allocation:

```bash
make profile BENCHMARK="$BENCHMARK"
```

Collect the public `gfx115x` hardware counters:

```bash
make profile-counters BENCHMARK="$BENCHMARK"
```

Collect all hot-kernel diagnostics, including the wait-counter fallback:

```bash
make profile-diagnostics BENCHMARK="$BENCHMARK"
```

Collect both profiles sequentially:

```bash
make profile-all BENCHMARK="$BENCHMARK"
```

Runtime traces are under `runtime/rocprofv3/`. Public hardware-counter CSV
files are under `counters/rocprofv3-pmc/pass_*/`. The focused jobs collect:

| Metric | Diagnostic |
|---|---|
| `SQ_INSTS_VALU` | Vector-ALU instruction activity |
| `MeanOccupancyPerActiveCU` | Mean resident-wave occupancy on active CUs |
| `L2CacheHit` | L2 cache hit rate |
| `SQ_WAVE_CYCLES` | Wave-cycle denominator for the wait percentage |
| `SQ_WAIT_ANY` or `SQ_WAIT_INST_ANY` | Public wave-wait counter, when exposed |

Counter collection is restricted to the relaxation, cooperative controller,
touched-state reset, predecessor-measurement/fill/materialization, and
queue-flag-clear kernel families.

The Makefile checks that every selected counter is present, has a positive
measurement, and contains materialization-kernel rows.

The focused collector reports `SQ_INSTS_VALU` and `SQ_WAVE_CYCLES` directly;
their per-dispatch `Counter_Value` fields replace the unreliable zero-valued
`SQ_INSTS_VALU_sum` and `SQ_WAVE_CYCLES_sum` columns from the broad
multi-worker rocprof-compute pass. `MeanOccupancyPerActiveCU` supplies the
resident-wave metric. The selected wait counter divided by
`SQ_WAVE_CYCLES` supplies the wait percentage.

Runtime-trace timings remain the source for kernel time allocation;
counter-pass timings are diagnostic because PMC collection perturbs execution.
If the runtime trace identifies a different hot kernel, copy the supplied YAML,
adjust its `kernel_include_regex` values, and select it with
`COUNTER_INPUT=/absolute/path/to/custom.yaml`.

List the generated files with:

```bash
find "profiling/$BENCHMARK" -type f | sort
```

## 5. Visualizing Profile Results

## 6. Runtime options

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
make profile-all \
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
make profile-all \
  BENCHMARK="$BENCHMARK" \
  PATHFINDER_ARGS='--parallel-net-workers 4 --net-limit 100 --keep-work-dir'
```

Profiled runs enable Delta-Stepping telemetry automatically. Use `DELTA=...`
for the bucket width instead of placing `--delta` in `PATHFINDER_ARGS`.

Display all wrapper options with:

```bash
./PathFinderFile --help
```
