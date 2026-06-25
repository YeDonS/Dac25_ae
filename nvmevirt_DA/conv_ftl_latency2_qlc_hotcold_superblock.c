// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ablation: latency2 scheduler + QLC hot/cold placement.
 *
 * Keeps the latency2 per-die idle/demand scheduler unchanged and enables only
 * QLC hot/cold page-type placement. QLC internal rebalance stays disabled.
 */
#define NVMEV_ENABLE_QLC_HOTCOLD 1
#define NVMEV_ENABLE_QLC_REBALANCE 0
#define NVMEV_TEST_PHASE_QLC_REBALANCE_ENABLE 0

#include "conv_ftl_latency2_superblock.c"
