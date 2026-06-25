#!/usr/bin/env python3
"""Capture and compare reproducibility manifests for fast_24 experiments."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import socket
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


CONTRACTS: tuple[tuple[re.Pattern[str], dict[str, int]], ...] = (
    (
        re.compile(r"^die_latency[123]_qlc_all_norp_sb$"),
        {
            "compile_read_repromotion_enabled": 0,
            "compile_die_batched_repromotion_enabled": 0,
            "compile_qlc_hotcold_enabled": 1,
            "compile_qlc_rebalance_enabled": 0,
            "compile_test_phase_repromotion_enabled": 0,
            "compile_test_phase_qlc_rebalance_enabled": 0,
        },
    ),
    (
        re.compile(r"^die_latency[123]_norp_sb$"),
        {
            "compile_read_repromotion_enabled": 0,
            "compile_die_batched_repromotion_enabled": 0,
            "compile_qlc_hotcold_enabled": 0,
            "compile_qlc_rebalance_enabled": 0,
            "compile_test_phase_repromotion_enabled": 0,
            "compile_test_phase_qlc_rebalance_enabled": 0,
        },
    ),
)


def sha256(path: Path) -> str | None:
    if not path.is_file():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def artifact_record(raw_path: str) -> dict[str, Any]:
    path = Path(raw_path).expanduser().resolve()
    return {
        "path": str(path),
        "exists": path.is_file(),
        "size_bytes": path.stat().st_size if path.is_file() else None,
        "sha256": sha256(path),
    }


def parse_pairs(items: list[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    for item in items:
        if "=" not in item:
            raise ValueError(f"parameter must be KEY=VALUE: {item!r}")
        key, value = item.split("=", 1)
        if not key:
            raise ValueError(f"parameter key is empty: {item!r}")
        out[key] = value
    return out


def parse_stats(path: Path | None) -> dict[str, Any]:
    if path is None or not path.is_file():
        return {}
    stats: dict[str, Any] = {}
    for raw in path.read_text(errors="replace").splitlines():
        parts = raw.split()
        if len(parts) != 2:
            continue
        key, value = parts
        try:
            stats[key] = int(value)
        except ValueError:
            stats[key] = value
    return stats


def expected_contract(variant: str) -> dict[str, int] | None:
    for pattern, expected in CONTRACTS:
        if pattern.match(variant):
            return expected
    return None


def contract_result(variant: str, stats: dict[str, Any]) -> dict[str, Any]:
    expected = expected_contract(variant)
    if expected is None:
        return {"status": "unrecognized_variant", "expected": {}, "observed": {}}
    observed = {key: stats.get(key, "MISSING") for key in expected}
    mismatches = {
        key: {"expected": value, "observed": observed[key]}
        for key, value in expected.items()
        if observed[key] != value
    }
    return {
        "status": "pass" if not mismatches else "fail",
        "expected": expected,
        "observed": observed,
        "mismatches": mismatches,
    }


def metric_contract_result(stats: dict[str, Any]) -> dict[str, Any]:
    required = (
        "read_req_latency_count",
        "read_req_latency_hist_bins",
    )
    missing = [key for key in required if key not in stats]
    if missing:
        return {"status": "fail", "missing": missing}

    expected_bins = int(stats["read_req_latency_hist_bins"])
    counts: list[int] = []
    incomplete: list[int] = []
    for bucket in range(expected_bins):
        upper_key = f"read_req_latency_hist_bin_{bucket:02d}_upper_ns"
        count_key = f"read_req_latency_hist_bin_{bucket:02d}_count"
        if upper_key not in stats or count_key not in stats:
            incomplete.append(bucket)
            continue
        counts.append(int(stats[count_key]))

    sample_sum = sum(counts)
    request_count = int(stats["read_req_latency_count"])
    status = "pass"
    if incomplete or sample_sum != request_count:
        status = "fail"
    return {
        "status": status,
        "expected_bins": expected_bins,
        "incomplete_bins": incomplete,
        "sample_sum": sample_sum,
        "request_count": request_count,
    }
def capture(args: argparse.Namespace) -> int:
    output = Path(args.output).expanduser().resolve()
    module = Path(args.module).expanduser().resolve() if args.module else None
    stats_path = Path(args.stats).expanduser().resolve() if args.stats else None
    stats = parse_stats(stats_path)
    contract = contract_result(args.variant, stats) if stats else {
        "status": "missing_stats" if args.status == "completed" else "pending",
        "expected": expected_contract(args.variant) or {},
        "observed": {},
    }
    model_contract = {
        "status": "pass" if stats.get("read_priority_model") == args.model else "fail",
        "expected": args.model,
        "observed": stats.get("read_priority_model", "MISSING"),
    } if stats else {"status": "pending", "expected": args.model, "observed": None}
    metric_contract = metric_contract_result(stats) if stats else {"status": "pending"}
    now = datetime.now(timezone.utc).isoformat()
    created_at = now
    if output.is_file():
        try:
            created_at = json.loads(output.read_text()).get("created_at", now)
        except (json.JSONDecodeError, OSError):
            pass

    manifest = {
        "schema": "fast24-research-run-manifest-v1",
        "created_at": created_at,
        "updated_at": now,
        "status": args.status,
        "workload": args.workload,
        "variant": args.variant,
        "scheduler_model": args.model,
        "parameters": parse_pairs(args.param),
        "module": artifact_record(str(module)) if module else None,
        "artifacts": [artifact_record(path) for path in args.artifact],
        "stats_file": artifact_record(str(stats_path)) if stats_path else None,
        "compile_contract": contract,
        "model_contract": model_contract,
        "metric_contract": metric_contract,
        "environment": {
            "hostname": socket.gethostname(),
            "platform": platform.platform(),
            "kernel": platform.release(),
            "python": platform.python_version(),
            "cwd": os.getcwd(),
        },
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(f"[run-manifest] wrote {output}")

    strict_failures = {
        "compile_contract": contract["status"],
        "model_contract": model_contract["status"],
        "metric_contract": metric_contract["status"],
    }
    if args.strict_contract and any(status != "pass" for status in strict_failures.values()):
        print(
            f"[run-manifest] strict contract failed: {strict_failures}",
            file=sys.stderr,
        )
        return 2
    return 0


def comparable_projection(manifest: dict[str, Any], ignored: set[str]) -> dict[str, Any]:
    parameters = {
        key: value
        for key, value in manifest.get("parameters", {}).items()
        if key not in ignored
    }
    return {
        "schema": manifest.get("schema"),
        "workload": manifest.get("workload"),
        "scheduler_model": manifest.get("scheduler_model"),
        "parameters": parameters,
    }


def compare(args: argparse.Namespace) -> int:
    paths = [Path(path).expanduser().resolve() for path in args.manifests]
    manifests = [json.loads(path.read_text()) for path in paths]
    ignored = set(args.ignore_param)
    reference = comparable_projection(manifests[0], ignored)
    mismatches: list[dict[str, Any]] = []

    for path, manifest in zip(paths, manifests):
        projection = comparable_projection(manifest, ignored)
        if projection != reference:
            mismatches.append({"path": str(path), "projection": projection})
        contract_status = manifest.get("compile_contract", {}).get("status")
        if contract_status != "pass":
            mismatches.append(
                {"path": str(path), "compile_contract_status": contract_status}
            )
        for contract_name in ("model_contract", "metric_contract"):
            status = manifest.get(contract_name, {}).get("status")
            if status != "pass":
                mismatches.append(
                    {"path": str(path), f"{contract_name}_status": status}
                )

    result = {
        "status": "pass" if not mismatches else "fail",
        "reference": str(paths[0]),
        "ignored_parameters": sorted(ignored),
        "mismatches": mismatches,
    }
    text = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        output = Path(args.output).expanduser().resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(text)
    print(text, end="")
    return 0 if not mismatches else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    capture_parser = subparsers.add_parser("capture")
    capture_parser.add_argument("--output", required=True)
    capture_parser.add_argument("--workload", required=True)
    capture_parser.add_argument("--variant", required=True)
    capture_parser.add_argument("--module")
    capture_parser.add_argument("--model", default="nonpreemptive_submit_gate")
    capture_parser.add_argument("--status", default="completed")
    capture_parser.add_argument("--stats")
    capture_parser.add_argument("--artifact", action="append", default=[])
    capture_parser.add_argument("--param", action="append", default=[])
    capture_parser.add_argument("--strict-contract", action="store_true")
    capture_parser.set_defaults(func=capture)

    compare_parser = subparsers.add_parser("compare")
    compare_parser.add_argument("manifests", nargs="+")
    compare_parser.add_argument("--ignore-param", action="append", default=[])
    compare_parser.add_argument("--output")
    compare_parser.set_defaults(func=compare)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        return int(args.func(args))
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
