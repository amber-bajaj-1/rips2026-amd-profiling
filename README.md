# RIPS AMD SSSP Profiling

This repository builds one GPU `pathfinder` executable with two runtime SSSP
engines: Delta-Stepping and Bellman-Ford. 'pathfinder' runs one SSSP calculation for each net in the benchmarks. 

## 1. Set up the AUP workspace

From this repository, choose a writable workspace root and run:

```bash
chmod +x setup-tpe.sh
./setup-tpe.sh /path/to/workspace
source /path/to/workspace/rips2026-amd-profiling/environment.sh
cd /path/to/workspace/rips2026-amd-profiling
```

The setup script retains the profiling configuration, installs the local
dependencies, prepares the benchmark/device artifacts, and builds the full
pipeline.

## 2. Build the pipeline

```bash
make pipeline
make device-graph
```

Both SSSP engines are compiled into the same `pathfinder` executable; selecting
an engine never requires rebuilding.

## 3. Select an SSSP engine

Use `PATHFINDER_SSSP_ENGINE=delta-step` (or the alias `delta-stepping`) for
Delta-Stepping. It is the default. Use
`PATHFINDER_SSSP_ENGINE=bellman-ford` for the unbounded active-frontier
Bellman-Ford backend.

## 4. Run a benchmark

Delta-Stepping example:

```bash
make run BENCHMARK=logicnets_jscl PATHFINDER_SSSP_ENGINE=delta-step
```

Bellman-Ford example:

```bash
make run BENCHMARK=logicnets_jscl PATHFINDER_SSSP_ENGINE=bellman-ford
```

Bundled benchmark names are `logicnets_jscl`, `boom_med_pb`, `vtr_mcml`, and
`rosetta_fd`. Routed physical netlists are written under `benchmarks/`.

## 5. Run profiling targets

The selected engine is forwarded to every profiling command:

```bash
make profile BENCHMARK=logicnets_jscl PATHFINDER_SSSP_ENGINE=delta-step
make profile-counters BENCHMARK=logicnets_jscl PATHFINDER_SSSP_ENGINE=bellman-ford
make profile-diagnostics BENCHMARK=logicnets_jscl
make profile-all BENCHMARK=logicnets_jscl
```

Results are written below
`profiling/<benchmark>/<YYYYMMDD-HHMMSS>/`. `profile` collects the runtime
trace and statistics; `profile-counters` collects the configured gfx115x
counters; `profile-diagnostics` also runs the selected wait-counter path; and
`profile-all` runs the existing profiling flow sequentially.

## 6. Important runtime options

Options can be passed through `PATHFINDER_ARGS`:

| Option | Applies to | Purpose |
|---|---|---|
| `--net-limit N` | Both | Route only the first `N` requests |
| `--parallel-net-workers N` | Both | Set independent workers; `0` selects automatically |
| `--max-sssp-iters N` | Both | Limit SSSP rounds; `-1` uses the engine default |
| `DELTA=auto` or `DELTA=N` | Delta only | Select automatic or numeric bucket width |
| `--delta-controller MODE` | Delta only | Select `host-checked` or `reduced-round-trip` |
| `--delta-controller-batch-size N` | Delta only | Set reduced-round-trip batch size |
| `--delta-force-legacy-parent` | Delta only | Use legacy predecessor recovery for comparison |
| `--delta-telemetry` | Delta only | Emit aggregate Delta telemetry on a regular run |
| `--strict-routing` | Both | Fail instead of writing partial routes |
| `--keep-work-dir` / `--work-dir PATH` | Both | Preserve or choose intermediate artifacts |

For example:

```bash
make run \
  BENCHMARK=logicnets_jscl \
  PATHFINDER_SSSP_ENGINE=bellman-ford \
  PATHFINDER_ARGS='--net-limit 100 --parallel-net-workers 2'
```

Delta-only options are rejected when Bellman-Ford is selected. Profiled Delta
runs add `--delta-telemetry` automatically; Bellman-Ford runs retain normal
ROCTX/runtime instrumentation without Delta telemetry.

Use `make help`, `./pathfinder --help`, or `./PathFinderFile --help` for the
complete command-line reference.
