#!/usr/bin/env python3
"""Regression checks for analyze_latency_run.py."""

from __future__ import annotations

import tempfile
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path

import analyze_latency_run


def parse_text(text: str):
    with tempfile.TemporaryDirectory() as tmpdir:
        path = Path(tmpdir) / "sample.log"
        path.write_text(text)
        return analyze_latency_run.parse_log(path)


def test_cold_read_bypass_takes_precedence() -> None:
    data = parse_text(
        """
[sqlite_init] tag=sample cold_full_read=1.0s
[sqlite_init] config init_drop_cache_each_read=1
[sqlite_init] read_plan total_reads=2 active_tables=1 top_tables=table0:2
[sqlite_init] read_event=1 completed tables=1 scan_ops=2 sqlite_rows_seen=8 drop_cache_each_read=1 elapsed=0.1s
[sqlite_init] read_event=1 ftl_heat global_read_sum_delta=8 host_read_nand_delta=8 global_read_sum=8 global_valid_pg_cnt=8 host_read_nand_ops=8
[sqlite_init] bg_nand phase=cold_read busy_ns=1 read_ns=1 write_ns=0 erase_ns=0 read_ops=1 write_ops=0 erase_ops=0 read_prio_bypass_ops=0 read_prio_bypass_ns=0 read_prio_ch_bypass_ops=0 read_prio_ch_bypass_ns=0 read_prio_pcie_bypass_ops=0 read_prio_pcie_bypass_ns=0 lp_host_write_ops=0 lp_host_write_ns=0
[sqlite_init] bg_nand phase=total busy_ns=1 read_ns=1 write_ns=0 erase_ns=0 read_ops=1 write_ops=0 erase_ops=0 read_prio_bypass_ops=10 read_prio_bypass_ns=100 read_prio_ch_bypass_ops=0 read_prio_ch_bypass_ns=0 read_prio_pcie_bypass_ops=0 read_prio_pcie_bypass_ns=0 lp_host_write_ops=0 lp_host_write_ns=0
read_requests 8
slc_to_qlc_nand_writes 1
repromote_nand_writes 1
"""
    )
    notes = analyze_latency_run.verdict(data)
    assert "preheat_sql=ok" in notes
    assert "preheat_ftl=ok" in notes
    assert "read_prio_bypass=zero" in notes
    assert "read_prio_bypass=ok" not in notes


def test_total_bypass_used_when_cold_read_missing() -> None:
    data = parse_text(
        """
[sqlite_init] tag=sample cold_full_read=1.0s
[sqlite_init] read_event=1 completed tables=1 scan_ops=1 sqlite_rows_seen=4 drop_cache_each_read=1 elapsed=0.1s
[sqlite_init] read_event=1 ftl_heat global_read_sum_delta=4 host_read_nand_delta=4 global_read_sum=4 global_valid_pg_cnt=4 host_read_nand_ops=4
[sqlite_init] bg_nand phase=total busy_ns=1 read_ns=1 write_ns=0 erase_ns=0 read_ops=1 write_ops=0 erase_ops=0 read_prio_bypass_ops=3 read_prio_bypass_ns=30 read_prio_ch_bypass_ops=0 read_prio_ch_bypass_ns=0 read_prio_pcie_bypass_ops=0 read_prio_pcie_bypass_ns=0 lp_host_write_ops=0 lp_host_write_ns=0
"""
    )
    notes = analyze_latency_run.verdict(data)
    assert "read_prio_bypass=ok" in notes


def test_channel_bypass_counts_as_read_priority() -> None:
    data = parse_text(
        """
[sqlite_init] tag=sample cold_full_read=1.0s
[sqlite_init] bg_nand phase=cold_read busy_ns=1 read_ns=1 write_ns=0 erase_ns=0 read_ops=1 write_ops=0 erase_ops=0 read_prio_bypass_ops=0 read_prio_bypass_ns=0 read_prio_ch_bypass_ops=7 read_prio_ch_bypass_ns=70 read_prio_pcie_bypass_ops=0 read_prio_pcie_bypass_ns=0 lp_host_write_ops=0 lp_host_write_ns=0
"""
    )
    notes = analyze_latency_run.verdict(data)
    assert "read_prio_bypass=ok" in notes


