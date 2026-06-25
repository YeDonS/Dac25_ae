#!/usr/bin/env python3
"""Validate fio_latency_mixed JSON output volume.

fio may return 0 even when a random job stops before issuing the configured
io_size. This checker rejects those runs by comparing the actual bytes in the
fio JSON with the bytes the experiment intended to issue.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


UNITS = {
    "": 1,
    "K": 1024,
    "M": 1024**2,
    "G": 1024**3,
    "T": 1024**4,
}


def size_to_bytes(raw: str) -> int:
    value = raw.strip()
    if not value:
        raise ValueError("empty size")
    unit = value[-1:].upper()
    if unit in UNITS and unit:
        number = value[:-1]
    else:
        unit = ""
        number = value
    if not number.isdigit():
        raise ValueError(f"invalid size {raw!r}; use bytes or K/M/G/T suffix")
    return int(number) * UNITS[unit]


def load_fio_json(path: Path) -> dict[str, Any]:
    text = path.read_text(errors="replace")
    start = text.find("{")
    if start < 0:
        raise ValueError("JSON object not found")
    return json.loads(text[start:])


def job_name(job: dict[str, Any]) -> str:
    return str(job.get("jobname", ""))


def job_desc(job: dict[str, Any]) -> str:
    return str(
        job.get("desc")
        or job.get("job options", {}).get("description")
        or ""
    )


def io_bytes(job: dict[str, Any], ddir: str) -> int:
    return int(job.get(ddir, {}).get("io_bytes") or 0)


def sum_io(jobs: list[dict[str, Any]], prefix: str, ddir: str) -> int:
    return sum(io_bytes(job, ddir) for job in jobs if job_name(job).startswith(prefix))


def actual_bytes(jobs: list[dict[str, Any]]) -> dict[str, int]:
    actual = {
        "init_write": sum_io(jobs, "init_write", "write"),
        "init_prewarm": sum_io(jobs, "init_prewarm", "read"),
        "measure_read": sum_io(jobs, "measure_reads", "read"),
        "measure_write": sum_io(jobs, "measure_writes", "write"),
    }

    # Older generated files used group_reporting=1, which aggregates the
    # measured read/write jobs into a single final object. Keep this fallback
    # so old broken outputs are reported as volume failures, not parse failures.
    if actual["measure_read"] == 0 and actual["measure_write"] == 0:
        measured_groups = [
            job
            for job in jobs
            if "Measured phase" in job_desc(job) or job_name(job) == "enter_test_phase"
        ]
        if measured_groups:
            actual["measure_read"] = sum(io_bytes(job, "read") for job in measured_groups)
            actual["measure_write"] = sum(io_bytes(job, "write") for job in measured_groups)

    return actual


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate fio_latency_mixed JSON byte counts."
    )
    parser.add_argument("json_file", type=Path)
    parser.add_argument("--expected-init-write", required=True, type=size_to_bytes)
    parser.add_argument("--expected-init-prewarm", required=True, type=size_to_bytes)
    parser.add_argument("--expected-measure-read", required=True, type=size_to_bytes)
    parser.add_argument("--expected-measure-write", required=True, type=size_to_bytes)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    expected = {
        "init_write": args.expected_init_write,
        "init_prewarm": args.expected_init_prewarm,
        "measure_read": args.expected_measure_read,
        "measure_write": args.expected_measure_write,
    }

    try:
        data = load_fio_json(args.json_file)
    except Exception as exc:  # noqa: BLE001 - command-line diagnostic
        print(f"[fio-validate] FAIL {args.json_file}: {exc}", file=sys.stderr)
        return 1

    jobs = data.get("jobs", [])
    if not isinstance(jobs, list) or not jobs:
        print(f"[fio-validate] FAIL {args.json_file}: no fio jobs in JSON", file=sys.stderr)
        return 1

    errors = [
        (job_name(job), job.get("error"))
        for job in jobs
        if isinstance(job, dict) and job.get("error", 0)
    ]
    if errors:
        print(f"[fio-validate] FAIL {args.json_file}: fio job errors: {errors}", file=sys.stderr)
        return 1

    actual = actual_bytes(jobs)

    print("[fio-validate] bytes actual/expected:")
    for key in ("init_write", "init_prewarm", "measure_read", "measure_write"):
        print(f"[fio-validate]   {key}: {actual[key]} / {expected[key]}")

    if actual != expected:
        print(
            f"[fio-validate] FAIL {args.json_file}: fio completed but did not "
            "issue the configured I/O volume",
            file=sys.stderr,
        )
        return 1

    print(f"[fio-validate] PASS {args.json_file}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
