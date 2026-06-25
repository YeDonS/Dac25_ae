# fast_24 Conversation Audit And Recoverable Handoff

私有项目材料。不要提交到公开能力包，不要复制原始对话、绝对路径或访问凭据。

## Goal Brief

| Field | Value |
|---|---|
| Outcome | 用可复现证据解释 latency1/2/3 的 request-level latency 与 write amplification 差异，并定义物理可信的调度模型边界。 |
| In scope | SQLite/FIO workload、HP/LP timing model、V2 per-die maintenance、QLC placement/rebalance、结果采集与对齐。 |
| Out of scope | 未经设备资料支持的真实 NAND preemption claim；只靠汇总吞吐的机制归因。 |
| Deliverables | 干净消融矩阵、指标字典、机制计数、模型语义测试、Linux module build/run record、论文可用结论。 |
| Completion evidence | 同配置 baseline/treatment、compile flags、seed、drain state、request histogram 和机制 counters 完整可追溯。 |

## Conversation Inventory

| Session | Date | Scope | Durable Artifact | Status |
|---|---|---|---|---|
| `019e81a6-39a1-7210-bc17-dde36ce9f788` | 2026-06-01 | SQLite latency summaries and metric semantics | `analysis_sqlite_latency_20260606/summary.{md,csv}` | Useful, but variants contain confounders |
| `019e81d4-6dc0-7b31-b4bb-7e34de6e8cda` | 2026-06-01 | FIO mixed workload, V2/GC behavior, wrapper build semantics | FIO scripts, aggregate/post-drain stats | Critical design decisions unresolved |
| `019e8e74-3ad9-73c1-b18f-943d434a4aed` | 2026-06-04 | Figure deck revisions | Slide-3 PPTX output | Delivery complete; do not re-open during scheduler diagnosis |
| `019ea405-e860-74c1-8538-f9f45579efa9` | 2026-06-08 | SQLite test phase, host request vs SQL metrics, CPU/workqueue, HP/LP tails | `LATENCY_QLC_ABLATIONS.md`, static tests, analyzer | Main diagnostic session; module build remains unverified |

## Verified State

| Fact | Evidence | Status |
|---|---|---|
| SQLite full-scan latency samples and FTL host-request counts are different metrics. | Conversation evidence plus analyzer/documentation | Verified |
| `cpus=` has a dispatcher role and I/O-worker roles; SQLite/FIO thread count does not equal FTL dispatcher count. | `LATENCY_QLC_ABLATIONS.md` and code path audit | Verified for current implementation |
| Submitted LUN/channel/PCIe work is non-preemptible and uses one committed tail. | `ssd.c`, `ssd_die.c`, deterministic model tests | Verified locally; kernel build pending |
| Read priority acts at the background submit gate, not through NAND preemption. | Worker yield/requeue code and single-tail model | Verified simulator contract |
| QLC placement/rebalance/repromotion settings can confound scheduler comparisons. | SQLite summary notes and FIO session audit | Verified |
| Current host lacks the matching kernel build tree, so recent kernel-module changes were not compiled/run locally. | Build attempt recorded in conversation | Verified environment blocker |

## Issue Register

| Priority | Issue | Evidence | Required Resolution |
|---|---|---|---|
| Resolved P0 | Old HP/LP queue deferral changed LP tails after returning completion timestamps. | Reproduced by deterministic model test. | Removed HP/LP tails; use non-preemptive submit gate. |
| P0 | Mechanism attribution is invalid when counters show zero yield/task execution but results improve. | Multiple FIO/SQLite discussions with zero or closed-window counters. | Require counter-gated claims: each claimed mechanism must have nonzero trigger/use counts and a matched ablation. |
| P0 | Treatment variants are not isolated. | QLC rebalance, hot/cold placement, repromotion and compile flags differ across logs. | Produce one manifest per run and reject comparisons with mismatched flags. |
| P1 | Metrics are mixed. | SQL samples, host request latency, `read_die_wait`, throughput and total drain time were repeatedly conflated. | Add a metric dictionary and report all layers separately. |
| Resolved P1 | Request metric and resource attribution were incomplete. | Test-phase histogram and LUN/channel/PCIe counters now exist. | Validate sample conservation and attribution on target runs. |
| P1 | Single-tail and submit-gate changes have static checks but no target kernel build/run validation. | Local host is macOS without kernel/FEMU. | Build on the experiment Linux kernel and run stress/concurrency tests. |
| P2 | Project is not a Git repository and artifacts are scattered. | Project inventory. | Initialize/version the code or create a source-of-truth repository before further semantic changes. |

