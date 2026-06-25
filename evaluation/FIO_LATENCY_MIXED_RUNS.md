# FIO Latency Mixed Runs

This is the FIO counterpart to the SQLite latency-maintenance run. It writes
an 8 GiB raw-device region in 512 MiB chunks by default, reads each just-written
chunk for 5x warmup, then measures a concurrent read/write phase. Measured reads stay in
the initialized 0-8 GiB region; measured writes use a separate region starting
at 8 GiB so reads do not hit data written during the measured phase.

Recommended full sweep:

```bash
VARIANTS="die_latency1_qlc_all_norp_sb die_latency2_qlc_all_norp_sb die_latency3_qlc_all_norp_sb die_latency1_norp_sb" \
FIO_GC_NAND_TIMING=1 \
FIO_ACCESS_DIST_LIST="zipf normal" \
ZIPF_ALPHA=0.75 \
FIO_RW_RATIOS="10:1 8:2 7:3" \
FIO_REGION_SIZE=8G \
FIO_INIT_WRITE_BYTES=8G \
FIO_INIT_CHUNK_SIZE=512M \
FIO_INIT_PREWARM_BYTES_PER_CHUNK=5x \
FIO_INIT_PREWARM_GUARD=1 \
FIO_INIT_ADVANCE_HEAT_EPOCH_EACH_CHUNK=1 \
FIO_PREWARM_SEQ_BYTES=0 \
FIO_PREWARM_RANDOM_BYTES=0 \
FIO_MEASURE_WRITE_BYTES=5G \
FIO_MEASURE_WRITE_OFFSET=8G \
FIO_MEASURE_WRITE_REGION_SIZE=io_size \
FIO_MEASURE_JOBS=1 \
FIO_MEASURE_IODEPTH=32 \
FIO_TEST_PHASE_RECENT_WRITE_GUARD=1 \
FIO_TEST_PHASE_GUARD_READ_REQS=256 \
bash fio_fragment_die_latency_mixed.sh
```

The generated separated-range job keeps measured writes fixed at
`FIO_MEASURE_WRITE_BYTES=5G` and computes measured reads from each ratio:
`10:1` gives 50 GiB reads, `8:2` gives 20 GiB reads, and `7:3` gives about
11.67 GiB reads. Reads come from 0-8 GiB; writes start at 8 GiB.

With `FIO_INIT_CHUNK_SIZE=512M` and
`FIO_INIT_PREWARM_BYTES_PER_CHUNK=5x`, the generated init has 16 rounds:
write 512 MiB, guarded distribution-shaped read of 2.5 GiB over that same
512 MiB chunk, then move to the next chunk. That totals 8 GiB init writes and
40 GiB init prewarm reads. The final test-phase stats are reset by the
`enter_test_phase` job immediately before `measure_reads` and `measure_writes`.
`FIO_INIT_ADVANCE_HEAT_EPOCH_EACH_CHUNK=1` advances `heat_epoch` after each
guarded init prewarm so unread/cold pages from the previous chunk are no longer
blocked solely by the recent-write epoch guard.

For non-zero-offset fio jobs, the generated `.fio` file emits `size` as
`offset + range_size`, not just `range_size`. fio 3.16 computes the working
range as `size - offset`; using `size=512M, offset=1G` makes random jobs end
early with `error=0` and too few bytes. The runner validates the JSON after
each run and fails the case unless init writes, init prewarm reads, measured
reads, and measured writes exactly match the configured byte counts.
Before fio starts, the runner also validates the generated jobfile. A valid run
prints `[fio-job-validate] PASS ...`; after fio completes it must also print
`[fio-validate] PASS ...`.

If the module variants need rebuilding first:

```bash
FIO_REBUILD_DIE_MODULES=1 \
FIO_STRICT_COMPILE_FLAGS=1 \
VARIANTS="die_latency1_qlc_all_norp_sb die_latency2_qlc_all_norp_sb die_latency3_qlc_all_norp_sb die_latency1_norp_sb" \
bash fio_fragment_die_latency_mixed.sh
```

`FIO_STRICT_COMPILE_FLAGS=1` makes the runner fail a case when a named ablation
does not match its expected compile-time switches. For example,
`die_latency*_qlc_all_norp_sb` must report read repromotion off, QLC hot/cold
on, and QLC rebalance off in `fio_test_phase_stats_<run>.txt`.

To separate foreground performance from delayed background debt, enable the
post-measure drain:

```bash
FIO_REBUILD_DIE_MODULES=1 \
FIO_STRICT_COMPILE_FLAGS=1 \
FIO_DRAIN_AFTER_MEASURE=1 \
FIO_REPEAT_COUNT=3 \
FIO_RANDSEED_BASE=240622 \
FIO_DRAIN_MIN_SEC=60 \
FIO_DRAIN_TIMEOUT_SEC=300 \
FIO_DRAIN_POLL_SEC=5 \
FIO_DRAIN_STABLE_POLLS=3 \
VARIANTS="die_latency1_qlc_all_norp_sb die_latency2_qlc_all_norp_sb die_latency3_qlc_all_norp_sb die_latency1_norp_sb" \
FIO_GC_NAND_TIMING=1 \
FIO_ACCESS_DIST_LIST="zipf" \
ZIPF_ALPHA=0.75 \
FIO_RW_RATIOS="10:1" \
FIO_REGION_SIZE=8G \
FIO_INIT_WRITE_BYTES=8G \
FIO_INIT_CHUNK_SIZE=512M \
FIO_INIT_PREWARM_BYTES_PER_CHUNK=5x \
FIO_MEASURE_WRITE_BYTES=1G \
FIO_MEASURE_WRITE_OFFSET=8G \
FIO_MEASURE_WRITE_REGION_SIZE=io_size \
FIO_TEST_PHASE_RECENT_WRITE_GUARD=1 \
FIO_TEST_PHASE_GUARD_READ_REQS=256 \
bash fio_fragment_die_latency_mixed.sh
```