def test_pcie_bypass_counts_as_read_priority() -> None:
    data = parse_text(
        """
[sqlite_init] tag=sample cold_full_read=1.0s
[sqlite_init] bg_nand phase=total busy_ns=1 read_ns=1 write_ns=0 erase_ns=0 read_ops=1 write_ops=0 erase_ops=0 read_prio_bypass_ops=0 read_prio_bypass_ns=0 read_prio_ch_bypass_ops=0 read_prio_ch_bypass_ns=0 read_prio_pcie_bypass_ops=5 read_prio_pcie_bypass_ns=50 lp_host_write_ops=0 lp_host_write_ns=0
"""
    )
    notes = analyze_latency_run.verdict(data)
    assert "read_prio_bypass=ok" in notes


def test_kernel_failure_signals_are_reported() -> None:
    data = parse_text(
        """
[sqlite_init] tag=sample cold_full_read=1.0s
[kernel_tail_status] status=captured start_line=10 end_line=20 matches=4
NVMeVirt: [chmodel_request] No free entry 0x1 0x2 0x3
NVMeVirt: SLOW_PATH: nvmev_proc_io_sq took 1000000000 ns! Processed 4 reqs.
INFO: task sqlite_append_d:6284 blocked for more than 120 seconds.
NVMeVirt: proc queue full sqid=1 entry=2 next=3 free_seq=2
NVMeVirt: writeback proc queue full; releasing 4096 bytes immediately to avoid write-buffer leak
NVMeVirt: [chmodel_request] credit scan capped req=0x1 cur=0x2 offs=0x3 valid_len=0x4 remaining=5
NVMeVirt: [chmodel_request] credit horizon overflow req=0x1 cur=0x2 offs=0x3 valid_len=0x4
"""
    )
    signals = data["kernel_signals"]
    assert signals["no_free_entry"] == 1
    assert signals["hung_task"] == 1
    assert signals["slow_proc_io_sq"] == 1
    assert signals["proc_queue_full"] == 1
    assert signals["writeback_queue_full"] == 1
    assert signals["credit_scan_capped"] == 1
    assert signals["credit_horizon_overflow"] == 1
    notes = analyze_latency_run.verdict(data)
    assert "io_completion=bad(no_free_entry=1,hung_task=1)" in notes
    assert "io_slow_path=warn(proc_io_sq=1,chmodel=0)" in notes
    assert "io_queue=warn(proc_full=1,writeback_full=1)" in notes
    assert "channel_overflow=warn(scan_capped=1,horizon_overflow=1)" in notes


def test_kernel_tail_unavailable_is_unknown() -> None:
    data = parse_text(
        """
[sqlite_init] tag=sample cold_full_read=1.0s
[kernel_tail_status] status=unavailable start_line=0 end_line=0 matches=0
"""
    )
    notes = analyze_latency_run.verdict(data)
    assert "io_completion=unknown(kernel_tail=unavailable)" in notes
    assert "io_completion=ok" not in notes


def test_kernel_tail_rotated_is_unknown() -> None:
    data = parse_text(
        """
[sqlite_init] tag=sample cold_full_read=1.0s
[kernel_tail_status] status=rotated start_line=1000 end_line=10 matches=0
"""
    )
    notes = analyze_latency_run.verdict(data)
    assert "io_completion=unknown(kernel_tail=rotated)" in notes
    assert "io_completion=ok" not in notes


def test_captured_kernel_tail_without_failures_is_ok() -> None:
    data = parse_text(
        """
[sqlite_init] tag=sample cold_full_read=1.0s
[kernel_tail_status] status=captured mode=time start_line=100 end_line=120 start_boot_s=123.45 matches=0
"""
    )
    notes = analyze_latency_run.verdict(data)
    assert "io_completion=ok" in notes
    assert not any(note.startswith("io_completion=unknown") for note in notes)


def test_write_buffer_allocation_failure_is_bad_completion() -> None:
    data = parse_text(
        """
[sqlite_init] tag=sample cold_full_read=1.0s
[kernel_tail_status] status=captured mode=time start_line=100 end_line=120 start_boot_s=123.45 matches=0
NVMeVirt: write buffer allocation failed after 100 retries (need=262144)
"""
    )
    signals = data["kernel_signals"]
    assert signals["write_buffer_alloc_failed"] == 1
    notes = analyze_latency_run.verdict(data)
    assert "io_completion=bad(write_buffer_alloc_failed=1)" in notes


