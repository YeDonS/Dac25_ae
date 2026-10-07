# QLC Background Maintenance And Read Priority Audit

Status: local source/static audit complete for the scheduler-model correction;
Linux module build/load and performance gates remain unverified.

## Request And Completion Path

1. The dispatcher observes an SQ doorbell and `nvmev_proc_io_sq()` creates an
   NVMeVirt request (`io.c:634`).
2. `__enqueue_io_req()` records `nsecs_start`, invokes the namespace FTL and
   inserts the result into the completion list ordered by `nsecs_target`
   (`io.c:219-319`).
3. `conv_proc_nvme_io_cmd()` dispatches to `conv_read()` or `conv_write()` in
   the selected latency source. A host read can emit several NAND reads; its
   request completion is the maximum sub-operation completion.
4. `ssd_advance_nand_internal()` serializes NAND on the target LUN, then uses
   the flash channel for read data or write data. Host DMA can additionally use
   PCIe. Internal migration uses LUN plus channel and does not use PCIe.
5. The I/O worker emits the CQ entry only after the simulated target time and
   any writeback/PCIe guard (`io.c:747-905`).

Background repromotion, QLC rebalance and SLC maintenance run on an unbound,
single-active workqueue. They are not SQ requests. Latency2 additionally splits
an SLC superblock into per-die migration/GC tasks and tracks generation/phase;
latency3 uses the V1 maintenance path with read-window worker gating.

## Variant Contract

| Variant | Mechanisms |
| --- | --- |
| latency1 | global V1 asynchronous SLC maintenance; normal serialized resource submission |
| latency2 | latency1 plus per-die FIFO, idle/slack and host-demand selection, bounded in-flight SB count, read-window yield and forced progress |
| latency3 | latency1 plus read-window yield/requeue for repromotion, QLC rebalance and SLC maintenance, with forced progress |

QLC wrappers only select hot/cold placement, rebalance and repromotion macros;
the scheduler implementation remains the included latency1/2/3 source. The
canonical `qlc_all_norp` comparison disables repromotion/rebalance so scheduler
effects are not mixed with placement-induced NAND work.

## Correct Priority Model

Each LUN, channel and PCIe link has one committed serial tail. Once a NAND or
transfer reservation is submitted, its completion timestamp escapes to the
caller and is immutable. Read priority therefore operates only at the worker
submit gate:

- active host reads, per-die demand and the simulated read window can defer the
  next background unit;
- yielded work is requeued, not dropped;
- forced progress admits bounded work after repeated yields;
- a read waits behind background NAND/channel work that was already submitted.

Internal migration cannot block PCIe because no host DMA occurs, but it must
and now does block its flash channel. Host writes can compete at LUN, channel
and PCIe according to their normal data path.

## Findings

### Correct behavior retained

- Host request latency is recorded only during the explicit test phase and has
  count/sum, p50/p95/p99/p999/max plus recomputable histogram buckets.
- Completion entries are ordered by simulated target time; delayed completion
  guards reinsert rather than complete early.
- Latency2 task generation/phase checks reject stale tasks, while requeue paths
  preserve unfinished tasks.
- Forced progress suppresses nested yield checks and caps catch-up work.
- Manifest comparison rejects model, workload and compile-contract mismatch.

### Design defects corrected

- The previous HP/LP rule started a read before an already submitted LP tail,
  then moved the LP tail later. The LP caller retained its old completion time,
  creating overlapping hardware intervals and an incorrect completion. This
  was queue-level time travel, not NAND preemption.
- Independent HP/LP fields were removed. Normal, low-priority and read-priority
  APIs now commit to the same non-preemptive tail.
- GC/migration reads and writes previously skipped the channel model. They now
  consume channel bandwidth while continuing to skip PCIe.
- The old priority channel/PCIe branches used fixed transfer latency and
  bypassed the shared credit model. All classes now call the same
  `chmodel_request()` path after serial-tail admission.

### Observability added

- LUN, channel and PCIe export conflict count and cumulative wait during the
  test phase.
- Each layer splits direct-predecessor attribution into read/read and
  read/background. The analyzer reports the remainder as `other` (primarily
  host-write blocking), avoiding false background attribution.
- Latency2 exports current and maximum per-die maintenance task backlog in
  addition to done/requeued/yielded/stale/no-progress counters.

### Remaining implementation gaps (deep source audit, 2026-06-23)

The following are correctness blockers for a claim that latency2/latency3
provide bounded, read-prioritized background submission.  They are source
findings, not performance conclusions; no FEMU experiment has been run for
them.

1. **Latency3 normal SLC migration is now one page per submit-gated worker
   pass.**  A complete page migration retains its dependent NAND read/write
   sequence as the non-preemptible unit.  Once that unit commits, the worker
   uses delayed requeue so the next page must pass a fresh read gate.  Forced
   progress is capped by `NVMEV_LATENCY3_FORCE_UNITS_PER_RUN=1`, rather than
   treating an entire superblock pass as one unit.

