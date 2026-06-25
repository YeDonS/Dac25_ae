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


def list_files(root: str):
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
    s = 0.0
    for w in weights:
        s += w
        cdf.append(s)
    cdf[-1] = 1.0
    return cdf


def sample_counts(n, ops, cdf, seed):
    rng = random.Random(seed)
    counts = [0] * n
    for _ in range(ops):
        r = rng.random()
        idx = bisect.bisect_left(cdf, r)
        if idx >= n:
            idx = n - 1
        counts[idx] += 1
    return counts


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


def execute_plan(files, counts):
    total_bytes = 0
    total_ops = 0
    start = time.time()
    for path, cnt in zip(files, counts):
        for _ in range(cnt):
            total_bytes += read_file_once(path)
            total_ops += 1
    elapsed = max(time.time() - start, 1e-9)
    return total_ops, total_bytes, elapsed


def write_plan_csv(path, files, weights, counts):
    with open(path, "w", newline="") as fh:
        writer = csv.writer(fh)
        writer.writerow(["rank", "path", "weight", "count"])
        for i, (p, w, c) in enumerate(zip(files, weights, counts)):
            writer.writerow([i, p, f"{w:.12f}", c])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--dist", required=True, choices=["zipf", "normal", "exp"])
    ap.add_argument("--ops", required=True, type=int)
    ap.add_argument("--seed", required=True, type=int)
    ap.add_argument("--normal-mean", type=float, default=-1)
    ap.add_argument("--normal-stddev", type=float, default=400)
    ap.add_argument("--zipf-alpha", type=float, default=1.2)
    ap.add_argument("--exp-lambda", type=float, default=0.0008)
    ap.add_argument("--plan-out")
    ap.add_argument("--label", default="dist-access")
    args = ap.parse_args()

    files = list_files(args.root)
    if not files:
        print(f"[{args.label}] ERROR: no files under {args.root}", file=sys.stderr)
        return 1
    if args.ops <= 0:
        print(f"[{args.label}] ERROR: ops must be > 0", file=sys.stderr)
        return 1

    weights = build_weights(
        len(files),
        args.dist,
        args.normal_mean,
        args.normal_stddev,
        args.zipf_alpha,
        args.exp_lambda,
    )
    cdf = build_cdf(weights)
    counts = sample_counts(len(files), args.ops, cdf, args.seed)
    if args.plan_out:
        write_plan_csv(args.plan_out, files, weights, counts)

    nonzero = [(i, c) for i, c in enumerate(counts) if c > 0]
    nonzero.sort(key=lambda x: x[1], reverse=True)
    total_ops, total_bytes, elapsed = execute_plan(files, counts)

    print(f"[{args.label}] root={args.root}")
    print(f"[{args.label}] dist={args.dist} files={len(files)} ops={args.ops} seed={args.seed}")
    print(f"[{args.label}] unique_files_touched={len(nonzero)}")
    print(
        f"[{args.label}] total_bytes={total_bytes} elapsed_sec={elapsed:.6f} "
        f"throughput_mb_s={(total_bytes / (1024.0 * 1024.0)) / elapsed:.2f}"
    )
    topn = min(10, len(nonzero))
    for rank, (idx, cnt) in enumerate(nonzero[:topn], start=1):
        print(f"[{args.label}] top{rank}: file_rank={idx} count={cnt} path={files[idx]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