## Decision And Assumption Log

| Item | Type | Current State | Reversal Condition |
|---|---|---|---|
| Use request-level latency as the scheduler primary metric. | User decision / method | Adopt; keep SQL latency as end-to-end secondary metric. | Only change if paper target requires application metric primary. |
| Reject HP/LP bypass and use one non-preemptive committed tail. | Model decision | Implemented. Legacy bypass counters must stay zero. | Only reverse with an explicit queued-command model that never changes returned completions. |
| Require strict compile-flag manifest and post-measure drain. | Method decision | Adopt for new benchmark runs. | None; this is a validity baseline. |
| Use `qlc_all_norp` wrappers only after wrapper compilation and loaded-module identity are logged. | Build decision | Adopt. | None. |
| Claim V2/yield benefit only with counter evidence. | Claim gate | Adopt. | A different independently measured causal path is demonstrated. |

## Repeated-Question Clusters

| Cluster | Canonical Answer | Evidence Needed Next | Do Not Repeat |
|---|---|---|---|
| “Why is latency2/3 not better?” | First check comparable manifests, submit-gate counters and per-layer background wait. If yield/task count is zero, that path cannot explain a change. | Flags, workload, read count, yield, force, V2 progress/backlog, LUN/channel wait and request p99/p999. | Do not tune quiet-window parameters before this table exists. |
| “What is a latency sample?” | SQL scan latency, host request latency, resource-tail wait, throughput and drain time answer different questions. | Metric dictionary attached to every result table. | Do not compare sample counts with raw host read counts. |
| “Do FIO/SQLite threads equal FTL concurrency?” | No. User threads produce host I/O; dispatcher/SQ/IO worker/workqueue and simulated resource tails determine FTL timing. | Run manifest with queue depth, jobs, module CPUs and read/write issue rate. | Do not infer FTL worker count from `THREAD_COUNTS`. |
| “Why did GC/write amplification change?” | It may be a scheduler effect, placement/rebalance/repromotion confounder, or post-measure work left undrained. | Matching compile flags plus foreground and post-drain counters. | Do not claim read scheduling reduced WA from one aggregate result. |
| “Does read priority preempt NAND?” | No. It defers not-yet-submitted background units; committed NAND/channel work remains ahead of later reads. | Single-tail tests and worker counters. | Do not describe submit gating as suspension/preemption. |

## Context Capsule

**Goal**: produce a physically bounded, counter-attributed evaluation of latency1/2/3.

**Do first**:
1. Freeze one 4-variant manifest: baseline, placement-only, V2 treatment, read-priority treatment. Record all compile macros, module name/hash, workload seed, thread/job/queue settings and drain policy.
2. Run the one-LUN synthetic traces for LP-first, read-first and interleaved submission. Verify that submitted LP completion times never move and tail bypass stays zero.
3. Add request-level latency histogram and wait decomposition. A result without this cannot support a scheduler tail-latency claim.
4. Build/load on the actual experiment kernel, then rerun static and runtime stress tests.

**Do not do**:
- Do not tune `quiet window`, token gate or thread counts before the manifest/counter gate passes.
- Do not reopen completed slide-design work while diagnosing scheduler semantics.
- Do not turn an assistant explanation into a paper claim without code/log/device evidence.

## 2026-06-22 Implementation Update

| Item | Result |
|---|---|
| Model boundary | The prior queue-deferral model was rejected: it moved an LP tail after returning the old completion time. Diagnostics now emit `read_priority_model nonpreemptive_submit_gate`; one-LUN tests require immutable submitted completions. |
| Shared resources | LUN, channel and PCIe use one committed serial tail. Internal migration now consumes the flash channel but not PCIe. |
| Request metric | All raw request-latency histogram bucket bounds/counts are exported; analyzer validates sample conservation. |
| Controls | `latency1/2/3_norp` wrappers now enable the same request-latency and priority diagnostics. |
| Run identity | FIO and SQLite produce JSON manifests with module/artifact hashes, workload parameters and compile contracts. |
| Comparison gate | Manifest comparison rejects workload/model mismatches and non-passing compile contracts. |
| Remaining blocker | Kernel module build/load and stress validation still require the target Linux experiment host. |

Operational instructions: `docs/FAST24_RESEARCH_RUNBOOK.md`.
