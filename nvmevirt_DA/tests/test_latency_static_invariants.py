#!/usr/bin/env python3
"""Static regression checks for latency2/3 read-priority wiring."""

from __future__ import annotations

import re
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//.*", "", text)
    return text


def read_source(name: str) -> str:
    return strip_comments((ROOT / name).read_text())


def extract_function(source: str, marker: str) -> str:
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 0
    for idx in range(brace, len(source)):
        char = source[idx]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start : idx + 1]
    raise AssertionError(f"function not closed: {marker}")


def assert_no_plain_nand_calls(source: str, name: str) -> None:
    bad = re.findall(r"\bssd_advance_nand\s*\(", source)
    assert not bad, f"{name}: plain ssd_advance_nand() calls bypass HP/LP model"


def assert_no_plain_wbuf_calls(source: str, name: str) -> None:
    bad = re.findall(r"\bssd_advance_write_buffer\s*\(", source)
    assert not bad, f"{name}: plain ssd_advance_write_buffer() calls bypass LP model"


def assert_latency_variant(name: str) -> None:
    source = read_source(name)
    conv_read_start = source.index("static bool conv_read")
    conv_read_end = source.index("static void migrate_page_to_slc", conv_read_start)
    conv_read = source[conv_read_start:conv_read_end]
    prefix = "latency2" if "latency2" in name else "latency3"

    assert_no_plain_nand_calls(source, name)
    assert_no_plain_wbuf_calls(source, name)
    assert "ssd_advance_nand_read_priority" in source, (
        f"{name}: host reads must use read-priority NAND path"
    )
    assert "ssd_advance_nand_low_priority" in source, (
        f"{name}: background/write NAND work must use low-priority path"
    )
    assert "ssd_advance_write_buffer_low_priority" in source, (
        f"{name}: host write-buffer DMA must use low-priority path"
    )
    assert "enqueue_writeback_io_req(" in source, (
        f"{name}: writeback release must be queued for asynchronous buffer return"
    )
    assert "enqueue_writeback_io_req_lun_guarded" not in source, (
        f"{name}: writeback release must not wait on a moving global LUN tail"
    )
    assert "completion_pcie_guard" not in conv_read, (
        f"{name}: host reads must not be delayed by write early-completion guard"
    )
    assert "completion_pcie_guard" not in source, (
        f"{name}: host write completion must not wait on a moving global PCIe tail"
    )
    assert "write buffer allocation failed after" not in source, (
        f"{name}: transient write-buffer pressure must backpressure, not complete EIO"
    )
    assert re.search(
        rf"force_after_yields\s*=\s*{prefix}_read_priority_should_force_progress",
        source,
    ), (
        f"{name}: FORCE_AFTER_YIELDS must be evaluated before emergency OR"
    )
    assert re.search(
        r"force_progress\s*=\s*\(level\s*>=\s*SLC_LEVEL_EMERGENCY\)\s*\|\|\s*force_after_yields",
        source,
    ), (
        f"{name}: emergency and FORCE_AFTER_YIELDS must both force progress"
    )

    repromote_start = source.rindex("static void bg_repromotion_worker")
    repromote_end = source.index("static void bg_qlc_rebalance_worker", repromote_start)
    repromote = source[repromote_start:repromote_end]
    qlc_start = repromote_end
    qlc_end = source.index("static void bg_repromotion_delayed_worker", qlc_start)
    qlc = source[qlc_start:qlc_end]
    if prefix == "latency3":
        slc_start = source.index("static void latency3_bg_slc_maint_run")
        slc_end = source.index("static void bg_slc_maint_worker", slc_start)
    else:
        slc_start = source.rindex("static void bg_slc_maint_worker")
        slc_end = source.index("static bool conv_read", slc_start)
    slc = source[slc_start:slc_end]
    latency_num = "2" if prefix == "latency2" else "3"

    assert f"NVMEV_LATENCY{latency_num}_FORCE_CATCHUP_MAX" in source, (
        f"{name}: forced progress must cap catch-up work explicitly"
    )
    assert "catchup_passes" in slc, (
        f"{name}: FORCE_AFTER_YIELDS must convert skipped work into catch-up passes"
    )
    assert re.search(r"for\s*\(\s*pass\s*=\s*0;\s*pass\s*<\s*catchup_passes", slc), (
        f"{name}: forced SLC maintenance must run bounded catch-up passes, not one pass"
    )
    if prefix == "latency2":
        assert re.search(
            r"bg_slc_maint_worker_v2\s*\(\s*conv_ftl\s*,\s*level\s*,\s*force_after_yields\s*\)",
            source,
        ), (
            f"{name}: latency2 V2 dispatcher must receive forced catch-up state"
        )
        assert "#define NVMEV_LATENCY2_MAX_INFLIGHT_MAINT_SBS 1U" in source, (
            f"{name}: latency2 V2 must cap concurrent maintenance SBs by default"
        )
        assert "maint_v2_can_enqueue_new_sb(conv_ftl)" in source, (
            f"{name}: latency2 V2 must not enqueue new SB work while the "
            "in-flight SB cap is reached"
        )
        assert "maint_v2_backlog_tasks" in source, (
            f"{name}: latency2 must expose current maintenance backlog"
        )
        assert "maint_v2_backlog_max" in source, (
            f"{name}: latency2 must expose maximum maintenance backlog"
        )

    for worker_name, body, requeue in (
        ("repromotion", repromote, f"{prefix}_repromotion_delayed_requeue"),
        ("qlc_rebalance", qlc, f"{prefix}_qlc_rebalance_delayed_requeue"),
        ("slc_maint", slc, f"{prefix}_slc_maint_delayed_requeue"),
    ):
        assert f"{prefix}_read_priority_should_yield" in body, (
            f"{name}: {worker_name} worker must yield during read window"
        )
        assert f"{prefix}_read_priority_note_yield" in body, (
            f"{name}: {worker_name} worker must count read-priority yields"
        )
        assert requeue in body, (
            f"{name}: {worker_name} worker must retry through delayed work"
        )

    rebalance_impl = extract_function(
        source[source.rindex("static bool qlc_maybe_rebalance_internal") :],
        "static bool qlc_maybe_rebalance_internal",
    )
    assert "uint32_t unit_budget" in rebalance_impl, (
        f"{name}: rebalance must receive an explicit submission-unit budget"
    )
    assert "units < unit_budget" in rebalance_impl, (
        f"{name}: rebalance must stop before reserving a second unit"
    )
    assert "units++" in rebalance_impl and "return more;" in rebalance_impl, (
        f"{name}: rebalance must report that unfinished work needs requeue"
    )
    assert f"qlc_maybe_rebalance_internal(conv_ftl, 1)" in qlc, (
        f"{name}: priority rebalance worker must admit at most one page unit"
    )
    assert re.search(
        rf"if\s*\(more\)\s*{prefix}_qlc_rebalance_delayed_requeue",
        qlc,
    ), (
        f"{name}: unfinished rebalance work must cross a fresh priority gate"
    )


