#!/usr/bin/env python3
"""Validate the focused rocprofv3 counter passes used by profile-all."""

from __future__ import annotations

import argparse
import csv
import math
import sys
from collections import defaultdict
from pathlib import Path


COMMON_COUNTERS = (
    "SQ_INSTS_VALU",
    "MeanOccupancyPerActiveCU",
    "L2CacheHit",
    "SQ_WAVE_CYCLES",
)


def fail(message: str) -> None:
    print(f"counter validation failed: {message}", file=sys.stderr)
    raise SystemExit(1)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--wait-counter",
        choices=("SQ_WAIT_ANY", "SQ_WAIT_INST_ANY", "none"),
        default="SQ_WAIT_ANY",
    )
    parser.add_argument("output_directory", type=Path)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    output_dir = args.output_directory
    required_counters = COMMON_COUNTERS
    if args.wait_counter != "none":
        required_counters = (*required_counters, args.wait_counter)
    csv_files = sorted(output_dir.glob("pass_*/*/*_counter_collection.csv"))
    if not csv_files:
        fail(f"no counter_collection.csv files found below {output_dir}")

    values: dict[str, list[float]] = defaultdict(list)
    materialize_rows: dict[str, int] = defaultdict(int)
    for csv_file in csv_files:
        with csv_file.open(newline="", encoding="utf-8") as handle:
            for row in csv.DictReader(handle):
                counter = row.get("Counter_Name", "")
                if counter not in required_counters:
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
                        "materialize_target_paths_kernel",
                    )
                ):
                    materialize_rows[counter] += 1

    problems: list[str] = []
    for counter in required_counters:
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
        for counter in required_counters
    )
    print(f"Counter validation passed: {summary}")


if __name__ == "__main__":
    main()
