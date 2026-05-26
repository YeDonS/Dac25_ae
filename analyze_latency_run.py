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
    "read_bg_conflicts",
    "read_die_conflicts",
    "read_die_wait_ns",
    "read_priority_yields",
    "read_priority_delayed_requeues",
    "read_priority_forced_progress_runs",
    "slc_to_qlc_migration_pages",
    "qlc_repromote_pages",
    "internal_write_pages_est",
    "slc_to_qlc_nand_reads",
    "slc_to_qlc_nand_writes",
    "repromote_nand_reads",
    "repromote_nand_writes",
    "hard_no_victim_count",
    "maint_v2_tasks_done",
    "maint_v2_skip_pct",
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
        "bg": {},
        "read_events": [],
        "read_plan": None,
        "init_config": {},
        "init_summary": {},
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
    bg: dict[str, dict[str, int]] = data["bg"]  # type: ignore[assignment]
    read_events: list[dict[str, int]] = data["read_events"]  # type: ignore[assignment]
    init_config: dict[str, int] = data["init_config"]  # type: ignore[assignment]
    init_summary: dict[str, int] = data["init_summary"]  # type: ignore[assignment]
    mig_monitor: dict[str, object] = data["mig_monitor"]  # type: ignore[assignment]
    kernel_signals: dict[str, int] = data["kernel_signals"]  # type: ignore[assignment]
    pending_events: dict[int, dict[str, int]] = {}

    for raw in path.read_text(errors="replace").splitlines():
        line = raw.strip()

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

        if "[sqlite_init] tag=" in line:
            pairs = parse_kv_pairs(line)
            data["tag"] = pairs.get("tag", data["tag"])
            if "cold_full_read" in pairs:
                try:
                    data["cold_full_read"] = float(pairs["cold_full_read"].rstrip("s"))
                except ValueError:
                    pass

        if "[sqlite_init] config " in line:
            pairs = parse_kv_pairs(line)
            if "init_drop_cache_each_read" in pairs:
                try:
                    data["init_drop_cache_each_read"] = int(pairs["init_drop_cache_each_read"])
                except ValueError:
                    pass
            for key in ("tables", "interleave_pages", "read_ops_per_event"):
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

        m = re.match(r"^([A-Za-z0-9_]+)\s+(-?\d+)$", line)
        if m and m.group(1) in KEYS:
            stats[m.group(1)] = int(m.group(2))

    read_events.extend(pending_events[k] for k in sorted(pending_events))
    return data


def get_int(mapping: dict[str, int], key: str) -> int:
    return int(mapping.get(key, 0))


def fmt_int(mapping: dict[str, int], key: str) -> str:
    if key not in mapping:
        return "missing"
    return str(get_int(mapping, key))


def ratio(num: int, den: int) -> str:
    if den <= 0:
        return "n/a"
    return f"{num / den:.3f}"


def ratio_float(num: int, den: int) -> float:
    if den <= 0:
        return 0.0
    return num / den