def assert_read_priority_window_uses_simulated_time(name: str) -> None:
    source = read_source(name)
    header = read_source("conv_ftl.h")
    prefix = "latency2" if "latency2" in name else "latency3"
    latency_num = "2" if prefix == "latency2" else "3"

    assert "atomic64_t test_phase_read_priority_busy_until_ns" in header, (
        "conv_ftl.h: read-priority window must keep a simulated busy-until tail"
    )
    assert "atomic64_t test_phase_read_priority_gate_tokens" in header, (
        "conv_ftl.h: read-priority window must keep bounded maintenance-gate tokens"
    )
    extend_helper = extract_function(
        header, "nvmev_test_phase_extend_read_priority_window"
    )
    assert "atomic64_cmpxchg" in extend_helper, (
        "conv_ftl.h: busy-until updates must be monotonic under concurrency"
    )
    assert "busy_until_ns > (uint64_t)old_until" in extend_helper, (
        "conv_ftl.h: busy-until helper must only extend the read window"
    )
    assert "read_priority_busy_until_ns" in header, (
        "conv_ftl.h: read-priority diagnostics must print busy_until_ns"
    )
    refresh_helper = extract_function(
        header, "nvmev_test_phase_refresh_read_priority_tokens"
    )
    assert "atomic64_cmpxchg" in refresh_helper and "tokens > (uint64_t)old_tokens" in refresh_helper, (
        "conv_ftl.h: read-priority token refresh must monotonically top up tokens"
    )
    consume_helper = extract_function(
        header, "nvmev_test_phase_consume_read_priority_token"
    )
    assert "atomic64_cmpxchg" in consume_helper and "old_tokens - 1" in consume_helper, (
        "conv_ftl.h: read-priority token consume must be atomic and bounded"
    )
    assert "read_priority_token_window_hits" in header, (
        "conv_ftl.h: read-priority diagnostics must print token-window hits"
    )
    for field in [
        "read_priority_window_active_read_hits",
        "read_priority_window_busy_hits",
        "read_priority_window_quiet_hits",
        "read_priority_token_empty",
    ]:
        assert field in header, (
            f"conv_ftl.h: read-priority diagnostics must print {field}"
        )

    window_start = source.rindex(
        f"static bool {prefix}_read_priority_read_window_active"
    )
    window_fn = extract_function(
        source[window_start:],
        f"static bool {prefix}_read_priority_read_window_active",
    )
    active_pos = window_fn.index("test_phase_active_reads")
    busy_pos = window_fn.index("test_phase_read_priority_busy_until_ns")
    wall_pos = window_fn.index("test_phase_last_read_ktime_ns")
    token_pos = window_fn.index("nvmev_test_phase_consume_read_priority_token")
    assert active_pos < busy_pos < wall_pos < token_pos, (
        f"{name}: read window must check active reads, then simulated "
        "busy-until, then wall-clock fallback, then bounded gate tokens"
    )
    assert "bool window_active = false;" in window_fn, (
        f"{name}: read window must separate window detection from token budget"
    )
    assert "if (!window_active)\n\t\treturn false;" in window_fn, (
        f"{name}: inactive read windows must not consume gate tokens"
    )
    assert "if (!nvmev_test_phase_consume_read_priority_token(conv_ftl))" in window_fn, (
        f"{name}: every active read window yield must consume one gate token"
    )
    assert "__get_ioclock(conv_ftl->ssd)" in window_fn, (
        f"{name}: simulated read window must compare against the FTL IO clock"
    )
    assert "io_now <= busy_until_ns" in window_fn, (
        f"{name}: simulated read window must stay active until busy_until_ns"
    )
    assert "ktime_get_ns()" in window_fn, (
        f"{name}: wall-clock quiet window should remain only as a fallback"
    )
    assert "test_phase_read_priority_token_window_hits" in window_fn, (
        f"{name}: token-window hits must be counted when they trigger yield"
    )
    for field in [
        "test_phase_read_priority_window_active_read_hits",
        "test_phase_read_priority_window_busy_hits",
        "test_phase_read_priority_window_quiet_hits",
        "test_phase_read_priority_token_empty",
    ]:
        assert field in window_fn, (
            f"{name}: read window diagnostics must count {field}"
        )

    conv_read_start = source.index("static bool conv_read")
    conv_read_end = source.index("static void migrate_page_to_slc", conv_read_start)
    conv_read = source[conv_read_start:conv_read_end]
    extend_call = f"{prefix}_extend_read_priority_window_all"
    end_call = f"{prefix}_note_read_window_end_all"
    assert extend_call in conv_read, (
        f"{name}: conv_read must extend the simulated read-priority window"
    )
    assert conv_read.rindex(extend_call) < conv_read.rindex(end_call), (
        f"{name}: read window must be extended before active_reads is decremented"
    )
    assert f"nsecs_latest + NVMEV_LATENCY{latency_num}_READ_QUIET_NS" in conv_read, (
        f"{name}: busy-until must be based on the simulated read completion time"
    )
    assert "atomic64_set(&conv_ftl->test_phase_read_priority_busy_until_ns, 0)" in source, (
        f"{name}: test-phase reset must clear the simulated read window"
    )
    assert "atomic64_set(&conv_ftl->test_phase_read_priority_gate_tokens, 0)" in source, (
        f"{name}: test-phase reset must clear read-priority gate tokens"
    )
    assert f"NVMEV_LATENCY{latency_num}_READ_WINDOW_GATE_TOKENS" in source, (
        f"{name}: read-priority gate-token window must be compile-time tunable"
    )
    assert "nvmev_test_phase_refresh_read_priority_tokens" in extract_function(
        source, "static void test_phase_note_read_begin"
    ), f"{name}: tracked host reads must refresh read-priority gate tokens"
    assert "test_phase_read_lp_bypass_ops" in source, (
        f"{name}: test-phase stats must expose read LP bypass op count"
    )
    assert "test_phase_read_lp_bypass_ns" in source, (
        f"{name}: test-phase stats must expose read LP bypass time"
    )
    kick_start = source.rindex("static void slc_maint_kick")
    kick_fn = extract_function(source[kick_start:], "static void slc_maint_kick")
    assert f"{prefix}_read_priority_should_yield(conv_ftl)" in kick_fn, (
        f"{name}: slc_maint_kick must use the common read-priority yield gate"
    )
    assert f"{prefix}_read_priority_note_yield(conv_ftl)" in kick_fn, (
        f"{name}: slc_maint_kick read-priority deferrals must count as yields"
    )
    assert f"{prefix}_read_priority_read_window_active(conv_ftl)" not in kick_fn, (
        f"{name}: slc_maint_kick must not consume read-window tokens directly"
    )

    assert "nvmev_test_phase_bind_read_wait(&srd, stats_ftl)" in conv_read, (
        f"{name}: conv_read must bind test-phase resource wait counters"
    )


