#!/usr/bin/env python3
"""Unit tests for analyzer verdicts on read-priority blocker cases."""

from __future__ import annotations

import tempfile
from pathlib import Path

import analyze_latency_run as analyzer


def make_data(stats: dict[str, object]) -> dict[str, object]:
    base_stats = {
        "read_requests": 1000,
        "read_priority_model": "nonpreemptive_submit_gate",
        "host_read_nand_ops": 1000,
        "host_read_slc_pages": 1000,
        "read_bg_conflicts": 0,
        "read_begin_bg_active": 0,
        "bg_begin_read_active": 0,
        "read_nand_bg_overlap_ops": 0,
        "read_die_conflicts": 0,
        "read_die_wait_ns": 0,
        "read_priority_yields": 0,
        "read_priority_should_yield_checks": 0,
        "read_priority_should_yield_true": 0,
        "read_priority_should_yield_gate_closed": 0,
        "read_priority_should_yield_window_closed": 0,
        "read_priority_window_active_read_hits": 0,
        "read_priority_window_busy_hits": 0,
        "read_priority_window_quiet_hits": 0,
        "read_priority_token_empty": 0,
        "read_issue_gap_count": 0,
        "read_issue_gap_sum_ns": 0,
        "read_issue_gap_max_ns": 0,
        "read_issue_same_time": 0,
        "read_issue_backwards": 0,
        "slc_to_qlc_nand_writes": 0,
        "repromote_nand_writes": 0,
        "qlc_repromote_pages": 0,
        "bg_repromote_backlog": 0,
        "bg_repromote_backlog_capacity": 31,
        "bg_qlc_closed_backlog": 0,
        "bg_qlc_closed_backlog_capacity": 32,
        "bg_slc_gc_backlog": 0,
        "bg_active_ops": 0,
    }
    base_stats.update(stats)
    return {
        "tag": "synthetic_latency2",
        "read_events": [
            {
                "scan_ops": 1,
                "sqlite_rows_seen": 1,
                "host_read_nand_delta": 1,
                "global_read_sum_delta": 1,
            }
        ],
        "bg": {
            "total": {
                "read_prio_bypass_ops": 0,
                "read_prio_bypass_ns": 0,
                "read_prio_ch_bypass_ops": 0,
                "read_prio_ch_bypass_ns": 0,
                "read_prio_pcie_bypass_ops": 0,
                "read_prio_pcie_bypass_ns": 0,
            }
        },
        "stats": base_stats,
        "kernel_signals": {},
        "kernel_tail_status": {"status": "captured"},
        "init_config": {},
        "init_summary": {"read_events": 1},
        "cold_extra": {},
        "cold_read_latency": {},
        "mig_monitor": {"count": 0, "last": {}},
        "heat_epoch_advances": 1,
    }


def notes_for(**stats: int) -> list[str]:
    return analyzer.verdict(make_data(dict(stats)))


def require_note(notes: list[str], prefix: str) -> None:
    if not any(note.startswith(prefix) for note in notes):
        raise AssertionError(f"missing note {prefix!r}; notes={notes!r}")


def test_no_overlap_foreground_read_queue() -> None:
    notes = notes_for(
        read_die_conflicts=100,
        read_die_wait_ns=50_000_000,
        read_priority_should_yield_checks=10,
        read_priority_should_yield_window_closed=10,
    )
    require_note(notes, "read_priority_blocker=no_read_bg_overlap")
    require_note(notes, "read_queue_pressure=foreground_read_or_service_dominated")
    require_note(notes, "read_priority_gate_state=no_read_window_seen")


def test_too_little_read_pressure() -> None:
    notes = notes_for(read_die_conflicts=10, read_die_wait_ns=500_000)
    require_note(notes, "read_queue_pressure=too_low(no_meaningful_die_wait)")


def test_gate_missed_background_start_during_read() -> None:
    notes = notes_for(bg_begin_read_active=5)
    require_note(notes, "read_priority_blocker=gate_missed_bg_start_during_read")


def test_read_arrived_after_background_issued() -> None:
    notes = notes_for(read_begin_bg_active=5)
    require_note(notes, "read_priority_blocker=read_arrived_after_bg_already_issued")


def test_yielding_background_during_read_window() -> None:
    notes = notes_for(
        bg_begin_read_active=5,
        read_priority_yields=3,
        read_priority_should_yield_checks=3,
        read_priority_should_yield_true=3,
        read_priority_window_active_read_hits=3,
    )
    require_note(notes, "read_priority_overlap=worker_yielded_during_read_window")
    require_note(notes, "read_priority_gate_state=yielding(checks=3,true=3,window_hits=3)")


