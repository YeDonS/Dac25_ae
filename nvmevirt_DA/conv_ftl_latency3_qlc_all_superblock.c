// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ablation: latency3 scheduler + QLC hot/cold placement + in-QLC rebalance.
 *
 * This is the QLC-enhanced latency3 point. It keeps latency3 read-priority
 * scheduling unchanged while enabling both QLC mechanisms.
 */
#define NVMEV_ENABLE_QLC_HOTCOLD 1
#define NVMEV_ENABLE_QLC_REBALANCE 1
#define NVMEV_TEST_PHASE_QLC_REBALANCE_ENABLE 1

#include "conv_ftl_latency3_superblock.c"
