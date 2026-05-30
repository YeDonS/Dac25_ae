// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ablation: latency3 scheduler + QLC hot/cold placement + in-QLC rebalance,
 * with QLC->SLC read repromotion disabled for the whole run.
 */
#define NVMEV_ENABLE_QLC_HOTCOLD 1
#define NVMEV_ENABLE_QLC_REBALANCE 1
#define NVMEV_TEST_PHASE_QLC_REBALANCE_ENABLE 1
#define NVMEV_ENABLE_READ_REPROMOTION 0
#define NVMEV_ENABLE_DIE_BATCHED_REPROMOTION 0
#define NVMEV_TEST_PHASE_REPROMOTION_ENABLE 0
#define NVMEV_QLC_REBALANCE_PERIOD_READS 128U
#define QLC_REBALANCE_SCAN_LIMIT 8192U

#include "conv_ftl_latency3_superblock.c"
