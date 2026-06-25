#!/usr/bin/env python3
"""Analyze the optimized FIO and SQLite latency experiment outputs.

The script intentionally avoids matplotlib so it can run on the local desktop
Python used in this workspace. It writes CSV summaries, a Markdown report, and
PNG charts using Pillow.
"""

from __future__ import annotations

import csv
import json
import math
import re
from pathlib import Path
from statistics import mean, median
from typing import Any

from PIL import Image, ImageDraw, ImageFont


ROOT = Path("/Users/ladorezr/Desktop/fast_24")
OUT_DIR = ROOT / "evaluation" / "result" / "optimized_fio_sqlite_analysis"

FIO_FILES = {
    "lat1_norp": Path("/Users/ladorezr/fio_mixed_die_latency1_norp_sb_zipf_rw10_1.json"),
    "lat1_qlc": Path("/Users/ladorezr/fio_mixed_die_latency1_qlc_all_norp_sb_zipf_rw10_1.json"),
    "lat2_qlc": Path("/Users/ladorezr/fio_mixed_die_latency2_qlc_all_norp_sb_zipf_rw10_1.json"),
    "lat3_qlc": Path("/Users/ladorezr/fio_mixed_die_latency3_qlc_all_norp_sb_zipf_rw10_1.json"),
}

SQLITE_FILES = {
    "lat1_norp": Path("/Users/ladorezr/sqlite_die_singlefile_pageflow_fileparallel_fullscan_init_die_latency1_norp_sb_full_scan_concurrent_zipf_wp960_cmt_8M_t1.txt"),
    "lat1_qlc": Path("/Users/ladorezr/sqlite_die_singlefile_pageflow_fileparallel_fullscan_init_die_latency1_qlc_all_norp_sb_full_scan_concurrent_zipf_wp960_cmt_8M_t1.txt"),
    "lat2_qlc": Path("/Users/ladorezr/sqlite_die_singlefile_pageflow_fileparallel_fullscan_init_die_latency2_qlc_all_norp_sb_full_scan_concurrent_zipf_wp960_cmt_8M_t1.txt"),
    "lat3_qlc": Path("/Users/ladorezr/sqlite_die_singlefile_pageflow_fileparallel_fullscan_init_die_latency3_qlc_all_norp_sb_full_scan_concurrent_zipf_wp960_cmt_8M_t1.txt"),
}

LABELS = {
    "lat1_norp": "lat1_norp",
    "lat1_qlc": "lat1_qlc",
    "lat2_qlc": "lat2_qlc",
    "lat3_qlc": "lat3_qlc",
}

COLORS = {
    "lat1_norp": (74, 112, 166),
    "lat1_qlc": (82, 153, 116),
    "lat2_qlc": (198, 132, 64),
    "lat3_qlc": (154, 93, 159),
}


def load_font(size: int = 13) -> ImageFont.ImageFont:
    for path in (
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/Library/Fonts/Arial.ttf",
    ):
        try:
            return ImageFont.truetype(path, size)
        except Exception:
            pass
    return ImageFont.load_default()


FONT = load_font(13)
SMALL = load_font(11)
TITLE = load_font(18)


