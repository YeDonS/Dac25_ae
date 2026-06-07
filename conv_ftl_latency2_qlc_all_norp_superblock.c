// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ablation: latency2 scheduler + QLC hot/cold placement only.
 * Disable both QLC->SLC read repromotion and in-QLC rebalance for the whole run.
 * Read-priority forced progress is explicit here. In this qlc_all_norp
 * variant repromotion and QLC rebalance are disabled, so the remaining
 * read-priority debt is SLC maintenance worker opportunities. Requeue a
 * skipped opportunity immediately and repay it with one forced catch-up run
 * before allowing another read-priority debt burst to accumulate. Keep V2 from
 * pre-cleaning many closed SBs at once: only one maintenance SB may be in
 * MIG/GC_RDY/GC phase at a time for this ablation.
 */
#define NVMEV_ENABLE_QLC_HOTCOLD 1
#define NVMEV_ENABLE_QLC_REBALANCE 0
#define NVMEV_TEST_PHASE_QLC_REBALANCE_ENABLE 0
#define NVMEV_ENABLE_READ_REPROMOTION 0
#define NVMEV_ENABLE_DIE_BATCHED_REPROMOTION 0
#define NVMEV_TEST_PHASE_REPROMOTION_ENABLE 0
#ifndef NVMEV_LATENCY2_REQUEUE_DELAY_US
#define NVMEV_LATENCY2_REQUEUE_DELAY_US 0U
#endif
#ifndef NVMEV_LATENCY2_FORCE_AFTER_YIELDS
#define NVMEV_LATENCY2_FORCE_AFTER_YIELDS 1U
#endif
#ifndef NVMEV_LATENCY2_FORCE_CATCHUP_MAX
#define NVMEV_LATENCY2_FORCE_CATCHUP_MAX 1U
#endif
#ifndef NVMEV_LATENCY2_MAX_INFLIGHT_MAINT_SBS
#define NVMEV_LATENCY2_MAX_INFLIGHT_MAINT_SBS 1U
#endif

#include "conv_ftl_latency2_superblock.c"
