// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ablation: current latency1 superblock baseline with read repromotion disabled.
 * Keep all other latency1_sb placement/scheduling defaults aligned.
 */
#define NVMEV_ENABLE_QLC_HOTCOLD 0
#define NVMEV_ENABLE_QLC_REBALANCE 0
#define NVMEV_ENABLE_READ_REPROMOTION 0
#define NVMEV_ENABLE_DIE_BATCHED_REPROMOTION 0
#define NVMEV_TEST_PHASE_REPROMOTION_ENABLE 0
#define NVMEV_TEST_PHASE_QLC_REBALANCE_ENABLE 0
#define NVMEV_TEST_PHASE_READ_REQ_LATENCY_STATS 1
#define NVMEV_TEST_PHASE_READ_PRIORITY_DIAG 1

#include "conv_ftl_latency1_superblock.c"