def test_write_buffer_waiting_is_pressure_warning() -> None:
    data = parse_text(
        """
[sqlite_init] tag=sample cold_full_read=1.0s
[kernel_tail_status] status=captured mode=time start_line=100 end_line=120 start_boot_s=123.45 matches=0
NVMeVirt: write buffer allocation waiting retry=100 need=262144 size=67108864
"""
    )
    signals = data["kernel_signals"]
    assert signals["write_buffer_alloc_waiting"] == 1
    notes = analyze_latency_run.verdict(data)
    assert "io_completion=ok" in notes
    assert "write_buffer_pressure=warn(waiting=1)" in notes


def test_missing_bypass_fields_print_as_missing() -> None:
    data = parse_text(
        """
[sqlite_init] tag=sample cold_full_read=1.0s
[sqlite_init] bg_nand phase=cold_read busy_ns=1 read_ns=1 write_ns=0 erase_ns=0 read_ops=1 write_ops=0 erase_ops=0
[kernel_tail_status] status=captured mode=time start_line=100 end_line=120 start_boot_s=123.45 matches=0
"""
    )
    out = StringIO()
    with redirect_stdout(out):
        analyze_latency_run.print_summary(data)
    text = out.getvalue()
    assert "bypass_ops=missing" in text
    assert "read_prio_bypass=missing" in text


def test_latency2_v2_inactive_verdict() -> None:
    data = parse_text(
        """
[sqlite_init] tag=sample cold_full_read=1.0s
[kernel_tail_status] status=captured mode=time start_line=100 end_line=120 start_boot_s=123.45 matches=0
maint_v2_tasks_done 0
maint_v2_skip_pct 100
"""
    )
    notes = analyze_latency_run.verdict(data)
    assert "latency2_v2=inactive(skip_pct=100)" in notes


def test_latency2_v2_active_verdict() -> None:
    data = parse_text(
        """
[sqlite_init] tag=sample cold_full_read=1.0s
[kernel_tail_status] status=captured mode=time start_line=100 end_line=120 start_boot_s=123.45 matches=0
maint_v2_tasks_done 42
maint_v2_skip_pct 80
maint_v2_no_slack_skips 5
maint_v2_ch_busy_skips 9
maint_v2_demand_skips 3
maint_v2_tasks_requeued 7
maint_v2_no_progress_runs 2
"""
    )
    notes = analyze_latency_run.verdict(data)
    assert "latency2_v2=active(tasks_done=42,skip_pct=80)" in notes
    assert "latency2_v2_detail=dominant_skip(ch_busy=9,requeued=7,no_progress=2)" in notes


def test_latency3_runtime_pressure_verdict() -> None:
    data = parse_text(
        """
[sqlite_init] tag=sample cold_full_read=1.0s
[sqlite_init] bg_nand phase=cold_read busy_ns=1 read_ns=1 write_ns=1 erase_ns=0 read_ops=1 write_ops=1 erase_ops=0 read_prio_bypass_ops=2 read_prio_bypass_ns=20 read_prio_ch_bypass_ops=3 read_prio_ch_bypass_ns=30 read_prio_pcie_bypass_ops=0 read_prio_pcie_bypass_ns=0 lp_host_write_ops=0 lp_host_write_ns=0
read_requests 1000
read_bg_conflicts 25
read_priority_yields 10
qlc_repromote_pages 250
host_read_qlc_nand_ops 300
slc_to_qlc_nand_writes 800
repromote_nand_writes 600
hard_no_victim_count 7
test_phase_guard_read_reqs_config 256
test_phase_recent_guard_skips 11
test_phase_recent_guard_forced 2
"""
    )
    notes = analyze_latency_run.verdict(data)
    assert "internal_writes_per_read=1.400" in notes
    assert "repromote_pages_per_read=0.250" in notes
    assert "read_bg_conflict_rate=0.025" in notes
    assert "qlc_read_rate=0.300" in notes
    assert "background_write_pressure=high(iw/read=1.400,repromote/read=0.250)" in notes
    assert "repromotion_bottleneck=high(pages/read=0.250)" in notes
    assert "hard_pressure=present(no_victim=7)" in notes
    assert "read_priority_runtime=active(bypass_ops=5,bypass_ns=50,read_bg_conflicts=25)" in notes
    assert "read_priority_masked=likely(background_write_pressure=high)" in notes
    assert "read_priority_force_progress=not_observed(yields=10,forced=0)" in notes
    assert "recent_write_guard=skips(11,forced=2,window_cfg=256)" in notes


