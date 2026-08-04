#!/usr/bin/env python3
"""Validate the single-worker rocprof-compute wait-counter replay."""

from __future__ import annotations

import csv
import math
import sys
from pathlib import Path


TARGET_KERNELS = (
    "relax_light_edges_kernel",
    "relax_heavy_edges_kernel",
    "reset_",
    "touched_vertices_kernel",
    "materialize_predecessors_kernel",
    "measure_edge_parent_target_paths_kernel",
    "fill_edge_parent_target_paths_kernel",
    "clear_workspace_kernel",
    "seed_sources_kernel",
    "relax_frontier_kernel",
    "update_target_status_kernel",
    "summarize_target_paths_kernel",
    "materialize_target_paths_kernel",
)
MATERIALIZE_KERNELS = (
    "materialize_predecessors_kernel",
    "measure_edge_parent_target_paths_kernel",
    "fill_edge_parent_target_paths_kernel",
    "materialize_target_paths_kernel",
)


def fail(message: str) -> None:
    print(f"wait-counter validation failed: {message}", file=sys.stderr)
    raise SystemExit(1)


def number(row: dict[str, str], name: str, source: Path) -> float:
    try:
        value = float(row[name])
    except (KeyError, TypeError, ValueError):
        fail(f"invalid {name} value in {source}")
    if not math.isfinite(value):
        fail(f"non-finite {name} value in {source}")
    return value


def main() -> None:
    if len(sys.argv) != 2:
        fail(f"usage: {Path(sys.argv[0]).name} ROCPROF_COMPUTE_OUTPUT_DIRECTORY")

    output_dir = Path(sys.argv[1])
    csv_files = sorted(output_dir.glob("pmc_perf_*.csv"))
    if not csv_files:
        fail(f"no pmc_perf_*.csv files found below {output_dir}")

    wait_values: list[float] = []
    wave_values: list[float] = []
    materialize_rows = 0
    for csv_file in csv_files:
        with csv_file.open(newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            fields = set(reader.fieldnames or ())
            if "SQ_WAIT_ANY" not in fields or "SQ_WAVE_CYCLES" not in fields:
                continue
            for row in reader:
                kernel = row.get("Kernel_Name", "")
                if not any(name in kernel for name in TARGET_KERNELS):
                    continue
                wait_values.append(number(row, "SQ_WAIT_ANY", csv_file))
                wave_values.append(number(row, "SQ_WAVE_CYCLES", csv_file))
                if any(name in kernel for name in MATERIALIZE_KERNELS):
                    materialize_rows += 1

    problems: list[str] = []
    if not wait_values:
        problems.append("SQ_WAIT_ANY is missing")
    elif not any(value > 0.0 for value in wait_values):
        problems.append("SQ_WAIT_ANY contains no positive measurements")
    if not wave_values:
        problems.append("SQ_WAVE_CYCLES is missing")
    elif not any(value > 0.0 for value in wave_values):
        problems.append("SQ_WAVE_CYCLES contains no positive measurements")
    if materialize_rows == 0:
        problems.append("the wait pass contains no materialization-kernel rows")
    if problems:
        fail("; ".join(problems))

    print(
        "Wait-counter validation passed: "
        f"SQ_WAIT_ANY={len(wait_values)} rows, "
        f"SQ_WAVE_CYCLES={len(wave_values)} rows"
    )


if __name__ == "__main__":
    main()