def assert_read_lp_bypass_diagnostics() -> None:
    header = read_source("ssd.h")
    ssd_die = read_source("ssd_die.c")
    conv_header = read_source("conv_ftl.h")
    analyzer = (ROOT.parent / "evaluation/sqlite/analyze_latency_run.py").read_text()

    for field in ["tracked_read_lp_bypass_ops", "tracked_read_lp_bypass_ns"]:
        assert field in header, f"ssd.h: legacy nand_cmd ABI must retain {field}"
    assert "atomic64_inc(ncmd->tracked_read_lp_bypass_ops)" not in ssd_die, (
        "ssd_die.c: non-preemptive model must not claim an LP-tail bypass"
    )

    assert "test_phase_read_lp_bypass_ops" in conv_header, (
        "conv_ftl.h: test phase must store read LP bypass op count"
    )
    assert "test_phase_read_lp_bypass_ns" in conv_header, (
        "conv_ftl.h: test phase must store read LP bypass time"
    )
    assert "read_lp_bypass_ops" in analyzer, (
        "analyzer must parse and print read LP bypass diagnostics"
    )
    assert "read_lp_bypass_avg_us/read" in analyzer, (
        "analyzer must report per-read LP bypass amount"
    )


def assert_resource_wait_attribution() -> None:
    header = read_source("ssd.h")
    conv_header = read_source("conv_ftl.h")
    analyzer = (ROOT.parent / "evaluation/sqlite/analyze_latency_run.py").read_text()

    for owner in (
        "SSD_OWNER_HOST_READ",
        "SSD_OWNER_HOST_WRITE",
        "SSD_OWNER_BACKGROUND",
    ):
        assert owner in header, f"ssd.h: missing resource owner {owner}"
    assert header.count("enum ssd_resource_owner tail_owner") == 3, (
        "ssd.h: LUN, channel and PCIe must each retain direct blocker identity"
    )
    for field in (
        "read_die_read_wait_ns",
        "read_die_bg_wait_ns",
        "read_ch_wait_ns",
        "read_ch_read_wait_ns",
        "read_ch_bg_wait_ns",
        "read_pcie_wait_ns",
        "read_pcie_read_wait_ns",
        "read_pcie_bg_wait_ns",
    ):
        assert f"test_phase_{field}" in conv_header, (
            f"conv_ftl.h: missing test-phase resource wait field {field}"
        )
        assert f'"{field}"' in analyzer, (
            f"analyzer: missing resource wait field {field}"
        )
    assert "nvmev_test_phase_read_wait_reset" in conv_header
    assert "nvmev_test_phase_read_wait_seq_print" in conv_header
    assert "nvmev_test_phase_bind_read_wait" in conv_header
    assert "nvmev_bg_backlog_seq_print" in conv_header

    for name in ("ssd.c", "ssd_die.c"):
        source = read_source(name)
        assert "ssd_track_read_wait" in source, (
            f"{name}: missing per-layer read wait attribution"
        )
        assert "tail_owner = owner" in source, (
            f"{name}: committed resource owner must follow the serialized tail"
        )
        assert "ssd_channel_xfer_time" not in source, (
            f"{name}: priority channel path must not bypass the shared credit model"
        )
        assert "ssd_pcie_xfer_time" not in source, (
            f"{name}: priority PCIe path must not bypass the shared credit model"
        )

    for name in (
        "conv_ftl_latency1_superblock.c",
        "conv_ftl_latency2_superblock.c",
        "conv_ftl_latency3_superblock.c",
    ):
        source = read_source(name)
        assert "nvmev_test_phase_bind_read_wait(&srd, stats_ftl)" in source, (
            f"{name}: host read must bind counters only after test-phase tracking begins"
        )
        assert "nvmev_test_phase_read_wait_reset(conv_ftl)" in source, (
            f"{name}: test-phase reset must clear resource wait counters"
        )
        assert "nvmev_test_phase_read_wait_seq_print(m, conv_ftl)" in source, (
            f"{name}: test-phase stats must export resource wait attribution"
        )
        assert "nvmev_bg_backlog_seq_print(m, conv_ftl)" in source, (
            f"{name}: stats must export bounded background backlog"
        )


def assert_read_overlap_and_issue_diagnostics() -> None:
    header = read_source("conv_ftl.h")
    analyzer = (ROOT.parent / "evaluation/sqlite/analyze_latency_run.py").read_text()

    for field in [
        "test_phase_read_begin_bg_active",
        "test_phase_bg_begin_read_active",
        "test_phase_last_read_issue_ns",
        "test_phase_read_issue_gap_count",
        "test_phase_read_issue_gap_sum_ns",
        "test_phase_read_issue_gap_max_ns",
        "test_phase_read_issue_same_time",
        "test_phase_read_issue_backwards",
    ]:
        assert field in header, f"conv_ftl.h: test phase must store {field}"

    assert "nvmev_test_phase_note_read_issue" in header, (
        "conv_ftl.h: read issue-time helper must be available to latency variants"
    )
    assert "nvmev_test_phase_read_overlap_seq_print" in header, (
        "conv_ftl.h: read overlap diagnostics must have a common proc printer"
    )

    for name in (
        "conv_ftl_latency2_superblock.c",
        "conv_ftl_latency3_superblock.c",
    ):
        source = read_source(name)
        note_read = extract_function(source, "static void test_phase_note_read_begin")
        note_bg = extract_function(source, "static void test_phase_note_bg_begin")
        conv_read_start = source.index("static bool conv_read")
        conv_read_end = source.index("static void migrate_page_to_slc", conv_read_start)
        conv_read = source[conv_read_start:conv_read_end]

        assert "uint64_t issue_ns" in note_read, (
            f"{name}: test_phase_note_read_begin must accept simulated issue time"
        )
        assert "nvmev_test_phase_note_read_issue(conv_ftl, issue_ns)" in note_read, (
            f"{name}: read begin must record simulated issue-time gaps"
        )
        assert "test_phase_read_begin_bg_active" in note_read, (
            f"{name}: read begin must count reads arriving while bg is active"
        )
        assert "test_phase_bg_begin_read_active" in note_bg, (
            f"{name}: bg begin must count bg starting while reads are active"
        )
        assert "test_phase_note_read_begin(stats_ftl, nsecs_start" in conv_read, (
            f"{name}: conv_read must pass req->nsecs_start into read-begin diagnostics"
        )
        assert "nvmev_test_phase_read_overlap_seq_print(m, conv_ftl)" in source, (
            f"{name}: proc stats must print read overlap and issue diagnostics"
        )

    for key in [
        "read_begin_bg_active",
        "bg_begin_read_active",
        "read_issue_same_time",
        "read_issue_gap_max_ns",
        "read_issue_backwards",
        "read_priority_blocker=",
        "read_queue_pressure=",
        "read_priority_gate_state=",
        "read_issue_pattern=",
    ]:
        assert key in analyzer, f"analyzer must parse and print {key}"