def first_note(notes: list[str], prefix: str, default: str = "n/a") -> str:
    for note in notes:
        if note.startswith(prefix):
            return note
    return default


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
    if not bypass_present:
        notes.append("read_prio_bypass=missing")
    elif bypass_ok:
        notes.append("read_prio_bypass=ok")
    else:
        notes.append("read_prio_bypass=zero")

    reads = get_int(stats, "read_requests") or get_int(stats, "host_read_nand_ops")
    iw = get_int(stats, "slc_to_qlc_nand_writes") + get_int(stats, "repromote_nand_writes")
    if reads:
        iw_per_read = ratio_float(iw, reads)
        repromote_per_read = ratio_float(get_int(stats, "qlc_repromote_pages"), reads)
        read_bg_conflict_rate = ratio_float(get_int(stats, "read_bg_conflicts"), reads)

        notes.append(f"internal_writes_per_read={iw_per_read:.3f}")
        notes.append(f"repromote_pages_per_read={repromote_per_read:.3f}")
        notes.append(f"read_bg_conflict_rate={read_bg_conflict_rate:.3f}")
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

        if bypass_ok:
            notes.append(
                "read_priority_runtime=active(bypass_ops={},bypass_ns={},read_bg_conflicts={})".format(
                    bypass_ops_total,
                    bypass_ns_total,
                    get_int(stats, "read_bg_conflicts"),
                )
            )
        elif get_int(stats, "read_priority_yields") > 0:
            notes.append(
                "read_priority_runtime=yield_only(yields={},bypass_ops=0)".format(
                    get_int(stats, "read_priority_yields"),
                )
            )
        elif bypass_present:
            notes.append("read_priority_runtime=no_observed_bypass")
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
        if v2_done == 0 and v2_skip >= 100:
            notes.append("latency2_v2=inactive(skip_pct=100)")
        elif v2_done == 0:
            notes.append(f"latency2_v2=inactive(skip_pct={v2_skip})")
        elif v2_skip >= 95:
            notes.append(f"latency2_v2=mostly_skipped(tasks_done={v2_done},skip_pct={v2_skip})")
        else:
            notes.append(f"latency2_v2=active(tasks_done={v2_done},skip_pct={v2_skip})")
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
    kernel_signals: dict[str, int] = data["kernel_signals"]  # type: ignore[assignment]
    kernel_tail_status = data["kernel_tail_status"]
    read_plan = data["read_plan"]
    init_config: dict[str, int] = data["init_config"]  # type: ignore[assignment]
    init_summary: dict[str, int] = data["init_summary"]  # type: ignore[assignment]

    print(f"== {data['tag']} ==")
    print(f"path: {data['path']}")
    if data["cold_full_read"] is not None:
        print(f"cold_full_read_s: {data['cold_full_read']}")
    if init_config or init_summary or data["heat_epoch_advances"]:
        print(
            "init_events: config_interleave_pages={} config_reads_per_event={} summary_read_events={} heat_epoch_advances={}".format(
                init_config.get("interleave_pages", "missing"),
                init_config.get("read_ops_per_event", "missing"),
                init_summary.get("read_events", "missing"),
                data["heat_epoch_advances"],
            )
        )
    if read_plan:
        print(
            "read_plan: total={total_reads} active_tables={active_tables} top={top_tables}".format(
                **read_plan  # type: ignore[arg-type]
            )
        )
    print(f"init_drop_cache_each_read: {data['init_drop_cache_each_read']}")

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

    reads = get_int(stats, "read_requests") or get_int(stats, "host_read_nand_ops")
    iw = get_int(stats, "slc_to_qlc_nand_writes") + get_int(stats, "repromote_nand_writes")
    print(
        "core: reads={} slc_to_qlc_pages={} repromote_pages={} internal_write_pages={} iw/read={} read_bg_conflicts={} read_die_conflicts={} hard_no_victim={}".format(
            reads,
            get_int(stats, "slc_to_qlc_migration_pages"),
            get_int(stats, "qlc_repromote_pages"),
            iw,
            ratio(iw, reads),
            get_int(stats, "read_bg_conflicts"),
            get_int(stats, "read_die_conflicts"),
            get_int(stats, "hard_no_victim_count"),
        )
    )
    if "maint_v2_tasks_done" in stats or "maint_v2_skip_pct" in stats:
        print(
            "latency2_v2: tasks_done={} skip_pct={}".format(
                get_int(stats, "maint_v2_tasks_done"),
                get_int(stats, "maint_v2_skip_pct"),
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
    print("verdict: " + ", ".join(verdict(data)))
    print()


def print_compare(rows: list[dict[str, object]]) -> None:
    headers = [
        "tag",
        "cold_s",
        "reads",
        "qlc_reads",
        "repromote_pg",
        "slc2qlc_pg",
        "iw/read",
        "read_bg_rate",
        "bypass_ops",
        "bypass_ns",
        "read_prio",
        "bg_pressure",
        "latency2_v2",
        "io",
    ]
    print("\t".join(headers))
    for data in rows:
        stats: dict[str, int] = data["stats"]  # type: ignore[assignment]
        bg: dict[str, dict[str, int]] = data["bg"]  # type: ignore[assignment]
        notes = verdict(data)
        reads = get_int(stats, "read_requests") or get_int(stats, "host_read_nand_ops")
        iw = get_int(stats, "slc_to_qlc_nand_writes") + get_int(stats, "repromote_nand_writes")
        _, bypass_ops, bypass_ns = read_priority_bypass_totals(bg)
        cold_s = data["cold_full_read"]
        print(
            "\t".join(
                [
                    str(data["tag"]),
                    "n/a" if cold_s is None else str(cold_s),
                    str(reads),
                    str(get_int(stats, "host_read_qlc_ops")),
                    str(get_int(stats, "qlc_repromote_pages")),
                    str(get_int(stats, "slc_to_qlc_migration_pages")),
                    ratio(iw, reads),
                    ratio(get_int(stats, "read_bg_conflicts"), reads),
                    str(bypass_ops),
                    str(bypass_ns),
                    first_note(notes, "read_priority_runtime="),
                    first_note(notes, "background_write_pressure="),
                    first_note(notes, "latency2_v2="),
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
