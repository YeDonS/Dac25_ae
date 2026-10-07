# fast_24 Research Runbook

This document is the source of truth for latency1/2/3 experiments. Results
that fail the gates below are diagnostic only and must not support a paper
claim.

## 1. Scheduler Model Boundary

The implemented rule is named `nonpreemptive_submit_gate`.

- LUN, channel and PCIe each have one committed serial tail.
- Once an operation enters a resource model, its returned completion time is
  immutable and a later read cannot bypass it.
- Read priority acts before submission: active-read/per-die demand and the
  simulated read window make workers yield and requeue the next bounded unit.
- Forced progress admits one bounded background unit after repeated yields;
  it does not preempt a read or open an independent hardware timeline.

Run the one-LUN semantic comparison before changing this rule:

```bash
python3 nvmevirt_DA/tests/read_priority_queue_model.py
```

The output covers LP-first, read-first, interleaved and batched-read ordering.
Any reported tail bypass must remain zero.

## 2. Canonical Comparison Set

Use this four-variant set for scheduler attribution:

```bash
export VARIANTS="die_latency1_norp_sb \
die_latency1_qlc_all_norp_sb \
die_latency2_qlc_all_norp_sb \
die_latency3_qlc_all_norp_sb"
```

Interpretation:

| Variant | Purpose |
| --- | --- |
| `die_latency1_norp_sb` | no-QLC-placement baseline |
| `die_latency1_qlc_all_norp_sb` | placement-only control |
| `die_latency2_qlc_all_norp_sb` | per-die maintenance treatment |
| `die_latency3_qlc_all_norp_sb` | read-priority treatment |

Repromotion and QLC rebalance are disabled in this comparison. Compile flags
are checked after every run; strict failure is enabled by default.

## 3. Build And Run

Build on the Linux host whose kernel headers match the experiment kernel:

```bash
cd nvmevirt_DA
bash build_die.sh \
  die_latency1_norp_sb \
  die_latency1_qlc_all_norp_sb \
  die_latency2_qlc_all_norp_sb \
  die_latency3_qlc_all_norp_sb
```

FIO smoke generation without loading a module:

```bash
cd evaluation
FIO_DRY_RUN=1 \
VARIANTS="die_latency1_qlc_all_norp_sb" \
FIO_ACCESS_DIST_LIST="zipf" \
FIO_RW_RATIOS="10:1" \
bash fio_fragment_die_latency_mixed.sh
```

Real FIO run:

```bash
FIO_STRICT_COMPILE_FLAGS=1 \
FIO_DRAIN_AFTER_MEASURE=1 \
FIO_REPEAT_COUNT=3 \
FIO_RANDSEED_BASE=240622 \
bash fio_fragment_die_latency_mixed.sh
```

SQLite run:

```bash
SQLITE_STRICT_COMPILE_FLAGS=1 \
VARIANTS="$VARIANTS" \
bash sqlite_fragment_die_test1_tablefile_pageflow_fileparallel_fullscan.sh
```

Each case writes `*_manifest.json`. It contains the module SHA-256, workload
parameters, artifact hashes, scheduler model and observed compile contract.

## 4. Manifest Comparability Gate

Compare only runs with the same workload projection:

```bash
python3 evaluation/research_run_manifest.py compare \
  result/path/baseline_manifest.json \
  result/path/treatment_manifest.json
```

The command fails when workload/model parameters differ or when any compile
contract is not `pass`. Do not manually waive seed, queue depth, byte count,
distribution, drain policy or model differences.

## 5. Metric Dictionary

| Layer | Metric | Meaning | Not interchangeable with |
| --- | --- | --- | --- |
| Application | SQLite scan p50/p95/p99/p999 | end-to-end SQL operation | host NVMe request latency |
| Host request | `read_req_latency_*` | simulated completion minus request issue | SQL sample or NAND op |
| Request histogram | `read_req_latency_hist_bin_*` | recomputable request distribution | summary percentile alone |
| LUN wait | `read_die_wait_ns` | wait attributed to target die tail | full request latency |
| Channel wait | `read_ch_*` | flash-channel wait, split by direct read/background predecessor | LUN or PCIe wait |
| PCIe wait | `read_pcie_*` | host-DMA wait, split by direct predecessor | internal migration cost |
| Legacy bypass | `read_lp_bypass_*` and global bypass counters | must be zero in this model | mechanism activity |
| Worker gate | yield/requeue/forced-progress counters | work deferred before resource submission | NAND preemption |
| Write cost | foreground and post-drain NAND/page counters | write amplification evidence | elapsed drain time |

The analyzer rejects a request histogram whose bucket-count sum differs from
`read_req_latency_count`.

## 6. Claim Gates

A submit-gate priority claim requires all of the following:

- Both manifests pass and compare successfully.
- Module hashes and loaded variant identities are recorded.
- Request histogram exists and its sample sum matches the request count.
- p50/p95/p99/p999, maximum and `read_die_wait` are reported separately.
- The claimed mechanism has nonzero trigger/use counters.
- Baseline and treatment use identical data, seeds, queue settings and drain
  policy.
- Foreground and post-drain write-cost counters are both retained.
- Kernel tail contains no I/O completion, queue-full or hung-task failure.

Summarize repetitions with medians and ranges:

```bash
python3 evaluation/fio_summarize_latency_mixed.py \
  evaluation/result/fio_latency_mixed \
  -o evaluation/result/fio_latency_mixed/summary.tsv \
  --aggregate-output evaluation/result/fio_latency_mixed/summary_median_range.tsv
```

Repeat the same matrix with `FIO_GC_NAND_TIMING=0` for the no-background-
contention overhead gate. Output paths encode `bg0` versus `bg1`, and the
summarizer groups them separately.

Evaluate the explicit 10%/5%/3% and drain gates only after both matrices finish:

```bash
python3 evaluation/evaluate_latency_acceptance.py \
  evaluation/result/fio_latency_mixed/summary_median_range.tsv
```

For a hardware claim, add device documentation or a device-level experiment
showing the supported suspend/preemption behavior. The simulator result alone
is insufficient.

## 7. Local Verification

```bash
python3 nvmevirt_DA/tests/test_latency_static_invariants.py
python3 evaluation/sqlite/test_analyze_latency_run.py
python3 evaluation/sqlite/test_analyze_latency_verdict.py
python3 evaluation/test_research_run_manifest.py
bash -n evaluation/fio_fragment_die_latency_mixed.sh
bash -n evaluation/sqlite_fragment_die_test1_tablefile_pageflow_fileparallel_fullscan.sh
```

The macOS development host can run these static/model tests, but it cannot
validate the kernel module. Module build/load and runtime stress remain required
on the target Linux experiment host.