def assert_latency2_yield_gates_v2_dispatcher() -> None:
    source = read_source("conv_ftl_latency2_superblock.c")
    worker_start = source.rindex("static void bg_slc_maint_worker")
    worker = extract_function(source[worker_start:], "static void bg_slc_maint_worker")

    yield_pos = worker.index(
        "if (!force_progress && latency2_read_priority_should_yield"
    )
    v2_pos = worker.index("if (maint_v2_enabled(conv_ftl))")
    assert yield_pos < v2_pos, (
        "latency2: read-priority yield must gate maintenance before the V2 "
        "per-die dispatcher runs"
    )
    assert "latency2_slc_maint_delayed_requeue(conv_ftl)" in worker, (
        "latency2: yielded SLC maintenance must be retried through delayed work"
    )
    assert re.search(
        r"bg_slc_maint_worker_v2\s*\(\s*conv_ftl\s*,\s*level\s*,\s*force_after_yields\s*\)",
        worker,
    ), (
        "latency2: non-yielded maintenance must use the V2 per-die dispatcher"
    )
    assert "if (maint_v2_alloc(conv_ftl) != 0)" in source, (
        "latency2: only V2 allocation failure should make the worker fall back"
    )
    assert "falling back to V1 worker" in source, (
        "latency2: V2 allocation fallback must be explicit in diagnostics"
    )


def assert_latency1_baseline() -> None:
    name = "conv_ftl_latency1_superblock.c"
    source = read_source(name)
    conv_read_start = source.index("static bool conv_read")
    conv_read_end = source.index("static void migrate_page_to_slc", conv_read_start)
    conv_read = source[conv_read_start:conv_read_end]

    assert "ssd_advance_nand(" in conv_read, (
        f"{name}: latency1 should remain the plain NAND baseline read path"
    )
    assert "ssd_advance_nand_read_priority" not in source, (
        f"{name}: latency1 baseline must not use read-priority NAND path"
    )
    assert "ssd_advance_nand_low_priority" not in source, (
        f"{name}: latency1 baseline must not use low-priority NAND path"
    )
    assert "ssd_advance_write_buffer_low_priority" not in source, (
        f"{name}: latency1 baseline must not use low-priority write-buffer path"
    )


def assert_slc_gc_eligibility_aligned() -> None:
    for name in (
        "conv_ftl_latency1_superblock.c",
        "conv_ftl_latency2_superblock.c",
        "conv_ftl_latency3_superblock.c",
    ):
        source = read_source(name)
        migrate_body = extract_function(
            source, "static uint32_t migrate_cold_pages_to_victim_queue_from_slc"
        )
        hard_body = extract_function(source, "static bool slc_hard_make_victim")

        assert not re.search(
            r"slc_sb_finish_migration\s*\(\s*conv_ftl\s*,\s*blk_id\s*,\s*moved\s*>\s*0\s*\)",
            source,
        ), (
            f"{name}: SLC SBs must not become GC victims merely because one "
            "cold page migrated"
        )
        assert "bool *scan_complete" in source, (
            f"{name}: SB migration must report whether the current pass scanned "
            "the SB without exhausting its local budget"
        )
        assert "#define NVMEV_SLC_GC_REQUIRE_COMPLETE_MIGRATION 1" in source, (
            f"{name}: strict SLC GC eligibility compile flag must default on"
        )
        assert "compile_slc_gc_requires_complete_migration" in source, (
            f"{name}: stats must expose strict SLC GC eligibility so runs can "
            "be audited"
        )
        assert "after.total_vpc == 0" in migrate_body, (
            f"{name}: GC eligibility must re-check that migration left no valid "
            "SLC pages behind"
        )
        assert "scan_complete && sb_moved > 0" in migrate_body, (
            f"{name}: partially migrated SBs may enter GC only after a complete "
            "filtered scan, not after the first moved page"
        )
        assert not re.search(
            r"else\s+if\s*\(\s*sb_moved\s*>\s*0\s*\)\s*enqueue_for_gc\s*=\s*true",
            migrate_body,
        ), f"{name}: moved>0 must not directly mark a SB GC-eligible"
        assert "!moved && !slc_has_any_victim" not in hard_body, (
            f"{name}: hard victim creation must force migration whenever the "
            "filtered pass did not produce a victim, even if it moved some pages"
        )
        assert re.search(
            r"if\s*\(\s*!slc_has_any_victim\s*\(\s*conv_ftl\s*\)\s*\)\s*"
            r"moved\s*\+=\s*slc_migrate_sb_pages_to_qlc\s*\([^;]*true\s*\)",
            hard_body,
            flags=re.S,
        ), (
            f"{name}: hard victim creation must keep progressing to force_all "
            "when the filtered pass leaves no GC victim"
        )


def assert_latency_qlc_wrapper(
    name: str,
    base_source: str,
    qlc_hotcold: int,
    qlc_rebalance: int,
    test_phase_rebalance: int,
    read_repromotion: int | None = None,
) -> None:
    source = read_source(name)

    assert f"#define NVMEV_ENABLE_QLC_HOTCOLD {qlc_hotcold}" in source, (
        f"{name}: wrong QLC hot/cold compile switch"
    )
    assert f"#define NVMEV_ENABLE_QLC_REBALANCE {qlc_rebalance}" in source, (
        f"{name}: wrong QLC rebalance compile switch"
    )
    assert (
        f"#define NVMEV_TEST_PHASE_QLC_REBALANCE_ENABLE {test_phase_rebalance}"
        in source
    ), f"{name}: wrong test-phase QLC rebalance switch"
    if read_repromotion is not None:
        assert f"#define NVMEV_ENABLE_READ_REPROMOTION {read_repromotion}" in source, (
            f"{name}: wrong read-repromotion compile switch"
        )
        assert f"#define NVMEV_TEST_PHASE_REPROMOTION_ENABLE {read_repromotion}" in source, (
            f"{name}: wrong test-phase repromotion switch"
        )
    assert f'#include "{base_source}"' in source, (
        f"{name}: must include {base_source} to keep latency variants aligned"
    )


def assert_latency_qlc_ablation_wrappers() -> None:
    for latency in (1, 2, 3):
        base = f"conv_ftl_latency{latency}_superblock.c"
        assert_latency_qlc_wrapper(
            f"conv_ftl_latency{latency}_norp_superblock.c",
            base,
            qlc_hotcold=0,
            qlc_rebalance=0,
            test_phase_rebalance=0,
            read_repromotion=0,
        )
        assert_latency_qlc_wrapper(
            f"conv_ftl_latency{latency}_qlc_hotcold_superblock.c",
            base,
            qlc_hotcold=1,
            qlc_rebalance=0,
            test_phase_rebalance=0,
        )
        assert_latency_qlc_wrapper(
            f"conv_ftl_latency{latency}_qlc_all_superblock.c",
            base,
            qlc_hotcold=1,
            qlc_rebalance=1,
            test_phase_rebalance=1,
        )
        assert_latency_qlc_wrapper(
            f"conv_ftl_latency{latency}_qlc_all_norp_superblock.c",
            base,
            qlc_hotcold=1,
            qlc_rebalance=0,
            test_phase_rebalance=0,
            read_repromotion=0,
        )


