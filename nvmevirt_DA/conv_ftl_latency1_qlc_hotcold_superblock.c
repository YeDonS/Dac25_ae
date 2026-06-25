// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ablation: latency1 scheduler + QLC hot/cold placement.
 *
 * Keeps the latency1 SLC maintenance/read-repromotion policy unchanged and
 * enables only QLC hot/cold page-type placement. QLC internal rebalance stays
 * disabled so this variant isolates placement from later in-QLC migration.
 */
#define NVMEV_ENABLE_QLC_HOTCOLD 1
#define NVMEV_ENABLE_QLC_REBALANCE 0
#define NVMEV_TEST_PHASE_QLC_REBALANCE_ENABLE 0

#include "conv_ftl_latency1_superblock.c"
