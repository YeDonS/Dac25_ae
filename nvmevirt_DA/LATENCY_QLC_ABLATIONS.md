# Latency + QLC Ablation Variants

This matrix keeps the latency scheduler family as the primary axis and adds
QLC mechanisms as a second axis. It avoids using `conv_ftl_all_superblock.c` as
the only QLC comparison point because `all` mixes multiple mechanisms at once.

## Scheduler Axis

| Variant | Source | Scheduler Meaning |
| --- | --- | --- |
| `die_latency1_sb` | `conv_ftl_latency1_superblock.c` | V1 global/background SLC maintenance worker |
| `die_latency2_sb` | `conv_ftl_latency2_superblock.c` | latency1 plus V2 per-die idle/demand maintenance scheduling |
| `die_latency3_sb` | `conv_ftl_latency3_superblock.c` | latency1 plus read-priority host reads and read-window worker yield |

## CPU, Workqueue, And Timing Model

The `cpus=` module parameter does not make every listed CPU a foreground FTL
worker. The first CPU is used by `nvmev_dispatcher`; the remaining CPUs become
NVMeVirt I/O workers. Foreground FTL scheduling is primarily driven when the
dispatcher observes SQ doorbells and runs `conv_read()` / `conv_write()`. The
background migration workqueue is separate from these SQ entries.

All scheduler variants use an unbound background workqueue:

```c
alloc_workqueue("nvmev_bg_mig", WQ_UNBOUND | WQ_MEM_RECLAIM, 1)
```

`WQ_UNBOUND` means the kernel may run the background worker on CPUs outside the
dispatcher/I/O-worker binding. `max_active=1` means one work item at a time per
workqueue, not one die and not one migrated page. A single work item can still
walk many pages and touch many PPAs; each PPA updates the timing tail for its
own channel/LUN.

The important competition is the simulated SSD resource tail, not whether the
background worker is a host SQ entry:

| Variant | Foreground host read | Background migration/GC | Competition point |
| --- | --- | --- | --- |
| latency1/baseline | normal NAND tail | normal NAND tail | same `next_*_avail_time` |
| latency2/3 | single committed tail | submit-gated background work | same serialized LUN/channel tails; PCIe only for host DMA |

Host read latency is request-level latency. A single read request may issue
multiple flash-page reads across multiple dies. `conv_read()` reports the
maximum completion time across those NAND/channel/PCIe sub-operations, while
`read_die_wait` is only the portion spent waiting for a LUN/die tail.

Read priority defers background work before its next NAND operation is
submitted. Already submitted NAND/channel work is non-preemptible. Internal
migration consumes the flash channel but not PCIe; host DMA consumes PCIe. If
background work was submitted before the read window opened, or reads dominate
the resource tails, SQL-level latency can stay flat or regress even when yield
counters increase.

Timing tails are shared by the dispatcher and the unbound background worker.
`ssd.c` and `ssd_die.c` protect LUN/channel/PCIe timing-tail updates with
per-resource timing locks so that concurrent foreground/background scheduling
does not corrupt the committed serialized tails.

This timing rule is explicitly named `nonpreemptive_submit_gate` in test-phase
output. Legacy bypass counters must remain zero; worker yield/requeue and lower
resource wait are the causal evidence. See `docs/FAST24_RESEARCH_RUNBOOK.md`.

Request-level statistics include p50/p95/p99/p999 and all raw logarithmic
histogram bucket boundaries/counts. The analyzer checks that the bucket-count
sum equals `read_req_latency_count`; a mismatch invalidates the run.
Test-phase resource statistics separately report LUN, flash-channel and PCIe
wait. Each layer labels the direct committed-tail predecessor as host read,
background, or `other`; this is queue attribution, not physical preemption.

## QLC Mechanism Axis

| QLC Class | Meaning | Enabled Macros |
| --- | --- | --- |
| base | No QLC hot/cold placement or in-QLC rebalance | `QLC_HOTCOLD=0`, `QLC_REBALANCE=0` |
| qlc_hotcold | QLC hot/cold page-type placement only | `QLC_HOTCOLD=1`, `QLC_REBALANCE=0` |
| qlc_all | QLC hot/cold placement plus internal QLC rebalance | `QLC_HOTCOLD=1`, `QLC_REBALANCE=1` |

## New Source Files

| Build Variant | Source File | Classification |
| --- | --- | --- |
| `die_latency1_qlc_hotcold_sb` | `conv_ftl_latency1_qlc_hotcold_superblock.c` | latency1 + qlc_hotcold |
| `die_latency2_qlc_hotcold_sb` | `conv_ftl_latency2_qlc_hotcold_superblock.c` | latency2 + qlc_hotcold |
| `die_latency3_qlc_hotcold_sb` | `conv_ftl_latency3_qlc_hotcold_superblock.c` | latency3 + qlc_hotcold |
| `die_latency1_qlc_all_sb` | `conv_ftl_latency1_qlc_all_superblock.c` | latency1 + qlc_all |
| `die_latency2_qlc_all_sb` | `conv_ftl_latency2_qlc_all_superblock.c` | latency2 + qlc_all |
| `die_latency3_qlc_all_sb` | `conv_ftl_latency3_qlc_all_superblock.c` | latency3 + qlc_all |

Each new file is a thin wrapper that defines the QLC feature macros and then
includes the corresponding latency source. This keeps scheduler code aligned
with latency1/2/3 and makes the ablation delta explicit.

## Recommended Experiment Sets

Scheduler-only baseline:

```bash
VARIANTS="die_latency1_sb die_latency2_sb die_latency3_sb"
```

QLC placement effect:

```bash
VARIANTS="die_latency1_sb die_latency1_qlc_hotcold_sb die_latency2_sb die_latency2_qlc_hotcold_sb die_latency3_sb die_latency3_qlc_hotcold_sb"
```

QLC internal migration effect:

```bash
VARIANTS="die_latency1_qlc_hotcold_sb die_latency1_qlc_all_sb die_latency2_qlc_hotcold_sb die_latency2_qlc_all_sb die_latency3_qlc_hotcold_sb die_latency3_qlc_all_sb"
```

Full latency QLC ablation build:

```bash
bash build_die.sh latency_qlc
```

Every new FIO/SQLite case emits a `*_manifest.json` file. Compare manifests
before comparing results:

```bash
python3 evaluation/research_run_manifest.py compare \
  path/to/baseline_manifest.json path/to/treatment_manifest.json
```