def assert_qlc_all_norp_wrappers_show_latency_policy() -> None:
    lat1 = read_source("conv_ftl_latency1_qlc_all_norp_superblock.c")
    lat2 = read_source("conv_ftl_latency2_qlc_all_norp_superblock.c")
    lat3 = read_source("conv_ftl_latency3_qlc_all_norp_superblock.c")

    assert "NVMEV_LATENCY1_FORCE_AFTER_YIELDS" not in lat1, (
        "latency1 qlc_all_norp wrapper must remain the placement-only control"
    )
    for latency, source in ((1, lat1), (2, lat2), (3, lat3)):
        assert "#define NVMEV_SLC_GC_REQUIRE_COMPLETE_MIGRATION 1" in source, (
            f"latency{latency} qlc_all_norp wrapper must explicitly opt into "
            "strict SLC GC eligibility"
        )
    for latency, source in ((2, lat2), (3, lat3)):
        assert "#define NVMEV_TEST_PHASE_READ_REQ_LATENCY_STATS 1" in source, (
            f"latency{latency} qlc_all_norp wrapper must emit host read-request "
            "latency stats for the compiled variant"
        )
        assert "#define NVMEV_TEST_PHASE_READ_PRIORITY_DIAG 1" in source, (
            f"latency{latency} qlc_all_norp wrapper must emit read-priority "
            "window/yield diagnostics for the compiled variant"
        )
        assert f'#include "conv_ftl_latency{latency}_superblock.c"' in source, (
            f"latency{latency} qlc_all_norp wrapper must include the latency"
            f"{latency} base implementation so read-window fixes stay synced"
        )
        assert f"#define NVMEV_LATENCY{latency}_REQUEUE_DELAY_US 0U" in source, (
            f"latency{latency} qlc_all_norp wrapper must immediately retry "
            "read-priority delayed maintenance"
        )
        assert f"#define NVMEV_LATENCY{latency}_FORCE_AFTER_YIELDS 8U" in source, (
            f"latency{latency} qlc_all_norp wrapper must explicitly set the "
            "read-priority force threshold"
        )
        assert f"#define NVMEV_LATENCY{latency}_FORCE_CATCHUP_MAX 8U" in source, (
            f"latency{latency} qlc_all_norp wrapper must explicitly cap "
            "forced catch-up to eight skipped maintenance opportunities"
        )
        assert f"#define NVMEV_LATENCY{latency}_READ_WINDOW_GATE_TOKENS 8U" in source, (
            f"latency{latency} qlc_all_norp wrapper must explicitly keep recent "
            "reads active across maintenance-gate checks"
        )
    assert "#define NVMEV_LATENCY2_MAX_INFLIGHT_MAINT_SBS 0U" in lat2, (
        "latency2 qlc_all_norp wrapper must explicitly allow multiple V2 "
        "in-flight maintenance SBs for this stronger scheduler run"
    )


def assert_io_completion_guard() -> None:
    source = read_source("io.c")

    assert "completion_pcie_guard" in source, "io.c: missing completion PCIe guard"
    assert "ssd_pcie_next_idle_time" in source, "io.c: missing PCIe tail lookup"
    assert "__reschedule_proc_entry" in source, "io.c: missing completion reschedule"


def assert_io_completion_queue_orders_by_target() -> None:
    source = read_source("io.c")

    assert "pi->proc_table[curr].nsecs_target <= ret->nsecs_target" in source, (
        "io.c: host completions must be inserted by simulated target time"
    )
    assert "pi->proc_table[curr].nsecs_target <= nsecs_target" in source, (
        "io.c: writeback completions must be inserted by simulated target time"
    )
    assert "__reschedule_proc_entry" in source, (
        "io.c: delayed guards must be able to reinsert entries by target time"
    )


def assert_ssd_hp_lp_time_model(name: str) -> None:
    source = read_source(name)

    assert "return max(lun->next_lun_avail_time, cmd_stime)" in source, (
        f"{name}: NAND must serialize against the single committed tail"
    )
    assert "return max(ch->next_ch_avail_time, request_time)" in source, (
        f"{name}: channel must serialize against the single committed tail"
    )
    assert "return max(pcie->next_pcie_avail_time, request_time)" in source, (
        f"{name}: PCIe must serialize against the single committed tail"
    )
    assert "ssd_lun_commit_normal(lun, end)" in source, (
        f"{name}: priority commit must not move previously returned LP work"
    )
    assert "if (ncmd->type != GC_IO && ncmd->interleave_pci_dma)" in source, (
        f"{name}: internal migration must use channel but skip host PCIe DMA"
    )
    assert "read_priority = force_read_priority && ncmd->type == USER_IO && c == NAND_READ" in source, (
        f"{name}: only host reads should enter the HP path"
    )
    assert "low_priority = force_low_priority && !read_priority" in source, (
        f"{name}: background identity must remain available to the submit gate"
    )


def assert_ssd_timing_tail_locks() -> None:
    header = read_source("ssd.h")

    assert "#include <linux/spinlock.h>" in header, (
        "ssd.h: timing tail locks require linux/spinlock.h"
    )
    assert header.count("spinlock_t timing_lock") >= 3, (
        "ssd.h: NAND LUN, channel, and PCIe timing tails must each have a lock"
    )

    for name in ("ssd.c", "ssd_die.c"):
        source = read_source(name)
        assert "spin_lock_init(&lun->timing_lock)" in source, (
            f"{name}: LUN timing lock must be initialized"
        )
        assert "spin_lock_init(&ch->timing_lock)" in source, (
            f"{name}: channel timing lock must be initialized"
        )
        assert "spin_lock_init(&pcie->timing_lock)" in source, (
            f"{name}: PCIe timing lock must be initialized"
        )
        for resource in ("pcie", "ch", "lun"):
            assert f"spin_lock(&{resource}->timing_lock)" in source, (
                f"{name}: {resource} timing tail updates must be serialized"
            )
            assert f"spin_unlock(&{resource}->timing_lock)" in source, (
                f"{name}: {resource} timing tail updates must release their lock"
            )
        assert "READ_ONCE(ch->next_ch_avail_time)" in source, (
            f"{name}: idle scans must read channel tail with READ_ONCE"
        )
        assert "READ_ONCE(lun->next_lun_avail_time)" in source, (
            f"{name}: idle scans must read LUN tail with READ_ONCE"
        )
        assert "READ_ONCE(ssd->pcie->next_pcie_avail_time)" in source, (
            f"{name}: idle scans must read PCIe tail with READ_ONCE"
        )


def assert_latency_qlc_ablation_docs_explain_competition() -> None:
    doc = (ROOT / "LATENCY_QLC_ABLATIONS.md").read_text()
    doc_flat = " ".join(doc.split())

    for phrase in (
        "The first CPU is used by `nvmev_dispatcher`",
        "`WQ_UNBOUND` means the kernel may run the background worker",
        "`max_active=1` means one work item at a time per workqueue",
	    "Read priority defers background work before its next NAND operation",
        "Host read latency is request-level latency",
        "per-resource timing locks",
    ):
        assert phrase in doc_flat, (
            "LATENCY_QLC_ABLATIONS.md: missing competition model note: "
            f"{phrase}"
        )


