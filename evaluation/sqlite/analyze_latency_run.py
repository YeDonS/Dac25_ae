#!/usr/bin/env python3
"""Summarize SQLite latency variant logs for read-priority validation.

This parser is intentionally narrow: it extracts the fields that decide whether
the tablefile pageflow test actually built FTL heat and whether latency3's
read-priority path moved host reads ahead of low-priority background work.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path


KEYS = [
    "read_requests",
    "host_read_nand_ops",
    "host_read_slc_ops",
    "host_read_qlc_ops",
    "host_read_slc_nand_ops",
    "host_read_qlc_nand_ops",
    "host_read_phys_slc_nand_ops",
    "host_read_phys_qlc_nand_ops",
    "host_read_tier_mismatch_nand_ops",
    "host_read_slc_pages",
    "host_read_qlc_pages",
    "host_read_phys_slc_pages",
    "host_read_phys_qlc_pages",
    "host_read_tier_mismatch_pages",
    "read_bg_conflicts",
    "read_begin_bg_active",
    "bg_begin_read_active",
    "read_nand_bg_overlap_ops",
    "read_die_conflicts",
    "read_die_wait_ns",
	"read_die_read_conflicts",
	"read_die_read_wait_ns",
	"read_die_bg_conflicts",
	"read_die_bg_wait_ns",
	"read_ch_conflicts",
	"read_ch_wait_ns",
	"read_ch_read_conflicts",
	"read_ch_read_wait_ns",
	"read_ch_bg_conflicts",
	"read_ch_bg_wait_ns",
	"read_pcie_conflicts",
	"read_pcie_wait_ns",
	"read_pcie_read_conflicts",
	"read_pcie_read_wait_ns",
	"read_pcie_bg_conflicts",
	"read_pcie_bg_wait_ns",
    "read_lp_bypass_ops",
    "read_lp_bypass_ns",
    "last_read_issue_ns",
    "read_issue_gap_count",
    "read_issue_gap_sum_ns",
    "read_issue_gap_max_ns",
    "read_issue_same_time",
    "read_issue_backwards",
    "read_priority_yields",
    "read_priority_delayed_requeues",
    "read_priority_forced_progress_runs",
    "read_req_latency_count",
    "read_req_latency_avg_ns",
    "read_req_latency_p50_ns",
    "read_req_latency_p95_ns",
    "read_req_latency_p99_ns",
    "read_req_latency_p999_ns",
    "read_req_latency_max_ns",
    "read_req_latency_hist_bins",
    "read_priority_gate_entries",
    "read_priority_should_yield_checks",
    "read_priority_should_yield_true",
    "read_priority_should_yield_force_blocked",
    "read_priority_should_yield_gate_closed",
    "read_priority_should_yield_window_closed",
    "read_priority_window_active_read_hits",
    "read_priority_window_busy_hits",
    "read_priority_window_quiet_hits",
    "read_priority_token_empty",
    "read_priority_gate_current",
    "read_priority_force_current",
    "read_priority_yield_streak_current",
    "read_priority_busy_until_ns",
    "read_priority_gate_tokens_current",
    "read_priority_token_window_hits",
    "slc_to_qlc_migration_pages",
    "qlc_repromote_pages",
    "internal_write_pages_est",
    "slc_to_qlc_nand_reads",
    "slc_to_qlc_nand_writes",
    "repromote_nand_reads",
    "repromote_nand_writes",
    "hard_no_victim_count",
    "test_phase_guard_read_reqs_config",
    "test_phase_guard_read_reqs",
    "test_phase_recent_guard_skips",
    "test_phase_recent_guard_forced",
    "maint_v2_tasks_done",
    "maint_v2_tasks_requeued",
    "maint_v2_tasks_yielded",
    "maint_v2_stale_tasks",
	"maint_v2_backlog_tasks",
	"maint_v2_backlog_max",
    "maint_v2_no_progress_runs",
    "maint_v2_no_slack_skips",
    "maint_v2_ch_busy_skips",
    "maint_v2_demand_skips",
    "maint_v2_emergency_overrides",
    "maint_v2_hard_skip_count",
    "maint_v2_skip_pct",
	"bg_repromote_backlog",
	"bg_repromote_backlog_capacity",
	"bg_qlc_closed_backlog",
	"bg_qlc_closed_backlog_capacity",
	"bg_slc_gc_backlog",
	"bg_active_ops",
]

BG_KEYS = [
    "busy_ns",
    "read_ns",
    "write_ns",
    "erase_ns",
    "read_ops",
    "write_ops",
    "erase_ops",
    "read_prio_bypass_ops",
    "read_prio_bypass_ns",
    "read_prio_ch_bypass_ops",
    "read_prio_ch_bypass_ns",
    "read_prio_pcie_bypass_ops",
    "read_prio_pcie_bypass_ns",
    "lp_host_write_ops",
    "lp_host_write_ns",
]


def parse_kv_pairs(text: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for key, value in re.findall(r"([A-Za-z0-9_]+)=([^,\s]+)", text):
        out[key] = value
    return out


def parse_log(path: Path) -> dict[str, object]:
    data: dict[str, object] = {
        "path": str(path),
        "tag": path.stem,
        "stats": {},
        "read_req_histogram": {},
        "bg": {},
        "read_events": [],
        "read_plan": None,
        "init_config": {},
        "init_summary": {},
        "cold_read_set_tier": {},
        "cold_extra": {},
        "cold_read_latency": {},
        "heat_epoch_advances": 0,
        "mig_monitor": {"count": 0, "last": {}},
        "cold_full_read": None,
        "init_drop_cache_each_read": None,
        "kernel_tail_status": None,
        "kernel_signals": {
            "no_free_entry": 0,
            "hung_task": 0,
            "slow_proc_io_sq": 0,
            "slow_chmodel": 0,
            "proc_queue_full": 0,
            "writeback_queue_full": 0,
            "write_buffer_alloc_failed": 0,
            "write_buffer_alloc_waiting": 0,
            "credit_scan_capped": 0,
            "credit_horizon_overflow": 0,
        },
    }
    stats: dict[str, int] = data["stats"]  # type: ignore[assignment]
    read_req_histogram: dict[int, dict[str, int]] = data["read_req_histogram"]  # type: ignore[assignment]
    bg: dict[str, dict[str, int]] = data["bg"]  # type: ignore[assignment]
    read_events: list[dict[str, int]] = data["read_events"]  # type: ignore[assignment]
    init_config: dict[str, int] = data["init_config"]  # type: ignore[assignment]
    init_summary: dict[str, int] = data["init_summary"]  # type: ignore[assignment]
    cold_read_set_tier: dict[str, dict[str, object]] = data["cold_read_set_tier"]  # type: ignore[assignment]
    cold_extra: dict[str, object] = data["cold_extra"]  # type: ignore[assignment]
    cold_read_latency: dict[str, float] = data["cold_read_latency"]  # type: ignore[assignment]
    mig_monitor: dict[str, object] = data["mig_monitor"]  # type: ignore[assignment]
    kernel_signals: dict[str, int] = data["kernel_signals"]  # type: ignore[assignment]
    pending_events: dict[int, dict[str, int]] = {}

    for raw in path.read_text(errors="replace").splitlines():
        line = raw.strip()
        model_match = re.match(r"read_priority_model\s+(\S+)", line)
        if model_match:
            stats["read_priority_model"] = model_match.group(1)  # type: ignore[assignment]

        if "No free entry" in line:
            kernel_signals["no_free_entry"] += 1
        if "blocked for more than" in line or "hung_task" in line:
            kernel_signals["hung_task"] += 1
        if "SLOW_PATH: nvmev_proc_io_sq" in line:
            kernel_signals["slow_proc_io_sq"] += 1
        if "SLOW_PATH: chmodel_request" in line:
            kernel_signals["slow_chmodel"] += 1
        if "proc queue full" in line and "writeback proc queue full" not in line:
            kernel_signals["proc_queue_full"] += 1
        if "writeback proc queue full" in line:
            kernel_signals["writeback_queue_full"] += 1
        if "write buffer allocation failed after" in line:
            kernel_signals["write_buffer_alloc_failed"] += 1
        if "write buffer allocation waiting" in line:
            kernel_signals["write_buffer_alloc_waiting"] += 1
        if "credit scan capped" in line:
            kernel_signals["credit_scan_capped"] += 1
        if "credit horizon overflow" in line:
            kernel_signals["credit_horizon_overflow"] += 1
        if "[MIG-MONITOR]" in line:
            pairs = parse_kv_pairs(line)
            last: dict[str, int] = {}
            for key in (
                "calls",
                "backlog_blocked",
                "cooldown_blocked",
                "sb_scan_visits",
                "valid_pages_sampled",
                "migrated",
                "victim_q",
                "victim_cap",
                "heat_epoch",
                "global_read_sum",
                "global_valid_pg_cnt",
                "valid_seen",
                "skip_hot",
                "skip_recent",
                "skip_not_slc",
                "skip_queued",
                "skip_active",
                "skip_empty",
                "move_fail",
                "gc_sb",
                "gc_invalid",
                "gc_valid",
                "hard_no_victim_delta",
                "active_sealed_hard_delta",
                "active_sealed_alloc_delta",
                "active_seal_migrated_delta",
                "active_seal_no_migrate_delta",
            ):
                if key in pairs:
                    try:
                        last[key] = int(pairs[key])
                    except ValueError:
                        pass
            mig_monitor["count"] = int(mig_monitor.get("count", 0)) + 1
            mig_monitor["last"] = last

        if "[TEST_PHASE]" in line:
            pairs = parse_kv_pairs(line)
            phase = pairs.get("TEST_PHASE")
            if pairs.get("reads") is not None:
                for src_key, dst_key in (
                    ("reads", "read_requests"),
                    ("overwrites", "overwrite_reqs"),
                    ("bg_repromote", "bg_repromote_ops"),
                    ("bg_qlc_rebalance", "bg_qlc_rebalance_ops"),
                    ("read_bg_conflicts", "read_bg_conflicts"),
                    ("read_die_conflicts", "read_die_conflicts"),
                    ("read_die_wait_ns", "read_die_wait_ns"),
                    ("phys_qlc_reads", "host_read_phys_qlc_nand_ops"),
                    ("tier_mismatch_reads", "host_read_tier_mismatch_nand_ops"),
                    ("phys_qlc_read_pages", "host_read_phys_qlc_pages"),
                    ("tier_mismatch_read_pages", "host_read_tier_mismatch_pages"),
                ):
                    if src_key in pairs and dst_key not in stats:
                        try:
                            stats[dst_key] = int(pairs[src_key])
                        except ValueError:
                            pass

        if "[sqlite_init] tag=" in line:
            pairs = parse_kv_pairs(line)
            data["tag"] = pairs.get("tag", data["tag"])
            if "cold_full_read" in pairs:
                try:
                    data["cold_full_read"] = float(pairs["cold_full_read"].rstrip("s"))
                except ValueError:
                    pass
            if "cold_mode" in pairs:
                init_summary["cold_mode"] = pairs["cold_mode"]  # type: ignore[assignment]

        if "[sqlite_init] config " in line:
            pairs = parse_kv_pairs(line)
            if "init_drop_cache_each_read" in pairs:
                try:
                    data["init_drop_cache_each_read"] = int(pairs["init_drop_cache_each_read"])
                except ValueError:
                    pass
            for key in ("tables", "total_rows", "interleave_pages", "read_ops_per_event"):
                if key in pairs:
                    try:
                        init_config[key] = int(pairs[key])
                    except ValueError:
                        pass

        if "[sqlite_init] heat_epoch_advance " in line:
            data["heat_epoch_advances"] = int(data["heat_epoch_advances"]) + 1

        if "[sqlite_init] tag=" in line and "read_events=" in line:
            pairs = parse_kv_pairs(line)
            for key in ("read_events", "total_rows"):
                if key in pairs:
                    try:
                        init_summary[key] = int(pairs[key])
                    except ValueError:
                        pass

        if "[sqlite_init] cold_extra_append " in line:
            pairs = parse_kv_pairs(line)
            if "mode" in pairs:
                cold_extra["mode"] = pairs["mode"]
            for key in (
                "target_bytes",
                "actual_payload_bytes",
                "rows_per_table",
                "total_extra_rows",
                "rows_written",
                "page_growth",
            ):
                if key in pairs:
                    try:
                        cold_extra[key] = int(pairs[key])
                    except ValueError:
                        pass
            for key in ("append_time", "stage_total"):
                if key in pairs:
                    try:
                        cold_extra[key] = float(pairs[key].rstrip("s"))
                    except ValueError:
                        pass

        if "[sqlite_init] cold_read_latency " in line:
            pairs = parse_kv_pairs(line)
            for key in ("count", "avg", "p50", "p95", "p99", "p999", "max"):
                if key in pairs:
                    try:
                        if key == "count":
                            cold_read_latency[key] = int(pairs[key])  # type: ignore[assignment]
                        else:
                            cold_read_latency[key] = float(pairs[key].rstrip("s"))
                    except ValueError:
                        pass

        if "[sqlite_init] cold_read_set_tier " in line:
            pairs = parse_kv_pairs(line)
            phase = pairs.get("phase", "unknown")
            phase_data: dict[str, object] = {}
            for key in (
                "mode",
                "active_tables",
                "file_lpn_slc",
                "file_lpn_qlc",
                "file_lpn_unknown",
                "weighted_slc",
                "weighted_qlc",
                "weighted_unknown",
                "file_lpn_raw_known",
                "file_lpn_tier_mismatch",
                "weighted_tier_mismatch",
                "tier_entries",
                "tier_parts",
                "tier_slc",
                "tier_qlc",
                "tier_raw_known",
                "tier_mismatch",
                "fiemap_files",
                "fallback_files",
                "fiemap_extents",
                "file_lpn_values",
                "file_lpn_global_values",
                "file_lpn_global_unique",
                "file_lpn_cross_file_dups",
                "file_lpn_min",
                "file_lpn_max",
            ):
                if key not in pairs:
                    continue
                if key == "mode":
                    phase_data[key] = pairs[key]
                else:
                    try:
                        phase_data[key] = int(pairs[key])
                    except ValueError:
                        pass
            for key in ("file_lpn_qlc_pct", "weighted_qlc_pct"):
                if key in pairs:
                    try:
                        phase_data[key] = float(pairs[key])
                    except ValueError:
                        pass
            cold_read_set_tier[phase] = phase_data

        if line.startswith("[kernel_tail_status]"):
            data["kernel_tail_status"] = parse_kv_pairs(line)

        if "[sqlite_init] read_plan " in line:
            pairs = parse_kv_pairs(line)
            data["read_plan"] = {
                "total_reads": int(pairs.get("total_reads", "0")),
                "active_tables": int(pairs.get("active_tables", "0")),
                "top_tables": line.split("top_tables=", 1)[-1] if "top_tables=" in line else "",
            }

        if "[sqlite_init] read_event=" in line and "sqlite_rows_seen=" in line:
            pairs = parse_kv_pairs(line)
            try:
                event_id = int(pairs["read_event"])
            except (KeyError, ValueError):
                continue
            event = pending_events.setdefault(event_id, {"event": event_id})
            for key in ("scan_ops", "sqlite_rows_seen", "drop_cache_each_read"):
                if key in pairs:
                    try:
                        event[key] = int(pairs[key])
                    except ValueError:
                        pass

        if "[sqlite_init] read_event=" in line and "ftl_heat" in line:
            pairs = parse_kv_pairs(line)
            try:
                event_id = int(pairs["read_event"])
            except (KeyError, ValueError):
                continue
            event = pending_events.setdefault(event_id, {"event": event_id})
            for key in (
                "global_read_sum_delta",
                "host_read_nand_delta",
                "global_read_sum",
                "global_valid_pg_cnt",
                "host_read_nand_ops",
            ):
                if key in pairs:
                    try:
                        event[key] = int(pairs[key])
                    except ValueError:
                        pass

        if "[sqlite_init] bg_nand phase=" in line:
            pairs = parse_kv_pairs(line)
            phase = pairs.get("phase")
            if phase:
                bg[phase] = {}
                for key in BG_KEYS:
                    if key in pairs:
                        try:
                            bg[phase][key] = int(pairs[key])
                        except ValueError:
                            pass

        hist_match = re.match(
            r"^read_req_latency_hist_bin_(\d+)_(upper_ns|count)\s+(\d+)$",
            line,
        )
        if hist_match:
            bucket = int(hist_match.group(1))
            field = hist_match.group(2)
            read_req_histogram.setdefault(bucket, {})[field] = int(
                hist_match.group(3)
            )
            continue

        m = re.match(r"^([A-Za-z0-9_]+)\s+(-?\d+)$", line)
        if m and m.group(1) in KEYS:
            stats[m.group(1)] = int(m.group(2))

    read_events.extend(pending_events[k] for k in sorted(pending_events))
    return data


def read_req_histogram_status(data: dict[str, object]) -> str:
    stats: dict[str, int] = data["stats"]  # type: ignore[assignment]
    histogram: dict[int, dict[str, int]] = data.get(  # type: ignore[assignment]
        "read_req_histogram", {}
    )

    if "read_req_latency_count" not in stats:
        return "not_enabled"
    if not histogram:
        return "summary_only"

    expected_bins = get_int(stats, "read_req_latency_hist_bins")
    if expected_bins and len(histogram) != expected_bins:
        return f"invalid(bin_count={len(histogram)},expected={expected_bins})"
    incomplete = [
        bucket
        for bucket, fields in histogram.items()
        if "upper_ns" not in fields or "count" not in fields
    ]
    if incomplete:
        return f"invalid(incomplete_bins={len(incomplete)})"

    histogram_count = sum(fields["count"] for fields in histogram.values())
    request_count = get_int(stats, "read_req_latency_count")
    if histogram_count != request_count:
        return (
            f"invalid(sample_sum={histogram_count},"
            f"request_count={request_count})"
        )
    nonzero = sum(1 for fields in histogram.values() if fields["count"])
    return f"ok(samples={histogram_count},nonzero_bins={nonzero})"


def get_int(mapping: dict[str, int], key: str) -> int:
    return int(mapping.get(key, 0))


def fmt_int(mapping: dict[str, int], key: str) -> str:
    if key not in mapping:
        return "missing"
    return str(get_int(mapping, key))


def fmt_ns_as_us(mapping: dict[str, int], key: str) -> str:
    if key not in mapping:
        return "missing"
    return f"{get_int(mapping, key) / 1000.0:.3f}"


def ratio(num: int, den: int) -> str:
    if den <= 0:
        return "n/a"
    return f"{num / den:.3f}"


def avg_ns_as_us(num_ns: int, den: int) -> str:
    if den <= 0:
        return "n/a"
    return f"{num_ns / den / 1000.0:.3f}"


def ratio_float(num: int, den: int) -> float:
    if den <= 0:
        return 0.0
    return num / den


def read_count(stats: dict[str, int]) -> int:
    return get_int(stats, "read_requests") or get_int(stats, "host_read_nand_ops")


def host_read_qlc_count(stats: dict[str, int]) -> int:
    return (
        get_int(stats, "host_read_qlc_pages")
        or get_int(stats, "host_read_qlc_nand_ops")
        or get_int(stats, "host_read_qlc_ops")
    )


def host_read_slc_count(stats: dict[str, int]) -> int:
    return (
        get_int(stats, "host_read_slc_pages")
        or get_int(stats, "host_read_slc_nand_ops")
        or get_int(stats, "host_read_slc_ops")
    )


def host_read_phys_qlc_count(stats: dict[str, int]) -> int:
    return get_int(stats, "host_read_phys_qlc_pages") or get_int(stats, "host_read_phys_qlc_nand_ops")


def host_read_phys_slc_count(stats: dict[str, int]) -> int:
    return get_int(stats, "host_read_phys_slc_pages") or get_int(stats, "host_read_phys_slc_nand_ops")


def host_read_tier_mismatch_count(stats: dict[str, int]) -> int:
    return (
        get_int(stats, "host_read_tier_mismatch_pages")
        or get_int(stats, "host_read_tier_mismatch_nand_ops")
    )


def fmt_seconds_as_ms(mapping: dict[str, float], key: str) -> str:
    if key not in mapping:
        return "n/a"
    return f"{float(mapping[key]) * 1000.0:.3f}"


def bytes_to_gib(value: object) -> float:
    try:
        return float(value) / float(1024 ** 3)
    except (TypeError, ValueError):
        return 0.0


def workload_label(data: dict[str, object]) -> str:
    init_summary: dict[str, object] = data["init_summary"]  # type: ignore[assignment]
    cold_extra: dict[str, object] = data["cold_extra"]  # type: ignore[assignment]
    cold_mode = str(init_summary.get("cold_mode", "n/a"))
    extra_mode = str(cold_extra.get("mode", "off"))
    extra_bytes = int(cold_extra.get("target_bytes", 0) or 0)

    if extra_bytes > 0 and extra_mode != "off":
        return f"{cold_mode}+append-{extra_mode}-{bytes_to_gib(extra_bytes):.1f}GiB"
    return cold_mode


def compare_signature(data: dict[str, object]) -> tuple[object, ...]:
    init_config: dict[str, object] = data["init_config"]  # type: ignore[assignment]
    init_summary: dict[str, object] = data["init_summary"]  # type: ignore[assignment]
    cold_extra: dict[str, object] = data["cold_extra"]  # type: ignore[assignment]
    cold_read_latency: dict[str, object] = data["cold_read_latency"]  # type: ignore[assignment]

    return (
        init_config.get("tables"),
        init_config.get("total_rows"),
        init_summary.get("total_rows"),
        init_summary.get("read_events"),
        init_config.get("interleave_pages"),
        init_config.get("read_ops_per_event"),
        init_summary.get("cold_mode"),
        cold_extra.get("mode", "off"),
        cold_extra.get("target_bytes", 0),
        cold_read_latency.get("count"),
    )


def compare_group_key(data: dict[str, object]) -> tuple[object, ...]:
    init_summary: dict[str, object] = data["init_summary"]  # type: ignore[assignment]
    cold_extra: dict[str, object] = data["cold_extra"]  # type: ignore[assignment]
    cold_read_latency: dict[str, object] = data["cold_read_latency"]  # type: ignore[assignment]

    return (
        init_summary.get("cold_mode"),
        cold_extra.get("mode", "off"),
        cold_extra.get("target_bytes", 0),
        cold_read_latency.get("count"),
    )


def compare_statuses(rows: list[dict[str, object]]) -> list[str]:
	if not rows:
		return []
	signatures = [compare_signature(row) for row in rows]
	if all(signature == signatures[0] for signature in signatures):
		return ["ok"] * len(rows)

	group_refs: dict[tuple[object, ...], tuple[object, ...]] = {}
	statuses: list[str] = []
	for row, signature in zip(rows, signatures):
		key = compare_group_key(row)
		ref = group_refs.get(key)
		if ref is None:
			group_refs[key] = signature
			statuses.append("ref")
		elif signature == ref:
			statuses.append("ok")
		else:
			statuses.append("mismatch")
	return statuses


def first_note(notes: list[str], prefix: str, default: str = "n/a") -> str:
    for note in notes:
        if note.startswith(prefix):
            return note
    return default


def has_note(notes: list[str], prefix: str) -> bool:
    return first_note(notes, prefix, "") != ""


def experiment_outcome(data: dict[str, object], compare_status: str = "ok") -> str:
    """Return a compact mechanism-level verdict for one latency run.

    The lower-level verdict list keeps the raw evidence. This folds the evidence
    into the experiment question: did this run prove read completion priority,
    merely worker yield, or was the result not trustworthy/comparable?
    """
    notes = verdict(data)
    tag = str(data.get("tag", ""))

    if compare_status == "mismatch":
        return "not_comparable(workload_mismatch)"

    hist_note = first_note(notes, "read_req_hist=", "")
    if hist_note.startswith("read_req_hist=invalid"):
        return "invalid_metrics(" + hist_note.split("=", 1)[1] + ")"

    io_note = first_note(notes, "io_completion=", "")
    if io_note.startswith("io_completion=bad"):
        return "invalid_io(" + io_note.split("=", 1)[1] + ")"
    if io_note.startswith("io_completion=unknown"):
        return "unknown_io(" + io_note.split("=", 1)[1] + ")"

    preheat_sql_ok = has_note(notes, "preheat_sql=ok")
    preheat_ftl_ok = has_note(notes, "preheat_ftl=ok")
    preheat_unproven = (
        has_note(notes, "preheat_diag=missing") or
        (has_note(notes, "preheat_sql=") and not preheat_sql_ok) or
        (has_note(notes, "preheat_ftl=") and not preheat_ftl_ok) or
        has_note(notes, "preheat_nand=") or
        has_note(notes, "preheat_heat=")
    )

    if has_note(notes, "read_priority_runtime=active"):
        suffix = "_preheat_unproven" if preheat_unproven else ""
        if has_note(notes, "read_priority_masked="):
            return "hp_lp_completion_priority_active_masked" + suffix
        return "hp_lp_completion_priority_active" + suffix
    if has_note(notes, "read_priority_runtime=yield_only"):
        if preheat_unproven:
            return "yield_only(no_completion_priority,preheat_unproven)"
        return "yield_only(no_completion_priority)"
    if preheat_unproven:
        return "preheat_unproven"
    if "latency1" in tag and not has_note(notes, "read_priority_runtime=active"):
        return "baseline(no_read_priority)"
    if has_note(notes, "read_priority_runtime=no_observed_bypass"):
        return "no_read_priority_bypass"
    if has_note(notes, "read_prio_bypass=missing"):
        return "missing_read_priority_stats"
    return "inconclusive"


def read_priority_bypass_totals(bg: dict[str, dict[str, int]]) -> tuple[bool, int, int]:
    total_bg = bg.get("total", {})
    cold_bg = bg.get("cold_read", {})
    bypass_pairs = (
        ("read_prio_bypass_ops", "read_prio_bypass_ns"),
        ("read_prio_ch_bypass_ops", "read_prio_ch_bypass_ns"),
        ("read_prio_pcie_bypass_ops", "read_prio_pcie_bypass_ns"),
    )
    if any(ops in cold_bg for ops, _ in bypass_pairs):
        scope = cold_bg
    else:
        scope = total_bg
    present = any(ops in cold_bg or ops in total_bg for ops, _ in bypass_pairs)
    return (
        present,
        sum(get_int(scope, ops) for ops, _ in bypass_pairs),
        sum(get_int(scope, ns) for _, ns in bypass_pairs),
    )


def verdict(data: dict[str, object]) -> list[str]:
    notes: list[str] = []
    events: list[dict[str, int]] = data["read_events"]  # type: ignore[assignment]
    bg: dict[str, dict[str, int]] = data["bg"]  # type: ignore[assignment]
    stats: dict[str, int] = data["stats"]  # type: ignore[assignment]
    kernel_signals: dict[str, int] = data["kernel_signals"]  # type: ignore[assignment]
    kernel_tail_status = data["kernel_tail_status"]
    init_config: dict[str, int] = data["init_config"]  # type: ignore[assignment]
    init_summary: dict[str, int] = data["init_summary"]  # type: ignore[assignment]
    mig_monitor: dict[str, object] = data["mig_monitor"]  # type: ignore[assignment]
    mig_last: dict[str, int] = mig_monitor.get("last", {})  # type: ignore[assignment]
    heat_epoch_advances = int(data["heat_epoch_advances"])
    event_count = max(len(events), get_int(init_summary, "read_events"),
                      heat_epoch_advances)
    hist_status = read_req_histogram_status(data)
    if hist_status != "not_enabled":
        notes.append(f"read_req_hist={hist_status}")

    if not events:
        notes.append("preheat_diag=missing")
    else:
        usable = [e for e in events if get_int(e, "scan_ops") > 0]
        if not usable:
            notes.append("preheat_sql=no_scans")
        elif any(get_int(e, "sqlite_rows_seen") == 0 for e in usable):
            notes.append("preheat_sql=has_zero_rows")
        else:
            notes.append("preheat_sql=ok")

        with_nand = [e for e in usable if "host_read_nand_delta" in e]
        if not with_nand:
            notes.append("preheat_ftl=missing")
        elif any(get_int(e, "host_read_nand_delta") == 0 for e in with_nand):
            notes.append("preheat_nand=has_zero_delta")
        elif any(get_int(e, "global_read_sum_delta") == 0 for e in with_nand):
            notes.append("preheat_heat=has_zero_delta")
        else:
            notes.append("preheat_ftl=ok")

    if event_count > 128:
        notes.append(
            "preheat_events=too_dense(events={},heat_epoch_advances={},interleave_pages={})".format(
                event_count,
                heat_epoch_advances,
                init_config.get("interleave_pages", "missing"),
            )
        )
    elif event_count:
        notes.append(
            "preheat_events=ok(events={},heat_epoch_advances={})".format(
                event_count,
                heat_epoch_advances,
            )
        )

    total_bg = bg.get("total", {})
    cold_bg = bg.get("cold_read", {})
    bypass_pairs = (
        ("read_prio_bypass_ops", "read_prio_bypass_ns"),
        ("read_prio_ch_bypass_ops", "read_prio_ch_bypass_ns"),
        ("read_prio_pcie_bypass_ops", "read_prio_pcie_bypass_ns"),
    )
    if any(ops in cold_bg for ops, _ in bypass_pairs):
        bypass_scope = cold_bg
    else:
        bypass_scope = total_bg
    bypass_present = any(
        ops in cold_bg or ops in total_bg for ops, _ in bypass_pairs
    )
    bypass_ok = any(
        get_int(bypass_scope, ops) > 0 and get_int(bypass_scope, ns) > 0
        for ops, ns in bypass_pairs
    )
    _, bypass_ops_total, bypass_ns_total = read_priority_bypass_totals(bg)
    nonpreemptive_submit_gate = (
        stats.get("read_priority_model") == "nonpreemptive_submit_gate"
    )
    if nonpreemptive_submit_gate:
        if bypass_ok or get_int(stats, "read_lp_bypass_ops") > 0:
            notes.append("read_prio_bypass=invalid_nonpreemptive_nonzero")
        else:
            notes.append("read_prio_bypass=expected_zero_nonpreemptive")
    elif not bypass_present:
        notes.append("read_prio_bypass=missing")
    elif bypass_ok:
        notes.append("read_prio_bypass=ok")
    else:
        notes.append("read_prio_bypass=zero")

    reads = read_count(stats)
    iw = get_int(stats, "slc_to_qlc_nand_writes") + get_int(stats, "repromote_nand_writes")
    if reads:
        iw_per_read = ratio_float(iw, reads)
        repromote_per_read = ratio_float(get_int(stats, "qlc_repromote_pages"), reads)
        read_bg_conflict_rate = ratio_float(get_int(stats, "read_bg_conflicts"), reads)
        qlc_read_rate = ratio_float(host_read_qlc_count(stats), reads)
        read_begin_bg = get_int(stats, "read_begin_bg_active")
        bg_begin_read = get_int(stats, "bg_begin_read_active")
        read_nand_bg_overlap = get_int(stats, "read_nand_bg_overlap_ops")
        die_conflicts = get_int(stats, "read_die_conflicts")
        die_wait_ns = get_int(stats, "read_die_wait_ns")
        resource_wait_parts = []
        for layer in ("die", "ch", "pcie"):
            total_wait = get_int(stats, f"read_{layer}_wait_ns")
            read_wait = get_int(stats, f"read_{layer}_read_wait_ns")
            bg_wait = get_int(stats, f"read_{layer}_bg_wait_ns")
            other_wait = max(0, total_wait - read_wait - bg_wait)
            resource_wait_parts.append(
                "{}:total_us/read={:.3f},read_us/read={:.3f},bg_us/read={:.3f},other_us/read={:.3f}".format(
                    layer,
                    total_wait / reads / 1000.0,
                    read_wait / reads / 1000.0,
                    bg_wait / reads / 1000.0,
                    other_wait / reads / 1000.0,
                )
            )
        notes.append("resource_wait=" + ";".join(resource_wait_parts))
        die_wait_us_per_read = die_wait_ns / reads / 1000.0
        overlap_present = (
            read_begin_bg > 0 or bg_begin_read > 0 or read_nand_bg_overlap > 0
        )

        notes.append(f"internal_writes_per_read={iw_per_read:.3f}")
        notes.append(f"repromote_pages_per_read={repromote_per_read:.3f}")
        notes.append(f"read_bg_conflict_rate={read_bg_conflict_rate:.3f}")
        notes.append(f"qlc_read_rate={qlc_read_rate:.3f}")
        notes.append(f"read_die_wait_us_per_read={die_wait_us_per_read:.3f}")
        if "read_begin_bg_active" in stats or "bg_begin_read_active" in stats:
            notes.append(
                "read_bg_overlap=read_begin_bg_active={},bg_begin_read_active={},read_nand_bg_overlap_ops={}".format(
                    read_begin_bg,
                    bg_begin_read,
                    read_nand_bg_overlap,
                )
            )
            if not overlap_present:
                notes.append("read_priority_blocker=no_read_bg_overlap")
            elif bg_begin_read > 0 and get_int(stats, "read_priority_yields") == 0:
                notes.append(
                    "read_priority_blocker=gate_missed_bg_start_during_read"
                )
            elif read_begin_bg > 0 and bg_begin_read == 0:
                notes.append(
                    "read_priority_blocker=read_arrived_after_bg_already_issued"
                )
            elif bg_begin_read > 0 and get_int(stats, "read_priority_yields") > 0:
                notes.append(
                    "read_priority_overlap=worker_yielded_during_read_window"
                )
        if die_wait_us_per_read < 1.0:
            notes.append("read_queue_pressure=too_low(no_meaningful_die_wait)")
        elif (die_conflicts > 0 and not overlap_present and
              read_bg_conflict_rate < 0.001):
            notes.append(
                "read_queue_pressure=foreground_read_or_service_dominated"
            )
        else:
            notes.append("read_queue_pressure=present")
        if "read_issue_gap_count" in stats:
            same_time = get_int(stats, "read_issue_same_time")
            gap_count = get_int(stats, "read_issue_gap_count")
            backwards = get_int(stats, "read_issue_backwards")
            same_rate = ratio_float(same_time, reads)
            if same_rate >= 0.5:
                notes.append(
                    "read_issue_pattern=batch_like_same_issue_time(rate={:.3f})".format(
                        same_rate,
                    )
                )
            elif gap_count > 0:
                gap_avg_us = get_int(stats, "read_issue_gap_sum_ns") / gap_count / 1000.0
                notes.append(
                    "read_issue_pattern=spaced(gap_avg_us={:.3f},gap_max_us={:.3f})".format(
                        gap_avg_us,
                        get_int(stats, "read_issue_gap_max_ns") / 1000.0,
                    )
                )
            if backwards > 0:
                notes.append(
                    "read_issue_order=concurrent_inversions(backwards={})".format(
                        backwards,
                    )
                )
        if iw_per_read >= 1.0 or repromote_per_read >= 0.1:
            notes.append(
                "background_write_pressure=high(iw/read={:.3f},repromote/read={:.3f})".format(
                    iw_per_read,
                    repromote_per_read,
                )
            )
        elif iw_per_read >= 0.1 or repromote_per_read > 0:
            notes.append(
                "background_write_pressure=moderate(iw/read={:.3f},repromote/read={:.3f})".format(
                    iw_per_read,
                    repromote_per_read,
                )
            )
        else:
            notes.append("background_write_pressure=low")

        if repromote_per_read >= 0.1:
            notes.append(
                "repromotion_bottleneck=high(pages/read={:.3f})".format(
                    repromote_per_read,
                )
            )
        elif repromote_per_read > 0:
            notes.append(
                "repromotion_bottleneck=present(pages/read={:.3f})".format(
                    repromote_per_read,
                )
            )
        else:
            notes.append("repromotion_bottleneck=none")

        if get_int(stats, "hard_no_victim_count") > 0:
            notes.append(
                "hard_pressure=present(no_victim={})".format(
                    get_int(stats, "hard_no_victim_count"),
                )
            )

        if nonpreemptive_submit_gate and get_int(stats, "read_priority_yields") > 0:
            notes.append(
                "read_priority_runtime=submit_gate_active(yields={},forced_progress={})".format(
                    get_int(stats, "read_priority_yields"),
                    get_int(stats, "read_priority_forced_progress_runs"),
                )
            )
        elif bypass_ok:
            notes.append(
                "read_priority_runtime=active(bypass_ops={},bypass_ns={},read_bg_conflicts={})".format(
                    bypass_ops_total,
                    bypass_ns_total,
                    get_int(stats, "read_bg_conflicts"),
                )
            )
            if iw_per_read >= 1.0 or repromote_per_read >= 0.1:
                notes.append("read_priority_masked=likely(background_write_pressure=high)")
        elif get_int(stats, "read_priority_yields") > 0:
            notes.append(
                "read_priority_runtime=yield_only(yields={},bypass_ops=0)".format(
                    get_int(stats, "read_priority_yields"),
                )
            )
        elif bypass_present:
            notes.append("read_priority_runtime=no_observed_bypass")
    if "read_priority_should_yield_checks" in stats:
        rp_checks = get_int(stats, "read_priority_should_yield_checks")
        rp_true = get_int(stats, "read_priority_should_yield_true")
        rp_window_closed = get_int(stats, "read_priority_should_yield_window_closed")
        rp_gate_closed = get_int(stats, "read_priority_should_yield_gate_closed")
        rp_token_empty = get_int(stats, "read_priority_token_empty")
        rp_window_detail_present = any(
            key in stats for key in (
                "read_priority_window_active_read_hits",
                "read_priority_window_busy_hits",
                "read_priority_window_quiet_hits",
            )
        )
        if rp_window_detail_present:
            rp_window_hits = (
                get_int(stats, "read_priority_window_active_read_hits") +
                get_int(stats, "read_priority_window_busy_hits") +
                get_int(stats, "read_priority_window_quiet_hits")
            )
        else:
            rp_window_hits = get_int(stats, "read_priority_token_window_hits")

        if rp_checks > 0 and rp_true == 0:
            if rp_gate_closed >= rp_checks:
                notes.append("read_priority_gate_state=closed")
            elif rp_window_closed >= rp_checks:
                notes.append("read_priority_gate_state=no_read_window_seen")
            else:
                notes.append("read_priority_gate_state=no_yields")
        elif rp_true > 0:
            notes.append(
                "read_priority_gate_state=yielding(checks={},true={},window_hits={})".format(
                    rp_checks,
                    rp_true,
                    rp_window_hits,
                )
            )
        if rp_token_empty > 0:
            notes.append(
                "read_priority_gate_state=token_budget_exhausted(empty={})".format(
                    rp_token_empty,
                )
            )
    if "read_priority_yields" in stats or "read_priority_forced_progress_runs" in stats:
        rp_yields = get_int(stats, "read_priority_yields")
        rp_forced = get_int(stats, "read_priority_forced_progress_runs")

        if rp_forced > 0:
            notes.append(
                "read_priority_force_progress=active(yields={},forced={})".format(
                    rp_yields,
                    rp_forced,
                )
            )
        elif rp_yields >= 8:
            notes.append(
                "read_priority_force_progress=not_observed(yields={},forced=0)".format(
                    rp_yields,
                )
            )
    if "test_phase_recent_guard_skips" in stats or "test_phase_recent_guard_forced" in stats:
        notes.append(
            "recent_write_guard=skips({},forced={},window_cfg={})".format(
                get_int(stats, "test_phase_recent_guard_skips"),
                get_int(stats, "test_phase_recent_guard_forced"),
                get_int(stats, "test_phase_guard_read_reqs_config"),
            )
        )
    if int(mig_monitor.get("count", 0)):
        notes.append(
            "slc_migration_monitor=active(samples={},last_migrated={},victim_q={}/{},heat_epoch={})".format(
                int(mig_monitor.get("count", 0)),
                get_int(mig_last, "migrated"),
                get_int(mig_last, "victim_q"),
                get_int(mig_last, "victim_cap"),
                get_int(mig_last, "heat_epoch"),
            )
        )
        if get_int(mig_last, "victim_cap") > 0 and (
            get_int(mig_last, "victim_q") >= get_int(mig_last, "victim_cap")
        ):
            notes.append(
                "slc_victim_queue=full(victim_q={},victim_cap={})".format(
                    get_int(mig_last, "victim_q"),
                    get_int(mig_last, "victim_cap"),
                )
            )
    if "maint_v2_tasks_done" in stats or "maint_v2_skip_pct" in stats:
        v2_done = get_int(stats, "maint_v2_tasks_done")
        v2_skip = get_int(stats, "maint_v2_skip_pct")
        v2_no_slack = get_int(stats, "maint_v2_no_slack_skips")
        v2_ch_busy = get_int(stats, "maint_v2_ch_busy_skips")
        v2_demand = get_int(stats, "maint_v2_demand_skips")
        v2_requeued = get_int(stats, "maint_v2_tasks_requeued")
        v2_no_progress = get_int(stats, "maint_v2_no_progress_runs")
        skip_reasons = {
            "no_slack": v2_no_slack,
            "ch_busy": v2_ch_busy,
            "demand": v2_demand,
        }
        dominant_reason, dominant_count = max(skip_reasons.items(), key=lambda item: item[1])
        if v2_done == 0 and v2_skip >= 100:
            notes.append("latency2_v2=inactive(skip_pct=100)")
        elif v2_done == 0:
            notes.append(f"latency2_v2=inactive(skip_pct={v2_skip})")
        elif v2_skip >= 95:
            notes.append(f"latency2_v2=mostly_skipped(tasks_done={v2_done},skip_pct={v2_skip})")
        else:
            notes.append(f"latency2_v2=active(tasks_done={v2_done},skip_pct={v2_skip})")
        notes.append(
            "latency2_v2_detail=dominant_skip({}={},requeued={},no_progress={})".format(
                dominant_reason,
                dominant_count,
                v2_requeued,
                v2_no_progress,
            )
        )
    if "bg_repromote_backlog" in stats:
        repromote_backlog = get_int(stats, "bg_repromote_backlog")
        repromote_capacity = get_int(stats, "bg_repromote_backlog_capacity")
        qlc_backlog = get_int(stats, "bg_qlc_closed_backlog")
        qlc_capacity = get_int(stats, "bg_qlc_closed_backlog_capacity")
        slc_backlog = get_int(stats, "bg_slc_gc_backlog")
        active_bg = get_int(stats, "bg_active_ops")
        if ((repromote_capacity and repromote_backlog > repromote_capacity) or
                (qlc_capacity and qlc_backlog > qlc_capacity)):
            notes.append("background_backlog=invalid_over_capacity")
        elif (repromote_backlog == 0 and qlc_backlog == 0 and
              slc_backlog == 0 and active_bg == 0):
            notes.append("background_backlog=drained")
        else:
            notes.append(
                "background_backlog=pending(repromote={},qlc_closed={},slc_gc={},active={})".format(
                    repromote_backlog, qlc_backlog, slc_backlog, active_bg
                )
            )
    if kernel_tail_status and kernel_tail_status.get("status") not in ("captured",):
        notes.append(f"io_completion=unknown(kernel_tail={kernel_tail_status.get('status')})")
    elif get_int(kernel_signals, "no_free_entry") or get_int(kernel_signals, "hung_task"):
        notes.append(
            "io_completion=bad(no_free_entry={},hung_task={})".format(
                get_int(kernel_signals, "no_free_entry"),
                get_int(kernel_signals, "hung_task"),
            )
        )
    elif get_int(kernel_signals, "write_buffer_alloc_failed"):
        notes.append(
            "io_completion=bad(write_buffer_alloc_failed={})".format(
                get_int(kernel_signals, "write_buffer_alloc_failed"),
            )
        )
    elif not kernel_tail_status:
        notes.append("io_completion=unknown(kernel_tail=missing)")
    else:
        notes.append("io_completion=ok")
    if get_int(kernel_signals, "slow_proc_io_sq") or get_int(kernel_signals, "slow_chmodel"):
        notes.append(
            "io_slow_path=warn(proc_io_sq={},chmodel={})".format(
                get_int(kernel_signals, "slow_proc_io_sq"),
                get_int(kernel_signals, "slow_chmodel"),
            )
        )
    if get_int(kernel_signals, "proc_queue_full") or get_int(kernel_signals, "writeback_queue_full"):
        notes.append(
            "io_queue=warn(proc_full={},writeback_full={})".format(
                get_int(kernel_signals, "proc_queue_full"),
                get_int(kernel_signals, "writeback_queue_full"),
            )
        )
    if get_int(kernel_signals, "write_buffer_alloc_waiting"):
        notes.append(
            "write_buffer_pressure=warn(waiting={})".format(
                get_int(kernel_signals, "write_buffer_alloc_waiting"),
            )
        )
    if get_int(kernel_signals, "credit_scan_capped") or get_int(kernel_signals, "credit_horizon_overflow"):
        notes.append(
            "channel_overflow=warn(scan_capped={},horizon_overflow={})".format(
                get_int(kernel_signals, "credit_scan_capped"),
                get_int(kernel_signals, "credit_horizon_overflow"),
            )
        )
    return notes


def print_summary(data: dict[str, object]) -> None:
    stats: dict[str, int] = data["stats"]  # type: ignore[assignment]
    bg: dict[str, dict[str, int]] = data["bg"]  # type: ignore[assignment]
    events: list[dict[str, int]] = data["read_events"]  # type: ignore[assignment]
    cold_read_latency: dict[str, float] = data["cold_read_latency"]  # type: ignore[assignment]
    kernel_signals: dict[str, int] = data["kernel_signals"]  # type: ignore[assignment]
    kernel_tail_status = data["kernel_tail_status"]
    read_plan = data["read_plan"]
    init_config: dict[str, int] = data["init_config"]  # type: ignore[assignment]
    init_summary: dict[str, int] = data["init_summary"]  # type: ignore[assignment]
    cold_read_set_tier: dict[str, dict[str, object]] = data["cold_read_set_tier"]  # type: ignore[assignment]

    print(f"== {data['tag']} ==")
    print(f"path: {data['path']}")
    if data["cold_full_read"] is not None:
        print(f"cold_full_read_s: {data['cold_full_read']}")
    if init_config or init_summary or data["heat_epoch_advances"]:
        print(
            "init_events: config_total_rows={} summary_total_rows={} config_interleave_pages={} config_reads_per_event={} summary_read_events={} heat_epoch_advances={}".format(
                init_config.get("total_rows", "missing"),
                init_summary.get("total_rows", "missing"),
                init_config.get("interleave_pages", "missing"),
                init_config.get("read_ops_per_event", "missing"),
                init_summary.get("read_events", "missing"),
                data["heat_epoch_advances"],
            )
        )
    print(f"workload: {workload_label(data)}")
    if read_plan:
        print(
            "read_plan: total={total_reads} active_tables={active_tables} top={top_tables}".format(
                **read_plan  # type: ignore[arg-type]
            )
        )
    for phase in ("pre_cold", "post_cold", "unknown"):
        if phase not in cold_read_set_tier:
            continue
        tier = cold_read_set_tier[phase]
        print(
            "cold_read_set_tier[{phase}]: mode={mode} active_tables={active_tables} file_slc={file_lpn_slc} file_qlc={file_lpn_qlc} file_unknown={file_lpn_unknown} file_qlc_pct={file_lpn_qlc_pct} weighted_slc={weighted_slc} weighted_qlc={weighted_qlc} weighted_unknown={weighted_unknown} weighted_qlc_pct={weighted_qlc_pct} tier_entries={tier_entries} tier_parts={tier_parts} tier_slc={tier_slc} tier_qlc={tier_qlc} fiemap_files={fiemap_files} fallback_files={fallback_files} fiemap_extents={fiemap_extents} file_values={file_lpn_values} file_global_values={file_lpn_global_values} file_unique={file_lpn_global_unique} cross_file_dups={file_lpn_cross_file_dups} file_lpn_range={file_lpn_min}-{file_lpn_max}".format(
                phase=phase,
                mode=tier.get("mode", "n/a"),
                active_tables=tier.get("active_tables", "n/a"),
                file_lpn_slc=tier.get("file_lpn_slc", "n/a"),
                file_lpn_qlc=tier.get("file_lpn_qlc", "n/a"),
                file_lpn_unknown=tier.get("file_lpn_unknown", "n/a"),
                file_lpn_qlc_pct=tier.get("file_lpn_qlc_pct", "n/a"),
                weighted_slc=tier.get("weighted_slc", "n/a"),
                weighted_qlc=tier.get("weighted_qlc", "n/a"),
                weighted_unknown=tier.get("weighted_unknown", "n/a"),
                weighted_qlc_pct=tier.get("weighted_qlc_pct", "n/a"),
                tier_entries=tier.get("tier_entries", "n/a"),
                tier_parts=tier.get("tier_parts", "n/a"),
                tier_slc=tier.get("tier_slc", "n/a"),
                tier_qlc=tier.get("tier_qlc", "n/a"),
                fiemap_files=tier.get("fiemap_files", "n/a"),
                fallback_files=tier.get("fallback_files", "n/a"),
                fiemap_extents=tier.get("fiemap_extents", "n/a"),
                file_lpn_values=tier.get("file_lpn_values", "n/a"),
                file_lpn_global_values=tier.get("file_lpn_global_values", "n/a"),
                file_lpn_global_unique=tier.get("file_lpn_global_unique", "n/a"),
                file_lpn_cross_file_dups=tier.get("file_lpn_cross_file_dups", "n/a"),
                file_lpn_min=tier.get("file_lpn_min", "n/a"),
                file_lpn_max=tier.get("file_lpn_max", "n/a"),
            )
        )
    print(f"init_drop_cache_each_read: {data['init_drop_cache_each_read']}")
    if cold_read_latency:
        print(
            "cold_read_latency: count={} avg_ms={} p50_ms={} p95_ms={} p99_ms={} p999_ms={} max_ms={}".format(
                cold_read_latency.get("count", "missing"),
                fmt_seconds_as_ms(cold_read_latency, "avg"),
                fmt_seconds_as_ms(cold_read_latency, "p50"),
                fmt_seconds_as_ms(cold_read_latency, "p95"),
                fmt_seconds_as_ms(cold_read_latency, "p99"),
                fmt_seconds_as_ms(cold_read_latency, "p999"),
                fmt_seconds_as_ms(cold_read_latency, "max"),
            )
        )
    if "read_req_latency_count" in stats:
        print(
            "read_req_latency: count={} avg_us={} p50_us={} p95_us={} p99_us={} p999_us={} max_us={} hist_bins={}".format(
                get_int(stats, "read_req_latency_count"),
                fmt_ns_as_us(stats, "read_req_latency_avg_ns"),
                fmt_ns_as_us(stats, "read_req_latency_p50_ns"),
                fmt_ns_as_us(stats, "read_req_latency_p95_ns"),
                fmt_ns_as_us(stats, "read_req_latency_p99_ns"),
                fmt_ns_as_us(stats, "read_req_latency_p999_ns"),
                fmt_ns_as_us(stats, "read_req_latency_max_ns"),
                get_int(stats, "read_req_latency_hist_bins"),
            )
        )
        print(f"read_req_histogram: {read_req_histogram_status(data)}")

    if events:
        usable = [e for e in events if get_int(e, "scan_ops") > 0]
        rows = sum(get_int(e, "sqlite_rows_seen") for e in usable)
        nand = sum(get_int(e, "host_read_nand_delta") for e in usable)
        heat = sum(get_int(e, "global_read_sum_delta") for e in usable)
        last_valid = next(
            (
                get_int(e, "global_valid_pg_cnt")
                for e in reversed(events)
                if "global_valid_pg_cnt" in e
            ),
            0,
        )
        zero_rows = sum(1 for e in usable if get_int(e, "sqlite_rows_seen") == 0)
        zero_nand = sum(1 for e in usable if "host_read_nand_delta" in e and get_int(e, "host_read_nand_delta") == 0)
        zero_heat = sum(1 for e in usable if "global_read_sum_delta" in e and get_int(e, "global_read_sum_delta") == 0)
        print(
            "read_events: count={} usable={} rows={} host_nand_delta={} heat_delta={} last_valid_pages={} nand/scan={} heat/row={} zero_rows={} zero_nand={} zero_heat={}".format(
                len(events),
                len(usable),
                rows,
                nand,
                heat,
                last_valid,
                ratio(nand, sum(get_int(e, "scan_ops") for e in usable)),
                ratio(heat, rows),
                zero_rows,
                zero_nand,
                zero_heat,
            )
        )
    else:
        print("read_events: missing")

    for phase in ("mixed_init", "cold_read", "total"):
        if phase in bg:
            p = bg[phase]
            print(
                "bg_nand[{phase}]: bypass_ops={ops} bypass_ns={ns} ch_ops={ch_ops} ch_ns={ch_ns} pcie_ops={pcie_ops} pcie_ns={pcie_ns} lp_host_write_ops={lpw}".format(
                    phase=phase,
                    ops=fmt_int(p, "read_prio_bypass_ops"),
                    ns=fmt_int(p, "read_prio_bypass_ns"),
                    ch_ops=fmt_int(p, "read_prio_ch_bypass_ops"),
                    ch_ns=fmt_int(p, "read_prio_ch_bypass_ns"),
                    pcie_ops=fmt_int(p, "read_prio_pcie_bypass_ops"),
                    pcie_ns=fmt_int(p, "read_prio_pcie_bypass_ns"),
                    lpw=fmt_int(p, "lp_host_write_ops"),
                )
            )

    reads = read_count(stats)
    iw = get_int(stats, "slc_to_qlc_nand_writes") + get_int(stats, "repromote_nand_writes")
    die_conflicts = get_int(stats, "read_die_conflicts")
    die_wait_ns = get_int(stats, "read_die_wait_ns")
    lp_bypass_ops = get_int(stats, "read_lp_bypass_ops")
    lp_bypass_ns = get_int(stats, "read_lp_bypass_ns")
    print(
        "core: reads={} slc_reads={} qlc_reads={} phys_slc_reads={} phys_qlc_reads={} tier_mismatch_reads={} slc_to_qlc_pages={} repromote_pages={} internal_write_pages={} iw/read={} read_bg_conflicts={} read_die_conflicts={} read_die_wait_avg_us/read={} read_die_wait_avg_us/conflict={} read_lp_bypass_ops={} read_lp_bypass_avg_us/op={} read_lp_bypass_avg_us/read={} hard_no_victim={}".format(
            reads,
            host_read_slc_count(stats),
            host_read_qlc_count(stats),
            host_read_phys_slc_count(stats),
            host_read_phys_qlc_count(stats),
            host_read_tier_mismatch_count(stats),
            get_int(stats, "slc_to_qlc_migration_pages"),
            get_int(stats, "qlc_repromote_pages"),
            iw,
            ratio(iw, reads),
            get_int(stats, "read_bg_conflicts"),
            die_conflicts,
            avg_ns_as_us(die_wait_ns, reads),
            avg_ns_as_us(die_wait_ns, die_conflicts),
            lp_bypass_ops,
            avg_ns_as_us(lp_bypass_ns, lp_bypass_ops),
            avg_ns_as_us(lp_bypass_ns, reads),
            get_int(stats, "hard_no_victim_count"),
        )
    )
    for layer in ("die", "ch", "pcie"):
        total_wait = get_int(stats, f"read_{layer}_wait_ns")
        read_wait = get_int(stats, f"read_{layer}_read_wait_ns")
        bg_wait = get_int(stats, f"read_{layer}_bg_wait_ns")
        print(
            "resource_wait[{layer}]: conflicts={conflicts} wait_us/read={total} read_conflicts={read_conflicts} read_wait_us/read={read_wait} bg_conflicts={bg_conflicts} bg_wait_us/read={bg_wait} other_wait_us/read={other}".format(
                layer=layer,
                conflicts=get_int(stats, f"read_{layer}_conflicts"),
                total=avg_ns_as_us(total_wait, reads),
                read_conflicts=get_int(stats, f"read_{layer}_read_conflicts"),
                read_wait=avg_ns_as_us(read_wait, reads),
                bg_conflicts=get_int(stats, f"read_{layer}_bg_conflicts"),
                bg_wait=avg_ns_as_us(bg_wait, reads),
                other=avg_ns_as_us(max(0, total_wait - read_wait - bg_wait), reads),
            )
        )
    if "read_begin_bg_active" in stats or "bg_begin_read_active" in stats:
        print(
            "read_overlap: read_begin_bg_active={} bg_begin_read_active={} read_nand_bg_overlap_ops={}".format(
                fmt_int(stats, "read_begin_bg_active"),
                fmt_int(stats, "bg_begin_read_active"),
                fmt_int(stats, "read_nand_bg_overlap_ops"),
            )
        )
    if "read_issue_gap_count" in stats:
        gap_count = get_int(stats, "read_issue_gap_count")
        print(
            "read_issue: same_time={} backwards={} gap_count={} gap_avg_us={} gap_max_us={} last_issue_ns={}".format(
                fmt_int(stats, "read_issue_same_time"),
                fmt_int(stats, "read_issue_backwards"),
                fmt_int(stats, "read_issue_gap_count"),
                avg_ns_as_us(get_int(stats, "read_issue_gap_sum_ns"), gap_count),
                fmt_ns_as_us(stats, "read_issue_gap_max_ns"),
                fmt_int(stats, "last_read_issue_ns"),
            )
        )
    if "maint_v2_tasks_done" in stats or "maint_v2_skip_pct" in stats:
        print(
            "latency2_v2: tasks_done={} skip_pct={}".format(
                get_int(stats, "maint_v2_tasks_done"),
                get_int(stats, "maint_v2_skip_pct"),
            )
        )
    if ("read_priority_yields" in stats or
            "read_priority_delayed_requeues" in stats or
            "read_priority_forced_progress_runs" in stats):
        print(
            "read_priority_ctrl: yields={} delayed_requeues={} forced_progress={}".format(
                get_int(stats, "read_priority_yields"),
                get_int(stats, "read_priority_delayed_requeues"),
                get_int(stats, "read_priority_forced_progress_runs"),
            )
        )
    if "read_priority_should_yield_checks" in stats:
        print(
            "read_priority_diag: gate_entries={} checks={} true={} force_blocked={} gate_closed={} window_closed={} active_hits={} busy_hits={} quiet_hits={} token_empty={} gate_current={} force_current={} streak_current={} busy_until_ns={} tokens_current={} token_hits={}".format(
                fmt_int(stats, "read_priority_gate_entries"),
                fmt_int(stats, "read_priority_should_yield_checks"),
                fmt_int(stats, "read_priority_should_yield_true"),
                fmt_int(stats, "read_priority_should_yield_force_blocked"),
                fmt_int(stats, "read_priority_should_yield_gate_closed"),
                fmt_int(stats, "read_priority_should_yield_window_closed"),
                fmt_int(stats, "read_priority_window_active_read_hits"),
                fmt_int(stats, "read_priority_window_busy_hits"),
                fmt_int(stats, "read_priority_window_quiet_hits"),
                fmt_int(stats, "read_priority_token_empty"),
                fmt_int(stats, "read_priority_gate_current"),
                fmt_int(stats, "read_priority_force_current"),
                fmt_int(stats, "read_priority_yield_streak_current"),
                fmt_int(stats, "read_priority_busy_until_ns"),
                fmt_int(stats, "read_priority_gate_tokens_current"),
                fmt_int(stats, "read_priority_token_window_hits"),
            )
        )
    if kernel_tail_status:
        status = kernel_tail_status  # type: ignore[assignment]
        print(
            "kernel_tail: status={status} mode={mode} start_line={start_line} end_line={end_line} start_boot_s={start_boot_s} start_filter_s={start_filter_s} time_slack_s={time_slack_s} matches={matches}".format(
                status=status.get("status", "unknown"),
                mode=status.get("mode", "n/a"),
                start_line=status.get("start_line", "n/a"),
                end_line=status.get("end_line", "n/a"),
                start_boot_s=status.get("start_boot_s", "n/a"),
                start_filter_s=status.get("start_filter_s", "n/a"),
                time_slack_s=status.get("time_slack_s", "n/a"),
                matches=status.get("matches", "n/a"),
            )
        )
    if any(kernel_signals.values()):
        print(
            "kernel_signals: no_free_entry={no_free_entry} hung_task={hung_task} slow_proc_io_sq={slow_proc_io_sq} slow_chmodel={slow_chmodel} proc_queue_full={proc_queue_full} writeback_queue_full={writeback_queue_full} write_buffer_alloc_failed={write_buffer_alloc_failed} write_buffer_alloc_waiting={write_buffer_alloc_waiting} credit_scan_capped={credit_scan_capped} credit_horizon_overflow={credit_horizon_overflow}".format(
                **kernel_signals
            )
        )
    print(f"outcome: {experiment_outcome(data)}")
    print("verdict: " + ", ".join(verdict(data)))
    print()


def print_compare(rows: list[dict[str, object]]) -> None:
    headers = [
        "tag",
        "workload",
        "compare",
        "outcome",
        "cold_s",
        "lat_avg_ms",
        "lat_p95_ms",
        "lat_p99_ms",
        "lat_p999_ms",
        "lat_max_ms",
        "reads",
        "lat_count",
        "req_lat_count",
        "req_p95_us",
        "req_p99_us",
        "req_p999_us",
	"die_wait_us/read",
	"die_bg_wait_us/read",
	"ch_wait_us/read",
	"ch_bg_wait_us/read",
	"pcie_wait_us/read",
	"pcie_bg_wait_us/read",
        "total_rows",
        "qlc_reads",
        "phys_qlc_reads",
        "tier_mismatch_reads",
        "repromote_pg",
        "slc2qlc_pg",
        "iw/read",
        "read_bg_rate",
        "read_begin_bg",
        "bg_begin_read",
        "qlc_read_rate",
        "issue_same",
        "issue_gap_avg_us",
        "issue_gap_max_us",
        "bypass_ops",
        "bypass_ns",
        "read_lp_bypass_ops",
        "read_lp_bypass_ns",
        "rp_yields",
        "rp_requeues",
        "rp_forced",
        "rp_gate_entries",
        "rp_checks",
        "rp_true",
        "rp_gate_closed",
        "rp_window_closed",
        "rp_active_hits",
        "rp_busy_hits",
        "rp_quiet_hits",
        "rp_token_empty",
        "rp_tokens",
        "rp_token_hits",
        "read_prio",
        "read_blocker",
        "queue_pressure",
        "gate_state",
        "issue_pattern",
        "bg_pressure",
        "repromotion",
        "masked",
        "guard",
        "v2_done",
        "v2_skip_pct",
        "v2_no_slack",
        "v2_ch_busy",
        "v2_demand",
        "v2_requeued",
        "v2_no_progress",
        "latency2_v2",
        "latency2_detail",
        "io",
    ]
    print("\t".join(headers))
    statuses = compare_statuses(rows)
    for idx, data in enumerate(rows):
        stats: dict[str, int] = data["stats"]  # type: ignore[assignment]
        bg: dict[str, dict[str, int]] = data["bg"]  # type: ignore[assignment]
        init_summary: dict[str, int] = data["init_summary"]  # type: ignore[assignment]
        cold_read_latency: dict[str, float] = data["cold_read_latency"]  # type: ignore[assignment]
        notes = verdict(data)
        reads = read_count(stats)
        iw = get_int(stats, "slc_to_qlc_nand_writes") + get_int(stats, "repromote_nand_writes")
        _, bypass_ops, bypass_ns = read_priority_bypass_totals(bg)
        cold_s = data["cold_full_read"]
        print(
            "\t".join(
                [
                    str(data["tag"]),
                    workload_label(data),
                    statuses[idx],
                    experiment_outcome(data, statuses[idx]),
                    "n/a" if cold_s is None else str(cold_s),
                    fmt_seconds_as_ms(cold_read_latency, "avg"),
                    fmt_seconds_as_ms(cold_read_latency, "p95"),
                    fmt_seconds_as_ms(cold_read_latency, "p99"),
                    fmt_seconds_as_ms(cold_read_latency, "p999"),
                    fmt_seconds_as_ms(cold_read_latency, "max"),
                    str(reads),
                    str(cold_read_latency.get("count", "n/a")),
                    fmt_int(stats, "read_req_latency_count"),
                    fmt_ns_as_us(stats, "read_req_latency_p95_ns"),
                    fmt_ns_as_us(stats, "read_req_latency_p99_ns"),
                    fmt_ns_as_us(stats, "read_req_latency_p999_ns"),
			    avg_ns_as_us(get_int(stats, "read_die_wait_ns"), reads),
			    avg_ns_as_us(get_int(stats, "read_die_bg_wait_ns"), reads),
			    avg_ns_as_us(get_int(stats, "read_ch_wait_ns"), reads),
			    avg_ns_as_us(get_int(stats, "read_ch_bg_wait_ns"), reads),
			    avg_ns_as_us(get_int(stats, "read_pcie_wait_ns"), reads),
			    avg_ns_as_us(get_int(stats, "read_pcie_bg_wait_ns"), reads),
                    str(init_summary.get("total_rows", "n/a")),
                    str(host_read_qlc_count(stats)),
                    str(host_read_phys_qlc_count(stats)),
                    str(host_read_tier_mismatch_count(stats)),
                    str(get_int(stats, "qlc_repromote_pages")),
                    str(get_int(stats, "slc_to_qlc_migration_pages")),
                    ratio(iw, reads),
                    ratio(get_int(stats, "read_bg_conflicts"), reads),
                    str(get_int(stats, "read_begin_bg_active")),
                    str(get_int(stats, "bg_begin_read_active")),
                    ratio(host_read_qlc_count(stats), reads),
                    str(get_int(stats, "read_issue_same_time")),
                    avg_ns_as_us(
                        get_int(stats, "read_issue_gap_sum_ns"),
                        get_int(stats, "read_issue_gap_count"),
                    ),
                    fmt_ns_as_us(stats, "read_issue_gap_max_ns"),
                    str(bypass_ops),
                    str(bypass_ns),
                    str(get_int(stats, "read_lp_bypass_ops")),
                    str(get_int(stats, "read_lp_bypass_ns")),
                    str(get_int(stats, "read_priority_yields")),
                    str(get_int(stats, "read_priority_delayed_requeues")),
                    str(get_int(stats, "read_priority_forced_progress_runs")),
                    fmt_int(stats, "read_priority_gate_entries"),
                    fmt_int(stats, "read_priority_should_yield_checks"),
                    fmt_int(stats, "read_priority_should_yield_true"),
                    fmt_int(stats, "read_priority_should_yield_gate_closed"),
                    fmt_int(stats, "read_priority_should_yield_window_closed"),
                    fmt_int(stats, "read_priority_window_active_read_hits"),
                    fmt_int(stats, "read_priority_window_busy_hits"),
                    fmt_int(stats, "read_priority_window_quiet_hits"),
                    fmt_int(stats, "read_priority_token_empty"),
                    fmt_int(stats, "read_priority_gate_tokens_current"),
                    fmt_int(stats, "read_priority_token_window_hits"),
                    first_note(notes, "read_priority_runtime="),
                    first_note(notes, "read_priority_blocker=", "n/a"),
                    first_note(notes, "read_queue_pressure=", "n/a"),
                    first_note(notes, "read_priority_gate_state=", "n/a"),
                    first_note(notes, "read_issue_pattern=", "n/a"),
                    first_note(notes, "background_write_pressure="),
                    first_note(notes, "repromotion_bottleneck="),
                    first_note(notes, "read_priority_masked=", "n/a"),
                    first_note(notes, "recent_write_guard="),
                    str(get_int(stats, "maint_v2_tasks_done")),
                    str(get_int(stats, "maint_v2_skip_pct")),
                    str(get_int(stats, "maint_v2_no_slack_skips")),
                    str(get_int(stats, "maint_v2_ch_busy_skips")),
                    str(get_int(stats, "maint_v2_demand_skips")),
                    str(get_int(stats, "maint_v2_tasks_requeued")),
                    str(get_int(stats, "maint_v2_no_progress_runs")),
                    first_note(notes, "latency2_v2="),
                    first_note(notes, "latency2_v2_detail="),
                    first_note(notes, "io_completion="),
                ]
            )
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path, help="sqlite init log files")
    parser.add_argument(
        "--compare",
        action="store_true",
        help="print one tab-separated cross-log comparison table",
    )
    args = parser.parse_args()

    rows = []
    for path in args.logs:
        if not path.exists():
            raise SystemExit(f"missing log: {path}")
        rows.append(parse_log(path))
    if args.compare:
        print_compare(rows)
    else:
        for row in rows:
            print_summary(row)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
