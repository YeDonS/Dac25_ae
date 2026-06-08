// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ablation: latency2 scheduler + QLC hot/cold placement only.
 * Disable both QLC->SLC read repromotion and in-QLC rebalance for the whole run.
 * Read-priority forced progress is explicit here. In this qlc_all_norp
 * variant repromotion and QLC rebalance are disabled, so the remaining
 * read-priority debt is SLC maintenance worker opportunities. Requeue skipped
 * opportunities immediately, force progress after eight consecutive yields,
 * and repay up to eight maintenance passes. V2 may keep multiple closed SBs in
 * MIG/GC_RDY/GC phase without an explicit in-flight SB cap so this run measures
 * the stronger per-die dispatcher.
 */
#define NVMEV_ENABLE_QLC_HOTCOLD 1
#define NVMEV_ENABLE_QLC_REBALANCE 0
#define NVMEV_TEST_PHASE_QLC_REBALANCE_ENABLE 0
#define NVMEV_ENABLE_READ_REPROMOTION 0
#define NVMEV_ENABLE_DIE_BATCHED_REPROMOTION 0
#define NVMEV_TEST_PHASE_REPROMOTION_ENABLE 0
#define NVMEV_SLC_GC_REQUIRE_COMPLETE_MIGRATION 1
#define NVMEV_TEST_PHASE_READ_REQ_LATENCY_STATS 1
#define NVMEV_TEST_PHASE_READ_PRIORITY_DIAG 1
#ifndef NVMEV_LATENCY2_REQUEUE_DELAY_US
#define NVMEV_LATENCY2_REQUEUE_DELAY_US 0U
#endif
#ifndef NVMEV_LATENCY2_FORCE_AFTER_YIELDS
#define NVMEV_LATENCY2_FORCE_AFTER_YIELDS 8U
#endif
#ifndef NVMEV_LATENCY2_FORCE_CATCHUP_MAX
#define NVMEV_LATENCY2_FORCE_CATCHUP_MAX 8U
#endif
#ifndef NVMEV_LATENCY2_READ_WINDOW_GATE_TOKENS
#define NVMEV_LATENCY2_READ_WINDOW_GATE_TOKENS 64U
#endif
#ifndef NVMEV_LATENCY2_MAX_INFLIGHT_MAINT_SBS
#define NVMEV_LATENCY2_MAX_INFLIGHT_MAINT_SBS 0U
#endif

#include "conv_ftl_latency2_superblock.c"