def test_latency3_yield_only_verdict() -> None:
    data = parse_text(
        """
[sqlite_init] tag=sample cold_full_read=1.0s
[sqlite_init] bg_nand phase=cold_read busy_ns=1 read_ns=1 write_ns=0 erase_ns=0 read_ops=1 write_ops=0 erase_ops=0 read_prio_bypass_ops=0 read_prio_bypass_ns=0 read_prio_ch_bypass_ops=0 read_prio_ch_bypass_ns=0 read_prio_pcie_bypass_ops=0 read_prio_pcie_bypass_ns=0 lp_host_write_ops=0 lp_host_write_ns=0
read_requests 1000
read_priority_yields 17
qlc_repromote_pages 0
slc_to_qlc_nand_writes 0
repromote_nand_writes 0
"""
    )
    notes = analyze_latency_run.verdict(data)
    assert "read_prio_bypass=zero" in notes
    assert "background_write_pressure=low" in notes
    assert "read_priority_runtime=yield_only(yields=17,bypass_ops=0)" in notes
    assert "read_priority_force_progress=not_observed(yields=17,forced=0)" in notes


def test_read_priority_forced_progress_active_verdict() -> None:
    data = parse_text(
        """
[sqlite_init] tag=sample cold_full_read=1.0s
[sqlite_init] bg_nand phase=cold_read busy_ns=1 read_ns=1 write_ns=0 erase_ns=0 read_ops=1 write_ops=0 erase_ops=0 read_prio_bypass_ops=1 read_prio_bypass_ns=10 read_prio_ch_bypass_ops=0 read_prio_ch_bypass_ns=0 read_prio_pcie_bypass_ops=0 read_prio_pcie_bypass_ns=0 lp_host_write_ops=0 lp_host_write_ns=0
read_requests 1000
read_priority_yields 21
read_priority_forced_progress_runs 3
"""
    )
    notes = analyze_latency_run.verdict(data)
    assert "read_priority_force_progress=active(yields=21,forced=3)" in notes


def test_dense_preheat_events_are_reported() -> None:
    data = parse_text(
        """
[sqlite_init] config tables=80 interleave_pages=960 read_ops_per_event=512 init_drop_cache_each_read=0
[sqlite_init] read_event=1 completed tables=80 scan_ops=512 sqlite_rows_seen=1024 drop_cache_each_read=0 elapsed=0.1s
[sqlite_init] read_event=1 ftl_heat global_read_sum_delta=1024 host_read_nand_delta=1024 global_read_sum=1024 global_valid_pg_cnt=2048 host_read_nand_ops=1024
[sqlite_init] heat_epoch_advance phase=init-read-event paths=1 first=/sys/kernel/debug/nvmev/ftl0/heat_epoch
[sqlite_init] tag=sample tables=80 total_rows=256000 read_events=2184 interleaved_read_time=1.0s
"""
    )
    notes = analyze_latency_run.verdict(data)
    assert "preheat_events=too_dense(events=2184,heat_epoch_advances=1,interleave_pages=960)" in notes


def test_coarse_preheat_events_are_ok() -> None:
    data = parse_text(
        """
[sqlite_init] config tables=80 interleave_pages=209715 read_ops_per_event=512 init_drop_cache_each_read=0
[sqlite_init] read_event=1 completed tables=80 scan_ops=512 sqlite_rows_seen=1024 drop_cache_each_read=0 elapsed=0.1s
[sqlite_init] read_event=1 ftl_heat global_read_sum_delta=1024 host_read_nand_delta=1024 global_read_sum=1024 global_valid_pg_cnt=2048 host_read_nand_ops=1024
[sqlite_init] heat_epoch_advance phase=init-read-event paths=1 first=/sys/kernel/debug/nvmev/ftl0/heat_epoch
[sqlite_init] tag=sample tables=80 total_rows=256000 read_events=10 interleaved_read_time=1.0s
"""
    )
    notes = analyze_latency_run.verdict(data)
    assert "preheat_events=ok(events=10,heat_epoch_advances=1)" in notes
    assert not any(note.startswith("preheat_events=too_dense") for note in notes)


