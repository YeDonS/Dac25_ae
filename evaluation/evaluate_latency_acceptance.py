#!/usr/bin/env python3
"""Evaluate latency2/3 acceptance gates from fio median/range aggregates."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


BASELINE = "die_latency1_qlc_all_norp_sb"
TREATMENTS = (
    "die_latency2_qlc_all_norp_sb",
    "die_latency3_qlc_all_norp_sb",
)


def number(row: dict[str, str], key: str) -> float:
    raw = row.get(key, "")
    if raw == "":
        raise ValueError(f"{row.get('variant')}: missing {key}")
    return float(raw)


def improvement(lower_is_better_baseline: float, treatment: float) -> float:
    if lower_is_better_baseline <= 0:
        raise ValueError("baseline metric must be positive")
    return 100.0 * (lower_is_better_baseline - treatment) / lower_is_better_baseline


def degradation_lower_is_better(baseline: float, treatment: float) -> float:
    return -improvement(baseline, treatment)


def degradation_higher_is_better(baseline: float, treatment: float) -> float:
    if baseline <= 0:
        raise ValueError("baseline throughput must be positive")
    return 100.0 * (baseline - treatment) / baseline


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle, delimiter="\t"))


def select(rows: list[dict[str, str]], variant: str, bg_timing: str,
           dist: str, ratio: str) -> dict[str, str]:
    matches = [
        row for row in rows
        if row.get("variant") == variant
        and row.get("bg_timing") == bg_timing
        and row.get("dist") == dist
        and row.get("ratio") == ratio
    ]
    if len(matches) != 1:
        raise ValueError(
            f"expected one row for {variant} bg={bg_timing} {dist} {ratio}, "
            f"found {len(matches)}"
        )
    if matches[0].get("repeat_status") != "pass":
        raise ValueError(f"{variant} bg={bg_timing}: fewer than three runs")
    return matches[0]


def bg_wait(row: dict[str, str]) -> float:
    return sum(
        number(row, key)
        for key in (
            "fg_read_die_bg_wait_ms_per_read_median",
            "fg_read_ch_bg_wait_ms_per_read_median",
        )
    )


def backlog_drained(row: dict[str, str]) -> bool:
    keys = (
        "post_bg_repromote_backlog_max",
        "post_bg_qlc_closed_backlog_max",
        "post_bg_slc_gc_backlog_max",
    )
    generic_zero = all(number(row, key) == 0 for key in keys)
    v2 = row.get("post_maint_v2_backlog_tasks_max", "")
    return generic_zero and (v2 == "" or float(v2) == 0)


def evaluate(rows: list[dict[str, str]], dist: str, ratio: str) -> list[dict[str, object]]:
    base_bg = select(rows, BASELINE, "1", dist, ratio)
    base_no_bg = select(rows, BASELINE, "0", dist, ratio)
    results: list[dict[str, object]] = []

    for variant in TREATMENTS:
        treatment_bg = select(rows, variant, "1", dist, ratio)
        treatment_no_bg = select(rows, variant, "0", dist, ratio)
        p99_improve = improvement(
            number(base_bg, "fg_read_req_latency_p99_ns_median"),
            number(treatment_bg, "fg_read_req_latency_p99_ns_median"),
        )
        p999_improve = improvement(
            number(base_bg, "fg_read_req_latency_p999_ns_median"),
            number(treatment_bg, "fg_read_req_latency_p999_ns_median"),
        )
        avg_degrade = degradation_lower_is_better(
            number(base_bg, "fg_read_req_latency_avg_ns_median"),
            number(treatment_bg, "fg_read_req_latency_avg_ns_median"),
        )
        iops_degrade = degradation_higher_is_better(
            number(base_bg, "read_iops_median"),
            number(treatment_bg, "read_iops_median"),
        )
        no_bg_avg_degrade = degradation_lower_is_better(
            number(base_no_bg, "fg_read_req_latency_avg_ns_median"),
            number(treatment_no_bg, "fg_read_req_latency_avg_ns_median"),
        )
        no_bg_iops_degrade = degradation_higher_is_better(
            number(base_no_bg, "read_iops_median"),
            number(treatment_no_bg, "read_iops_median"),
        )
        base_wait = bg_wait(base_bg)
        treatment_wait = bg_wait(treatment_bg)
        checks = {
            "p99_ge_10": p99_improve >= 10.0,
            "p999_ge_10": p999_improve >= 10.0,
            "mean_or_iops_within_5": avg_degrade <= 5.0 or iops_degrade <= 5.0,
            "background_wait_reduced": base_wait > 0 and treatment_wait < base_wait,
            "no_bg_mean_within_3": no_bg_avg_degrade <= 3.0,
            "no_bg_iops_within_3": no_bg_iops_degrade <= 3.0,
            "backlog_drained": backlog_drained(treatment_bg),
        }
        results.append(
            {
                "variant": variant,
                "p99_improve_pct": p99_improve,
                "p999_improve_pct": p999_improve,
                "avg_degrade_pct": avg_degrade,
                "iops_degrade_pct": iops_degrade,
                "no_bg_avg_degrade_pct": no_bg_avg_degrade,
                "no_bg_iops_degrade_pct": no_bg_iops_degrade,
                "baseline_bg_wait_ms_per_read": base_wait,
                "treatment_bg_wait_ms_per_read": treatment_wait,
                "checks": checks,
                "pass": all(checks.values()),
            }
        )
    return results


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("aggregate_tsv", type=Path)
    parser.add_argument("--dist", default="zipf_a0_75")
    parser.add_argument("--ratio", default="rw10_1")
    args = parser.parse_args()

    try:
        results = evaluate(read_rows(args.aggregate_tsv), args.dist, args.ratio)
    except ValueError as exc:
        print(f"FAIL acceptance input: {exc}")
        return 1

    all_pass = True
    for result in results:
        checks = result.pop("checks")
        print(result)
        print(checks)
        all_pass = all_pass and bool(result["pass"])
    print("PASS latency acceptance" if all_pass else "FAIL latency acceptance")
    return 0 if all_pass else 1


if __name__ == "__main__":
    raise SystemExit(main())
