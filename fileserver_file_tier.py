#!/usr/bin/env python3
import argparse
import csv
import math
import os
import re
import statistics
import subprocess
from bisect import bisect_left
from collections import Counter


def load_page_tier(path):
    entries = {}
    with open(path, "r") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            lpn = int(parts[0])
            in_slc = int(parts[1])
            qlc_zone = int(parts[2]) if len(parts) >= 3 else -1
            entries[lpn] = (in_slc, qlc_zone)
    return entries


def load_page_die(path):
    entries = {}
    with open(path, "r") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            entries[int(parts[0])] = int(parts[1])
    return entries


def load_access_count(path):
    entries = {}
    with open(path, "r") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            entries[int(parts[0])] = int(parts[1])
    return entries


def load_plan(path):
    plan = {}
    if not path or not os.path.exists(path):
        return plan
    with open(path, "r", newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            try:
                plan[row["path"]] = int(row["count"])
            except Exception:
                continue
    return plan


def find_partition_offset_bytes(root):
    st = os.stat(root)
    sysfs = f"/sys/dev/block/{os.major(st.st_dev)}:{os.minor(st.st_dev)}/start"
    try:
        with open(sysfs, "r") as fh:
            start_sector = int(fh.read().strip())
        return start_sector * 512
    except Exception:
        return 0


def find_block_size(root):
    vfs = os.statvfs(root)
    return vfs.f_frsize or vfs.f_bsize or 4096


def list_files(root):
    paths = []
    for dirpath, _, filenames in os.walk(root):
        for name in filenames:
            paths.append(os.path.join(dirpath, name))
    paths.sort()
    return paths


FILEFRAG_RE = re.compile(
    r"^\s*\d+:\s*(\d+)\.\.(\d+):\s*(\d+)\.\.(\d+):\s*(\d+):"
)


def filefrag_extents(path):
    try:
        proc = subprocess.run(
            ["filefrag", "-e", "-v", path],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except Exception:
        return None
    extents = []
    for line in proc.stdout.splitlines():
        m = FILEFRAG_RE.match(line)
        if not m:
            continue
        logical_start = int(m.group(1))
        logical_end = int(m.group(2))
        physical_start = int(m.group(3))
        physical_end = int(m.group(4))
        extents.append((logical_start, logical_end, physical_start, physical_end))
    return extents or None


def lpns_for_file(path, part_offset_bytes, fs_block_bytes, host_page_bytes):
    extents = filefrag_extents(path)
    if not extents:
        return []
    lpns = []
    for logical_start, logical_end, physical_start, physical_end in extents:
        del logical_start, logical_end, physical_end
        blocks = physical_end - physical_start + 1
        phys_start = part_offset_bytes + physical_start * fs_block_bytes
        phys_end = phys_start + blocks * fs_block_bytes - 1
        lpn_start = phys_start // host_page_bytes
        lpn_end = phys_end // host_page_bytes
        lpns.extend(range(lpn_start, lpn_end + 1))
    lpns = sorted(set(lpns))
    return lpns


def percentile(vals, p):
    if not vals:
        return 0
    vals = sorted(vals)
    idx = int(math.floor((len(vals) - 1) * p))
    return vals[idx]


def effective_parallelism(die_counts):
    total = sum(die_counts.values())
    if total <= 0:
        return 0.0
    s = 0.0
    for c in die_counts.values():
        frac = c / total
        s += frac * frac
    if s <= 0:
        return 0.0
    return 1.0 / s


def summarize_group(rows):
    if not rows:
        return None
    total_lpn = sum(r["lpn_count"] for r in rows)
    total_known = sum(r["known_tier_lpn"] for r in rows)
    total_slc = sum(r["slc_lpn"] for r in rows)
    total_fast = sum(r["qlc_fast_lpn"] for r in rows)
    total_slow = sum(r["qlc_slow_lpn"] for r in rows)
    total_plan = sum(r["planned_count"] for r in rows)
    return {
        "files": len(rows),
        "lpn_count": total_lpn,
        "planned_count": total_plan,
        "slc_ratio_known": (total_slc / total_known) if total_known else 0.0,
        "qlc_fast_ratio_known": (total_fast / total_known) if total_known else 0.0,
        "qlc_slow_ratio_known": (total_slow / total_known) if total_known else 0.0,
        "mean_effective_die_parallelism": statistics.fmean(r["effective_die_parallelism"] for r in rows),
        "mean_dominant_die_lpn_ratio": statistics.fmean(r["dominant_die_lpn_ratio"] for r in rows),
        "mean_acc": statistics.fmean(r["acc_mean"] for r in rows),
    }


def write_summary(path, rows):
    ranked = sorted(rows, key=lambda r: (-r["planned_count"], r["path"]))
    hot_n = max(1, len(ranked) // 10)
    hot = ranked[:hot_n]
    cold = ranked[hot_n:]
    hot_s = summarize_group(hot)
    cold_s = summarize_group(cold)
    with open(path, "w") as fh:
        fh.write("[all_files]\n")
        fh.write(f"files={len(rows)}\n")
        fh.write(f"lpns={sum(r['lpn_count'] for r in rows)}\n")
        fh.write(f"planned_ops={sum(r['planned_count'] for r in rows)}\n")
        for name, stats in [("hottest_10pct", hot_s), ("coldest_90pct", cold_s)]:
            if not stats:
                continue
            fh.write(f"[{name}]\n")
            for k, v in stats.items():
                fh.write(f"{k}={v}\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--page-tier", required=True)
    ap.add_argument("--page-die", required=True)
    ap.add_argument("--access-count", required=True)
    ap.add_argument("--out-csv", required=True)
    ap.add_argument("--out-summary", required=True)
    ap.add_argument("--plan-csv")
    ap.add_argument("--host-page-bytes", type=int, default=4096)
    args = ap.parse_args()

    tier = load_page_tier(args.page_tier)
    die = load_page_die(args.page_die)
    acc = load_access_count(args.access_count)
    plan = load_plan(args.plan_csv)
    files = list_files(args.root)
    part_off = find_partition_offset_bytes(args.root)
    fs_block = find_block_size(args.root)

    rows = []
    for rank, path in enumerate(files):
        lpns = lpns_for_file(path, part_off, fs_block, args.host_page_bytes)
        if not lpns:
            continue
        slc_lpn = 0
        qlc_lpn = 0
        qlc_fast_lpn = 0
        qlc_slow_lpn = 0
        qlc_unknown_lpn = 0
        unknown_lpn = 0
        acc_vals = []
        die_counts = Counter()
        for lpn in lpns:
            if lpn in tier:
                in_slc, qlc_zone = tier[lpn]
                if in_slc:
                    slc_lpn += 1
                else:
                    qlc_lpn += 1
                    if qlc_zone in (0, 1):
                        qlc_fast_lpn += 1
                    elif qlc_zone in (2, 3):
                        qlc_slow_lpn += 1
                    else:
                        qlc_unknown_lpn += 1
            else:
                unknown_lpn += 1
            acc_vals.append(acc.get(lpn, 0))
            if lpn in die:
                die_counts[die[lpn]] += 1

        total = len(lpns)
        known_tier = slc_lpn + qlc_lpn
        dominant_die = max(die_counts, key=die_counts.get) if die_counts else -1
        dominant_die_lpn = die_counts[dominant_die] if die_counts else 0
        row = {
            "rank": rank,
            "path": path,
            "size_bytes": os.path.getsize(path),
            "planned_count": plan.get(path, 0),
            "lpn_count": total,
            "known_tier_lpn": known_tier,
            "slc_lpn": slc_lpn,
            "qlc_lpn": qlc_lpn,
            "qlc_fast_lpn": qlc_fast_lpn,
            "qlc_slow_lpn": qlc_slow_lpn,
            "qlc_unknown_lpn": qlc_unknown_lpn,
            "unknown_lpn": unknown_lpn,
            "acc_mean": (sum(acc_vals) / len(acc_vals)) if acc_vals else 0.0,
            "acc_p50": percentile(acc_vals, 0.50),
            "acc_p90": percentile(acc_vals, 0.90),
            "acc_p99": percentile(acc_vals, 0.99),
            "acc_max": max(acc_vals) if acc_vals else 0,
            "distinct_die": len(die_counts),
            "dominant_die": dominant_die,
            "dominant_die_lpn": dominant_die_lpn,
            "dominant_die_lpn_ratio": (dominant_die_lpn / total) if total else 0.0,
            "effective_die_parallelism": effective_parallelism(die_counts),
        }
        rows.append(row)

    with open(args.out_csv, "w", newline="") as fh:
        writer = csv.DictWriter(
            fh,
            fieldnames=[
                "rank",
                "path",
                "size_bytes",
                "planned_count",
                "lpn_count",
                "known_tier_lpn",
                "slc_lpn",
                "qlc_lpn",
                "qlc_fast_lpn",
                "qlc_slow_lpn",
                "qlc_unknown_lpn",
                "unknown_lpn",
                "acc_mean",
                "acc_p50",
                "acc_p90",
                "acc_p99",
                "acc_max",
                "distinct_die",
                "dominant_die",
                "dominant_die_lpn",
                "dominant_die_lpn_ratio",
                "effective_die_parallelism",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)

    write_summary(args.out_summary, rows)
    print(f"[file-tier] files={len(rows)} root={args.root}")
    print(f"[file-tier] out_csv={args.out_csv}")
    print(f"[file-tier] out_summary={args.out_summary}")


if __name__ == "__main__":
    main()