def test_read_event_summary_reports_ftl_heat_ratios() -> None:
    data = parse_text(
        """
[sqlite_init] tag=sample cold_full_read=1.0s
[sqlite_init] read_event=1 completed tables=2 scan_ops=4 sqlite_rows_seen=16 drop_cache_each_read=0 elapsed=0.1s
[sqlite_init] read_event=1 ftl_heat global_read_sum_delta=8 host_read_nand_delta=4 global_read_sum=8 global_valid_pg_cnt=32 host_read_nand_ops=4
[sqlite_init] read_event=2 completed tables=2 scan_ops=4 sqlite_rows_seen=16 drop_cache_each_read=0 elapsed=0.1s
[sqlite_init] read_event=2 ftl_heat global_read_sum_delta=16 host_read_nand_delta=8 global_read_sum=24 global_valid_pg_cnt=64 host_read_nand_ops=12
"""
    )
    out = StringIO()
    with redirect_stdout(out):
        analyze_latency_run.print_summary(data)
    text = out.getvalue()
    assert "host_nand_delta=12" in text
    assert "heat_delta=24" in text
    assert "last_valid_pages=64" in text
    assert "nand/scan=1.500" in text
    assert "heat/row=0.750" in text


def test_cold_read_latency_summary_reports_tail_ms() -> None:
    data = parse_text(
        """
[sqlite_init] tag=sample cold_full_read=1.0s
[sqlite_init] config tables=80 total_rows=256000 interleave_pages=209715 read_ops_per_event=512 init_drop_cache_each_read=0
[sqlite_init] cold_extra_append mode=concurrent target_bytes=5368709120 rows_per_table=2048 total_extra_rows=163840
[sqlite_init] tag=sample tables=80 total_rows=419840 read_events=9 interleaved_read_time=10.0s cold_full_read=1.0s cold_full_read_tp=10.0MB/s cold_mode=random-row-concurrent multifile=1
[sqlite_init] cold_read_latency count=100 avg=0.000214000s p50=0.000040000s p95=0.001120000s p99=0.002848000s p999=0.003568000s max=0.065900000s
"""
    )
    out = StringIO()
    with redirect_stdout(out):
        analyze_latency_run.print_summary(data)
    text = out.getvalue()
    assert "cold_read_latency: count=100 avg_ms=0.214" in text
    assert "p95_ms=1.120" in text
    assert "p99_ms=2.848" in text
    assert "p999_ms=3.568" in text
    assert "max_ms=65.900" in text
    assert "workload: random-row-concurrent+append-concurrent-5.0GiB" in text


def test_missing_kernel_tail_is_unknown() -> None:
    data = parse_text(
        """
[sqlite_init] tag=sample cold_full_read=1.0s
"""
    )
    notes = analyze_latency_run.verdict(data)
    assert "io_completion=unknown(kernel_tail=missing)" in notes
    assert "io_completion=ok" not in notes