def assert_cpu_and_workqueue_competition_model() -> None:
    main = read_source("main.c")
    io = read_source("io.c")

    assert re.search(
        r"if\s*\(\s*first\s*\)\s*\{\s*config->cpu_nr_dispatcher\s*=\s*cpu_nr\s*;",
        main,
    ), (
        "main.c: first cpus= entry must be assigned to the dispatcher"
    )
    assert "config->cpu_nr_io_workers[config->nr_io_cpu] = cpu_nr" in main, (
        "main.c: non-first cpus= entries must become I/O worker CPUs"
    )
    assert "kthread_bind(nvmev_vdev->nvmev_manager, nvmev_vdev->config.cpu_nr_dispatcher)" in main, (
        "main.c: dispatcher kthread must bind to cpu_nr_dispatcher"
    )
    assert "kthread_bind(pi->nvmev_io_worker, nvmev_vdev->config.cpu_nr_io_workers[proc_idx])" in io, (
        "io.c: I/O worker kthreads must bind to cpu_nr_io_workers[]"
    )
    assert "return (sqid - 1) % nvmev_vdev->config.nr_io_cpu" in io, (
        "io.c: SQ-to-worker mapping must be bounded by nr_io_cpu"
    )

    for name in (
        "conv_ftl_latency1_superblock.c",
        "conv_ftl_latency2_superblock.c",
        "conv_ftl_latency3_superblock.c",
    ):
        source = read_source(name)
        assert re.search(
            r"alloc_workqueue\s*\(\s*\"nvmev_bg_mig\"\s*,\s*"
            r"WQ_UNBOUND\s*\|\s*WQ_MEM_RECLAIM\s*,\s*1\s*\)",
            source,
        ), (
            f"{name}: background migration workqueue must remain "
            "WQ_UNBOUND with max_active=1"
        )


def assert_latency3_emergency_progress() -> None:
    source = read_source("conv_ftl_latency3_superblock.c")
    start = source.index("static void latency3_bg_slc_maint_run")
    end = source.index("static void bg_slc_maint_worker", start)
    body = source[start:end]

    assert "force_progress = (level >= SLC_LEVEL_EMERGENCY) ||" in body, (
        "latency3: emergency SLC pressure must bypass read-priority yield"
    )
    assert "if (!force_progress && latency3_read_priority_should_yield" in body, (
        "latency3: normal BG/URGENT maintenance should yield to active reads"
    )
    assert "if (level >= SLC_LEVEL_EMERGENCY && !slc_has_any_victim" in body, (
        "latency3: emergency maintenance must synchronously manufacture a victim"
    )
    assert re.search(
        r"do_gc_superblock_slc\s*\(\s*conv_ftl\s*,\s*level\s*>=\s*SLC_LEVEL_EMERGENCY\s*\)",
        body,
    ), (
        "latency3: emergency maintenance must force SLC GC when a victim exists"
    )
    assert "budget = 1;" in body, (
        "latency3: normal maintenance must submit one page migration per pass"
    )
    assert "NVMEV_LATENCY3_FORCE_UNITS_PER_RUN" in source, (
        "latency3: forced progress must cap physical maintenance units"
    )
    assert "latency3_slc_maint_delayed_requeue(conv_ftl)" in body, (
        "latency3: a completed unit must requeue through a fresh read gate"
    )


def assert_channel_scan_cap() -> None:
    source = read_source("channel_model.c")
    header = read_source("channel_model.h")

    assert "CHMODEL_SCAN_LIMIT" in header, "channel_model.h: missing scan cap"
    assert "CHMODEL_SCAN_LIMIT" in source, "channel_model.c: scan cap unused"
    assert "overflow_tail" in source, "channel_model.c: missing overflow tail fallback"


def assert_qlc_allocation_reserves_pages() -> None:
    for name in (
        "conv_ftl_latency1_superblock.c",
        "conv_ftl_latency2_superblock.c",
        "conv_ftl_latency3_superblock.c",
    ):
        source = read_source(name)
        start = source.index("static int qlc_try_allocate_zone")
        end = source.index("static int qlc_do_allocate", start)
        body = source[start:end]

        assert "page_reserved_for_new_write" in source, (
            f"{name}: mark_page_valid must accept reserved pages"
        )
        assert "spin_lock(&conv_ftl->qlc_lock)" in body, (
            f"{name}: QLC page allocation must reserve under qlc_lock"
        )
        assert "reserve_page_for_new_write(page)" in body, (
            f"{name}: QLC allocation must reserve the chosen page before returning"
        )


def assert_page_tier_accounting_consistent() -> None:
    header = read_source("conv_ftl.h")
    for field in (
        "test_phase_host_read_slc_pages",
        "test_phase_host_read_qlc_pages",
        "test_phase_host_read_phys_slc_pages",
        "test_phase_host_read_phys_qlc_pages",
        "test_phase_host_read_tier_mismatch_pages",
    ):
        assert f"atomic64_t {field}" in header, (
            f"conv_ftl.h: struct conv_ftl must declare {field}"
        )

    for name in (
        "conv_ftl_latency_superblock.c",
        "conv_ftl_latency1_superblock.c",
        "conv_ftl_latency2_superblock.c",
        "conv_ftl_latency3_superblock.c",
    ):
        source = read_source(name)
        start = source.index("static inline void set_maptbl_ent_reason")
        end = source.index("static inline void set_maptbl_ent(", start)
        body = source[start:end]

        assert "conv_ftl->page_in_slc[lpn]" in body, (
            f"{name}: page_in_slc must be synchronized at the maptbl update point"
        )
        assert "new_mapped && is_slc_block(conv_ftl, ppa->g.blk)" in body, (
            f"{name}: page_in_slc must reflect the new physical PPA tier"
        )

    for name in (
        "conv_ftl_latency1_superblock.c",
        "conv_ftl_latency2_superblock.c",
        "conv_ftl_latency3_superblock.c",
    ):
        source = read_source(name)
        assert "page_tier_raw" in source, f"{name}: missing raw tier debugfs view"
        assert "mapped_phys_qlc_pages" in source, (
            f"{name}: missing mapped physical QLC diagnostic"
        )
        assert "host_read_phys_qlc_nand_ops" in source, (
            f"{name}: missing physical QLC read counter"
        )
        assert "host_read_phys_qlc_pages" in source, (
            f"{name}: missing active page-level physical QLC read counter"
        )
        assert "host_read_tier_mismatch_nand_ops" in source, (
            f"{name}: missing logical/physical tier mismatch read counter"
        )
        assert "static void test_phase_note_host_read_nand(struct conv_ftl *stats_ftl" in source, (
            f"{name}: host-read tier accounting helper is missing"
        )
        assert "uint32_t xfer_size" in source and "atomic64_add(read_pages" in source, (
            f"{name}: host-read tier accounting must record active read pages"
        )


