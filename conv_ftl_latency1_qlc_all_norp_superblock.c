// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ablation: latency1 scheduler + QLC hot/cold placement only.
 * Disable both QLC->SLC read repromotion and in-QLC rebalance for the whole run.
 */
#define NVMEV_ENABLE_QLC_HOTCOLD 1
#define NVMEV_ENABLE_QLC_REBALANCE 0
#define NVMEV_TEST_PHASE_QLC_REBALANCE_ENABLE 0
#define NVMEV_ENABLE_READ_REPROMOTION 0
#define NVMEV_ENABLE_DIE_BATCHED_REPROMOTION 0
#define NVMEV_TEST_PHASE_REPROMOTION_ENABLE 0

#include "conv_ftl_latency1_superblock.c"
