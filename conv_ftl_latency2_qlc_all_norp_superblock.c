// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ablation: latency2 scheduler + QLC hot/cold placement only.
 * Disable both QLC->SLC read repromotion and in-QLC rebalance for the whole run.
 * Read-priority forced progress is explicit here: after eight skipped
 * maintenance opportunities, latency2 runs bounded catch-up passes.
 */
#define NVMEV_ENABLE_QLC_HOTCOLD 1
#define NVMEV_ENABLE_QLC_REBALANCE 0
#define NVMEV_TEST_PHASE_QLC_REBALANCE_ENABLE 0
#define NVMEV_ENABLE_READ_REPROMOTION 0
#define NVMEV_ENABLE_DIE_BATCHED_REPROMOTION 0
#define NVMEV_TEST_PHASE_REPROMOTION_ENABLE 0
#ifndef NVMEV_LATENCY2_FORCE_AFTER_YIELDS
#define NVMEV_LATENCY2_FORCE_AFTER_YIELDS 8U
#endif
#ifndef NVMEV_LATENCY2_FORCE_CATCHUP_MAX
#define NVMEV_LATENCY2_FORCE_CATCHUP_MAX NVMEV_LATENCY2_FORCE_AFTER_YIELDS
#endif

#include "conv_ftl_latency2_superblock.c"
