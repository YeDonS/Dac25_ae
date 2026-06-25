#!/usr/bin/env python3
import argparse
import bisect
import csv
import math
import os
import random
import sys
import time


POSIX_FADV_DONTNEED = 4
READ_CHUNK = 1 << 20


def list_files(root):
    paths = []
    for dirpath, _, filenames in os.walk(root):
        for name in filenames:
            paths.append(os.path.join(dirpath, name))
    paths.sort()
    return paths


def build_weights(n, dist, mean, stddev, alpha, lambd):
    if n <= 0:
        return []
    weights = []
    if dist == "zipf":
        for i in range(n):
            weights.append(1.0 / math.pow(i + 1, alpha))
    elif dist == "exp":
        for i in range(n):
            weights.append(math.exp(-lambd * i))
    elif dist == "normal":
        mu = mean if mean >= 0 else (n - 1) / 2.0
        sigma = max(stddev, 1e-9)
        for i in range(n):
            x = (i - mu) / sigma
            weights.append(math.exp(-0.5 * x * x))
    else:
        raise ValueError(f"unsupported distribution: {dist}")
    total = sum(weights)
    if total <= 0:
        raise ValueError("distribution weights sum to zero")
    return [w / total for w in weights]


def build_cdf(weights):
    cdf = []
    total = 0.0
    for w in weights:
        total += w
        cdf.append(total)
    if cdf:
        cdf[-1] = 1.0
    return cdf


def pick_index(rng, cdf):
    r = rng.random()
    idx = bisect.bisect_left(cdf, r)
    if idx >= len(cdf):
        idx = len(cdf) - 1
    return idx


def ensure_dir(path):
    os.makedirs(path, exist_ok=True)


def create_file(path, size_bytes):
    fd = os.open(path, os.O_CREAT | os.O_TRUNC | os.O_RDWR, 0o666)
    try:
        if hasattr(os, "posix_fallocate"):
            os.posix_fallocate(fd, 0, size_bytes)
        else:
            os.ftruncate(fd, size_bytes)
    finally:
        os.close(fd)


def create_files(root, file_count, init_size_bytes):
    ensure_dir(root)
    paths = []
    start = time.time()
    for idx in range(file_count):
        path = os.path.join(root, f"f{idx:05d}.bin")
        create_file(path, init_size_bytes)
        paths.append(path)
    os.sync()
    return paths, max(time.time() - start, 1e-9)


def append_once(path, buf, do_sync):
    fd = os.open(path, os.O_WRONLY | os.O_APPEND)
    try:
        written = 0
        while written < len(buf):
            ret = os.write(fd, buf[written:])
            if ret <= 0:
                raise OSError("short write during append")
            written += ret
        if do_sync:
            os.fdatasync(fd)
    finally:
        os.close(fd)
    return written


def execute_append_phase(paths, cdf, seed, append_rounds, append_size_bytes,
                         sync_every, label):
    rng = random.Random(seed)
    append_counts = [0] * len(paths)
    buf = bytes([0xA5]) * append_size_bytes
    total_bytes = 0
    start = time.time()

    for op in range(append_rounds):
        idx = pick_index(rng, cdf)
        do_sync = sync_every <= 1 or ((op + 1) % sync_every == 0)
        total_bytes += append_once(paths[idx], buf, do_sync)
        append_counts[idx] += 1
        if sync_every > 1 and ((op + 1) % sync_every == 0):
            os.sync()

    if sync_every > 1 and append_rounds > 0 and (append_rounds % sync_every) != 0:
        os.sync()
    elapsed = max(time.time() - start, 1e-9)
    print(f"[{label}] append_rounds={append_rounds} total_append_bytes={total_bytes} "
          f"elapsed_sec={elapsed:.6f} throughput_mb_s="
          f"{(total_bytes / (1024.0 * 1024.0)) / elapsed:.2f}")
    return append_counts, total_bytes, elapsed


def load_state_csv(path):
    rows = []
    with open(path, "r", newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            rows.append(
                {
                    "rank": int(row["rank"]),
                    "path": row["path"],
                    "weight": float(row["weight"]),
                    "append_count": int(row["append_count"]),
                    "heat": int(row["heat"]),
                    "size_bytes": int(row["size_bytes"]),
                }
            )
    rows.sort(key=lambda row: row["rank"])
    return rows


def read_file_once(path):
    total = 0
    fd = os.open(path, os.O_RDONLY)
    try:
        while True:
            data = os.read(fd, READ_CHUNK)
            if not data:
                break
            total += len(data)
        if hasattr(os, "posix_fadvise"):
            try:
                os.posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)
            except OSError:
                pass
    finally:
        os.close(fd)
    return total


def execute_read_phase(rows, total_read_ops, cover_all, seed, label):
    if total_read_ops <= 0:
        raise ValueError("total_read_ops must be > 0")

    read_counts = [0] * len(rows)
    heat_weights = [row["heat"] for row in rows]
    total_heat = sum(heat_weights)
    if total_heat <= 0:
        raise ValueError("heat sum must be > 0")
    heat_cdf = build_cdf([w / total_heat for w in heat_weights])

    cover_reads = min(len(rows), total_read_ops) if cover_all else 0
    weighted_reads = total_read_ops - cover_reads
    rng = random.Random(seed)
    total_bytes = 0
    total_ops = 0
    start = time.time()

    for idx in range(cover_reads):
        total_bytes += read_file_once(rows[idx]["path"])
        read_counts[idx] += 1
        total_ops += 1

    for _ in range(weighted_reads):
        idx = pick_index(rng, heat_cdf)
        total_bytes += read_file_once(rows[idx]["path"])
        read_counts[idx] += 1
        total_ops += 1

    elapsed = max(time.time() - start, 1e-9)
    print(f"[{label}] total_read_ops={total_ops} cover_reads={cover_reads} "
          f"weighted_reads={weighted_reads}")
    print(f"[{label}] total_read_bytes={total_bytes} elapsed_sec={elapsed:.6f} "
          f"throughput_mb_s={(total_bytes / (1024.0 * 1024.0)) / elapsed:.2f}")
    return read_counts, total_ops, total_bytes, elapsed


