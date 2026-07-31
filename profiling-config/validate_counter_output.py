#!/usr/bin/env python3
"""Validate the focused rocprofv3 counter passes used by profile-all."""

from __future__ import annotations

import csv
import math
import sys
from collections import defaultdict
from pathlib import Path


REQUIRED_COUNTERS = (
    "SQ_INSTS_VALU",
    "MeanOccupancyPerActiveCU",
    "L2CacheHit",
    "SQ_WAIT_ANY",
    "SQ_WAVE_CYCLES",
)


def fail(message: str) -> None:
    print(f"counter validation failed: {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> None:
    if len(sys.argv) != 2:
        fail(f"usage: {Path(sys.argv[0]).name} COUNTER_OUTPUT_DIRECTORY")

    output_dir = Path(sys.argv[1])
    csv_files = sorted(output_dir.glob("pass_*/*/*_counter_collection.csv"))
    if not csv_files:
        fail(f"no counter_collection.csv files found below {output_dir}")

    values: dict[str, list[float]] = defaultdict(list)
    materialize_rows: dict[str, int] = defaultdict(int)
    for csv_file in csv_files:
        with csv_file.open(newline="", encoding="utf-8") as handle:
            for row in csv.DictReader(handle):
                counter = row.get("Counter_Name", "")
                if counter not in REQUIRED_COUNTERS:
                    continue
                try:
                    value = float(row["Counter_Value"])
                except (KeyError, TypeError, ValueError):
                    fail(f"invalid {counter} value in {csv_file}")
                if not math.isfinite(value):
                    fail(f"non-finite {counter} value in {csv_file}")
                values[counter].append(value)
                kernel = row.get("Kernel_Name", "")
                if any(
                    name in kernel
                    for name in (
                        "materialize_predecessors_kernel",
                        "measure_edge_parent_target_paths_kernel",
                        "fill_edge_parent_target_paths_kernel",
                    )
                ):
                    materialize_rows[counter] += 1

    problems: list[str] = []
    for counter in REQUIRED_COUNTERS:
        counter_values = values[counter]
        if not counter_values:
            problems.append(f"{counter} is missing")
            continue
        if not any(value > 0.0 for value in counter_values):
            problems.append(f"{counter} contains no positive measurements")
        if materialize_rows[counter] == 0:
            problems.append(f"{counter} contains no materialization-kernel rows")

    if problems:
        fail("; ".join(problems))

    summary = ", ".join(
        f"{counter}={len(values[counter])} rows"
        for counter in REQUIRED_COUNTERS
    )
    print(f"Counter validation passed: {summary}")


if __name__ == "__main__":
    main()