def assert_qlc_live_accounting_consistent() -> None:
    for name in (
        "conv_ftl_latency1_superblock.c",
        "conv_ftl_latency2_superblock.c",
        "conv_ftl_latency3_superblock.c",
    ):
        source = read_source(name)

        valid_helper = extract_function(source, "static void qlc_note_resident_valid_locked")
        invalid_helper = extract_function(source, "static void qlc_note_resident_invalid_locked")
        without_helpers = source.replace(valid_helper, "").replace(invalid_helper, "")

        assert "qlc_note_resident_valid_locked" in source, (
            f"{name}: QLC valid accounting must be centralized"
        )
        assert "qlc_note_resident_invalid_locked" in source, (
            f"{name}: QLC invalid accounting must be centralized"
        )
        assert "qlc_note_resident_valid_locked" in extract_function(
            source, "static void update_qlc_latency_zone"
        ), f"{name}: QLC placement must record live resident pages"
        assert "qlc_note_resident_invalid_locked" in extract_function(
            source, "static void mark_page_invalid"
        ), f"{name}: QLC invalidation must decrement live resident pages"
        assert "qlc_note_resident_valid_locked" in extract_function(
            source, "static int migrate_page_within_qlc"
        ), f"{name}: QLC internal migration must re-add the destination page"

        for token in (
            "qlc_fast_count++",
            "qlc_slow_count++",
            "qlc_fast_count--",
            "qlc_slow_count--",
            "qlc_resident_page_cnt++",
            "qlc_resident_page_cnt--",
        ):
            assert token not in without_helpers, (
                f"{name}: {token} must stay inside QLC live-accounting helpers"
            )


def assert_sqlite_preheat_diagnostics() -> None:
    source = (ROOT.parent / "evaluation/sqlite/sqlite_append_die_affinity_tablefile_pageflow_fileparallel.c").read_text()

    assert "[sqlite_init] read_plan total_reads=%llu active_tables=%u top_tables=" in source, (
        "sqlite workload: missing read_plan top-table diagnostic"
    )
    assert "[sqlite_init] read_event=%u ftl_heat global_read_sum_delta=%llu " in source, (
        "sqlite workload: missing per-read-event global heat delta diagnostic"
    )
    assert "host_read_nand_delta=%llu" in source, (
        "sqlite workload: missing per-read-event host NAND delta diagnostic"
    )
    assert "global_valid_pg_cnt=%llu" in source, (
        "sqlite workload: missing valid-page count diagnostic"
    )
    assert "cold_read_set_tier mode=%s phase=%s" in source, (
        "sqlite workload: missing pre/post cold read-set tier phase diagnostic"
    )
    assert "tier_entries=%zu tier_parts=%u" in source, (
        "sqlite workload: missing page-tier loader coverage diagnostic"
    )
    assert "fiemap_files=%u fallback_files=%u fiemap_extents=%u" in source, (
        "sqlite workload: missing FIEMAP/fallback coverage diagnostic"
    )
    assert "file_lpn_global_values=%llu" in source and "file_lpn_global_unique=%llu" in source and "file_lpn_cross_file_dups=%llu" in source, (
        "sqlite workload: missing cross-file LPN duplicate diagnostic"
    )


def assert_build_and_workload_variant_mapping() -> None:
    build = read_source("build_die.sh")
    workload = (
        ROOT.parent /
        "evaluation/sqlite_fragment_die_test1_tablefile_pageflow_fileparallel_fullscan.sh"
    ).read_text()

    for variant, source in (
        ("die_latency1_sb", "conv_ftl_latency1_superblock.c"),
        ("die_latency2_sb", "conv_ftl_latency2_superblock.c"),
        ("die_latency3_sb", "conv_ftl_latency3_superblock.c"),
        ("die_latency1_norp_sb", "conv_ftl_latency1_norp_superblock.c"),
        ("die_latency2_norp_sb", "conv_ftl_latency2_norp_superblock.c"),
        ("die_latency3_norp_sb", "conv_ftl_latency3_norp_superblock.c"),
        ("die_latency1_qlc_all_norp_sb", "conv_ftl_latency1_qlc_all_norp_superblock.c"),
        ("die_latency2_qlc_all_norp_sb", "conv_ftl_latency2_qlc_all_norp_superblock.c"),
        ("die_latency3_qlc_all_norp_sb", "conv_ftl_latency3_qlc_all_norp_superblock.c"),
    ):
        assert variant in build and source in build, (
            f"build_die.sh: {variant} must map to {source}"
        )
    assert "bash build_die.sh $VARIANTS" in workload, (
        "sqlite workload: FORCE_REBUILD path must rebuild requested variants"
    )
    assert 'load_die_module "$variant"' in workload, (
        "sqlite workload: each run must load the requested variant module"
    )
    assert 'python3 "$SCRIPT_DIR/sqlite/analyze_latency_run.py" "$init_txt"' in workload, (
        "sqlite workload: latency logs must be auto-analyzed when enabled"
    )


def assert_fio_latency_mixed_four_variant_matrix() -> None:
    workload = (ROOT.parent / "evaluation/fio_fragment_die_latency_mixed.sh").read_text()
    docs = (ROOT.parent / "evaluation/FIO_LATENCY_MIXED_RUNS.md").read_text()
    expected = (
        "die_latency1_qlc_all_norp_sb "
        "die_latency2_qlc_all_norp_sb "
        "die_latency3_qlc_all_norp_sb "
        "die_latency1_norp_sb"
    )

    assert f'VARIANTS="${{VARIANTS:-{expected}}}"' in workload, (
        "fio mixed runner: default variants must match the current four-case matrix"
    )
    expected_flags_fn = extract_function(workload, "expected_compile_flags_for_variant()")
    assert re.search(
        r"die_latency1_norp_sb\).*?"
        r"compile_qlc_hotcold_enabled 0.*?"
        r"compile_qlc_rebalance_enabled 0.*?"
        r"compile_test_phase_qlc_rebalance_enabled 0",
        expected_flags_fn,
        flags=re.S,
    ), (
        "fio mixed runner: strict compile flags must treat die_latency1_norp_sb "
        "as hot/cold off and QLC rebalance off"
    )
    assert expected in docs, (
        "fio mixed docs: recommended commands must use the current four-case matrix"
    )