With drain enabled, the normal stats file is the immediate foreground-window
snapshot, captured before disabling `test_phase`:

```text
fio_test_phase_stats_aggregate_<run>.txt
```

The delayed-work snapshot is captured after `active_reads=0`,
`active_overwrites=0`, `active_bg_ops=0`, and the configured background counters
are stable for `FIO_DRAIN_STABLE_POLLS` polls after at least
`FIO_DRAIN_MIN_SEC`:

```text
fio_test_phase_stats_aggregate_<run>_post_drain.txt
```

The runner also writes two one-file post-drain views:

```text
fio_test_phase_stats_aggregate_<run>_foreground_post_drain.txt
fio_post_drain_<run>.txt
```

The first file is a compact table with `key foreground post_drain delta`. The
second bundles the post-drain raw stats, aggregate stats, superblock stats, die
stats, background NAND stats, and the foreground/post-drain aggregate table.

Use the difference between post-drain and foreground counters to estimate
deferred background debt. For example, compare
`slc_gc_valid_copy_pages`, `slc_gc_invalid_reclaimed_pages`,
`slc_to_qlc_migration_pages`, `internal_write_pages_est`,
`bg_repromote_ops`, and `bg_qlc_rebalance_ops`.

To avoid opening each JSON/stat file manually, generate a single summary table:

```bash
python3 fio_summarize_latency_mixed.py result/fio_latency_mixed \
  -o result/fio_latency_mixed/summary.tsv \
  --aggregate-output result/fio_latency_mixed/summary_median_range.tsv
```

The summarizer scans all `fio_mixed_*.json` files, finds matching
`fio_test_phase_stats_aggregate_<run>.txt` and
`fio_test_phase_stats_aggregate_<run>_post_drain.txt` when present, and emits
one row per run. The aggregate table reports median, minimum and maximum for
request p99/p999, throughput, resource waits, gate counters and backlog; groups
with fewer than three runs are marked `insufficient`. Use `--format csv` for a
comma-separated file.

Fast generation-only check, without loading NVMeVirt or running fio:

```bash
FIO_DRY_RUN=1 \
VARIANTS="die_latency1_qlc_all_norp_sb" \
FIO_ACCESS_DIST_LIST="zipf normal" \
FIO_RW_RATIOS="10:1 8:2 7:3" \
bash fio_fragment_die_latency_mixed.sh
```

Outputs are stored under `result/fio_latency_mixed/` from the `evaluation/`
directory. Result filenames use stable repeat indices; for example:

```text
fio_mixed_die_latency1_norp_sb_zipf_rw10_1_bg1_r1.json
```

Each repeat is stored under `.../rw<ratio>/bg<timing>/r<index>/`; rerunning one repeat
clears only that repeat's stale files. Each run keeps the
exact generated `.fio` file, the fio JSON output, latency/bandwidth/IOPS logs,
and available `test_phase`/`superblock` stats.

Run the no-background-contention overhead control with the identical mixed
workload but background NAND timing disabled. This preserves foreground read
and write traffic while removing migration/GC resource occupancy:

```bash
FIO_GC_NAND_TIMING=0 \
FIO_REPEAT_COUNT=3 \
FIO_DRAIN_AFTER_MEASURE=1 \
VARIANTS="die_latency1_qlc_all_norp_sb die_latency2_qlc_all_norp_sb die_latency3_qlc_all_norp_sb" \
FIO_ACCESS_DIST_LIST="zipf" \
FIO_RW_RATIOS="10:1" \
bash fio_fragment_die_latency_mixed.sh
```

Compare these `bg0` groups separately from the contention-enabled `bg1`
groups; never pool them in one median.

After generating `summary_median_range.tsv`, run:

```bash
python3 evaluate_latency_acceptance.py \
  result/fio_latency_mixed/summary_median_range.tsv
```

The evaluator fails unless both latency2 and latency3 pass request p99/p999,
mean-or-throughput, background-wait attribution, no-contention overhead and
post-drain backlog gates.

Trial `rN` uses seed `FIO_RANDSEED_BASE + N`; every variant in that trial uses
the same seed. Do not set `randrepeat=0` for causal comparisons.

Manual validation for a 10:1 run with 1 GiB measured writes:

```bash
python3 fio_validate_latency_mixed_json.py \
  result/fio_latency_mixed/<variant>/zipf/rw10_1/<run>.json \
  --expected-init-write 8G \
  --expected-init-prewarm 40G \
  --expected-measure-read 10G \
  --expected-measure-write 1G
```

For the default 5 GiB measured-write command, use
`--expected-measure-read 50G --expected-measure-write 5G` for `10:1`.
