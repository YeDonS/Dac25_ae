// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ablation: latency1 scheduler + QLC hot/cold placement only.
 * Disable both QLC->SLC read repromotion and in-QLC rebalance for the whole run.
 * This is the placement-only control: latency1 intentionally has no
 * read-priority yield/catch-up policy.
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

#include "conv_ftl_latency1_superblock.c"
