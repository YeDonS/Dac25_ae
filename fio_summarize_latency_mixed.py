#!/usr/bin/env python3
"""Summarize fio_latency_mixed JSON and test-phase stats into one table."""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path
from typing import Any


DIST_NAMES = ("zipf", "normal", "uniform", "random")

STATS_KEYS = [
    "compile_read_repromotion_enabled",
    "compile_die_batched_repromotion_enabled",
    "compile_qlc_hotcold_enabled",
    "compile_qlc_rebalance_enabled",
    "compile_test_phase_repromotion_enabled",
    "compile_test_phase_qlc_rebalance_enabled",
    "read_requests",
    "overwrite_requests",
    "host_write_pages",
    "host_write_nand_ops",
    "read_bg_conflicts",
    "read_nand_bg_overlap_ops",
    "read_die_conflicts",
    "read_die_wait_ns",
    "host_read_nand_ops",
    "host_read_slc_nand_ops",
    "host_read_qlc_nand_ops",
    "bg_repromote_ops",
    "bg_qlc_rebalance_ops",
    "slc_to_qlc_migration_pages",
    "slc_gc_valid_copy_pages",
    "slc_gc_invalid_reclaimed_pages",
    "qlc_repromote_pages",
    "internal_write_pages_est",
    "internal_write_pages_per_host_write_x1000",
    "slc_to_qlc_nand_reads",
    "slc_to_qlc_nand_writes",
    "repromote_nand_reads",
    "repromote_nand_writes",
    "active_reads",
    "active_overwrites",
    "active_bg_ops",
]

DRAIN_DELTA_KEYS = [
    "bg_repromote_ops",
    "bg_qlc_rebalance_ops",
    "slc_to_qlc_migration_pages",
    "slc_gc_valid_copy_pages",
    "slc_gc_invalid_reclaimed_pages",
    "qlc_repromote_pages",
    "internal_write_pages_est",
    "slc_to_qlc_nand_reads",
    "slc_to_qlc_nand_writes",
    "repromote_nand_reads",
    "repromote_nand_writes",
]


def load_fio_json(path: Path) -> dict[str, Any]:
    text = path.read_text(errors="replace")
    start = text.find("{")
    if start < 0:
        raise ValueError(f"{path}: JSON object not found")
    return json.loads(text[start:])


def job_name(job: dict[str, Any]) -> str:
    return str(job.get("jobname", ""))


def io_bytes(job: dict[str, Any], ddir: str) -> int:
    return int(job.get(ddir, {}).get("io_bytes") or 0)


def sum_io(jobs: list[dict[str, Any]], prefix: str, ddir: str) -> int:
    return sum(io_bytes(job, ddir) for job in jobs if job_name(job).startswith(prefix))


def sum_runtime_ms(jobs: list[dict[str, Any]], prefix: str) -> int:
    total = 0
    for job in jobs:
        if job_name(job).startswith(prefix):
            total += int(job.get("read", {}).get("runtime") or 0)
            total += int(job.get("write", {}).get("runtime") or 0)
    return total


def pct_ms(op: dict[str, Any], key: str) -> float:
    percentiles = op.get("clat_ns", {}).get("percentile", {})
    value = percentiles.get(key)
    if value is None:
        try:
            value = percentiles.get(float(key))
        except ValueError:
            value = None
    return float(value or 0) / 1_000_000.0


def mib_s(op: dict[str, Any]) -> float:
    return float(op.get("bw") or 0) / 1024.0


def parse_run_name(path: Path) -> dict[str, str]:
    stem = path.stem
    match = re.match(
        r"^fio_mixed_(?P<body>.+)_rw(?P<ratio>[0-9]+(?:_[0-9]+)?)(?:_(?P<ts>[0-9]{8}_[0-9]{6}))?$",
        stem,
    )
    if not match:
        return {
            "tag": stem,
            "variant": path.parent.parent.parent.name if len(path.parents) >= 3 else "",
            "dist": path.parent.parent.name if len(path.parents) >= 2 else "",
            "ratio": path.parent.name if path.parent.name.startswith("rw") else "",
            "timestamp": "",
        }

    body = match.group("body")
    dist = ""
    variant = body
    for candidate in DIST_NAMES:
        suffix = f"_{candidate}"
        if body.endswith(suffix):
            dist = candidate
            variant = body[: -len(suffix)]
            break

    ratio = f"rw{match.group('ratio')}"
    return {
        "tag": stem,
        "variant": variant,
        "dist": dist,
        "ratio": ratio,
        "timestamp": match.group("ts") or "",
    }


