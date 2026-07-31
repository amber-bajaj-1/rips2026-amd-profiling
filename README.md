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
  PATHFINDER_ARGS='--net-limit 1000 --parallel-net-workers 2'
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

### Package a profiling run on AUP Cloud

From the `rips2026-amd-profiling` directory, list the available runs:

```bash
find "profiling/$BENCHMARK" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | sort
```

Set `PROFILE_RUN` to the timestamp you want to visualize, then create and
validate an archive:

```bash
PROFILE_RUN=YYYYMMDD-HHMMSS
ARCHIVE="${BENCHMARK}-${PROFILE_RUN}-profiling.tar.gz"

tar -czf "$ARCHIVE" \
  -C "profiling/$BENCHMARK" \
  "$PROFILE_RUN"

tar -tzf "$ARCHIVE" >/dev/null && ls -lh "$ARCHIVE"
```

The archive is created in the repository root. Download it, then upload that archive to ChatGPT.

### ChatGPT prompt

Use this prompt with the uploaded archive:

```text
I attached a tar.gz archive containing one rips2026-amd-profiling run.
Extract it and inspect all files recursively.

Create and display one dashboard visualization with these exact section
headers:

1. Kernel time allocation
2. Hot-kernel diagnostics

For kernel time allocation, use the rocprofv3 runtime kernel statistics—not
wall-clock time. Group aggregate kernel duration into:

- Relax: relax_light_edges_kernel and relax_heavy_edges_kernel
- Reset: reset/touched-vertex kernels
- Materialize: measure_edge_parent_target_paths_kernel,
  fill_edge_parent_target_paths_kernel, and
  materialize_predecessors_kernel
- Other: every remaining kernel

Show a horizontal stacked allocation bar with each phase's percentage and
aggregate duration. Use blue for Relax, orange for Reset, green for
Materialize, and dark gray for Other.

Under Hot-kernel diagnostics, create separate Relax, Reset, and Materialize
columns. For each phase, show:

- VALU / peak
- L2 hit
- Resident waves, displayed as value / 64
- Wait-any cycles

Read the focused rocprofv3 counter_collection.csv files for
SQ_INSTS_VALU, MeanOccupancyPerActiveCU, L2CacheHit, and SQ_WAVE_CYCLES.
Use SQ_WAIT_ANY or SQ_WAIT_INST_ANY when present. If the public wait counter
is absent, read SQ_WAIT_ANY and SQ_WAVE_CYCLES from the
rocprof-compute-wait/pmc_perf_*.csv fallback files.

Use duration-weighted values where appropriate. Calculate wait percentage as
100 × sum(wait counter) / sum(SQ_WAVE_CYCLES). Derive VALU / peak using the
GPU architecture information recorded in the profile, and state any hardware
assumptions used. Label the wait metric if it came from the dedicated
single-worker replay.

Keep the dashboard compact and dark-themed, with colored progress bars matching
the kernel-allocation colors. Do not estimate missing data. If any requested
counter, phase, or architecture value is missing or contains no positive
measurements, show N/A for that metric and give me a clear warning explaining
which input is absent.
```

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