def assert_fio_latency_mixed_result_naming_and_drain_bundle() -> None:
    workload = (ROOT.parent / "evaluation/fio_fragment_die_latency_mixed.sh").read_text()
    run_body = extract_function(workload, "run_fio_one_case()")
    drain_body = run_body[run_body.index('if [[ "$FIO_DRAIN_AFTER_MEASURE" == "1" ]]; then'):]

    assert 'tag="fio_mixed_${variant}_$(dist_tag "$access_dist")_rw$(ratio_tag "$ratio")_bg${FIO_GC_NAND_TIMING}_r${repeat_index}"' in run_body, (
        "fio mixed runner: result tag must include a stable repeat index"
    )
    assert "date +%Y%m%d_%H%M%S" not in run_body, (
        "fio mixed runner: per-case output filenames must stay timestamp-free"
    )
    assert "cleanup_case_outputs \"$tag\" \"$out_dir\" \"$log_prefix\"" in run_body, (
        "fio mixed runner: no-timestamp reruns must clear stale same-case outputs"
    )
    assert 'FIO_REPEAT_COUNT="${FIO_REPEAT_COUNT:-3}"' in workload, (
        "fio mixed runner: acceptance runs must default to three repetitions"
    )
    assert 'FIO_RANDSEED_BASE="${FIO_RANDSEED_BASE:-240622}"' in workload, (
        "fio mixed runner: repetitions need deterministic per-trial seeds"
    )
    assert 'echo "randseed=$((FIO_RANDSEED_BASE + repeat_index))"' in workload, (
        "fio mixed runner: each variant in a trial must share the same seed"
    )
    assert "--model nonpreemptive_submit_gate" in workload, (
        "fio mixed runner: manifest must require the current scheduler model"
    )
    sqlite_workload = (
        ROOT.parent /
        "evaluation/sqlite_fragment_die_test1_tablefile_pageflow_fileparallel_fullscan.sh"
    ).read_text()
    assert "--model nonpreemptive_submit_gate" in sqlite_workload, (
        "SQLite runner: manifest must require the current scheduler model"
    )
    assert "write_foreground_post_drain_aggregate \"$tag\" \"$out_dir\"" in drain_body, (
        "fio mixed runner: post-drain must emit one foreground/post-drain aggregate table"
    )
    assert "write_post_drain_bundle \"$tag\" \"$out_dir\"" in drain_body, (
        "fio mixed runner: post-drain stats must also be bundled into one file"
    )


def assert_read_priority_tail_model() -> None:
    source = ROOT / "tests/read_priority_tail_model_test.c"

    with tempfile.TemporaryDirectory() as tmpdir:
        binary = Path(tmpdir) / "read_priority_tail_model_test"
        subprocess.run(
            [
                "cc",
                "-std=c99",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(source),
                "-o",
                str(binary),
            ],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        result = subprocess.run(
            [str(binary)],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    assert "PASS non-preemptive tail model" in result.stdout, (
        "read-priority tail model did not report success"
    )


def assert_read_priority_queue_model() -> None:
    source = ROOT / "tests/read_priority_queue_model.py"
    result = subprocess.run(
        ["python3", str(source)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    for expected in [
        "submitted LP work is non-preemptible",
        "[submitted_active_plus_queued_lp]",
        "[hypothetical_controller_queue]",
	    "start=200 end=220 request_latency=210",
        "start=100 end=120 request_latency=110",
        "if the read batch is scheduled first, bg follows the HP tail",
        "same-issue reads still serialize behind earlier HP reads, not the LP tail",
        "gate reduces LP tail shifting and background interference during read bursts",
        "with many queued reads, HP read/read tail dominates",
    ]:
        assert expected in result.stdout, (
            "read_priority_queue_model.py: missing scenario conclusion "
            f"{expected!r}"
        )


def assert_request_latency_histogram_is_reproducible() -> None:
    header = read_source("conv_ftl.h")
    analyzer = (ROOT.parent / "evaluation/sqlite/analyze_latency_run.py").read_text()

    assert "read_req_latency_hist_bin_%02u_upper_ns" in header, (
        "conv_ftl.h: request histogram must expose every bucket boundary"
    )
    assert "read_req_latency_hist_bin_%02u_count" in header, (
        "conv_ftl.h: request histogram must expose every bucket count"
    )
    assert "read_priority_model nonpreemptive_submit_gate" in header, (
        "conv_ftl.h: diagnostics must label the scheduler model semantics"
    )
    assert "read_req_histogram_status" in analyzer, (
        "analyzer must validate that histogram samples equal request samples"
    )
    for name in (
        "conv_ftl_latency1_norp_superblock.c",
        "conv_ftl_latency2_norp_superblock.c",
        "conv_ftl_latency3_norp_superblock.c",
    ):
        source = read_source(name)
        assert "#define NVMEV_TEST_PHASE_READ_REQ_LATENCY_STATS 1" in source, (
            f"{name}: control variant must enable request-latency statistics"
        )


def assert_sqlite_analyzer_verdict_cases() -> None:
    source = ROOT.parent / "evaluation/sqlite/test_analyze_latency_verdict.py"
    result = subprocess.run(
        ["python3", str(source)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert "PASS analyze_latency_run verdict cases" in result.stdout, (
        "analyze_latency_run verdict case tests did not report success"
    )


def assert_acceptance_evaluator_cases() -> None:
    source = ROOT.parent / "evaluation/test_evaluate_latency_acceptance.py"
    result = subprocess.run(
        ["python3", str(source)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert "PASS latency acceptance evaluator" in result.stdout, (
        "latency acceptance evaluator tests did not report success"
    )


def assert_fio_summary_cases() -> None:
    source = ROOT.parent / "evaluation/test_fio_summarize_latency_mixed.py"
    result = subprocess.run(
        ["python3", str(source)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert "PASS fio summary discovery and aggregation" in result.stdout, (
        "fio summary tests did not report success"
    )


def main() -> int:
    assert_latency1_baseline()
    assert_slc_gc_eligibility_aligned()
    assert_latency_qlc_ablation_wrappers()
    assert_qlc_all_norp_wrappers_show_latency_policy()
    assert_latency_variant("conv_ftl_latency3_superblock.c")
    assert_latency_variant("conv_ftl_latency2_superblock.c")
    assert_read_priority_window_uses_simulated_time("conv_ftl_latency3_superblock.c")
    assert_read_priority_window_uses_simulated_time("conv_ftl_latency2_superblock.c")
    assert_read_lp_bypass_diagnostics()
    assert_resource_wait_attribution()
    assert_read_overlap_and_issue_diagnostics()
    assert_latency2_yield_gates_v2_dispatcher()
    assert_io_completion_guard()
    assert_io_completion_queue_orders_by_target()
    assert_ssd_timing_tail_locks()
    assert_latency_qlc_ablation_docs_explain_competition()
    assert_cpu_and_workqueue_competition_model()
    assert_ssd_hp_lp_time_model("ssd.c")
    assert_ssd_hp_lp_time_model("ssd_die.c")
    assert_latency3_emergency_progress()
    assert_channel_scan_cap()
    assert_qlc_allocation_reserves_pages()
    assert_page_tier_accounting_consistent()
    assert_qlc_live_accounting_consistent()
    assert_sqlite_preheat_diagnostics()
    assert_build_and_workload_variant_mapping()
    assert_fio_latency_mixed_four_variant_matrix()
    assert_fio_latency_mixed_result_naming_and_drain_bundle()
    assert_read_priority_tail_model()
    assert_read_priority_queue_model()
    assert_request_latency_histogram_is_reproducible()
    assert_sqlite_analyzer_verdict_cases()
    assert_acceptance_evaluator_cases()
    assert_fio_summary_cases()
    print("PASS latency static invariants")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