def parse_stats_file(path: Path | None) -> dict[str, int]:
    if not path or not path.exists():
        return {}
    stats: dict[str, int] = {}
    for line in path.read_text(errors="replace").splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1].lstrip("-").isdigit():
            stats[parts[0]] = int(parts[1])
    return stats


def collect_json_files(paths: list[Path]) -> list[Path]:
    files: list[Path] = []
    for path in paths:
        if path.is_dir():
            files.extend(sorted(path.rglob("fio_mixed_*.json")))
        elif path.is_file() and path.suffix == ".json":
            files.append(path)
        else:
            print(f"[fio-summary] skip missing/non-json path: {path}", file=sys.stderr)
    return sorted(dict.fromkeys(files))


def build_stats_index(paths: list[Path], json_files: list[Path]) -> dict[str, Path]:
    roots = {path if path.is_dir() else path.parent for path in paths}
    roots.update(path.parent for path in json_files)
    index: dict[str, Path] = {}
    for root in roots:
        if not root.exists():
            continue
        for path in root.rglob("fio_test_phase_stats_aggregate_*.txt"):
            index[path.name] = path
    return index


def find_stats(index: dict[str, Path], json_path: Path, tag: str, post_drain: bool) -> Path | None:
    suffix = "_post_drain" if post_drain else ""
    name = f"fio_test_phase_stats_aggregate_{tag}{suffix}.txt"
    local = json_path.parent / name
    if local.exists():
        return local
    return index.get(name)


def fmt_float(value: float, digits: int = 3) -> str:
    return f"{value:.{digits}f}"


def stat_value(stats: dict[str, int], key: str) -> str:
    return "" if key not in stats else str(stats[key])


def ratio(numerator: int, denominator: int) -> str:
    if denominator <= 0:
        return ""
    return fmt_float(numerator / denominator, 6)


def summarize_one(json_path: Path, stats_index: dict[str, Path]) -> dict[str, str]:
    meta = parse_run_name(json_path)
    data = load_fio_json(json_path)
    jobs = data.get("jobs", [])
    if not isinstance(jobs, list):
        raise ValueError(f"{json_path}: jobs is not a list")
    job_map = {job_name(job): job for job in jobs if isinstance(job, dict)}
    read_job = job_map.get("measure_reads", {})
    write_job = job_map.get("measure_writes", {})
    read_op = read_job.get("read", {})
    write_op = write_job.get("write", {})
    errors = sum(1 for job in jobs if isinstance(job, dict) and int(job.get("error") or 0) != 0)

    fg_path = find_stats(stats_index, json_path, meta["tag"], post_drain=False)
    post_path = find_stats(stats_index, json_path, meta["tag"], post_drain=True)
    fg = parse_stats_file(fg_path)
    post = parse_stats_file(post_path)

    row: dict[str, str] = {
        "variant": meta["variant"],
        "dist": meta["dist"],
        "ratio": meta["ratio"],
        "timestamp": meta["timestamp"],
        "json_file": str(json_path),
        "foreground_stats": str(fg_path or ""),
        "post_drain_stats": str(post_path or ""),
        "fio_errors": str(errors),
        "jobs": str(len(jobs)),
        "init_write_gib": fmt_float(sum_io(jobs, "init_write_", "write") / 2**30),
        "init_prewarm_gib": fmt_float(sum_io(jobs, "init_prewarm_", "read") / 2**30),
        "measure_read_gib": fmt_float(int(read_op.get("io_bytes") or 0) / 2**30),
        "measure_write_gib": fmt_float(int(write_op.get("io_bytes") or 0) / 2**30),
        "read_iops": fmt_float(float(read_op.get("iops") or 0)),
        "read_mib_s": fmt_float(mib_s(read_op)),
        "read_mean_ms": fmt_float(float(read_op.get("clat_ns", {}).get("mean") or 0) / 1_000_000.0),
        "read_p99_ms": fmt_float(pct_ms(read_op, "99.000000")),
        "read_p999_ms": fmt_float(pct_ms(read_op, "99.900000")),
        "read_p9999_ms": fmt_float(pct_ms(read_op, "99.990000")),
        "read_max_s": fmt_float(float(read_op.get("clat_ns", {}).get("max") or 0) / 1_000_000_000.0),
        "write_iops": fmt_float(float(write_op.get("iops") or 0)),
        "write_mib_s": fmt_float(mib_s(write_op)),
        "write_mean_ms": fmt_float(float(write_op.get("clat_ns", {}).get("mean") or 0) / 1_000_000.0),
        "write_p99_ms": fmt_float(pct_ms(write_op, "99.000000")),
        "write_p999_ms": fmt_float(pct_ms(write_op, "99.900000")),
        "write_p9999_ms": fmt_float(pct_ms(write_op, "99.990000")),
        "write_max_s": fmt_float(float(write_op.get("clat_ns", {}).get("max") or 0) / 1_000_000_000.0),
        "init_write_s": fmt_float(sum_runtime_ms(jobs, "init_write_") / 1000.0),
        "init_prewarm_s": fmt_float(sum_runtime_ms(jobs, "init_prewarm_") / 1000.0),
        "read_runtime_s": fmt_float(float(read_op.get("runtime") or 0) / 1000.0),
        "write_runtime_s": fmt_float(float(write_op.get("runtime") or 0) / 1000.0),
    }

    for key in STATS_KEYS:
        row[f"fg_{key}"] = stat_value(fg, key)
        row[f"post_{key}"] = stat_value(post, key)

    for key in DRAIN_DELTA_KEYS:
        if key in fg and key in post:
            row[f"delta_{key}"] = str(post[key] - fg[key])
        else:
            row[f"delta_{key}"] = ""

    fg_reads = fg.get("read_requests", 0)
    post_reads = post.get("read_requests", 0)
    fg_host_writes = fg.get("host_write_pages", 0)
    post_host_writes = post.get("host_write_pages", 0)
    row["fg_read_die_wait_ms_per_read"] = ratio(fg.get("read_die_wait_ns", 0) / 1_000_000.0, fg_reads)
    row["post_read_die_wait_ms_per_read"] = ratio(post.get("read_die_wait_ns", 0) / 1_000_000.0, post_reads)
    row["fg_valid_copy_per_host_write"] = ratio(fg.get("slc_gc_valid_copy_pages", 0), fg_host_writes)
    row["post_valid_copy_per_host_write"] = ratio(post.get("slc_gc_valid_copy_pages", 0), post_host_writes)
    row["fg_internal_write_per_host_write"] = (
        fmt_float(fg["internal_write_pages_per_host_write_x1000"] / 1000.0, 6)
        if "internal_write_pages_per_host_write_x1000" in fg
        else ""
    )
    row["post_internal_write_per_host_write"] = (
        fmt_float(post["internal_write_pages_per_host_write_x1000"] / 1000.0, 6)
        if "internal_write_pages_per_host_write_x1000" in post
        else ""
    )
    return row