2. **QLC rebalance is now also page-granular in latency2 and latency3.**
   `qlc_maybe_rebalance_internal(..., 1)` submits at most one promote or
   demote migration and reports unfinished work for delayed requeue.  This
   removes the previous full promote/demote budget reservation after a single
   entry check.

3. **Latency2's V2 splitter still covers only SLC migration.**
   `migrate_sb_die_portion()` has a per-page `die_has_host_demand()` check and
   requeues from `MAINT_TASK_YIELDED`, which is the right structural direction.
   The V2 GC task still scans/rescues all residual valid pages before its erase
   without a yield boundary.  Consequently the mechanism is still
   heterogeneous: migration and rebalance pages are bounded, while a
   residual-GC scan is not.  A tail-latency claim must not describe that as one
   uniform priority scheduler.

4. **SLC GC remains the unclosed boundary.**  Both latency3's V1 GC and
   latency2's residual-GC recovery can submit all remaining valid-page copies
   and erases for a selected victim after GC has started.  Existing NAND work
   must remain non-preemptible, but the GC state machine still needs an
   explicit per-page/per-erase cursor before the read-priority claim can cover
   this path.

5. **The read-priority token is consumed by the yield predicate, not by a
   committed deferral.**  `latency[23]_read_priority_read_window_active()`
   consumes a token before its caller records/requeues a background unit.
   This makes `token_empty` a rate-limit diagnostic, not an accounting of
   deferred NAND work.  It cannot by itself show that a read avoided a
   resource conflict.  V2 migration instead uses `die_has_host_demand()` and
   does not consume this token at its per-page boundary, further separating
   token metrics from the actual V2 scheduling decision.

6. **Latency3 has operation/progress counters but no single exported
   current/max backlog across all worker types.**  Its remaining queued
   repromotion/rebalance/maintenance work cannot yet be proved bounded from a
   single post-drain metric.

Required correction before performance validation: split GC into complete page
copy and erase units with persisted victim/cursor state; check the gate only
between such units; and export a unified latency3 backlog.  Do not interrupt
the middle of a page migration, because that would turn a scheduler correction
into an FTL consistency risk.

### Statistical defects

- Legacy `read_lp_bypass_*` and global bypass counters described the invalid
  model. They are retained for output compatibility but must be zero under
  `nonpreemptive_submit_gate` and cannot establish mechanism activity.
- Existing old logs were generated before this model correction and cannot
  validate the new scheduler. SQLite full-scan latency is application-level and
  cannot substitute for host request latency.

### Experimental defects

- Available old results do not contain three comparable repetitions of the new
  model, a loaded-module hash/contract, and post-drain proof together. The FIO
  runner now defaults to three indexed repetitions with a deterministic
  per-trial seed shared by all variants, and separates `bg1` contention from
  the `bg0` no-background-timing control.
- The FIO summarizer previously accepted `*_manifest.json` as a result JSON,
  producing zero-valued pseudo-runs from dry-run manifests. It now excludes
  manifests and fails when no actual fio JSON exists.
- The local host is macOS without Linux kernel headers, NVMeVirt/FEMU device,
  `fio`, or `nvme`; module build/load and contention experiments are impossible
  locally.

## Explanation Of Earlier Symptoms

- `yields=0`: the gate was closed, the worker checked outside the simulated
  read window, tokens were exhausted, or background work had already been
  submitted. A zero trigger/use count cannot explain a latency change.
- `bypass=0`: either there was no queued LP tail in the old model or the read
  path did not use it. In the corrected model zero is mandatory because
  submitted work is non-preemptible.
- mechanism counters nonzero but read latency unchanged: worker control can
  trigger after the expensive background unit is submitted, reads can contend
  mostly with reads, or channel/service time can dominate LUN wait.
- latency2 regression: per-die task splitting, scans, stale/requeue churn and
  forced catch-up add CPU and maintenance work. Without a reduction in request
  LUN/channel wait, those costs are overhead rather than a causal benefit.

## Evidence Required For Acceptance

For each run retain the source/module hash, loaded variant, macros, workload,
seed, initialization artifact hashes, test-phase boundaries and drain policy.
Run at least three repetitions and compare medians plus ranges. Acceptance
requires request p99 and p999 improvement, bounded mean/throughput cost,
matching lower resource wait/background conflict, zero illegal bypass,
bounded backlog and final backlog zero after drain. No-background runs must be
reported separately from controlled read/background contention.

`fio_summarize_latency_mixed.py` emits per-run and median/min/max tables;
`evaluate_latency_acceptance.py` enforces the 10% tail, 5% foreground, 3%
no-contention, background-wait and post-drain backlog gates.
