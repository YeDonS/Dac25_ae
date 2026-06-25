#!/usr/bin/env python3
"""Regression tests for fio result discovery and repetition aggregation."""

from __future__ import annotations

import importlib.util
import tempfile
from pathlib import Path


MODULE = Path(__file__).with_name("fio_summarize_latency_mixed.py")
SPEC = importlib.util.spec_from_file_location("fio_summary", MODULE)
assert SPEC and SPEC.loader
SUMMARY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SUMMARY)


def main() -> int:
    with tempfile.TemporaryDirectory() as tmpdir:
        root = Path(tmpdir)
        result = root / "fio_mixed_variant_zipf_rw10_1_bg1_r1.json"
        manifest = root / "fio_mixed_variant_zipf_rw10_1_bg1_r1_manifest.json"
        result.write_text('{"jobs": []}')
        manifest.write_text('{"status": "dry_run"}')
        files = SUMMARY.collect_json_files([root])
        if files != [result]:
            raise AssertionError(f"unexpected fio discovery: {files!r}")

    rows = []
    for trial, iops in (("1", "100"), ("2", "90"), ("3", "80")):
        row = {column: "" for column in SUMMARY.columns()}
        row.update(
            variant="v",
            dist="zipf_a0_75",
            ratio="rw10_1",
            bg_timing="1",
            trial=trial,
            read_iops=iops,
        )
        rows.append(row)
    aggregate = SUMMARY.aggregate_rows(rows)
    if aggregate[0]["repeat_status"] != "pass":
        raise AssertionError(aggregate)
    if aggregate[0]["read_iops_median"] != "90.000000":
        raise AssertionError(aggregate)
    print("PASS fio summary discovery and aggregation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
