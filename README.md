# RIPS AMD SSSP profiling

This repository implements two SSSP kernels and runs all-source SSSP on the benchmarks from the FPGA24 Routing Contest (without accounting for congestion): 

- Bellman-Ford (`bellman-ford` or `bf11`)
- Delta-Stepping (`delta-step` or `delta-stepping`)

## Set up and build

From this repository, choose a writable workspace root and run:

```bash
chmod +x setup-tpe.sh
./setup-tpe.sh /home/jovyan
source /home/jovyan/rips2026-amd-profiling/environment.sh
cd /home/jovyan/rips2026-amd-profiling
make pipeline
make device-graph
```

Both engines are compiled into the same executable, so engine selection does
not require a rebuild.

## Quick start 
Bounding boxes with margins x=2 and y=14 are used by default. To run the bounding box versions of delta stepping and bellman ford, use: 

```bash
make run BENCHMARK=logicnets_jscl PATHFINDER_SSSP_ENGINE=delta-step
make run BENCHMARK=logicnets_jscl PATHFINDER_SSSP_ENGINE=bf11
```

To run with profiling, use: 

```bash
make profile-all BENCHMARK=logicnets_jscl PATHFINDER_SSSP_ENGINE=delta-step
make profile-all BENCHMARK=logicnets_jscl PATHFINDER_SSSP_ENGINE=bf11
```

## Coordinate bounds and CSR v3

Normal routing is bounded by default for both engines. The default inclusive
query box is the envelope of every known source and target coordinate,
expanded by X=2 and Y=14. A known-coordinate destination outside that box is
not relaxed or enqueued. Nodes whose route-end coordinate is the paired
missing sentinel remain admissible, matching BF11.

Bounded routing requires a CSR v3 artifact with validated `route_end_x` and
`route_end_y` sidecars. The normal conversion pipeline writes v3. If an older
artifact is supplied while bounds are enabled, PathFinder fails with an error
that asks you to regenerate it or choose explicit unbounded mode. Legacy CSR
v1/v2 remains supported with `--unbounded` (or `--bf11-unbounded`).

Useful controls, passed through `PATHFINDER_ARGS`, are:

| Option | Default | Meaning |
|---|---:|---|
| `--bbox-margin-x N` | `2` | Nonnegative horizontal margin |
| `--bbox-margin-y N` | `14` | Nonnegative vertical margin |
| `--unbounded` | off | Disable coordinate bounds explicitly |
| `--no-unbounded-fallback` | off | Keep an unreachable bounded query bounded |
| `--target-check-interval N` | `1` | BF11 target-certificate check interval |

The BF11 compatibility names remain accepted and forwarded:
`--bf11-bbox-margin-x`, `--bf11-bbox-margin-y`, `--bf11-unbounded`,
`--bf11-no-unbounded-fallback`, and `--bf11-target-check-interval`.

When fallback is enabled, a bounded query that does not return all requested
targets with a completion certificate is reset and rerun unbounded exactly
once. A target without coordinates starts unbounded when fallback is enabled;
that is recorded separately and is not a retry. If fallback is disabled, such
a target is an actionable input error. Fallback does not compare a successful
in-bounds path with possible out-of-bounds paths, so bounded routing is not a
global-shortest-path guarantee.

PathFinder accepts a route only when the engine reports `converged` or
`stopped_on_target`. A finite distance from an iteration-limited,
uncertified run is never extracted as a final route.

Each run emits a `routing_bounds` JSON record containing the enabled state,
margins, fallback setting, bounded-query count, missing-coordinate query count,
unbounded-retry count, and a small sample of computed query boxes. Per-net
route JSON also records certificate, bounded, and retry state.

## Run both engines on identical bounds

Delta-Stepping is the default:

```bash
make run BENCHMARK=logicnets_jscl PATHFINDER_SSSP_ENGINE=delta-step
```

Run BF11 with the same default bounds:

```bash
make run BENCHMARK=logicnets_jscl PATHFINDER_SSSP_ENGINE=bf11
```

Use identical custom bounds for an A/B comparison:

```bash
make run \
  BENCHMARK=logicnets_jscl \
  PATHFINDER_SSSP_ENGINE=delta-step \
  PATHFINDER_ARGS='--bbox-margin-x 4 --bbox-margin-y 20'

make run \
  BENCHMARK=logicnets_jscl \
  PATHFINDER_SSSP_ENGINE=bf11 \
  PATHFINDER_ARGS='--bbox-margin-x 4 --bbox-margin-y 20'
```

Bundled benchmark names are `logicnets_jscl`, `boom_med_pb`, `vtr_mcml`, and
`rosetta_fd`. Routed physical netlists are written under `benchmarks/`.

## Profiling

The selected engine and all bounds controls are forwarded to every profiling
entry point:

```bash
make profile BENCHMARK=logicnets_jscl PATHFINDER_SSSP_ENGINE=delta-step
make profile-counters BENCHMARK=logicnets_jscl PATHFINDER_SSSP_ENGINE=bf11
make profile-diagnostics BENCHMARK=logicnets_jscl
make profile-all BENCHMARK=logicnets_jscl
```

Results are written below
`profiling/<benchmark>/<YYYYMMDD-HHMMSS>/`. Profiled Delta runs enable Delta
telemetry; profiled BF11 runs enable BF11 telemetry. BF11 output includes
persistent/host-controller counts, sparse reset counts, workspace memory, and
phase timing when telemetry is enabled. The generic Delta output includes its
resolved delta, controller, queue/work counters, and worker count.

Important shared and engine-specific controls include:

| Option | Applies to | Purpose |
|---|---|---|
| `--net-limit N` | Both | Route only the first `N` requests |
| `--parallel-net-workers N` | Both | Independent streams/workspaces; `0` selects automatically |
| `--max-sssp-iters N` | Both | Limit SSSP work; `-1` uses the engine default |
| `DELTA=auto` or `DELTA=N` | Delta | Automatic or numeric bucket width |
| `--delta-multiplier N` | Delta | Scale automatic delta |
| `--delta-controller MODE` | Delta | `host-checked` or `reduced-round-trip` |
| `--delta-controller-batch-size N` | Delta | Reduced-round-trip batch size |
| `--delta-force-legacy-parent` | Delta | Legacy predecessor recovery A/B control |
| `--delta-telemetry` | Delta | Emit aggregate scheduler telemetry |
| `--bf11-telemetry` | BF11 | Emit phase, work, reset, and memory telemetry |
| `--strict-routing` | Both | Fail instead of writing partial routes |
| `--keep-work-dir` / `--work-dir PATH` | Both | Preserve or choose intermediate artifacts |

Delta-only options are rejected with BF11. This focused build always uses the
real generic Delta bucket scheduler, including for unit-weight inputs; the
compatibility flag `--delta-force-generic` is therefore accepted as a no-op.

## Tests

Run all tests available without an AMD GPU:

```bash
make test-host
```

On a ROCm system with an AMD GPU, also run:

```bash
make test-hip
```

Use `make help`, `./pathfinder --help`, or `./PathFinderFile --help` for the
complete command-line reference.