def write_plan_csv(path, rows, weights, counts):
    with open(path, "w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(["rank", "path", "weight", "count"])
        for row, weight, count in zip(rows, weights, counts):
            writer.writerow([row["rank"], row["path"], f"{weight:.12f}", count])


def write_state_csv(path, rows):
    with open(path, "w", newline="") as fh:
        writer = csv.DictWriter(
            fh,
            fieldnames=["rank", "path", "weight", "append_count", "heat", "size_bytes"],
        )
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def print_top_counts(label, rows, counts, topn):
    ranked = []
    for row, count in zip(rows, counts):
        if count > 0:
            ranked.append((count, row["rank"], row["path"]))
    ranked.sort(reverse=True)
    for order, item in enumerate(ranked[:topn], start=1):
        count, rank, path = item
        print(f"[{label}] top{order}: rank={rank} count={count} path={path}")


def phase_create_append(args):
    weights = build_weights(
        args.file_count,
        args.dist,
        args.normal_mean,
        args.normal_stddev,
        args.zipf_alpha,
        args.exp_lambda,
    )
    cdf = build_cdf(weights)
    paths, create_elapsed = create_files(args.root, args.file_count, args.init_size_bytes)
    print(f"[{args.label}] created_files={len(paths)} init_size_bytes={args.init_size_bytes} "
          f"elapsed_sec={create_elapsed:.6f}")

    rows = []
    for idx, (path, weight) in enumerate(zip(paths, weights)):
        rows.append(
            {
                "rank": idx,
                "path": path,
                "weight": weight,
                "append_count": 0,
                "heat": 1,
                "size_bytes": args.init_size_bytes,
            }
        )

    append_counts, total_append_bytes, append_elapsed = execute_append_phase(
        paths,
        cdf,
        args.seed,
        args.append_rounds,
        args.append_size_bytes,
        args.sync_every,
        args.label,
    )

    for row, append_count in zip(rows, append_counts):
        row["append_count"] = append_count
        row["heat"] = 1 + append_count
        row["size_bytes"] = args.init_size_bytes + append_count * args.append_size_bytes

    if args.state_out:
        write_state_csv(args.state_out, rows)
    if args.plan_out:
        write_plan_csv(args.plan_out, rows, weights, append_counts)

    unique_append_files = sum(1 for count in append_counts if count > 0)
    total_size = sum(row["size_bytes"] for row in rows)
    print(f"[{args.label}] unique_append_files={unique_append_files}")
    print(f"[{args.label}] final_total_size_bytes={total_size}")
    print_top_counts(args.label, rows, append_counts, args.topn)
    return 0


def phase_read(args):
    rows = load_state_csv(args.state_in)
    if not rows:
        print(f"[{args.label}] ERROR: empty state file {args.state_in}", file=sys.stderr)
        return 1

    weights = [row["heat"] / max(sum(r["heat"] for r in rows), 1) for row in rows]
    read_counts, total_ops, total_bytes, elapsed = execute_read_phase(
        rows,
        args.read_ops,
        args.cover_all,
        args.seed,
        args.label,
    )

    if args.plan_out:
        write_plan_csv(args.plan_out, rows, weights, read_counts)

    unique_read_files = sum(1 for count in read_counts if count > 0)
    print(f"[{args.label}] unique_read_files={unique_read_files}")
    print_top_counts(args.label, rows, read_counts, args.topn)
    print(f"[{args.label}] read_ops={total_ops} total_read_bytes={total_bytes} "
          f"elapsed_sec={elapsed:.6f}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--phase", required=True, choices=["create-append", "read"])
    ap.add_argument("--root", required=True)
    ap.add_argument("--dist", choices=["zipf", "normal", "exp"], default="normal")
    ap.add_argument("--seed", type=int, required=True)
    ap.add_argument("--normal-mean", type=float, default=-1)
    ap.add_argument("--normal-stddev", type=float, default=400)
    ap.add_argument("--zipf-alpha", type=float, default=1.2)
    ap.add_argument("--exp-lambda", type=float, default=0.0008)
    ap.add_argument("--file-count", type=int, default=5000)
    ap.add_argument("--init-size-bytes", type=int, default=131072)
    ap.add_argument("--append-size-bytes", type=int, default=32768)
    ap.add_argument("--append-rounds", type=int, default=0)
    ap.add_argument("--read-ops", type=int, default=50000)
    ap.add_argument("--cover-all", type=int, choices=[0, 1], default=1)
    ap.add_argument("--sync-every", type=int, default=1)
    ap.add_argument("--state-in")
    ap.add_argument("--state-out")
    ap.add_argument("--plan-out")
    ap.add_argument("--label", default="heat-fileserver")
    ap.add_argument("--topn", type=int, default=10)
    args = ap.parse_args()

    try:
        if args.phase == "create-append":
            return phase_create_append(args)
        if not args.state_in:
            print("--state-in is required for read phase", file=sys.stderr)
            return 1
        args.cover_all = bool(args.cover_all)
        return phase_read(args)
    except Exception as exc:
        print(f"[{args.label}] ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
