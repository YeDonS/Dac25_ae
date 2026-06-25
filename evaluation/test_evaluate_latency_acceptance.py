#!/usr/bin/env python3
"""Synthetic pass/fail tests for the latency acceptance evaluator."""

from __future__ import annotations

import evaluate_latency_acceptance as evaluator


def row(variant: str, bg: str, p99: float, p999: float, avg: float,
        iops: float, bg_wait: float) -> dict[str, str]:
    return {
        "variant": variant,
        "dist": "zipf_a0_75",
        "ratio": "rw10_1",
        "bg_timing": bg,
        "repeat_status": "pass",
        "fg_read_req_latency_p99_ns_median": str(p99),
        "fg_read_req_latency_p999_ns_median": str(p999),
        "fg_read_req_latency_avg_ns_median": str(avg),
        "read_iops_median": str(iops),
        "fg_read_die_bg_wait_ms_per_read_median": str(bg_wait),
        "fg_read_ch_bg_wait_ms_per_read_median": "0",
        "post_bg_repromote_backlog_max": "0",
        "post_bg_qlc_closed_backlog_max": "0",
        "post_bg_slc_gc_backlog_max": "0",
        "post_maint_v2_backlog_tasks_max": "0",
    }


def fixture() -> list[dict[str, str]]:
    rows = [
        row(evaluator.BASELINE, "1", 100, 200, 50, 1000, 2.0),
        row(evaluator.BASELINE, "0", 50, 80, 30, 1200, 0),
    ]
    for variant in evaluator.TREATMENTS:
        rows.append(row(variant, "1", 85, 175, 52, 970, 1.0))
        rows.append(row(variant, "0", 50.5, 81, 30.6, 1180, 0))
    return rows


def main() -> int:
    results = evaluator.evaluate(fixture(), "zipf_a0_75", "rw10_1")
    if not all(result["pass"] for result in results):
        raise AssertionError(results)

    failed = fixture()
    failed[2]["fg_read_req_latency_p99_ns_median"] = "95"
    results = evaluator.evaluate(failed, "zipf_a0_75", "rw10_1")
    if results[0]["pass"] or results[0]["checks"]["p99_ge_10"]:
        raise AssertionError(results[0])

    print("PASS latency acceptance evaluator")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