def test_nonpreemptive_submit_gate_and_resource_attribution() -> None:
    notes = notes_for(
        read_priority_yields=4,
        read_priority_forced_progress_runs=1,
        read_die_wait_ns=12_000_000,
        read_die_bg_wait_ns=8_000_000,
        read_die_read_wait_ns=3_000_000,
        read_ch_wait_ns=5_000_000,
        read_ch_bg_wait_ns=4_000_000,
        read_pcie_wait_ns=1_000_000,
        read_pcie_read_wait_ns=1_000_000,
    )
    require_note(notes, "read_prio_bypass=expected_zero_nonpreemptive")
    require_note(notes, "read_priority_runtime=submit_gate_active(yields=4,forced_progress=1)")
    require_note(notes, "resource_wait=die:total_us/read=12.000")
    require_note(notes, "background_backlog=drained")


def test_same_issue_batch_pattern() -> None:
    notes = notes_for(read_issue_same_time=700)
    require_note(notes, "read_issue_pattern=batch_like_same_issue_time")


def test_spaced_issue_pattern() -> None:
    notes = notes_for(
        read_issue_gap_count=10,
        read_issue_gap_sum_ns=10_000_000,
        read_issue_gap_max_ns=2_000_000,
    )
    require_note(notes, "read_issue_pattern=spaced(gap_avg_us=1000.000")


def test_token_budget_exhausted() -> None:
    notes = notes_for(
        read_priority_should_yield_checks=10,
        read_priority_token_empty=4,
    )
    require_note(notes, "read_priority_gate_state=token_budget_exhausted(empty=4)")


def test_parse_model_and_resource_wait_fields() -> None:
    with tempfile.TemporaryDirectory() as tmpdir:
        path = Path(tmpdir) / "synthetic.log"
        path.write_text(
            "\n".join(
                [
                    "read_priority_model nonpreemptive_submit_gate",
                    "read_requests 10",
                    "read_ch_wait_ns 5000",
                    "read_ch_bg_wait_ns 3000",
                    "bg_repromote_backlog 0",
                ]
            )
        )
        data = analyzer.parse_log(path)
    stats = data["stats"]
    if stats.get("read_priority_model") != "nonpreemptive_submit_gate":
        raise AssertionError(f"model not parsed: {stats!r}")
    if stats.get("read_ch_wait_ns") != 5000 or stats.get("read_ch_bg_wait_ns") != 3000:
        raise AssertionError(f"resource waits not parsed: {stats!r}")


def make_compare_row(lat_count: int, total_rows: int = 256000) -> dict[str, object]:
    data = make_data({})
    data["init_config"] = {
        "tables": 80,
        "total_rows": total_rows,
        "interleave_pages": 209715,
        "read_ops_per_event": 102,
    }
    data["init_summary"] = {
        "total_rows": total_rows,
        "read_events": 9,
        "cold_mode": "full-scan-concurrent",
    }
    data["cold_extra"] = {
        "mode": "concurrent",
        "target_bytes": 1024 ** 3,
    }
    data["cold_read_latency"] = {"count": lat_count}
    return data


def test_compare_statuses_group_by_workload_count() -> None:
    rows = [
        make_compare_row(102),
        make_compare_row(102),
        make_compare_row(408),
        make_compare_row(408),
    ]
    statuses = analyzer.compare_statuses(rows)
    if statuses != ["ref", "ok", "ref", "ok"]:
        raise AssertionError(f"unexpected grouped compare statuses: {statuses!r}")


def test_compare_statuses_mismatch_within_group() -> None:
    rows = [
        make_compare_row(102, total_rows=256000),
        make_compare_row(102, total_rows=128000),
    ]
    statuses = analyzer.compare_statuses(rows)
    if statuses != ["ref", "mismatch"]:
        raise AssertionError(f"unexpected mismatch compare statuses: {statuses!r}")


def main() -> int:
    tests = [
        test_no_overlap_foreground_read_queue,
        test_too_little_read_pressure,
        test_gate_missed_background_start_during_read,
        test_read_arrived_after_background_issued,
        test_yielding_background_during_read_window,
        test_nonpreemptive_submit_gate_and_resource_attribution,
        test_same_issue_batch_pattern,
        test_spaced_issue_pattern,
        test_token_budget_exhausted,
        test_parse_model_and_resource_wait_fields,
        test_compare_statuses_group_by_workload_count,
        test_compare_statuses_mismatch_within_group,
    ]
    for test in tests:
        test()
    print("PASS analyze_latency_run verdict cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