def parse_keyvals(text: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for key, value in re.findall(r"([A-Za-z0-9_./+-]+)=([^,\s)]+)", text):
        values[key] = value
    return values


def pct_ms(op: dict[str, Any], key: str) -> float:
    percentiles = op.get("clat_ns", {}).get("percentile", {})
    return float(percentiles.get(key, 0.0)) / 1_000_000.0


def clat_mean_ms(op: dict[str, Any]) -> float:
    return float(op.get("clat_ns", {}).get("mean", 0.0)) / 1_000_000.0


def parse_fio(label: str, path: Path) -> dict[str, Any]:
    text = path.read_text(errors="replace")
    data = json.loads(text[text.find("{") :])
    jobs = data.get("jobs", [])
    job_map = {job.get("jobname", ""): job for job in jobs}
    read_job = job_map["measure_reads"]
    write_job = job_map["measure_writes"]
    read = read_job["read"]
    write = write_job["write"]

    init_write_bytes = sum(int(job.get("write", {}).get("io_bytes", 0)) for job in jobs if job.get("jobname", "").startswith("init_write_"))
    init_prewarm_bytes = sum(int(job.get("read", {}).get("io_bytes", 0)) for job in jobs if job.get("jobname", "").startswith("init_prewarm_"))
    init_write_s = sum(int(job.get("write", {}).get("runtime", 0)) for job in jobs if job.get("jobname", "").startswith("init_write_")) / 1000.0
    init_prewarm_s = sum(int(job.get("read", {}).get("runtime", 0)) for job in jobs if job.get("jobname", "").startswith("init_prewarm_")) / 1000.0
    errors = sum(1 for job in jobs if int(job.get("error", 0)) != 0)

    row: dict[str, Any] = {
        "label": label,
        "variant": re.sub(r"^fio_mixed_|_zipf_rw10_1$", "", path.stem),
        "file": str(path),
        "fio_version": data.get("fio version", ""),
        "jobs": len(jobs),
        "errors": errors,
        "bs": data.get("global options", {}).get("bs", ""),
        "iodepth": data.get("global options", {}).get("iodepth", ""),
        "direct": data.get("global options", {}).get("direct", ""),
        "ioengine": data.get("global options", {}).get("ioengine", ""),
        "init_write_gib": init_write_bytes / 2**30,
        "init_prewarm_gib": init_prewarm_bytes / 2**30,
        "init_write_s": init_write_s,
        "init_prewarm_s": init_prewarm_s,
        "measure_read_gib": int(read.get("io_bytes", 0)) / 2**30,
        "measure_write_gib": int(write.get("io_bytes", 0)) / 2**30,
        "read_runtime_s": float(read.get("runtime", 0)) / 1000.0,
        "write_runtime_s": float(write.get("runtime", 0)) / 1000.0,
        "read_iops": float(read.get("iops", 0.0)),
        "write_iops": float(write.get("iops", 0.0)),
        "read_mib_s": float(read.get("bw", 0.0)) / 1024.0,
        "write_mib_s": float(write.get("bw", 0.0)) / 1024.0,
        "read_mean_ms": clat_mean_ms(read),
        "read_p50_ms": pct_ms(read, "50.000000"),
        "read_p95_ms": pct_ms(read, "95.000000"),
        "read_p99_ms": pct_ms(read, "99.000000"),
        "read_p999_ms": pct_ms(read, "99.900000"),
        "read_p9999_ms": pct_ms(read, "99.990000"),
        "read_max_s": float(read.get("clat_ns", {}).get("max", 0.0)) / 1_000_000_000.0,
        "write_mean_ms": clat_mean_ms(write),
        "write_p50_ms": pct_ms(write, "50.000000"),
        "write_p95_ms": pct_ms(write, "95.000000"),
        "write_p99_ms": pct_ms(write, "99.000000"),
        "write_p999_ms": pct_ms(write, "99.900000"),
        "write_p9999_ms": pct_ms(write, "99.990000"),
        "write_max_s": float(write.get("clat_ns", {}).get("max", 0.0)) / 1_000_000_000.0,
    }
    row["foreground_runtime_s"] = max(row["read_runtime_s"], row["write_runtime_s"])
    return row


def parse_stats_block(lines: list[str], marker: str) -> dict[str, int]:
    stats: dict[str, int] = {}
    capture = False
    for line in lines:
        if line.strip() == marker:
            capture = True
            continue
        if capture and line.startswith("[") and line.strip() != marker:
            break
        if capture:
            parts = line.split()
            if len(parts) == 2 and re.fullmatch(r"-?[0-9]+", parts[1]):
                stats[parts[0]] = int(parts[1])
    return stats


def parse_sqlite(label: str, path: Path) -> dict[str, Any]:
    text = path.read_text(errors="replace")
    lines = text.splitlines()
    config = {}
    cold_extra = {}
    cold_extra_result = {}
    read_events: list[float] = []
    table_times: list[float] = []
    table_throughputs: list[float] = []
    cold_latency: dict[str, float] = {}
    summary: dict[str, str] = {}
    core: dict[str, str] = {}
    read_priority: dict[str, str] = {}
    kernel_signals: dict[str, str] = {}
    verdict = ""
    outcome = ""

    for line in lines:
        if line.startswith("[sqlite_init] config "):
            config = parse_keyvals(line)
        elif line.startswith("[sqlite_init] cold_extra_append "):
            values = parse_keyvals(line)
            if "read_ratio" in values:
                cold_extra = values
            elif "actual_payload_bytes" in values:
                cold_extra_result = values
        elif "[sqlite_init] read_event=" in line and "elapsed=" in line:
            match = re.search(r"elapsed=([0-9.]+)s", line)
            if match:
                read_events.append(float(match.group(1)))
        elif line.startswith("[sqlite_init] cold_read_latency "):
            cold_latency = {k: float(v.rstrip("s")) for k, v in parse_keyvals(line).items() if k != "count"}
            count_match = re.search(r"count=([0-9]+)", line)
            if count_match:
                cold_latency["count"] = float(count_match.group(1))
        elif line.startswith("[sqlite_cold_table] "):
            match = re.search(r"time=([0-9.]+)s throughput=([0-9.]+)MB/s", line)
            if match:
                table_times.append(float(match.group(1)))
                table_throughputs.append(float(match.group(2)))
        elif line.startswith("cold_full_read_s:"):
            summary["cold_full_read_s"] = line.split(":", 1)[1].strip()
        elif line.startswith("init_events:"):
            summary.update(parse_keyvals(line))
        elif line.startswith("workload:"):
            summary["workload"] = line.split(":", 1)[1].strip()
        elif line.startswith("cold_read_latency:"):
            for key, value in parse_keyvals(line).items():
                if key.endswith("_ms") or key == "count":
                    summary[f"summary_{key}"] = value
        elif line.startswith("core:"):
            core = parse_keyvals(line)
        elif line.startswith("read_priority_ctrl:"):
            read_priority = parse_keyvals(line)
        elif line.startswith("kernel_signals:"):
            kernel_signals = parse_keyvals(line)
        elif line.startswith("outcome:"):
            outcome = line.split(":", 1)[1].strip()
        elif line.startswith("verdict:"):
            verdict = line.split(":", 1)[1].strip()

    stats = parse_stats_block(lines, "[test_phase_stats_aggregate]")
    row: dict[str, Any] = {
        "label": label,
        "variant": re.search(r"init_(die_latency[^_]+(?:_[a-z0-9]+)*_sb)_", path.name).group(1)
        if re.search(r"init_(die_latency[^_]+(?:_[a-z0-9]+)*_sb)_", path.name)
        else label,
        "file": str(path),
        "config_tables": int(config.get("tables", 0) or 0),
        "config_rows": int(config.get("total_rows", 0) or 0),
        "config_interleave_pages": int(config.get("interleave_pages", 0) or 0),
        "config_reads_per_event": int(config.get("read_ops_per_event", 0) or 0),
        "cold_extra_target_gib": int(cold_extra.get("target_bytes", 0) or 0) / 2**30,
        "cold_extra_read_ratio": float(cold_extra.get("read_ratio", 0.0) or 0.0),
        "cold_extra_actual_gib": int(cold_extra_result.get("actual_payload_bytes", 0) or 0) / 2**30,
        "cold_extra_append_time_s": float(str(cold_extra_result.get("append_time", "0")).rstrip("s") or 0.0),
        "read_events": len(read_events),
        "read_event_last_elapsed_s": read_events[-1] if read_events else 0.0,
        "cold_full_read_s": float(summary.get("cold_full_read_s", 0.0) or 0.0),
        "cold_latency_count": int(float(summary.get("summary_count", cold_latency.get("count", 0)) or 0)),
        "cold_avg_ms": float(summary.get("summary_avg_ms", cold_latency.get("avg", 0) * 1000.0) or 0.0),
        "cold_p50_ms": float(summary.get("summary_p50_ms", cold_latency.get("p50", 0) * 1000.0) or 0.0),
        "cold_p95_ms": float(summary.get("summary_p95_ms", cold_latency.get("p95", 0) * 1000.0) or 0.0),
        "cold_p99_ms": float(summary.get("summary_p99_ms", cold_latency.get("p99", 0) * 1000.0) or 0.0),
        "cold_p999_ms": float(summary.get("summary_p999_ms", cold_latency.get("p999", 0) * 1000.0) or 0.0),
        "cold_max_ms": float(summary.get("summary_max_ms", cold_latency.get("max", 0) * 1000.0) or 0.0),
        "table_count": len(table_times),
        "table_time_avg_s": mean(table_times) if table_times else 0.0,
        "table_time_p50_s": median(table_times) if table_times else 0.0,
        "table_time_p95_s": sorted(table_times)[int(math.ceil(0.95 * len(table_times))) - 1] if table_times else 0.0,
        "table_time_max_s": max(table_times) if table_times else 0.0,
        "table_throughput_avg_mb_s": mean(table_throughputs) if table_throughputs else 0.0,
        "reads": int(core.get("reads", stats.get("read_requests", 0)) or 0),
        "host_read_nand_ops": int(stats.get("host_read_nand_ops", 0)),
        "slc_reads": int(core.get("slc_reads", stats.get("host_read_slc_nand_ops", 0)) or 0),
        "qlc_reads": int(core.get("qlc_reads", stats.get("host_read_qlc_nand_ops", 0)) or 0),
        "host_write_nand_ops": int(stats.get("host_write_nand_ops", 0)),
        "slc_to_qlc_pages": int(core.get("slc_to_qlc_pages", stats.get("slc_to_qlc_migration_pages", 0)) or 0),
        "internal_write_pages": int(core.get("internal_write_pages", stats.get("internal_write_pages_est", 0)) or 0),
        "iw_per_read": float(core.get("iw/read", 0.0) or 0.0),
        "read_bg_conflicts": int(core.get("read_bg_conflicts", stats.get("read_bg_conflicts", 0)) or 0),
        "read_die_conflicts": int(core.get("read_die_conflicts", stats.get("read_die_conflicts", 0)) or 0),
        "read_die_wait_ms": int(stats.get("read_die_wait_ns", stats.get("test_phase_read_die_wait_ns", 0))) / 1_000_000.0,
        "hard_no_victim": int(core.get("hard_no_victim", 0) or 0),
        "rp_yields": int(read_priority.get("yields", stats.get("test_phase_read_priority_yields", 0)) or 0),
        "rp_delayed_requeues": int(read_priority.get("delayed_requeues", stats.get("test_phase_read_priority_delayed_requeues", 0)) or 0),
        "rp_forced_progress": int(read_priority.get("forced_progress", stats.get("test_phase_read_priority_forced_progress_runs", 0)) or 0),
        "recent_guard_skips": int(stats.get("test_phase_recent_guard_skips", 0)),
        "slc_gc_erase_ops": int(stats.get("slc_sb_gc_erase_ops", 0)),
        "kernel_slow_proc_io_sq": int(kernel_signals.get("slow_proc_io_sq", 0) or 0),
        "kernel_slow_chmodel": int(kernel_signals.get("slow_chmodel", 0) or 0),
        "soft_lockup_count": text.count("soft lockup"),
        "page_not_free_count": text.count("Page not FREE"),
        "blk_vpc_zero_count": text.count("blk->vpc already 0"),
        "outcome": outcome,
        "verdict": verdict,
    }
    row["qlc_read_rate"] = row["qlc_reads"] / row["host_read_nand_ops"] if row["host_read_nand_ops"] else 0.0
    row["_table_times"] = table_times
    return row


def rel_improve(base: float, value: float) -> float:
    if base == 0:
        return 0.0
    return (base - value) / base * 100.0


def write_csv(path: Path, rows: list[dict[str, Any]], exclude_private: bool = True) -> None:
    keys: list[str] = []
    for row in rows:
        for key in row:
            if exclude_private and key.startswith("_"):
                continue
            if key not in keys:
                keys.append(key)
    with path.open("w", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=keys)
        writer.writeheader()
        for row in rows:
            writer.writerow({k: row.get(k, "") for k in keys})


def canvas(width: int, height: int, title: str) -> tuple[Image.Image, ImageDraw.ImageDraw]:
    image = Image.new("RGB", (width, height), (255, 255, 255))
    draw = ImageDraw.Draw(image)
    draw.text((24, 18), title, fill=(28, 35, 45), font=TITLE)
    return image, draw


def nice_max(value: float) -> float:
    if value <= 0:
        return 1.0
    exp = math.floor(math.log10(value))
    frac = value / (10**exp)
    if frac <= 1:
        nice = 1
    elif frac <= 2:
        nice = 2
    elif frac <= 5:
        nice = 5
    else:
        nice = 10
    return nice * 10**exp


def draw_axes(draw: ImageDraw.ImageDraw, box: tuple[int, int, int, int], ymax: float, ylabel: str, log: bool = False) -> None:
    left, top, right, bottom = box
    draw.line((left, bottom, right, bottom), fill=(80, 86, 95), width=2)
    draw.line((left, top, left, bottom), fill=(80, 86, 95), width=2)
    draw.text((left, bottom + 32), ylabel, fill=(55, 60, 68), font=SMALL)
    ticks = [0, 0.25, 0.5, 0.75, 1.0]
    for tick in ticks:
        y = bottom - tick * (bottom - top)
        draw.line((left - 4, y, right, y), fill=(225, 228, 232), width=1)
        if log:
            value = 10 ** (tick * math.log10(max(ymax, 1)))
            label = f"{value:.1f}" if value < 10 else f"{value:.0f}"
        else:
            value = tick * ymax
            label = f"{value:.1f}" if ymax < 20 else f"{value:.0f}"
        draw.text((left - 54, y - 7), label, fill=(70, 76, 84), font=SMALL)


def bar_chart(path: Path, title: str, series: dict[str, list[float]], categories: list[str], ylabel: str, log: bool = False) -> None:
    width, height = 1180, 680
    image, draw = canvas(width, height, title)
    box = (92, 82, width - 44, height - 120)
    vals = [v for values in series.values() for v in values]
    ymax = nice_max(max(vals) * 1.1 if vals else 1.0)
    draw_axes(draw, box, ymax, ylabel, log=log)

    left, top, right, bottom = box
    group_w = (right - left) / len(categories)
    names = list(series.keys())
    bar_w = max(8, group_w / (len(names) + 1.5))
    for ci, category in enumerate(categories):
        gx = left + ci * group_w
        draw.text((gx + group_w * 0.15, bottom + 8), category, fill=(45, 50, 58), font=SMALL)
        for si, name in enumerate(names):
            value = series[name][ci]
            if log:
                scaled = math.log10(max(value, 1e-6)) / math.log10(max(ymax, 1.000001))
            else:
                scaled = value / ymax
            x0 = gx + (si + 0.25) * bar_w
            x1 = x0 + bar_w * 0.82
            y0 = bottom - max(0, min(1, scaled)) * (bottom - top)
            draw.rectangle((x0, y0, x1, bottom), fill=COLORS.get(name, (100, 120, 150)))
            label = f"{value:.1f}" if value < 100 else f"{value:.0f}"
            draw.text((x0 - 2, max(top, y0 - 16)), label, fill=(35, 40, 48), font=SMALL)
    lx = left
    ly = height - 62
    for name in names:
        draw.rectangle((lx, ly, lx + 16, ly + 16), fill=COLORS.get(name, (100, 120, 150)))
        draw.text((lx + 22, ly - 1), LABELS.get(name, name), fill=(35, 40, 48), font=SMALL)
        lx += 150
    image.save(path)


def histogram_grid(path: Path, title: str, data: dict[str, list[float]], xlabel: str, bins: int = 16) -> None:
    width, height = 1180, 760
    image, draw = canvas(width, height, title)
    values = [v for arr in data.values() for v in arr]
    vmin, vmax = min(values), max(values)
    span = vmax - vmin or 1.0
    bin_edges = [vmin + i * span / bins for i in range(bins + 1)]
    panels = {
        "lat1_norp": (70, 92, 555, 355),
        "lat1_qlc": (630, 92, 1115, 355),
        "lat2_qlc": (70, 445, 555, 708),
        "lat3_qlc": (630, 445, 1115, 708),
    }
    for name, box in panels.items():
        arr = data.get(name, [])
        counts = [0] * bins
        for value in arr:
            idx = min(bins - 1, int((value - vmin) / span * bins))
            counts[idx] += 1
        ymax = max(counts) or 1
        left, top, right, bottom = box
        draw.text((left, top - 26), f"{LABELS[name]}  n={len(arr)}", fill=(35, 40, 48), font=FONT)
        draw.line((left, bottom, right, bottom), fill=(80, 86, 95), width=2)
        draw.line((left, top, left, bottom), fill=(80, 86, 95), width=2)
        bar_w = (right - left) / bins
        for i, count in enumerate(counts):
            x0 = left + i * bar_w + 1
            x1 = left + (i + 1) * bar_w - 1
            y0 = bottom - count / ymax * (bottom - top)
            draw.rectangle((x0, y0, x1, bottom), fill=COLORS[name])
        for tick in (vmin, (vmin + vmax) / 2, vmax):
            x = left + (tick - vmin) / span * (right - left)
            draw.line((x, bottom, x, bottom + 4), fill=(80, 86, 95), width=1)
            draw.text((x - 24, bottom + 8), f"{tick:.0f}", fill=(55, 60, 68), font=SMALL)
        draw.text((left, bottom + 32), xlabel, fill=(55, 60, 68), font=SMALL)
    image.save(path)


def table_markdown(rows: list[dict[str, Any]], cols: list[str]) -> str:
    out = ["|" + "|".join(cols) + "|", "|" + "|".join(["---"] * len(cols)) + "|"]
    for row in rows:
        cells = []
        for col in cols:
            val = row.get(col, "")
            if isinstance(val, float):
                cells.append(f"{val:.3f}")
            else:
                cells.append(str(val))
        out.append("|" + "|".join(cells) + "|")
    return "\n".join(out)


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    fio_rows = [parse_fio(label, path) for label, path in FIO_FILES.items()]
    sqlite_rows = [parse_sqlite(label, path) for label, path in SQLITE_FILES.items()]

    fio_base = next(row for row in fio_rows if row["label"] == "lat1_norp")
    sqlite_base = next(row for row in sqlite_rows if row["label"] == "lat1_norp")
    for row in fio_rows:
        row["read_runtime_improve_vs_lat1_norp_pct"] = rel_improve(fio_base["read_runtime_s"], row["read_runtime_s"])
        row["read_p99_improve_vs_lat1_norp_pct"] = rel_improve(fio_base["read_p99_ms"], row["read_p99_ms"])
        row["write_runtime_improve_vs_lat1_norp_pct"] = rel_improve(fio_base["write_runtime_s"], row["write_runtime_s"])
    for row in sqlite_rows:
        row["cold_full_read_improve_vs_lat1_norp_pct"] = rel_improve(sqlite_base["cold_full_read_s"], row["cold_full_read_s"])
        row["cold_p99_improve_vs_lat1_norp_pct"] = rel_improve(sqlite_base["cold_p99_ms"], row["cold_p99_ms"])

    write_csv(OUT_DIR / "fio_summary.csv", fio_rows)
    write_csv(OUT_DIR / "sqlite_summary.csv", sqlite_rows)

    order = ["lat1_norp", "lat1_qlc", "lat2_qlc", "lat3_qlc"]
    fio_by = {row["label"]: row for row in fio_rows}
    sqlite_by = {row["label"]: row for row in sqlite_rows}

    bar_chart(
        OUT_DIR / "fio_runtime_s.png",
        "FIO measured phase runtime (lower is better)",
        {name: [fio_by[name]["read_runtime_s"], fio_by[name]["write_runtime_s"]] for name in order},
        ["read 10GiB", "write 1GiB"],
        "seconds",
    )
    bar_chart(
        OUT_DIR / "fio_iops.png",
        "FIO measured phase IOPS (higher is better)",
        {name: [fio_by[name]["read_iops"], fio_by[name]["write_iops"]] for name in order},
        ["read", "write"],
        "IOPS",
    )
    bar_chart(
        OUT_DIR / "fio_read_tail_latency_ms_log.png",
        "FIO read completion latency percentiles (log scale, ms)",
        {name: [fio_by[name]["read_p50_ms"], fio_by[name]["read_p95_ms"], fio_by[name]["read_p99_ms"], fio_by[name]["read_p999_ms"], fio_by[name]["read_p9999_ms"]] for name in order},
        ["p50", "p95", "p99", "p99.9", "p99.99"],
        "ms log",
        log=True,
    )
    bar_chart(
        OUT_DIR / "fio_write_tail_latency_ms_log.png",
        "FIO write completion latency percentiles (log scale, ms)",
        {name: [fio_by[name]["write_p50_ms"], fio_by[name]["write_p95_ms"], fio_by[name]["write_p99_ms"], fio_by[name]["write_p999_ms"], fio_by[name]["write_p9999_ms"]] for name in order},
        ["p50", "p95", "p99", "p99.9", "p99.99"],
        "ms log",
        log=True,
    )
    bar_chart(
        OUT_DIR / "sqlite_cold_full_read_s.png",
        "SQLite cold full-scan total time (lower is better)",
        {name: [sqlite_by[name]["cold_full_read_s"]] for name in order},
        ["cold_full_read"],
        "seconds",
    )
    bar_chart(
        OUT_DIR / "sqlite_cold_read_latency_ms_log.png",
        "SQLite cold read latency percentiles (log scale, ms)",
        {name: [sqlite_by[name]["cold_p50_ms"], sqlite_by[name]["cold_p95_ms"], sqlite_by[name]["cold_p99_ms"], sqlite_by[name]["cold_p999_ms"], sqlite_by[name]["cold_max_ms"]] for name in order},
        ["p50", "p95", "p99", "p99.9", "max"],
        "ms log",
        log=True,
    )
    histogram_grid(
        OUT_DIR / "sqlite_table_time_hist.png",
        "SQLite per-table cold full-scan time histogram",
        {name: sqlite_by[name]["_table_times"] for name in order},
        "table scan seconds",
    )
    bar_chart(
        OUT_DIR / "sqlite_internal_costs.png",
        "SQLite FTL cost and conflict indicators",
        {name: [sqlite_by[name]["iw_per_read"], sqlite_by[name]["qlc_read_rate"], sqlite_by[name]["read_die_wait_ms"] / 100.0, sqlite_by[name]["kernel_slow_proc_io_sq"]] for name in order},
        ["iw/read", "qlc_read_rate", "die_wait_ms/100", "slow_path_count"],
        "mixed units",
    )

    report_lines = [
        "# Optimized FIO/SQLite Result Analysis",
        "",
        "## FIO Summary",
        table_markdown(
            fio_rows,
            [
                "label",
                "measure_read_gib",
                "measure_write_gib",
                "read_runtime_s",
                "read_iops",
                "read_p99_ms",
                "read_p999_ms",
                "write_runtime_s",
                "write_iops",
                "write_p99_ms",
                "read_runtime_improve_vs_lat1_norp_pct",
            ],
        ),
        "",
        "## SQLite Summary",
        table_markdown(
            sqlite_rows,
            [
                "label",
                "cold_full_read_s",
                "cold_avg_ms",
                "cold_p50_ms",
                "cold_p99_ms",
                "cold_p999_ms",
                "table_time_avg_s",
                "iw_per_read",
                "qlc_read_rate",
                "rp_yields",
                "rp_forced_progress",
                "kernel_slow_proc_io_sq",
                "soft_lockup_count",
                "cold_full_read_improve_vs_lat1_norp_pct",
            ],
        ),
        "",
        "## Charts",
        "",
        "![FIO runtime](fio_runtime_s.png)",
        "",
        "![FIO IOPS](fio_iops.png)",
        "",
        "![FIO read tail latency](fio_read_tail_latency_ms_log.png)",
        "",
        "![FIO write tail latency](fio_write_tail_latency_ms_log.png)",
        "",
        "![SQLite cold full read](sqlite_cold_full_read_s.png)",
        "",
        "![SQLite cold read latency](sqlite_cold_read_latency_ms_log.png)",
        "",
        "![SQLite table histogram](sqlite_table_time_hist.png)",
        "",
        "![SQLite internal costs](sqlite_internal_costs.png)",
    ]
    (OUT_DIR / "analysis_report.md").write_text("\n".join(report_lines))

    print(f"Wrote analysis to {OUT_DIR}")
    for path in sorted(OUT_DIR.iterdir()):
        print(path)


if __name__ == "__main__":
    main()