def columns() -> list[str]:
    base = [
        "variant",
        "dist",
        "ratio",
        "timestamp",
        "fio_errors",
        "jobs",
        "init_write_gib",
        "init_prewarm_gib",
        "measure_read_gib",
        "measure_write_gib",
        "read_iops",
        "read_mib_s",
        "read_mean_ms",
        "read_p99_ms",
        "read_p999_ms",
        "read_p9999_ms",
        "read_max_s",
        "write_iops",
        "write_mib_s",
        "write_mean_ms",
        "write_p99_ms",
        "write_p999_ms",
        "write_p9999_ms",
        "write_max_s",
        "init_write_s",
        "init_prewarm_s",
        "read_runtime_s",
        "write_runtime_s",
        "foreground_stats",
        "post_drain_stats",
    ]
    derived = [
        "fg_read_die_wait_ms_per_read",
        "post_read_die_wait_ms_per_read",
        "fg_valid_copy_per_host_write",
        "post_valid_copy_per_host_write",
        "fg_internal_write_per_host_write",
        "post_internal_write_per_host_write",
    ]
    stats = [f"fg_{key}" for key in STATS_KEYS] + [f"post_{key}" for key in STATS_KEYS]
    deltas = [f"delta_{key}" for key in DRAIN_DELTA_KEYS]
    return base + derived + deltas + stats + ["json_file"]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Summarize fio_latency_mixed JSON files and foreground/post-drain stats."
    )
    parser.add_argument(
        "paths",
        nargs="*",
        type=Path,
        default=[Path("result/fio_latency_mixed")],
        help="Result directories or fio_mixed_*.json files.",
    )
    parser.add_argument("--output", "-o", type=Path, help="Write the table to this file.")
    parser.add_argument("--format", choices=("tsv", "csv"), default="tsv")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    json_files = collect_json_files(args.paths)
    if not json_files:
        print("[fio-summary] no fio_mixed_*.json files found", file=sys.stderr)
        return 1

    stats_index = build_stats_index(args.paths, json_files)
    rows = [summarize_one(path, stats_index) for path in json_files]

    delimiter = "\t" if args.format == "tsv" else ","
    output = args.output.open("w", newline="") if args.output else sys.stdout
    try:
        writer = csv.DictWriter(
            output,
            fieldnames=columns(),
            delimiter=delimiter,
            extrasaction="ignore",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(rows)
    finally:
        if args.output:
            output.close()

    if args.output:
        print(f"[fio-summary] wrote {len(rows)} rows to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