def test_compare_table_reports_core_latency_metrics() -> None:
    data = parse_text(
        """
[sqlite_init] tag=die_latency3_sb cold_full_read=12.5s
[sqlite_init] config tables=80 total_rows=256000 interleave_pages=209715 read_ops_per_event=512 init_drop_cache_each_read=0
[sqlite_init] cold_extra_append mode=concurrent target_bytes=5368709120 rows_per_table=2048 total_extra_rows=163840
[sqlite_init] cold_read_latency count=1000 avg=0.000500000s p50=0.000035000s p95=0.001680000s p99=0.009310000s p999=0.022000000s max=19.880000000s
[sqlite_init] tag=die_latency3_sb tables=80 total_rows=419840 read_events=9 interleaved_read_time=10.0s cold_full_read=12.5s cold_full_read_tp=10.0MB/s cold_mode=random-row-concurrent multifile=1
[sqlite_init] bg_nand phase=cold_read busy_ns=1 read_ns=1 write_ns=1 erase_ns=0 read_ops=1 write_ops=1 erase_ops=0 read_prio_bypass_ops=2 read_prio_bypass_ns=20 read_prio_ch_bypass_ops=3 read_prio_ch_bypass_ns=30 read_prio_pcie_bypass_ops=0 read_prio_pcie_bypass_ns=0 lp_host_write_ops=1 lp_host_write_ns=10
[kernel_tail_status] status=captured mode=time start_line=100 end_line=120 start_boot_s=123.45 matches=0
read_requests 1000
host_read_qlc_nand_ops 300
qlc_repromote_pages 250
slc_to_qlc_migration_pages 400
slc_to_qlc_nand_writes 800
repromote_nand_writes 600
read_bg_conflicts 25
read_priority_yields 10
read_priority_delayed_requeues 8
read_priority_forced_progress_runs 2
maint_v2_tasks_done 42
maint_v2_skip_pct 80
maint_v2_no_slack_skips 5
maint_v2_ch_busy_skips 9
maint_v2_demand_skips 3
maint_v2_tasks_requeued 7
maint_v2_no_progress_runs 2
"""
    )
    out = StringIO()
    with redirect_stdout(out):
        analyze_latency_run.print_compare([data])
    text = out.getvalue()
    assert "tag\tworkload\tcompare\toutcome\tcold_s\tlat_avg_ms\tlat_p95_ms\tlat_p99_ms\tlat_p999_ms\tlat_max_ms" in text
    assert "\tv2_done\tv2_skip_pct\tv2_no_slack\tv2_ch_busy\tv2_demand\tv2_requeued\tv2_no_progress\t" in text
    assert "die_latency3_sb\trandom-row-concurrent+append-concurrent-5.0GiB\tok\thp_lp_completion_priority_active_masked_preheat_unproven\t12.5\t0.500\t1.680\t9.310\t22.000\t19880.000\t1000\t1000\t419840\t300\t250\t400\t1.400\t0.025\t0.300\t5\t50\t10\t8\t2" in text
    assert "\t42\t80\t5\t9\t3\t7\t2\tlatency2_v2=active(tasks_done=42,skip_pct=80)" in text
    assert "read_priority_runtime=active(bypass_ops=5,bypass_ns=50,read_bg_conflicts=25)" in text
    assert "background_write_pressure=high(iw/read=1.400,repromote/read=0.250)" in text
    assert "repromotion_bottleneck=high(pages/read=0.250)" in text
    assert "read_priority_masked=likely(background_write_pressure=high)" in text
    assert "io_completion=ok" in text


def test_compare_table_marks_workload_mismatch() -> None:
    pure = parse_text(
        """
[sqlite_init] config tables=80 total_rows=256000 interleave_pages=209715 read_ops_per_event=512 init_drop_cache_each_read=0
[sqlite_init] cold_read_latency count=100 avg=0.000200000s p50=0.000030000s p95=0.001000000s p99=0.002000000s p999=0.003000000s max=0.010000000s
[sqlite_init] tag=pure tables=80 total_rows=256000 read_events=9 interleaved_read_time=10.0s cold_full_read=10.0s cold_full_read_tp=10.0MB/s cold_mode=random-row-concurrent multifile=1
read_requests 100
"""
    )
    append = parse_text(
        """
[sqlite_init] config tables=80 total_rows=256000 interleave_pages=209715 read_ops_per_event=512 init_drop_cache_each_read=0
[sqlite_init] cold_extra_append mode=concurrent target_bytes=5368709120 rows_per_table=2048 total_extra_rows=163840
[sqlite_init] cold_read_latency count=100 avg=0.000200000s p50=0.000030000s p95=0.001000000s p99=0.002000000s p999=0.003000000s max=0.010000000s
[sqlite_init] tag=append tables=80 total_rows=419840 read_events=9 interleaved_read_time=10.0s cold_full_read=10.0s cold_full_read_tp=10.0MB/s cold_mode=random-row-concurrent multifile=1
read_requests 100
"""
    )
    out = StringIO()
    with redirect_stdout(out):
        analyze_latency_run.print_compare([pure, append])
    text = out.getvalue()
    assert "pure\trandom-row-concurrent\tref" in text
    assert "append\trandom-row-concurrent+append-concurrent-5.0GiB\tmismatch\tnot_comparable(workload_mismatch)" in text


def test_summary_reports_mechanism_outcome() -> None:
    data = parse_text(
        """
[sqlite_init] tag=die_latency3_sb cold_full_read=1.0s
[sqlite_init] read_event=1 completed tables=1 scan_ops=2 sqlite_rows_seen=8 drop_cache_each_read=1 elapsed=0.1s
[sqlite_init] read_event=1 ftl_heat global_read_sum_delta=8 host_read_nand_delta=8 global_read_sum=8 global_valid_pg_cnt=8 host_read_nand_ops=8
[sqlite_init] bg_nand phase=cold_read busy_ns=1 read_ns=1 write_ns=0 erase_ns=0 read_ops=1 write_ops=0 erase_ops=0 read_prio_bypass_ops=4 read_prio_bypass_ns=40 read_prio_ch_bypass_ops=0 read_prio_ch_bypass_ns=0 read_prio_pcie_bypass_ops=0 read_prio_pcie_bypass_ns=0 lp_host_write_ops=0 lp_host_write_ns=0
[kernel_tail_status] status=captured mode=time start_line=100 end_line=120 start_boot_s=123.45 matches=0
read_requests 8
read_priority_yields 3
read_priority_delayed_requeues 2
read_priority_forced_progress_runs 1
"""
    )
    out = StringIO()
    with redirect_stdout(out):
        analyze_latency_run.print_summary(data)
    text = out.getvalue()
    assert "outcome: hp_lp_completion_priority_active" in text
    assert "read_priority_ctrl: yields=3 delayed_requeues=2 forced_progress=1" in text


def test_latency1_baseline_does_not_hide_missing_preheat() -> None:
    data = parse_text(
        """
[sqlite_init] tag=die_latency1_sb cold_full_read=1.0s
[kernel_tail_status] status=captured mode=time start_line=100 end_line=120 start_boot_s=123.45 matches=0
read_requests 8
"""
    )
    assert analyze_latency_run.experiment_outcome(data) == "preheat_unproven"


def test_latency1_clean_baseline_outcome() -> None:
    data = parse_text(
        """
[sqlite_init] tag=die_latency1_sb cold_full_read=1.0s
[sqlite_init] read_event=1 completed tables=1 scan_ops=2 sqlite_rows_seen=8 drop_cache_each_read=1 elapsed=0.1s
[sqlite_init] read_event=1 ftl_heat global_read_sum_delta=8 host_read_nand_delta=8 global_read_sum=8 global_valid_pg_cnt=8 host_read_nand_ops=8
[kernel_tail_status] status=captured mode=time start_line=100 end_line=120 start_boot_s=123.45 matches=0
read_requests 8
"""
    )
    assert analyze_latency_run.experiment_outcome(data) == "baseline(no_read_priority)"


def main() -> int:
    test_cold_read_bypass_takes_precedence()
    test_total_bypass_used_when_cold_read_missing()
    test_channel_bypass_counts_as_read_priority()
    test_pcie_bypass_counts_as_read_priority()
    test_kernel_failure_signals_are_reported()
    test_kernel_tail_unavailable_is_unknown()
    test_kernel_tail_rotated_is_unknown()
    test_captured_kernel_tail_without_failures_is_ok()
    test_write_buffer_allocation_failure_is_bad_completion()
    test_write_buffer_waiting_is_pressure_warning()
    test_missing_bypass_fields_print_as_missing()
    test_latency2_v2_inactive_verdict()
    test_latency2_v2_active_verdict()
    test_latency3_runtime_pressure_verdict()
    test_latency3_yield_only_verdict()
    test_read_priority_forced_progress_active_verdict()
    test_dense_preheat_events_are_reported()
    test_coarse_preheat_events_are_ok()
    test_read_event_summary_reports_ftl_heat_ratios()
    test_cold_read_latency_summary_reports_tail_ms()
    test_missing_kernel_tail_is_unknown()
    test_compare_table_reports_core_latency_metrics()
    test_compare_table_marks_workload_mismatch()
    test_summary_reports_mechanism_outcome()
    test_latency1_baseline_does_not_hide_missing_preheat()
    test_latency1_clean_baseline_outcome()
    print("PASS analyze_latency_run regression tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
