// SPDX-License-Identifier: GPL-2.0-only


#include <linux/sched/clock.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/atomic.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/math64.h>
#include <linux/ctype.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/moduleparam.h>
#include <linux/workqueue.h>

#include "nvmev.h"
#include "conv_ftl.h"
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#ifndef NVMEV_WARN
#define NVMEV_WARN(fmt, ...) pr_warn("[nvmev] " fmt, ##__VA_ARGS__)
#endif

#define NVMEV_LPN_HASH_BITS 12
struct nvmev_lpn_inflight {
	struct hlist_node hnode;
	u64 lpn;
	u32 count;
	u16 last_cid;
	u16 last_sqid;
};

static DEFINE_HASHTABLE(nvmev_lpn_ht, NVMEV_LPN_HASH_BITS);
static DEFINE_SPINLOCK(nvmev_lpn_lock);
static bool nvmev_lpn_track = true;
module_param_named(lpn_track, nvmev_lpn_track, bool, 0644);
MODULE_PARM_DESC(lpn_track, "Detect concurrent writes to same LPN (default on)");

static unsigned long long nvmev_ftl_slow_ns;
module_param_named(ftl_slow_ns, nvmev_ftl_slow_ns, ullong, 0644);
MODULE_PARM_DESC(ftl_slow_ns, "Log FTL command time if above threshold (ns), 0 disables");

void enqueue_writeback_io_req(int sqid, unsigned long long nsecs_target,
			      struct buffer *write_buffer, unsigned int buffs_to_release);

#ifndef NVMEV_ENABLE_CHAIN_AGGREGATION
#define NVMEV_ENABLE_CHAIN_AGGREGATION 1
#endif
#ifndef NVMEV_ENABLE_HOST_DIE_HINT
#define NVMEV_ENABLE_HOST_DIE_HINT 1
#endif
#ifndef NVMEV_ENABLE_QLC_HOTCOLD
#define NVMEV_ENABLE_QLC_HOTCOLD 0
#endif
#ifndef NVMEV_ENABLE_READ_REPROMOTION
#define NVMEV_ENABLE_READ_REPROMOTION 1
#endif
#ifndef NVMEV_ENABLE_INTERNAL_DIE_AFFINITY
#define NVMEV_ENABLE_INTERNAL_DIE_AFFINITY 0
#endif
#ifndef NVMEV_ENABLE_DIE_BATCHED_REPROMOTION
#define NVMEV_ENABLE_DIE_BATCHED_REPROMOTION 0
#endif
#ifndef NVMEV_ENABLE_QLC_REBALANCE
#define NVMEV_ENABLE_QLC_REBALANCE 0
#endif
#ifndef NVMEV_ENABLE_CHAIN_BLOCK_REPROMOTION
#define NVMEV_ENABLE_CHAIN_BLOCK_REPROMOTION 1
#endif
/* Variant: chain/block aggregation enabled with host append/overwrite die hint retained.
 * Superblock variant: free-line accounting is reported and thresholded at one
 * block-id stripe across all dies. Chain-private SLC writers are capped to
 * NVMEV_SUPERBLOCK_ACTIVE_LIMIT concurrently open superblocks.
 */

#define NVMEV_SUPERBLOCK_ACCOUNTING 1
#define NVMEV_SUPERBLOCK_ACTIVE_LIMIT 14U

/* superblock state machine.
 * NVMEV_SB_FREE:   not yet opened for writes (or just-erased)
 * NVMEV_SB_ACTIVE: open for writes; some dies may already be filled (die_full_mask)
 * NVMEV_SB_CLOSED: every die's portion is full; only readable until GC erases it.
 * On erase, transitions back to NVMEV_SB_FREE.
 */
enum slc_sb_state_e {
	NVMEV_SB_FREE = 0,
	NVMEV_SB_ACTIVE = 1,
	NVMEV_SB_CLOSED = 2,
};

/* chain-triggered QLC->SLC repromotion. Per-chain host read counter; when it
 * crosses REPROMOTE_READ_TRIGGER we kick the bg worker to scan that chain's
 * QLC pages and rewrite the hot ones back to SLC in a per-chain batch. */
#define REPROMOTE_READ_TRIGGER 1024U
#define REPROMOTE_BATCH_PAGES 512U
#define REPROMOTE_HEAT_FLOOR 4U
#define REPROMOTE_SCAN_WINDOW 65536U
#define REPROMOTE_CHAINS_PER_RUN 16U
#define QLC_CLOSED_REPROMOTE_TRIGGER 10U

#define RECENT_WRITE_GUARD_PCT 10U
#define SLC_EMERGENCY_RESERVE 10
#define INVALID_CHAIN_ID U32_MAX
#define INVALID_CHAIN_DIE 0xFFU
#define SLC_MIGRATE_FREE_PCT 15U
#define SLC_MIGRATE_TARGET_FREE_PCT 10U
#define SLC_SOFT_GC_FREE_PCT 10U
#define SLC_HARD_GC_FREE_PCT 5U
#define SLC_REPROMOTE_GUARD_FREE_PCT 5U
#define SLC_SB_RECENT_GUARD_PCT 10U
/* QLC GC trigger: kick in when QLC free drops to this fraction of total.
 * Per the design we want QLC GC to start earlier so chain repacking and
 * opportunistic SLC repromotion have more headroom to work with. */
#define QLC_GC_FREE_PCT 30U
#define QLC_FAST_HIGH_WM_PCT 90U
#define QLC_FAST_TARGET_WM_PCT 80U
#define QLC_PROMOTE_RATIO_NUM 1U
#define QLC_PROMOTE_RATIO_DEN 1U
#define QLC_REBALANCE_SCAN_LIMIT 4096U
#define CHAIN_COLD_MIN_BATCH_PAGES 2U
#define CHAIN_COLD_BLOCK_COLD_PCT 80U
#define CHAIN_COLD_CHUNK_COLD_PCT 50U
#define CHAIN_HOT_REPROMOTE_OWNER_PCT 90U
#define CHAIN_HOT_REPROMOTE_BLOCK_HOT_PCT 90U
#define CHAIN_HOT_REPROMOTE_CHUNK_HOT_PCT 80U
#define CHAIN_HOT_REPROMOTE_RATIO_NUM 1U
#define CHAIN_HOT_REPROMOTE_RATIO_DEN 1U
#define CHAIN_HOT_REPROMOTE_SCAN_BLOCKS 2048U
#define CHAIN_HOT_REPROMOTE_MIN_PAGES 2U
#define NVMEV_EVENT_LOG_CAP 256U

struct nvmev_cmd_debug {
	bool valid;
	u8 opcode;
	u32 nsid;
	u64 slba;
	u32 len;
	int sqid;
	u64 ts;
};

static void nvmev_lpn_mark(u64 lpn, u16 sqid, u16 cid)
{
	struct nvmev_lpn_inflight *entry;
	unsigned long flags;

	spin_lock_irqsave(&nvmev_lpn_lock, flags);
	hash_for_each_possible(nvmev_lpn_ht, entry, hnode, lpn) {
		if (entry->lpn == lpn) {
			entry->count++;
			if (printk_ratelimit()) {
				NVMEV_ERROR("lpn overlap: lpn=%llu prev sqid=%u cid=%u new sqid=%u cid=%u count=%u\n",
					    lpn, entry->last_sqid, entry->last_cid, sqid, cid,
					    entry->count);
			}
			entry->last_sqid = sqid;
			entry->last_cid = cid;
			spin_unlock_irqrestore(&nvmev_lpn_lock, flags);
			return;
		}
	}
	spin_unlock_irqrestore(&nvmev_lpn_lock, flags);

	entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		return;
	entry->lpn = lpn;
	entry->count = 1;
	entry->last_sqid = sqid;
	entry->last_cid = cid;

	spin_lock_irqsave(&nvmev_lpn_lock, flags);
	hash_add(nvmev_lpn_ht, &entry->hnode, lpn);
	spin_unlock_irqrestore(&nvmev_lpn_lock, flags);
}

static void nvmev_lpn_unmark(u64 lpn)
{
	struct nvmev_lpn_inflight *entry;
	unsigned long flags;

	spin_lock_irqsave(&nvmev_lpn_lock, flags);
	hash_for_each_possible(nvmev_lpn_ht, entry, hnode, lpn) {
		if (entry->lpn == lpn) {
			if (entry->count > 1) {
				entry->count--;
				spin_unlock_irqrestore(&nvmev_lpn_lock, flags);
				return;
			}
			hash_del(&entry->hnode);
			spin_unlock_irqrestore(&nvmev_lpn_lock, flags);
			kfree(entry);
			return;
		}
	}
	spin_unlock_irqrestore(&nvmev_lpn_lock, flags);
}

static void nvmev_lpn_mark_range(u64 start_lpn, u64 end_lpn, u16 sqid, u16 cid)
{
	u64 lpn;

	if (!nvmev_lpn_track)
		return;

	for (lpn = start_lpn; lpn <= end_lpn; lpn++)
		nvmev_lpn_mark(lpn, sqid, cid);
}

static void nvmev_lpn_unmark_range(u64 start_lpn, u64 end_lpn)
{
	u64 lpn;

	if (!nvmev_lpn_track)
		return;

	for (lpn = start_lpn; lpn <= end_lpn; lpn++)
		nvmev_lpn_unmark(lpn);
}

static DEFINE_PER_CPU(struct nvmev_cmd_debug, nvmev_last_cmd);

struct nvmev_maptbl_debug {
	const char *site;
	u64 lpn;
};

static DEFINE_PER_CPU(struct nvmev_maptbl_debug, nvmev_last_maptbl);

static inline void nvmev_set_maptbl_site(const char *site, u64 lpn)
{
	struct nvmev_maptbl_debug *dbg = this_cpu_ptr(&nvmev_last_maptbl);

	if (dbg) {
		dbg->site = site;
		dbg->lpn = lpn;
	}
}

static bool recent_write_guard(struct conv_ftl *conv_ftl, uint64_t lpn);
static inline uint64_t total_slc_pages(const struct conv_ftl *conv_ftl);
static inline uint32_t superblock_die_count(const struct conv_ftl *conv_ftl);
static inline uint32_t slc_pages_per_superblock(const struct conv_ftl *conv_ftl);
static inline uint32_t qlc_pages_per_superblock(const struct conv_ftl *conv_ftl);
static void slc_resident_track_page(struct conv_ftl *conv_ftl, uint64_t lpn, uint32_t die);
static void slc_resident_untrack_page(struct conv_ftl *conv_ftl, uint64_t lpn);
static bool slc_has_any_victim(struct conv_ftl *conv_ftl);
static bool qlc_has_any_victim(struct conv_ftl *conv_ftl);
static inline void decode_die(struct ssdparams *spp, uint32_t die,
			      uint32_t *ch, uint32_t *lun);

static inline void compute_line_distribution(uint32_t total_lines,
					     uint32_t *slc_lines,
					     uint32_t *qlc_lines)
{
	uint64_t numerator = (uint64_t)QLC_BLOCK_CAPACITY_FACTOR * SLC_LINE_RATIO_NUM;
	uint64_t denominator = (uint64_t)SLC_BLOCK_CAPACITY_FACTOR * QLC_LINE_RATIO_NUM +
			       (uint64_t)QLC_BLOCK_CAPACITY_FACTOR * SLC_LINE_RATIO_NUM;
	uint32_t slc = div_u64((uint64_t)total_lines * numerator, denominator);
	if (slc == 0)
		slc = 1;
	if (slc >= total_lines)
		slc = total_lines - 1;
	*slc_lines = slc;
	*qlc_lines = total_lines - slc;
}

static inline uint32_t blk_from_line(uint32_t line_id)
{
	return line_id;
}

static inline uint32_t line_from_blk(uint32_t blk_id)
{
	return blk_id;
}

enum chain_place_tier {
	CHAIN_TIER_SLC = 0,
	CHAIN_TIER_QLC = 1,
};

static inline bool chain_id_valid(struct conv_ftl *conv_ftl, uint32_t chain_id)
{
#if !NVMEV_ENABLE_CHAIN_AGGREGATION
	(void)conv_ftl;
	(void)chain_id;
	return false;
#else
	return conv_ftl && chain_id != INVALID_CHAIN_ID &&
	       chain_id < conv_ftl->next_chain_id &&
	       chain_id < conv_ftl->chain_capacity;
#endif
}

static inline uint32_t chain_alloc(struct conv_ftl *conv_ftl, uint16_t seed_die)
{
	uint32_t chain_id;

#if !NVMEV_ENABLE_CHAIN_AGGREGATION
	(void)conv_ftl;
	(void)seed_die;
	return INVALID_CHAIN_ID;
#endif
	if (!conv_ftl || !conv_ftl->chain_slc_next_die || !conv_ftl->chain_qlc_next_die ||
	    !conv_ftl->chain_slc_rr_pages || !conv_ftl->chain_qlc_rr_pages ||
	    conv_ftl->next_chain_id >= conv_ftl->chain_capacity)
		return INVALID_CHAIN_ID;

	chain_id = conv_ftl->next_chain_id++;
	if (conv_ftl->die_count)
		conv_ftl->chain_slc_next_die[chain_id] = seed_die % conv_ftl->die_count;
	else
		conv_ftl->chain_slc_next_die[chain_id] = 0;
	conv_ftl->chain_qlc_next_die[chain_id] = INVALID_CHAIN_DIE;
	conv_ftl->chain_slc_rr_pages[chain_id] = 0;
	conv_ftl->chain_qlc_rr_pages[chain_id] = 0;
	if (conv_ftl->chain_last_slc_touch)
		conv_ftl->chain_last_slc_touch[chain_id] = ktime_get_ns();
	return chain_id;
}

static inline uint32_t chain_id_for_lpn(struct conv_ftl *conv_ftl, uint64_t lpn)
{
	if (!conv_ftl || !conv_ftl->lpn_chain_id || lpn >= conv_ftl->ssd->sp.tt_pgs)
		return INVALID_CHAIN_ID;
	return conv_ftl->lpn_chain_id[lpn];
}

static inline uint8_t *chain_next_die_array(struct conv_ftl *conv_ftl,
					    enum chain_place_tier tier)
{
	if (!conv_ftl)
		return NULL;
	return (tier == CHAIN_TIER_SLC) ?
	       conv_ftl->chain_slc_next_die : conv_ftl->chain_qlc_next_die;
}

static inline uint8_t *chain_rr_pages_array(struct conv_ftl *conv_ftl,
					    enum chain_place_tier tier)
{
	if (!conv_ftl)
		return NULL;
	return (tier == CHAIN_TIER_SLC) ?
	       conv_ftl->chain_slc_rr_pages : conv_ftl->chain_qlc_rr_pages;
}

static inline uint8_t chain_seed_die(struct conv_ftl *conv_ftl, uint32_t chain_id,
				     enum chain_place_tier tier)
{
	uint32_t mix;

	if (!conv_ftl || !conv_ftl->die_count)
		return 0;

	mix = chain_id * 2654435761U;
	mix ^= (tier == CHAIN_TIER_SLC) ? 0x85ebca6bU : 0xc2b2ae35U;
	return (uint8_t)(mix % conv_ftl->die_count);
}

static inline uint32_t normalize_fallback_die(struct conv_ftl *conv_ftl, int32_t fallback_die)
{
	if (!conv_ftl || !conv_ftl->die_count)
		return 0;
	if (fallback_die < 0)
		return 0;
	return (uint32_t)fallback_die % conv_ftl->die_count;
}

static inline uint32_t chain_place_die(struct conv_ftl *conv_ftl, uint32_t chain_id,
				       enum chain_place_tier tier, int32_t fallback_die)
{
	uint8_t *next_die;

#if !NVMEV_ENABLE_CHAIN_AGGREGATION
	(void)conv_ftl;
	(void)chain_id;
	(void)tier;
	return normalize_fallback_die(conv_ftl, fallback_die);
#endif
	if (!chain_id_valid(conv_ftl, chain_id))
		return normalize_fallback_die(conv_ftl, fallback_die);

	next_die = chain_next_die_array(conv_ftl, tier);
	if (!next_die)
		return normalize_fallback_die(conv_ftl, fallback_die);

	if (next_die[chain_id] == INVALID_CHAIN_DIE)
		next_die[chain_id] = chain_seed_die(conv_ftl, chain_id, tier);

	return conv_ftl->die_count ? (next_die[chain_id] % conv_ftl->die_count) : 0;
}

static inline uint32_t chain_place_die_for_lpn(struct conv_ftl *conv_ftl, uint64_t lpn,
					       enum chain_place_tier tier,
					       int32_t fallback_die)
{
	return chain_place_die(conv_ftl, chain_id_for_lpn(conv_ftl, lpn),
			       tier, fallback_die);
}

static inline void chain_place_note_write(struct conv_ftl *conv_ftl, uint32_t chain_id,
					  enum chain_place_tier tier, uint32_t actual_die)
{
	struct ssdparams *spp;
	uint8_t *next_die;
	uint8_t *rr_pages;
	uint32_t unit;

#if !NVMEV_ENABLE_CHAIN_AGGREGATION
	(void)conv_ftl;
	(void)chain_id;
	(void)tier;
	(void)actual_die;
	return;
#endif
	if (!chain_id_valid(conv_ftl, chain_id) || !conv_ftl->die_count)
		return;

	next_die = chain_next_die_array(conv_ftl, tier);
	rr_pages = chain_rr_pages_array(conv_ftl, tier);
	if (!next_die || !rr_pages)
		return;

	spp = &conv_ftl->ssd->sp;
	actual_die %= conv_ftl->die_count;
	unit = spp->pgs_per_oneshotpg ? spp->pgs_per_oneshotpg : 1;
	if (unit > INVALID_CHAIN_DIE)
		unit = INVALID_CHAIN_DIE;

	/* Defensive reset in case unit/state changed before this chain becomes active. */
	if (rr_pages[chain_id] >= unit)
		rr_pages[chain_id] = 0;

	rr_pages[chain_id]++;
	if (rr_pages[chain_id] >= unit) {
		rr_pages[chain_id] = 0;
		next_die[chain_id] = (actual_die + 1) % conv_ftl->die_count;
	} else {
		next_die[chain_id] = actual_die;
	}
}

static inline void chain_place_note_lpn_write(struct conv_ftl *conv_ftl, uint64_t lpn,
					      enum chain_place_tier tier,
					      uint32_t actual_die)
{
	chain_place_note_write(conv_ftl, chain_id_for_lpn(conv_ftl, lpn),
			       tier, actual_die);
}

static inline uint64_t chain_slc_touch_now(struct conv_ftl *conv_ftl)
{
	if (!conv_ftl)
		return 0;
	return ktime_get_ns();
}

static inline void chain_slc_touch(struct conv_ftl *conv_ftl, uint32_t chain_id)
{
#if !NVMEV_ENABLE_CHAIN_AGGREGATION
	(void)conv_ftl;
	(void)chain_id;
#else
	if (!chain_id_valid(conv_ftl, chain_id) || !conv_ftl->chain_last_slc_touch)
		return;
	WRITE_ONCE(conv_ftl->chain_last_slc_touch[chain_id], chain_slc_touch_now(conv_ftl));
#endif
}

static inline uint32_t chain_slc_page_count_get(struct conv_ftl *conv_ftl, uint32_t chain_id)
{
#if !NVMEV_ENABLE_CHAIN_AGGREGATION
	(void)conv_ftl;
	(void)chain_id;
	return 0;
#else
	if (!chain_id_valid(conv_ftl, chain_id) || !conv_ftl->chain_slc_page_count)
		return 0;
	return READ_ONCE(conv_ftl->chain_slc_page_count[chain_id]);
#endif
}

static inline void chain_slc_page_count_inc(struct conv_ftl *conv_ftl, uint32_t chain_id)
{
#if !NVMEV_ENABLE_CHAIN_AGGREGATION
	(void)conv_ftl;
	(void)chain_id;
#else
	uint32_t cur;

	if (!chain_id_valid(conv_ftl, chain_id) || !conv_ftl->chain_slc_page_count)
		return;

	cur = READ_ONCE(conv_ftl->chain_slc_page_count[chain_id]);
	if (cur < U32_MAX)
		WRITE_ONCE(conv_ftl->chain_slc_page_count[chain_id], cur + 1);
#endif
}

static inline void chain_slc_page_count_dec(struct conv_ftl *conv_ftl, uint32_t chain_id)
{
#if !NVMEV_ENABLE_CHAIN_AGGREGATION
	(void)conv_ftl;
	(void)chain_id;
#else
	uint32_t cur;

	if (!chain_id_valid(conv_ftl, chain_id) || !conv_ftl->chain_slc_page_count)
		return;

	cur = READ_ONCE(conv_ftl->chain_slc_page_count[chain_id]);
	if (cur > 0)
		WRITE_ONCE(conv_ftl->chain_slc_page_count[chain_id], cur - 1);
#endif
}

static inline void chain_slc_note_state_change(struct conv_ftl *conv_ftl,
					       uint32_t old_chain_id, bool old_in_slc,
					       uint32_t new_chain_id, bool new_in_slc,
					       bool touch_new)
{
#if !NVMEV_ENABLE_CHAIN_AGGREGATION
	(void)conv_ftl;
	(void)old_chain_id;
	(void)old_in_slc;
	(void)new_chain_id;
	(void)new_in_slc;
	(void)touch_new;
#else
	if (old_in_slc && new_in_slc && old_chain_id != new_chain_id) {
		unsigned long flags;

		spin_lock_irqsave(&conv_ftl->slc_lock, flags);
		chain_slc_page_count_dec(conv_ftl, old_chain_id);
		chain_slc_page_count_inc(conv_ftl, new_chain_id);
		spin_unlock_irqrestore(&conv_ftl->slc_lock, flags);
	}

	if (touch_new && new_in_slc)
		chain_slc_touch(conv_ftl, new_chain_id);
#endif
}

static inline uint64_t block_meta_index(struct conv_ftl *conv_ftl, const struct ppa *ppa)
{
	struct ssdparams *spp;

	if (!conv_ftl || !conv_ftl->ssd || !ppa)
		return U64_MAX;

	spp = &conv_ftl->ssd->sp;
	return ((((uint64_t)ppa->g.ch * spp->luns_per_ch) + ppa->g.lun) *
		spp->pls_per_lun + ppa->g.pl) * spp->blks_per_pl + ppa->g.blk;
}

static inline void block_meta_reset(struct conv_ftl *conv_ftl, const struct ppa *ppa)
{
	uint64_t idx = block_meta_index(conv_ftl, ppa);

#if !NVMEV_ENABLE_CHAIN_AGGREGATION
	(void)conv_ftl;
	(void)ppa;
	(void)idx;
	return;
#endif
	if (!conv_ftl || idx == U64_MAX || idx >= conv_ftl->ssd->sp.tt_blks ||
	    !conv_ftl->blk_owner_chain || !conv_ftl->blk_owner_pages ||
	    !conv_ftl->blk_valid_pages || !conv_ftl->blk_mixed_pages)
		return;

	conv_ftl->blk_owner_chain[idx] = INVALID_CHAIN_ID;
	conv_ftl->blk_owner_pages[idx] = 0;
	conv_ftl->blk_valid_pages[idx] = 0;
	conv_ftl->blk_mixed_pages[idx] = 0;
}

static inline void block_meta_note_valid(struct conv_ftl *conv_ftl, const struct ppa *ppa,
					 uint64_t lpn)
{
	uint64_t idx = block_meta_index(conv_ftl, ppa);
	uint32_t chain_id;

#if !NVMEV_ENABLE_CHAIN_AGGREGATION
	(void)conv_ftl;
	(void)ppa;
	(void)lpn;
	(void)idx;
	return;
#endif
	if (!conv_ftl || idx == U64_MAX || idx >= conv_ftl->ssd->sp.tt_blks ||
	    !conv_ftl->blk_owner_chain || !conv_ftl->blk_owner_pages ||
	    !conv_ftl->blk_valid_pages || !conv_ftl->blk_mixed_pages ||
	    !conv_ftl->lpn_chain_id || lpn >= conv_ftl->ssd->sp.tt_pgs)
		return;

	chain_id = conv_ftl->lpn_chain_id[lpn];
	if (conv_ftl->blk_valid_pages[idx] < U16_MAX)
		conv_ftl->blk_valid_pages[idx]++;

	if (!chain_id_valid(conv_ftl, chain_id)) {
		conv_ftl->blk_owner_chain[idx] = INVALID_CHAIN_ID;
		conv_ftl->blk_owner_pages[idx] = 0;
		conv_ftl->blk_mixed_pages[idx] = conv_ftl->blk_valid_pages[idx];
		return;
	}

	if (!chain_id_valid(conv_ftl, conv_ftl->blk_owner_chain[idx]) ||
	    conv_ftl->blk_owner_pages[idx] == 0) {
		conv_ftl->blk_owner_chain[idx] = chain_id;
		conv_ftl->blk_owner_pages[idx] = 1;
		conv_ftl->blk_mixed_pages[idx] =
			(conv_ftl->blk_valid_pages[idx] > 0) ?
			(conv_ftl->blk_valid_pages[idx] - 1) : 0;
		return;
	}

	if (conv_ftl->blk_owner_chain[idx] == chain_id) {
		if (conv_ftl->blk_owner_pages[idx] < U16_MAX)
			conv_ftl->blk_owner_pages[idx]++;
	} else if (conv_ftl->blk_mixed_pages[idx] < U16_MAX) {
		conv_ftl->blk_mixed_pages[idx]++;
	}

	if (conv_ftl->blk_owner_pages[idx] + conv_ftl->blk_mixed_pages[idx] >
	    conv_ftl->blk_valid_pages[idx]) {
		conv_ftl->blk_mixed_pages[idx] =
			conv_ftl->blk_valid_pages[idx] - conv_ftl->blk_owner_pages[idx];
	}
}

static inline void block_meta_note_invalid(struct conv_ftl *conv_ftl, const struct ppa *ppa,
					   uint64_t lpn)
{
	uint64_t idx = block_meta_index(conv_ftl, ppa);
	uint32_t chain_id = INVALID_CHAIN_ID;

#if !NVMEV_ENABLE_CHAIN_AGGREGATION
	(void)conv_ftl;
	(void)ppa;
	(void)lpn;
	(void)idx;
	return;
#endif
	if (!conv_ftl || idx == U64_MAX || idx >= conv_ftl->ssd->sp.tt_blks ||
	    !conv_ftl->blk_owner_chain || !conv_ftl->blk_owner_pages ||
	    !conv_ftl->blk_valid_pages || !conv_ftl->blk_mixed_pages)
		return;

	if (conv_ftl->blk_valid_pages[idx] > 0)
		conv_ftl->blk_valid_pages[idx]--;
	if (!conv_ftl->blk_valid_pages[idx]) {
		block_meta_reset(conv_ftl, ppa);
		return;
	}

	if (conv_ftl->lpn_chain_id && lpn < conv_ftl->ssd->sp.tt_pgs)
		chain_id = conv_ftl->lpn_chain_id[lpn];

	if (chain_id_valid(conv_ftl, chain_id) &&
	    conv_ftl->blk_owner_chain[idx] == chain_id &&
	    conv_ftl->blk_owner_pages[idx] > 0) {
		conv_ftl->blk_owner_pages[idx]--;
	} else if (conv_ftl->blk_mixed_pages[idx] > 0) {
		conv_ftl->blk_mixed_pages[idx]--;
	}

	if (!conv_ftl->blk_owner_pages[idx]) {
		conv_ftl->blk_owner_chain[idx] = INVALID_CHAIN_ID;
		conv_ftl->blk_mixed_pages[idx] = conv_ftl->blk_valid_pages[idx];
	}

	if (conv_ftl->blk_owner_pages[idx] + conv_ftl->blk_mixed_pages[idx] >
	    conv_ftl->blk_valid_pages[idx]) {
		conv_ftl->blk_mixed_pages[idx] =
			conv_ftl->blk_valid_pages[idx] - conv_ftl->blk_owner_pages[idx];
	}
}

static inline bool block_meta_high_purity(struct conv_ftl *conv_ftl, const struct ppa *ppa,
					  uint32_t *owner_chain_out)
{
	uint64_t idx = block_meta_index(conv_ftl, ppa);
	uint32_t owner_chain;
	uint16_t owner_pages, valid_pages;
	uint32_t chunk_pages;

#if !NVMEV_ENABLE_CHAIN_AGGREGATION
	(void)conv_ftl;
	(void)ppa;
	(void)owner_chain_out;
	(void)idx;
	return false;
#endif
	if (!conv_ftl || idx == U64_MAX || idx >= conv_ftl->ssd->sp.tt_blks ||
	    !conv_ftl->blk_owner_chain || !conv_ftl->blk_owner_pages ||
	    !conv_ftl->blk_valid_pages)
		return false;

	owner_chain = conv_ftl->blk_owner_chain[idx];
	owner_pages = conv_ftl->blk_owner_pages[idx];
	valid_pages = conv_ftl->blk_valid_pages[idx];
	chunk_pages = conv_ftl->ssd->sp.pgs_per_oneshotpg;

	if (owner_chain_out)
		*owner_chain_out = owner_chain;

	if (!chain_id_valid(conv_ftl, owner_chain) || valid_pages < chunk_pages)
		return false;

	return owner_pages * 4 >= valid_pages * 3;
}

/* Forward declaration needed before early debug/event helpers use it. */
static inline uint32_t encode_die(struct ssdparams *spp, const struct ppa *ppa);

struct chain_block_heat {
	uint64_t heat_sum;
	uint32_t heat_pages;
	uint32_t cold_pages;
	uint32_t owned_pages;
	uint32_t seed_pg;
};

static const char *chain_alloc_reason_name(uint8_t reason)
{
	switch (reason) {
	case NVMEV_CHAIN_ALLOC_REASON_NO_PREV:
		return "no_prev";
	case NVMEV_CHAIN_ALLOC_REASON_PREV_CHAIN_INVALID:
		return "prev_chain_invalid";
	case NVMEV_CHAIN_ALLOC_REASON_CAPACITY:
		return "capacity";
	default:
		return "unknown";
	}
}

static const char *chain_cold_method_name(uint8_t method)
{
	switch (method) {
	case NVMEV_CHAIN_COLD_METHOD_BLOCK:
		return "block";
	case NVMEV_CHAIN_COLD_METHOD_CHUNK:
		return "chunk";
	default:
		return "unknown";
	}
}

static void note_chain_alloc_event(struct conv_ftl *conv_ftl, uint32_t chain_id,
				   uint64_t lpn, uint64_t prev_link_lpn,
				   uint32_t prev_chain_id, uint16_t seed_die,
				   bool is_append, bool is_overwrite,
				   uint8_t reason)
{
	unsigned long flags;
	uint32_t slot;
	struct nvmev_chain_alloc_event *evt;

	if (!conv_ftl)
		return;

	switch (reason) {
	case NVMEV_CHAIN_ALLOC_REASON_NO_PREV:
		conv_ftl->chain_alloc_no_prev++;
		break;
	case NVMEV_CHAIN_ALLOC_REASON_PREV_CHAIN_INVALID:
		conv_ftl->chain_alloc_prev_chain_invalid++;
		break;
	case NVMEV_CHAIN_ALLOC_REASON_CAPACITY:
		conv_ftl->chain_alloc_capacity_fail++;
		break;
	default:
		break;
	}

	if (!conv_ftl->chain_alloc_events)
		return;

	spin_lock_irqsave(&conv_ftl->event_log_lock, flags);
	slot = conv_ftl->chain_alloc_event_head % NVMEV_EVENT_LOG_CAP;
	evt = &conv_ftl->chain_alloc_events[slot];
	memset(evt, 0, sizeof(*evt));
	evt->seq = ++conv_ftl->chain_alloc_event_seq;
	evt->lpn = lpn;
	evt->prev_link_lpn = prev_link_lpn;
	evt->chain_id = chain_id;
	evt->prev_chain_id = prev_chain_id;
	evt->seed_die = seed_die;
	evt->is_append = is_append ? 1 : 0;
	evt->is_overwrite = is_overwrite ? 1 : 0;
	evt->reason = reason;
	conv_ftl->chain_alloc_event_head = (slot + 1) % NVMEV_EVENT_LOG_CAP;
	if (conv_ftl->chain_alloc_event_count < NVMEV_EVENT_LOG_CAP)
		conv_ftl->chain_alloc_event_count++;
	spin_unlock_irqrestore(&conv_ftl->event_log_lock, flags);
}

static __maybe_unused void note_chain_cold_event(struct conv_ftl *conv_ftl, uint32_t chain_id,
				  const struct ppa *ppa,
				  const struct chain_block_heat *heat,
				  uint32_t pages_moved, uint8_t method)
{
	unsigned long flags;
	uint32_t slot;
	struct nvmev_chain_cold_event *evt;

	if (!conv_ftl || !conv_ftl->chain_cold_events || !ppa || !heat)
		return;

	spin_lock_irqsave(&conv_ftl->event_log_lock, flags);
	slot = conv_ftl->chain_cold_event_head % NVMEV_EVENT_LOG_CAP;
	evt = &conv_ftl->chain_cold_events[slot];
	memset(evt, 0, sizeof(*evt));
	evt->seq = ++conv_ftl->chain_cold_event_seq;
	evt->chain_id = chain_id;
	evt->die = encode_die(&conv_ftl->ssd->sp, ppa);
	evt->blk = ppa->g.blk;
	evt->pages_moved = pages_moved;
	evt->heat_sum = heat->heat_sum;
	evt->heat_pages = heat->heat_pages;
	evt->cold_pages = heat->cold_pages;
	evt->owned_pages = heat->owned_pages;
	evt->ch = ppa->g.ch;
	evt->lun = ppa->g.lun;
	evt->pl = ppa->g.pl;
	evt->method = method;
	conv_ftl->chain_cold_event_head = (slot + 1) % NVMEV_EVENT_LOG_CAP;
	if (conv_ftl->chain_cold_event_count < NVMEV_EVENT_LOG_CAP)
		conv_ftl->chain_cold_event_count++;
	spin_unlock_irqrestore(&conv_ftl->event_log_lock, flags);
}

static void note_gc_victim_event(struct conv_ftl *conv_ftl, const struct ppa *ppa,
				 bool is_slc, uint32_t vpc, uint32_t ipc,
				 uint32_t owner_chain, bool high_purity)
{
	unsigned long flags;
	uint32_t slot;
	struct nvmev_gc_victim_event *evt;

	if (!conv_ftl || !conv_ftl->gc_victim_events || !ppa)
		return;

	spin_lock_irqsave(&conv_ftl->event_log_lock, flags);
	slot = conv_ftl->gc_victim_event_head % NVMEV_EVENT_LOG_CAP;
	evt = &conv_ftl->gc_victim_events[slot];
	memset(evt, 0, sizeof(*evt));
	evt->seq = ++conv_ftl->gc_victim_event_seq;
	evt->die = encode_die(&conv_ftl->ssd->sp, ppa);
	evt->blk = ppa->g.blk;
	evt->owner_chain = owner_chain;
	evt->vpc = vpc;
	evt->ipc = ipc;
	evt->is_slc = is_slc ? 1 : 0;
	evt->high_purity = high_purity ? 1 : 0;
	conv_ftl->gc_victim_event_head = (slot + 1) % NVMEV_EVENT_LOG_CAP;
	if (conv_ftl->gc_victim_event_count < NVMEV_EVENT_LOG_CAP)
		conv_ftl->gc_victim_event_count++;
	spin_unlock_irqrestore(&conv_ftl->event_log_lock, flags);
}

static inline uint32_t chain_for_write(struct conv_ftl *conv_ftl, uint64_t lpn,
				       uint64_t prev_link_lpn, uint16_t seed_die,
				       bool is_append, bool is_overwrite)
{
	uint32_t chain_id = INVALID_CHAIN_ID;
	uint32_t prev_chain_id = INVALID_CHAIN_ID;
	uint8_t reason;

#if !NVMEV_ENABLE_CHAIN_AGGREGATION
	(void)conv_ftl;
	(void)lpn;
	(void)prev_link_lpn;
	(void)seed_die;
	(void)is_append;
	(void)is_overwrite;
	return INVALID_CHAIN_ID;
#endif
	if (!conv_ftl || !conv_ftl->lpn_chain_id)
		return INVALID_CHAIN_ID;

	if (prev_link_lpn != INVALID_LPN && prev_link_lpn < conv_ftl->ssd->sp.tt_pgs)
		prev_chain_id = conv_ftl->lpn_chain_id[prev_link_lpn];

	if (chain_id_valid(conv_ftl, prev_chain_id))
		return prev_chain_id;

	reason = (prev_link_lpn == INVALID_LPN) ?
		NVMEV_CHAIN_ALLOC_REASON_NO_PREV :
		NVMEV_CHAIN_ALLOC_REASON_PREV_CHAIN_INVALID;
	chain_id = chain_alloc(conv_ftl, seed_die);
	if (!chain_id_valid(conv_ftl, chain_id))
		reason = NVMEV_CHAIN_ALLOC_REASON_CAPACITY;
	note_chain_alloc_event(conv_ftl, chain_id, lpn, prev_link_lpn, prev_chain_id,
			       seed_die, is_append, is_overwrite, reason);
	return chain_id;
}

static struct dentry *nvmev_debug_root;
static DEFINE_MUTEX(nvmev_debug_lock);
static atomic_t nvmev_debug_counter = ATOMIC_INIT(0);

static struct dentry *nvmev_debugfs_root(void)
{
	struct dentry *root;

	mutex_lock(&nvmev_debug_lock);
	if (!nvmev_debug_root) {
		struct dentry *created = debugfs_create_dir("nvmev", NULL);

		if (IS_ERR(created)) {
			if (PTR_ERR(created) == -EEXIST) {
				created = debugfs_lookup("nvmev", NULL);
				if (!created)
					created = NULL;
			} else {
				created = NULL;
			}
		}

		nvmev_debug_root = created;
	}

	root = nvmev_debug_root;
	mutex_unlock(&nvmev_debug_lock);

	return root;
}

void nvmev_debugfs_cleanup_root(void)
{
	mutex_lock(&nvmev_debug_lock);
	if (nvmev_debug_root) {
		debugfs_remove_recursive(nvmev_debug_root);
		nvmev_debug_root = NULL;
	}
	atomic_set(&nvmev_debug_counter, 0);
	mutex_unlock(&nvmev_debug_lock);
}

static void nvmev_debugfs_init_instance(struct conv_ftl *conv_ftl)
{
	struct dentry *root = nvmev_debugfs_root();
	struct dentry *dir = NULL;
	int inst_id;
	char name[32];

	if (!root)
		return;

	inst_id = atomic_inc_return(&nvmev_debug_counter) - 1;
	snprintf(name, sizeof(name), "ftl%d", inst_id);
	dir = debugfs_create_dir(name, root);
	if (IS_ERR(dir))
		dir = NULL;

	conv_ftl->debug_dir = dir;
}

struct line_pool_stats {
	uint32_t total;
	uint32_t free;
	uint32_t victim;
	uint32_t full;
};

static void collect_pool_stats(struct conv_ftl *conv_ftl, bool slc,
			       struct line_pool_stats *stats)
{
	uint32_t die_count, line_cnt, pages_per_line, blk;
	struct line_mgmt *array;
	spinlock_t *lock;

	memset(stats, 0, sizeof(*stats));

	if (!conv_ftl)
		return;

	die_count = superblock_die_count(conv_ftl);
	array = slc ? conv_ftl->slc_lunlm : conv_ftl->qlc_lunlm;
	line_cnt = slc ? conv_ftl->slc_blks_per_pl : conv_ftl->qlc_blks_per_pl;
	pages_per_line = slc ? conv_ftl->slc_pgs_per_blk : conv_ftl->qlc_pgs_per_blk;
	lock = slc ? &conv_ftl->slc_lock : &conv_ftl->qlc_lock;

	if (!array || !line_cnt || !pages_per_line)
		return;

	spin_lock(lock);
	stats->total = line_cnt;
	for (blk = 0; blk < line_cnt; blk++) {
		uint32_t die;
		bool all_free = true;
		bool all_full = true;
		bool any_victim = false;
		bool any_open = false;
		bool any_accounted = false;

		for (die = 0; die < die_count; die++) {
			struct line_mgmt *lm = &array[die];
			struct line *line;

			if (!lm->lines || blk >= lm->tt_lines) {
				all_free = false;
				all_full = false;
				continue;
			}

			line = &lm->lines[blk];
			if (line->vpc || line->ipc || line->pos || !list_empty(&line->entry))
				any_accounted = true;
			if (!(line->vpc == 0 && line->ipc == 0 && line->pos == 0 &&
			      !list_empty(&line->entry)))
				all_free = false;
			if (!(line->vpc == pages_per_line && line->ipc == 0))
				all_full = false;
			if (line->pos || line->ipc > 0)
				any_victim = true;
			if (slc) {
				struct write_pointer *host_wp = conv_ftl->slc_lunwp ?
					&conv_ftl->slc_lunwp[die] : NULL;
				struct write_pointer *gc_wp = conv_ftl->gc_slc_lunwp ?
					&conv_ftl->gc_slc_lunwp[die] : NULL;

				if ((host_wp && host_wp->curline == line) ||
				    (gc_wp && gc_wp->curline == line) ||
				    (conv_ftl->slc_sb_state &&
				     blk < conv_ftl->ssd->sp.blks_per_pl &&
				     conv_ftl->slc_sb_state[blk] == NVMEV_SB_ACTIVE))
					any_open = true;
			} else if (line->pos == 0 && list_empty(&line->entry) &&
				   line->vpc < pages_per_line) {
				any_open = true;
			}
		}

		if (all_free) {
			stats->free++;
		} else if (any_open) {
			continue;
		} else if (all_full) {
			stats->full++;
		} else if (any_victim || any_accounted) {
			stats->victim++;
		}
	}
	spin_unlock(lock);
}

static inline void collect_slc_stats(struct conv_ftl *conv_ftl,
				     struct line_pool_stats *stats)
{
	collect_pool_stats(conv_ftl, true, stats);
}

static inline void collect_qlc_stats(struct conv_ftl *conv_ftl,
				     struct line_pool_stats *stats)
{
	collect_pool_stats(conv_ftl, false, stats);
}

static inline uint32_t pick_locked_qlc_page_type(struct conv_ftl *conv_ftl, bool warm)
{
	uint32_t type;

	if (!conv_ftl)
		return QLC_PAGE_TYPE_L;

#if !NVMEV_ENABLE_QLC_HOTCOLD
	(void)warm;
	type = conv_ftl->qlc_zone_rr_cursor++ % QLC_PAGE_PATTERN;
#else
	if (warm)
		type = (conv_ftl->qlc_zone_rr_cursor++ & 0x1) ?
		       QLC_PAGE_TYPE_CL : QLC_PAGE_TYPE_L;
	else
		type = (conv_ftl->qlc_zone_rr_cursor++ & 0x1) ?
		       QLC_PAGE_TYPE_U : QLC_PAGE_TYPE_CU;
#endif

	return type;
}

static inline uint32_t internal_rr_die(struct conv_ftl *conv_ftl, bool qlc_dest)
{
	uint32_t die;

	if (!conv_ftl || !conv_ftl->die_count)
		return 0;

	die = qlc_dest ? conv_ftl->bg_qlc_rr_die : conv_ftl->bg_slc_rr_die;
	return die % conv_ftl->die_count;
}

static inline void internal_rr_note_write(struct conv_ftl *conv_ftl, bool qlc_dest,
					  uint32_t actual_die)
{
	uint32_t unit;
	uint32_t *die_cursor;
	uint32_t *page_cursor;

	if (!conv_ftl || !conv_ftl->die_count || !conv_ftl->ssd)
		return;

	actual_die %= conv_ftl->die_count;
	unit = conv_ftl->ssd->sp.pgs_per_oneshotpg ?
	       conv_ftl->ssd->sp.pgs_per_oneshotpg : 1;
	die_cursor = qlc_dest ? &conv_ftl->bg_qlc_rr_die : &conv_ftl->bg_slc_rr_die;
	page_cursor = qlc_dest ? &conv_ftl->bg_qlc_rr_pages : &conv_ftl->bg_slc_rr_pages;

	if (*page_cursor >= unit)
		*page_cursor = 0;

	(*page_cursor)++;
	if (*page_cursor >= unit) {
		*page_cursor = 0;
		*die_cursor = (actual_die + 1) % conv_ftl->die_count;
	} else {
		*die_cursor = actual_die;
	}
}

static inline uint32_t internal_fallback_die_for_ppa(struct conv_ftl *conv_ftl,
						      const struct ppa *src_ppa,
						      bool qlc_dest)
{
#if NVMEV_ENABLE_INTERNAL_DIE_AFFINITY
	if (!conv_ftl || !conv_ftl->die_count || !src_ppa)
		return 0;
	return encode_die(&conv_ftl->ssd->sp, src_ppa) % conv_ftl->die_count;
#else
	(void)src_ppa;
	return internal_rr_die(conv_ftl, qlc_dest);
#endif
}

static inline uint32_t internal_place_die_for_lpn(struct conv_ftl *conv_ftl, uint64_t lpn,
						  const struct ppa *src_ppa,
						  enum chain_place_tier tier,
						  bool qlc_dest)
{
	uint32_t fallback_die;

	fallback_die = internal_fallback_die_for_ppa(conv_ftl, src_ppa, qlc_dest);
	return chain_place_die_for_lpn(conv_ftl, lpn, tier, fallback_die);
}

static inline void internal_place_note_lpn_write(struct conv_ftl *conv_ftl, uint64_t lpn,
						 enum chain_place_tier tier,
						 bool qlc_dest, uint32_t actual_die)
{
	if (chain_id_valid(conv_ftl, chain_id_for_lpn(conv_ftl, lpn))) {
		chain_place_note_lpn_write(conv_ftl, lpn, tier, actual_die);
		return;
	}
#if !NVMEV_ENABLE_INTERNAL_DIE_AFFINITY
	internal_rr_note_write(conv_ftl, qlc_dest, actual_die);
#else
	(void)conv_ftl;
	(void)qlc_dest;
	(void)actual_die;
	(void)tier;
	(void)lpn;
#endif
}

static inline bool qlc_zone_is_fast(uint8_t zone)
{
	return zone == QLC_PAGE_TYPE_L || zone == QLC_PAGE_TYPE_CL;
}

/* 前向声明以避免隐式声明错误 */
static noinline struct ppa get_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn);
static noinline uint64_t get_rmap_ent(struct conv_ftl *conv_ftl, struct ppa *ppa);
static inline bool mapped_ppa(struct ppa *ppa);
static inline bool valid_ppa(struct conv_ftl *conv_ftl, struct ppa *ppa);
static bool is_slc_block(struct conv_ftl *conv_ftl, uint32_t blk_id);
static int migrate_page_to_qlc(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *slc_ppa);
static void migrate_page_to_slc(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *qlc_ppa,
				uint64_t *migration_done);
static inline uint8_t get_qlc_zone_for_read(struct conv_ftl *conv_ftl, struct ppa *ppa);
static int qlc_get_new_page(struct conv_ftl *conv_ftl, uint32_t chain_id,
			    uint32_t die, uint32_t zone_hint, struct ppa *ppa_out);
static int qlc_get_new_gc_page(struct conv_ftl *conv_ftl, uint32_t die, uint32_t zone_hint,
			       struct ppa *ppa_out);
static void advance_slc_write_pointer(struct conv_ftl *conv_ftl, uint32_t die);
static inline bool slc_block_has_active_writer(struct conv_ftl *conv_ftl, const struct ppa *ppa);
static void advance_chain_slc_write_pointer(struct conv_ftl *conv_ftl, uint32_t chain_id,
					    uint32_t die);
static struct ppa get_new_slc_page(struct conv_ftl *conv_ftl);
static struct ppa get_new_chain_slc_page(struct conv_ftl *conv_ftl, uint32_t chain_id,
					 uint32_t die);
static struct write_pointer *resolve_chain_active_wp_locked(struct conv_ftl *conv_ftl,
							    uint32_t chain_id,
							    uint32_t die);
/* 新增：GC 专用 SLC 写指针函数声明（仅在本文件使用） */
static void advance_gc_slc_write_pointer(struct conv_ftl *conv_ftl, uint32_t die);
static struct ppa get_new_gc_slc_page(struct conv_ftl *conv_ftl, uint32_t die);
static uint64_t get_dynamic_cold_threshold(struct conv_ftl *conv_ftl);
static void qlc_maybe_rebalance_internal(struct conv_ftl *conv_ftl);
static uint32_t migrate_hot_from_closed_qlc(struct conv_ftl *conv_ftl);
static void bg_repromotion_worker(struct work_struct *work);
static void bg_qlc_rebalance_worker(struct work_struct *work);
static void qlc_closed_repromote_note_open_locked(struct conv_ftl *conv_ftl,
						  uint32_t blk);

static uint32_t migrate_chain_chunk_from_slc(struct conv_ftl *conv_ftl, struct ppa *seed_ppa,
					     uint32_t budget, uint64_t dyn_thresh)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_block *blk;
	uint32_t chunk_pages = spp->pgs_per_oneshotpg;
	uint32_t base_pg;
	uint32_t end_pg;
	uint32_t moved = 0;
	uint32_t pg_idx;
	uint64_t seed_lpn;
	uint32_t seed_chain = INVALID_CHAIN_ID;

	if (!conv_ftl || !seed_ppa || !budget || !chunk_pages)
		return 0;

	blk = get_blk(conv_ftl->ssd, seed_ppa);
	if (!blk)
		return 0;
	if (slc_block_has_active_writer(conv_ftl, seed_ppa))
		return 0;
	seed_lpn = get_rmap_ent(conv_ftl, seed_ppa);
	if (seed_lpn == INVALID_LPN || seed_lpn >= conv_ftl->ssd->sp.tt_pgs ||
	    !conv_ftl->lpn_chain_id)
		return 0;
	seed_chain = conv_ftl->lpn_chain_id[seed_lpn];
	if (!chain_id_valid(conv_ftl, seed_chain))
		return 0;

	base_pg = (seed_ppa->g.pg / chunk_pages) * chunk_pages;
	end_pg = min_t(uint32_t, base_pg + chunk_pages, blk->npgs);
	conv_ftl->chain_chunk_migration_attempts++;

	for (pg_idx = base_pg; pg_idx < end_pg; pg_idx++) {
		struct ppa ppa = *seed_ppa;
		struct nand_page *pg;
		uint64_t lpn;
		uint64_t acc = 0;

		if (moved >= budget)
			break;

		ppa.g.pg = pg_idx;
		pg = get_pg(conv_ftl->ssd, &ppa);
		if (!pg || pg->status != PG_VALID)
			continue;

		lpn = get_rmap_ent(conv_ftl, &ppa);
		if (lpn == INVALID_LPN || lpn >= conv_ftl->ssd->sp.tt_pgs)
			continue;
		if (!conv_ftl->page_in_slc[lpn])
			continue;
		if (conv_ftl->lpn_chain_id[lpn] != seed_chain)
			continue;

		acc = conv_ftl->heat_track.access_count ? conv_ftl->heat_track.access_count[lpn] : 0;
		if (acc > dyn_thresh || recent_write_guard(conv_ftl, lpn))
			continue;

		if (migrate_page_to_qlc(conv_ftl, lpn, &ppa) == 0)
			moved++;
	}

	conv_ftl->chain_chunk_migration_pages += moved;
	return moved;
}

static bool slc_chain_page_is_cold(struct conv_ftl *conv_ftl, uint64_t lpn,
				   uint64_t dyn_thresh)
{
	struct heat_tracking *ht;
	uint64_t acc;

	if (!conv_ftl || lpn >= conv_ftl->ssd->sp.tt_pgs)
		return false;
	if (recent_write_guard(conv_ftl, lpn))
		return false;

	ht = &conv_ftl->heat_track;
	if (!ht || !ht->access_count)
		return false;

	acc = ht->access_count[lpn];
	return acc <= dyn_thresh;
}

static uint32_t migrate_chain_block_from_slc(struct conv_ftl *conv_ftl, struct ppa *seed_ppa,
					     uint32_t budget, uint64_t dyn_thresh)
{
	struct nand_block *blk;
	uint32_t owner_chain = INVALID_CHAIN_ID;
	uint32_t eligible = 0;
	uint32_t moved = 0;
	uint32_t pg_idx;

	if (!conv_ftl || !seed_ppa || !budget)
		return 0;
	if (!block_meta_high_purity(conv_ftl, seed_ppa, &owner_chain))
		return 0;
	if (slc_block_has_active_writer(conv_ftl, seed_ppa))
		return 0;

	blk = get_blk(conv_ftl->ssd, seed_ppa);
	if (!blk)
		return 0;

	conv_ftl->chain_block_migration_attempts++;

	for (pg_idx = 0; pg_idx < blk->npgs; pg_idx++) {
		struct ppa ppa = *seed_ppa;
		struct nand_page *pg;
		uint64_t lpn;

		ppa.g.pg = pg_idx;
		pg = get_pg(conv_ftl->ssd, &ppa);
		if (!pg || pg->status != PG_VALID)
			continue;

		lpn = get_rmap_ent(conv_ftl, &ppa);
		if (lpn == INVALID_LPN || lpn >= conv_ftl->ssd->sp.tt_pgs)
			continue;
		if (!conv_ftl->page_in_slc[lpn])
			continue;
		if (!conv_ftl->lpn_chain_id ||
		    conv_ftl->lpn_chain_id[lpn] != owner_chain)
			continue;
		if (!slc_chain_page_is_cold(conv_ftl, lpn, dyn_thresh))
			return 0;

		eligible++;
	}

	if (!eligible)
		return 0;
	if (eligible > budget) {
		conv_ftl->chain_block_migration_skip_budget++;
		return 0;
	}

	for (pg_idx = 0; pg_idx < blk->npgs; pg_idx++) {
		struct ppa ppa = *seed_ppa;
		struct nand_page *pg;
		uint64_t lpn;

		ppa.g.pg = pg_idx;
		pg = get_pg(conv_ftl->ssd, &ppa);
		if (!pg || pg->status != PG_VALID)
			continue;

		lpn = get_rmap_ent(conv_ftl, &ppa);
		if (lpn == INVALID_LPN || lpn >= conv_ftl->ssd->sp.tt_pgs)
			continue;
		if (!conv_ftl->page_in_slc[lpn])
			continue;
		if (!conv_ftl->lpn_chain_id ||
		    conv_ftl->lpn_chain_id[lpn] != owner_chain)
			continue;

		if (migrate_page_to_qlc(conv_ftl, lpn, &ppa) == 0)
			moved++;
	}

	conv_ftl->chain_block_migration_pages += moved;
	return moved;
}

static bool slc_chain_gc_should_migrate_to_qlc(struct conv_ftl *conv_ftl,
					       struct ppa *old_ppa,
					       uint64_t lpn)
{
	uint32_t owner_chain = INVALID_CHAIN_ID;
	uint64_t dyn_thresh = 0;

	if (!conv_ftl || !old_ppa || lpn >= conv_ftl->ssd->sp.tt_pgs ||
	    !conv_ftl->lpn_chain_id)
		return false;
	if (!block_meta_high_purity(conv_ftl, old_ppa, &owner_chain))
		return false;
	if (slc_block_has_active_writer(conv_ftl, old_ppa))
		return false;
	if (conv_ftl->lpn_chain_id[lpn] != owner_chain)
		return false;

	dyn_thresh = get_dynamic_cold_threshold(conv_ftl);
	if (dyn_thresh == 0)
		dyn_thresh = 1;

	return slc_chain_page_is_cold(conv_ftl, lpn, dyn_thresh);
}

struct slc_sb_summary_no1 {
	uint32_t blk_id;
	uint32_t total_ipc;
	uint32_t total_vpc;
	uint32_t owner_chain;
	uint32_t owner_pages;
	uint32_t valid_pages;
	uint64_t heat_sum;
	uint64_t avg_heat;
	uint32_t heat_count;
	uint16_t eligible_mask;
	uint16_t full_mask;
	bool open_writer;
	bool active;
};

static bool slc_sb_collect_summary(struct conv_ftl *conv_ftl, uint32_t blk_id,
				   struct slc_sb_summary_no1 *sum);
static uint8_t slc_sb_summary_tier(const struct slc_sb_summary_no1 *sum,
				   uint64_t global_avg);
static uint32_t migrate_chain_superblock_from_slc(struct conv_ftl *conv_ftl,
						  uint32_t blk_id,
						  uint32_t budget,
						  uint64_t dyn_thresh);
static uint32_t migrate_pure_cold_superblocks_from_slc(struct conv_ftl *conv_ftl,
						       uint32_t max_pages,
						       uint64_t dyn_thresh,
						       uint32_t *sampled_out,
						       uint32_t *blocks_out);
static uint32_t migrate_coldest_mixed_superblock_from_slc(struct conv_ftl *conv_ftl,
							  uint32_t max_pages,
							  uint32_t *sampled_out,
							  uint32_t *blocks_out);
static uint32_t migrate_coldest_superblock_to_victim_queue_from_slc(struct conv_ftl *conv_ftl,
								    uint32_t max_pages,
								    uint32_t *sampled_out,
								    uint32_t *blocks_out);
static bool slc_sb_recent_guarded(struct conv_ftl *conv_ftl, uint32_t blk_id);
static void superblock_stats_log_summary(struct conv_ftl *conv_ftl, const char *phase);

struct chain_heat_summary {
	uint64_t heat_sum;
	uint32_t heat_pages;
	uint32_t cold_pages;
	uint32_t measured_blocks;
	uint32_t eligible_blocks;
};

static __maybe_unused bool chain_measure_block_heat(struct conv_ftl *conv_ftl,
				     struct ppa *seed_ppa,
				     uint32_t chain_id, uint64_t dyn_thresh,
				     uint32_t min_pages,
				     struct chain_block_heat *heat_out)
{
	struct nand_block *blk;
	uint32_t pg_idx;
	struct chain_block_heat heat = {0};

	if (!conv_ftl || !seed_ppa || !heat_out)
		return false;

	blk = get_blk(conv_ftl->ssd, seed_ppa);
	if (!blk || !blk->npgs || !conv_ftl->lpn_chain_id || !conv_ftl->page_in_slc)
		return false;

	for (pg_idx = 0; pg_idx < blk->npgs; pg_idx++) {
		struct ppa ppa = *seed_ppa;
		struct nand_page *pg;
		uint64_t lpn;
		uint64_t acc;

		ppa.g.pg = pg_idx;
		pg = get_pg(conv_ftl->ssd, &ppa);
		if (!pg || pg->status != PG_VALID)
			continue;

		lpn = get_rmap_ent(conv_ftl, &ppa);
		if (lpn == INVALID_LPN || lpn >= conv_ftl->ssd->sp.tt_pgs)
			continue;
		if (!conv_ftl->page_in_slc[lpn])
			continue;
		if (conv_ftl->lpn_chain_id[lpn] != chain_id)
			continue;

		heat.owned_pages++;
		if (recent_write_guard(conv_ftl, lpn))
			continue;

		acc = conv_ftl->heat_track.access_count ?
			conv_ftl->heat_track.access_count[lpn] : 0;
		heat.heat_sum += acc;
		heat.heat_pages++;
		if (heat.heat_pages == 1)
			heat.seed_pg = pg_idx;
		if (acc <= dyn_thresh) {
			if (heat.cold_pages == 0)
				heat.seed_pg = pg_idx;
			heat.cold_pages++;
		}
	}

	if (heat.owned_pages < min_pages || heat.heat_pages == 0 || heat.cold_pages == 0)
		return false;

	*heat_out = heat;
	return true;
}

static inline bool __maybe_unused chain_block_heat_is_colder(const struct ppa *cand_ppa,
					      const struct chain_block_heat *cand,
					      const struct ppa *best_ppa,
					      const struct chain_block_heat *best)
{
	if (!cand || !cand->heat_pages)
		return false;
	if (!best || !best->heat_pages)
		return true;

	if (cand->heat_sum * best->heat_pages != best->heat_sum * cand->heat_pages)
		return cand->heat_sum * best->heat_pages <
		       best->heat_sum * cand->heat_pages;
	if (cand->cold_pages != best->cold_pages)
		return cand->cold_pages > best->cold_pages;
	if (cand->heat_pages != best->heat_pages)
		return cand->heat_pages > best->heat_pages;
	if (cand_ppa->g.ch != best_ppa->g.ch)
		return cand_ppa->g.ch < best_ppa->g.ch;
	if (cand_ppa->g.lun != best_ppa->g.lun)
		return cand_ppa->g.lun < best_ppa->g.lun;
	if (cand_ppa->g.pl != best_ppa->g.pl)
		return cand_ppa->g.pl < best_ppa->g.pl;
	return cand_ppa->g.blk < best_ppa->g.blk;
}

static inline bool __maybe_unused chain_summary_is_colder(const struct chain_heat_summary *cand,
					   const struct chain_heat_summary *best)
{
	if (!cand || !cand->heat_pages || !cand->eligible_blocks)
		return false;
	if (!best || !best->heat_pages || !best->eligible_blocks)
		return true;

	if (cand->heat_sum * best->heat_pages != best->heat_sum * cand->heat_pages)
		return cand->heat_sum * best->heat_pages <
		       best->heat_sum * cand->heat_pages;
	if (cand->cold_pages != best->cold_pages)
		return cand->cold_pages > best->cold_pages;
	return cand->eligible_blocks > best->eligible_blocks;
}

static __maybe_unused bool chain_scan_cold_blocks(struct conv_ftl *conv_ftl, uint32_t chain_id,
				   int32_t target_die, uint64_t dyn_thresh,
				   struct chain_heat_summary *summary_out,
				   struct ppa *best_ppa_out,
				   struct chain_block_heat *best_heat_out,
				   uint32_t *blocks_checked_out)
{
	struct ssdparams *spp;
	struct chain_heat_summary summary = { 0 };
	struct chain_block_heat best_heat = { 0 };
	struct ppa best_ppa = { .ppa = 0 };
	uint32_t die_count, die_start, die_span, die_step;
	uint32_t die, ch, lun, pl, blk;

	if (!conv_ftl || !conv_ftl->ssd || !summary_out || !best_ppa_out ||
	    !best_heat_out || !blocks_checked_out || !conv_ftl->blk_owner_chain)
		return false;

	spp = &conv_ftl->ssd->sp;
	die_count = conv_ftl->die_count ? conv_ftl->die_count : 1;
	if (target_die >= 0) {
		die_start = (uint32_t)target_die % die_count;
		die_span = 1;
	} else {
		die_start = 0;
		die_span = die_count;
	}

	*blocks_checked_out = 0;
	for (die_step = 0; die_step < die_span; die_step++) {
		die = (die_start + die_step) % die_count;
		decode_die(spp, die, &ch, &lun);
		for (pl = 0; pl < spp->pls_per_lun; pl++) {
			for (blk = 0; blk < conv_ftl->slc_blks_per_pl; blk++) {
				struct ppa candidate_ppa = { .ppa = 0 };
				struct chain_block_heat heat;
				struct nand_block *blk_meta;
				uint64_t idx;

				candidate_ppa.g.ch = ch;
				candidate_ppa.g.lun = lun;
				candidate_ppa.g.pl = pl;
				candidate_ppa.g.blk = blk;
				candidate_ppa.g.pg = 0;
				(*blocks_checked_out)++;

				idx = block_meta_index(conv_ftl, &candidate_ppa);
				if (idx == U64_MAX || idx >= conv_ftl->ssd->sp.tt_blks)
					continue;
				if (conv_ftl->blk_owner_chain[idx] != chain_id)
					continue;
				if (slc_block_has_active_writer(conv_ftl, &candidate_ppa))
					continue;

				blk_meta = get_blk(conv_ftl->ssd, &candidate_ppa);
				if (!blk_meta || blk_meta->vpc == 0)
					continue;

				if (!chain_measure_block_heat(conv_ftl, &candidate_ppa, chain_id,
							      dyn_thresh, 1, &heat))
					continue;

				summary.heat_sum += heat.heat_sum;
				summary.heat_pages += heat.heat_pages;
				summary.cold_pages += heat.cold_pages;
				summary.measured_blocks++;

				if (heat.cold_pages * 100 <
				    heat.heat_pages * CHAIN_COLD_CHUNK_COLD_PCT)
					continue;

				summary.eligible_blocks++;
				if (chain_block_heat_is_colder(&candidate_ppa, &heat,
							       &best_ppa, &best_heat)) {
					best_ppa = candidate_ppa;
					best_heat = heat;
				}
			}
		}
	}

	if (!summary.heat_pages || !summary.eligible_blocks || !best_heat.heat_pages)
		return false;

	*summary_out = summary;
	*best_ppa_out = best_ppa;
	*best_heat_out = best_heat;
	return true;
}

static uint32_t migrate_some_cold_from_slc_chain(struct conv_ftl *conv_ftl, uint32_t max_pages,
						 int32_t target_die, uint64_t dyn_thresh,
						 uint32_t *sampled_out,
						 uint32_t *blocks_out,
						 uint32_t *mixed_migrated_out)
{
#if !NVMEV_ENABLE_CHAIN_AGGREGATION
	(void)conv_ftl;
	(void)max_pages;
	(void)target_die;
	(void)dyn_thresh;
	(void)sampled_out;
	(void)blocks_out;
	(void)mixed_migrated_out;
	return 0;
#else
	uint32_t sampled = 0, blocks = 0;
	uint32_t migrated = 0;

	if (sampled_out)
		*sampled_out = 0;
	if (blocks_out)
		*blocks_out = 0;
	if (mixed_migrated_out)
		*mixed_migrated_out = 0;
	if (!conv_ftl || !conv_ftl->chain_slc_page_count ||
	    !conv_ftl->blk_owner_chain || max_pages == 0)
		return 0;

	(void)target_die;
	(void)dyn_thresh;
	/* Single-layer physical-SB migration: choose the coldest processed SB by
	 * average heat, migrate what can be demoted to QLC, then mark that SB as
	 * the only eligible source for later SLC GC. GC will rank this migrated
	 * set by invalid pages instead of scanning unrelated mixed SBs. */
	migrated = migrate_coldest_superblock_to_victim_queue_from_slc(conv_ftl,
								       max_pages,
								       &sampled,
								       &blocks);

	if (sampled_out)
		*sampled_out = sampled;
	if (blocks_out)
		*blocks_out = blocks;
	if (mixed_migrated_out)
		*mixed_migrated_out = migrated;
	return migrated;
#endif
}

static __maybe_unused uint32_t migrate_some_cold_from_slc_cursor(struct conv_ftl *conv_ftl, uint32_t max_pages,
						  int32_t target_die,
						  uint32_t *scanned_out)
{
	struct heat_tracking *ht = &conv_ftl->heat_track;
	uint32_t migrated = 0;
	uint32_t scanned = 0;
	uint64_t dyn_thresh = 0;
	uint32_t die_count, die_start, die_span, die_step;

	if (scanned_out)
		*scanned_out = 0;
	if (!conv_ftl || !conv_ftl->page_in_slc || !conv_ftl->slc_resident_lpns ||
	    !conv_ftl->slc_resident_slot || !conv_ftl->slc_die_resident_count ||
	    !conv_ftl->slc_die_resident_cursor || !ht || !ht->access_count || max_pages == 0)
		return 0;

	die_count = conv_ftl->die_count ? conv_ftl->die_count : 1;
	if (target_die >= 0) {
		die_start = (uint32_t)target_die % die_count;
		die_span = 1;
	} else {
		die_start = conv_ftl->lunpointer % die_count;
		die_span = die_count;
	}

	dyn_thresh = get_dynamic_cold_threshold(conv_ftl);
	if (dyn_thresh == 0)
		dyn_thresh = 1;

	for (die_step = 0; die_step < die_span && scanned < 4096 && migrated < max_pages; die_step++) {
		uint32_t die = (die_start + die_step) % die_count;
		uint32_t visited = 0;

		while (scanned < 4096 && migrated < max_pages) {
			uint32_t count, cursor, cap, slot;
			uint64_t lpn;
			uint64_t acc;
			struct ppa old_ppa;

			spin_lock(&conv_ftl->slc_lock);
			count = conv_ftl->slc_die_resident_count[die];
			if (!count) {
				spin_unlock(&conv_ftl->slc_lock);
				break;
			}
			if (visited >= count) {
				spin_unlock(&conv_ftl->slc_lock);
				break;
			}

			cap = conv_ftl->slc_resident_capacity_per_die;
			cursor = conv_ftl->slc_die_resident_cursor[die] % count;
			slot = die * cap + cursor;
			lpn = conv_ftl->slc_resident_lpns[slot];
			conv_ftl->slc_die_resident_cursor[die] = (cursor + 1) % count;
			spin_unlock(&conv_ftl->slc_lock);

			visited++;
			scanned++;
			if (lpn >= conv_ftl->ssd->sp.tt_pgs || !conv_ftl->page_in_slc[lpn]) {
				slc_resident_untrack_page(conv_ftl, lpn);
				continue;
			}

			acc = ht->access_count[lpn];
			if (acc > dyn_thresh)
				continue;
			if (recent_write_guard(conv_ftl, lpn))
				continue;

			old_ppa = get_maptbl_ent(conv_ftl, lpn);
			if (!(mapped_ppa(&old_ppa) && is_slc_block(conv_ftl, old_ppa.g.blk))) {
				slc_resident_untrack_page(conv_ftl, lpn);
				continue;
			}
			if (slc_block_has_active_writer(conv_ftl, &old_ppa))
				continue;

#if NVMEV_ENABLE_CHAIN_AGGREGATION
			{
				uint32_t owner_chain = INVALID_CHAIN_ID;
				if (block_meta_high_purity(conv_ftl, &old_ppa, &owner_chain) &&
				    conv_ftl->lpn_chain_id &&
				    lpn < conv_ftl->ssd->sp.tt_pgs &&
				    conv_ftl->lpn_chain_id[lpn] == owner_chain) {
					uint32_t block_moved =
						migrate_chain_block_from_slc(conv_ftl, &old_ppa,
									 max_pages - migrated,
									 dyn_thresh);
					if (block_moved > 0) {
						migrated += block_moved;
						continue;
					}

					block_moved =
						migrate_chain_chunk_from_slc(conv_ftl, &old_ppa,
									 max_pages - migrated,
									 dyn_thresh);
					if (block_moved > 0) {
						migrated += block_moved;
						continue;
					}
				}
			}
#endif

			if (migrate_page_to_qlc(conv_ftl, lpn, &old_ppa) == 0)
				migrated++;
		}
	}

	if (scanned_out)
		*scanned_out = scanned;
	return migrated;
}

/* 无阈值：总是尝试从 SLC 迁移少量更冷页面到 QLC */
static uint32_t migrate_some_cold_from_slc(struct conv_ftl *conv_ftl, uint32_t max_pages,
					   int32_t target_die)
{
	uint32_t migrated;
	uint32_t sb_sampled, sb_scanned;
	uint32_t sb_migrated, sb_mixed_migrated;
	uint64_t dyn_thresh = 0;

	NVMEV_DEBUG("[MIGRATION_DEBUG] migrate_some_cold_from_slc called: max_pages=%u target_die=%d\n",
		    max_pages, target_die);

	if (!conv_ftl || max_pages == 0)
		return 0;

	sb_sampled = 0;
	sb_scanned = 0;
	/* Physical-SB scan: choose the coldest closed SLC SB by avg read count
	 * and migrate its valid pages as one demotion unit. dyn_thresh is kept
	 * only for legacy helpers and is not used by this active path. */
	sb_mixed_migrated = 0;
	sb_migrated =
		migrate_some_cold_from_slc_chain(conv_ftl, max_pages, target_die, dyn_thresh,
						 &sb_sampled, &sb_scanned,
						 &sb_mixed_migrated);
	migrated = sb_migrated;

	NVMEV_DEBUG("[MIGRATION_DEBUG] Migration attempt complete: migrated=%u\n", migrated);
	if (migrated > 0)
		NVMEV_DEBUG("Migrated %u pages from SLC to QLC\n", migrated);

	{
		static uint64_t mig_last_ns = 0;
		static uint32_t mig_total_calls = 0;
		static uint32_t mig_total_sb_scanned = 0;
		static uint32_t mig_total_sb_sampled = 0;
		static uint32_t mig_total_sb_migrated = 0;
		static uint32_t mig_total_sb_mixed_migrated = 0;
		uint64_t now_ns = ktime_get_ns();

		mig_total_calls++;
		mig_total_sb_scanned += sb_scanned;
		mig_total_sb_sampled += sb_sampled;
		mig_total_sb_migrated += sb_migrated;
		mig_total_sb_mixed_migrated += sb_mixed_migrated;

		if (mig_last_ns == 0)
			mig_last_ns = now_ns;
		if (now_ns - mig_last_ns >= 5000000000ULL) {
			NVMEV_ERROR("[MIG-MONITOR] SLC->QLC cold migration: calls=%u sb_scanned=%u sb_sampled=%u sb_migrated=%u sb_mixed_migrated=%u victim_q=%u (mode=coldest_sb)\n",
				    mig_total_calls, mig_total_sb_scanned, mig_total_sb_sampled,
				    mig_total_sb_migrated, mig_total_sb_mixed_migrated,
				    conv_ftl->slc_sb_migrated_victim_count);
			mig_total_calls = 0;
			mig_total_sb_scanned = 0;
			mig_total_sb_sampled = 0;
			mig_total_sb_migrated = 0;
			mig_total_sb_mixed_migrated = 0;
			mig_last_ns = now_ns;
		}
	}

	return migrated;
}
static inline bool last_pg_in_wordline(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	return (ppa->g.pg % spp->pgs_per_oneshotpg) == (spp->pgs_per_oneshotpg - 1);
}

static inline bool should_gc_high(struct conv_ftl *conv_ftl)
{
	struct line_pool_stats slc_stats, qlc_stats;
	uint32_t total_free_lines;
	collect_slc_stats(conv_ftl, &slc_stats);
	collect_qlc_stats(conv_ftl, &qlc_stats);
	total_free_lines = slc_stats.free + qlc_stats.free;
	return total_free_lines <= conv_ftl->cp.gc_thres_lines_high;
}
static inline bool should_gc_slc_soft(struct conv_ftl *conv_ftl)
{
	struct line_pool_stats slc_stats;
	collect_slc_stats(conv_ftl, &slc_stats);
	return slc_stats.free <= conv_ftl->slc_gc_free_thres_high;
}

static inline bool should_gc_slc_hard(struct conv_ftl *conv_ftl)
{
	struct line_pool_stats slc_stats;
	collect_slc_stats(conv_ftl, &slc_stats);
	return slc_stats.free < conv_ftl->slc_gc_free_thres_low;
}

static inline bool should_gc_qlc_high(struct conv_ftl *conv_ftl)
{
	struct line_pool_stats qlc_stats;
	collect_qlc_stats(conv_ftl, &qlc_stats);
	return qlc_stats.free <= conv_ftl->qlc_gc_free_thres_high;
}

static inline bool should_gc_slc_any_die_critical(struct conv_ftl *conv_ftl,
						   uint32_t *starved_die)
{
	uint32_t die_count = conv_ftl->die_count ? conv_ftl->die_count : 1;
	uint32_t die;

	if (!conv_ftl->slc_lunlm)
		return false;

	spin_lock(&conv_ftl->slc_lock);
	for (die = 0; die < die_count; die++) {
		struct line_mgmt *lm = &conv_ftl->slc_lunlm[die];
		if (lm->free_line_cnt == 0 && lm->victim_line_cnt > 0) {
			spin_unlock(&conv_ftl->slc_lock);
			*starved_die = die;
			return true;
		}
	}
	spin_unlock(&conv_ftl->slc_lock);
	return false;
}

static inline uint32_t pages_to_lines_ceil(uint64_t pages, uint32_t pgs_per_blk)
{
	if (!pgs_per_blk)
		return 0;
	return (uint32_t)div_u64(pages + pgs_per_blk - 1, pgs_per_blk);
}

enum foreground_gc_mode {
	FGC_MODE_SOFT = 0,
	FGC_MODE_HARD,
};

static noinline struct ppa get_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn)
{
	unsigned seq;
	struct ppa entry;

	if (unlikely(!conv_ftl || !conv_ftl->maptbl || !conv_ftl->ssd)) {
		if (printk_ratelimit()) {
			NVMEV_ERROR("get_maptbl_ent: bad state conv_ftl=%p maptbl=%p ssd=%p lpn=%llu\n",
				    conv_ftl,
				    conv_ftl ? conv_ftl->maptbl : NULL,
				    conv_ftl ? conv_ftl->ssd : NULL,
				    lpn);
		}
		return (struct ppa){ .ppa = UNMAPPED_PPA };
	}
	if (unlikely(lpn >= conv_ftl->ssd->sp.tt_pgs)) {
		if (printk_ratelimit()) {
			struct nvmev_cmd_debug *dbg = this_cpu_ptr(&nvmev_last_cmd);
			struct nvmev_maptbl_debug *md = this_cpu_ptr(&nvmev_last_maptbl);

			NVMEV_ERROR("get_maptbl_ent: lpn out of range lpn=%llu tt_pgs=%lu\n",
				    lpn, conv_ftl->ssd->sp.tt_pgs);
			NVMEV_ERROR("get_maptbl_ent: caller=%pS\n",
				    __builtin_return_address(0));
			NVMEV_ERROR("get_maptbl_ent: pid=%d comm=%s cpu=%d\n",
				    current->pid, current->comm, raw_smp_processor_id());
			if (md && md->site) {
				NVMEV_ERROR("get_maptbl_ent: callsite=%s lpn=%llu\n",
					    md->site, md->lpn);
			}
			if (dbg && dbg->valid) {
				NVMEV_ERROR("get_maptbl_ent: last_cmd opcode=0x%x nsid=%u slba=%llu len=%u sqid=%d ts=%llu\n",
					    dbg->opcode, dbg->nsid, dbg->slba, dbg->len,
					    dbg->sqid, dbg->ts);
			} else {
				NVMEV_ERROR("get_maptbl_ent: last_cmd unavailable\n");
			}
			dump_stack();
		}
		return (struct ppa){ .ppa = UNMAPPED_PPA };
	}
	do {
		seq = read_seqbegin(&conv_ftl->maptbl_lock);
		entry = conv_ftl->maptbl[lpn];
	} while (read_seqretry(&conv_ftl->maptbl_lock, seq));
	return entry;
}

static inline void die_change_reason_inc(struct conv_ftl *conv_ftl, uint8_t reason)
{
	switch (reason) {
	case NVMEV_DIE_CHANGE_HOST_APPEND:
		conv_ftl->lpn_changed_host_append++;
		break;
	case NVMEV_DIE_CHANGE_HOST_OVERWRITE:
		conv_ftl->lpn_changed_host_overwrite++;
		break;
	case NVMEV_DIE_CHANGE_GC:
		conv_ftl->lpn_changed_gc++;
		break;
	case NVMEV_DIE_CHANGE_SLC_TO_QLC:
		conv_ftl->lpn_changed_slc_to_qlc++;
		break;
	case NVMEV_DIE_CHANGE_REPROMOTE:
		conv_ftl->lpn_changed_repromote++;
		break;
	case NVMEV_DIE_CHANGE_QLC_REBALANCE:
		conv_ftl->lpn_changed_qlc_rebalance++;
		break;
	default:
		break;
	}
}

static inline void die_change_reason_dec(struct conv_ftl *conv_ftl, uint8_t reason)
{
	switch (reason) {
	case NVMEV_DIE_CHANGE_HOST_APPEND:
		conv_ftl->lpn_changed_host_append--;
		break;
	case NVMEV_DIE_CHANGE_HOST_OVERWRITE:
		conv_ftl->lpn_changed_host_overwrite--;
		break;
	case NVMEV_DIE_CHANGE_GC:
		conv_ftl->lpn_changed_gc--;
		break;
	case NVMEV_DIE_CHANGE_SLC_TO_QLC:
		conv_ftl->lpn_changed_slc_to_qlc--;
		break;
	case NVMEV_DIE_CHANGE_REPROMOTE:
		conv_ftl->lpn_changed_repromote--;
		break;
	case NVMEV_DIE_CHANGE_QLC_REBALANCE:
		conv_ftl->lpn_changed_qlc_rebalance--;
		break;
	default:
		break;
	}
}

static inline void set_maptbl_ent_reason(struct conv_ftl *conv_ftl, uint64_t lpn,
					 struct ppa *ppa, uint8_t reason)
{
	uint32_t new_die = 0;
	uint32_t old_die = 0;
	bool new_mapped;
	bool old_mapped;
	bool new_changed = false;
	bool old_changed = false;
	uint8_t old_reason = NVMEV_DIE_CHANGE_NONE;
	uint8_t new_reason = NVMEV_DIE_CHANGE_NONE;

	if (unlikely(!conv_ftl || !conv_ftl->maptbl || !conv_ftl->ssd)) {
		if (printk_ratelimit()) {
			NVMEV_ERROR("set_maptbl_ent: bad state conv_ftl=%p maptbl=%p ssd=%p lpn=%llu\n",
				    conv_ftl,
				    conv_ftl ? conv_ftl->maptbl : NULL,
				    conv_ftl ? conv_ftl->ssd : NULL,
				    lpn);
		}
		return;
	}
	NVMEV_ASSERT(lpn < conv_ftl->ssd->sp.tt_pgs);
	write_seqlock(&conv_ftl->maptbl_lock);
	old_mapped = mapped_ppa(&conv_ftl->maptbl[lpn]) && valid_ppa(conv_ftl, &conv_ftl->maptbl[lpn]);
	if (old_mapped)
		old_die = conv_ftl->maptbl[lpn].g.lun * conv_ftl->ssd->sp.nchs +
			  conv_ftl->maptbl[lpn].g.ch;
	new_mapped = mapped_ppa(ppa) && valid_ppa(conv_ftl, ppa);
	if (conv_ftl->lpn_die_changed)
		old_changed = conv_ftl->lpn_die_changed[lpn] != 0;
	if (conv_ftl->lpn_die_change_reason)
		old_reason = conv_ftl->lpn_die_change_reason[lpn];

	if (new_mapped && conv_ftl->lpn_initial_die) {
		new_die = ppa->g.lun * conv_ftl->ssd->sp.nchs + ppa->g.ch;
		if (conv_ftl->lpn_initial_die[lpn] == U16_MAX) {
			conv_ftl->lpn_initial_die[lpn] = (uint16_t)new_die;
			conv_ftl->lpn_initial_die_tracked++;
		}
		new_changed = ((uint16_t)new_die != conv_ftl->lpn_initial_die[lpn]);
	}

	if (conv_ftl->lpn_die_changed) {
		if (old_changed && !new_changed) {
			conv_ftl->lpn_current_die_changed--;
			die_change_reason_dec(conv_ftl, old_reason);
		} else if (!old_changed && new_changed) {
			conv_ftl->lpn_current_die_changed++;
			die_change_reason_inc(conv_ftl, reason);
		} else if (old_changed && new_changed && old_reason != reason &&
			   (!old_mapped || old_die != new_die)) {
			die_change_reason_dec(conv_ftl, old_reason);
			die_change_reason_inc(conv_ftl, reason);
		}
		conv_ftl->lpn_die_changed[lpn] = new_changed ? 1 : 0;
	}
	if (conv_ftl->lpn_die_change_reason) {
		if (new_changed)
			new_reason = reason;
		conv_ftl->lpn_die_change_reason[lpn] = new_reason;
	}
	conv_ftl->maptbl[lpn] = *ppa;
	write_sequnlock(&conv_ftl->maptbl_lock);
}

static inline void set_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *ppa)
{
	set_maptbl_ent_reason(conv_ftl, lpn, ppa, NVMEV_DIE_CHANGE_NONE);
}

static inline void clear_lpn_mapping(struct conv_ftl *conv_ftl, uint64_t lpn)
{
	struct ppa invalid = { .ppa = UNMAPPED_PPA };
	uint32_t old_chain_id = chain_id_for_lpn(conv_ftl, lpn);
	bool was_in_slc = false;

	if (conv_ftl && conv_ftl->page_in_slc && lpn < conv_ftl->ssd->sp.tt_pgs)
		was_in_slc = conv_ftl->page_in_slc[lpn];

	set_maptbl_ent(conv_ftl, lpn, &invalid);
	slc_resident_untrack_page(conv_ftl, lpn);
	if (conv_ftl->page_in_slc)
		conv_ftl->page_in_slc[lpn] = false;
	chain_slc_note_state_change(conv_ftl, old_chain_id, was_in_slc,
				       INVALID_CHAIN_ID, false, false);
	if (conv_ftl->lpn_chain_id)
		conv_ftl->lpn_chain_id[lpn] = INVALID_CHAIN_ID;
}

static uint64_t ppa2pgidx(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint64_t pgidx;
	uint64_t blk_offset;

	NVMEV_DEBUG("ppa2pgidx: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", ppa->g.ch, ppa->g.lun,
		    ppa->g.pl, ppa->g.blk, ppa->g.pg);

	if ((uint32_t)ppa->g.blk < conv_ftl->slc_blks_per_pl)
		blk_offset = (uint64_t)ppa->g.blk * conv_ftl->slc_pgs_per_blk;
	else
		blk_offset = (uint64_t)conv_ftl->slc_blks_per_pl * conv_ftl->slc_pgs_per_blk +
			     (uint64_t)(ppa->g.blk - conv_ftl->slc_blks_per_pl) *
			     conv_ftl->qlc_pgs_per_blk;

	pgidx = ppa->g.ch * spp->pgs_per_ch + ppa->g.lun * spp->pgs_per_lun +
		ppa->g.pl * spp->pgs_per_pl + blk_offset + ppa->g.pg;

	NVMEV_ASSERT(pgidx < spp->tt_pgs);

	return pgidx;
}

static noinline uint64_t get_rmap_ent(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(conv_ftl, ppa);
	unsigned seq;
	uint64_t lpn;

	do {
		seq = read_seqbegin(&conv_ftl->maptbl_lock);
		lpn = conv_ftl->rmap[pgidx];
	} while (read_seqretry(&conv_ftl->maptbl_lock, seq));
	return lpn;
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(conv_ftl, ppa);

	write_seqlock(&conv_ftl->maptbl_lock);
	conv_ftl->rmap[pgidx] = lpn;
	write_sequnlock(&conv_ftl->maptbl_lock);
}

static bool calc_global_avg_reads(struct conv_ftl *conv_ftl, uint64_t *avg_out)
{
	if (!conv_ftl || !avg_out)
		return false;

	if (!conv_ftl->global_valid_pg_cnt) {
		*avg_out = 0;
		return false;
	}

	*avg_out = div64_u64(conv_ftl->global_read_sum, conv_ftl->global_valid_pg_cnt);
	return true;
}

static bool calc_migration_avg_reads(struct conv_ftl *conv_ftl, uint64_t *avg_out)
{
	bool has_avg = false;

	if (!conv_ftl || !avg_out)
		return false;

	spin_lock(&conv_ftl->qlc_zone_lock);
	if (conv_ftl->qlc_migration_page_cnt) {
		*avg_out = div64_u64(conv_ftl->qlc_migration_read_sum,
				     conv_ftl->qlc_migration_page_cnt);
		has_avg = true;
	}
	spin_unlock(&conv_ftl->qlc_zone_lock);

	return has_avg;
}

static uint64_t get_dynamic_cold_threshold(struct conv_ftl *conv_ftl)
{
	struct heat_tracking *ht = conv_ftl ? &conv_ftl->heat_track : NULL;
	uint64_t avg_reads;

	if (calc_global_avg_reads(conv_ftl, &avg_reads)) {
		if (ht)
			ht->migration_threshold = avg_reads;
		return avg_reads;
	}

	return ht ? ht->migration_threshold : 0;
}

static bool recent_write_guard(struct conv_ftl *conv_ftl, uint64_t lpn)
{
	struct heat_tracking *ht;
	uint64_t epoch, age, guard_window;

	if (!conv_ftl)
		return false;

	ht = &conv_ftl->heat_track;
	if (!ht || !ht->write_epoch)
		return false;

	epoch = ht->write_epoch[lpn];
	if (epoch == 0)
		return false;

	age = conv_ftl->total_host_writes - epoch;
	guard_window = total_slc_pages(conv_ftl) * RECENT_WRITE_GUARD_PCT / 100U;
	if (!guard_window)
		guard_window = 1;

	return age < guard_window;
}

static void update_qlc_latency_zone(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *ppa)
{
	struct heat_tracking *ht = &conv_ftl->heat_track;
	struct nand_page *pg;
	uint64_t old_cnt, new_cnt;
	uint64_t read_cnt = 0;
	uint64_t avg_reads;
	uint8_t zone;

	if (!conv_ftl || !conv_ftl->qlc_page_wcnt)
		return;

	if (!valid_ppa(conv_ftl, ppa))
		return;
	if (lpn >= conv_ftl->ssd->sp.tt_pgs)
		return;

	pg = get_pg(conv_ftl->ssd, ppa);
	if (!pg)
		return;

	if (ht && ht->access_count)
		read_cnt = ht->access_count[lpn];

	spin_lock(&conv_ftl->qlc_zone_lock);

	old_cnt = conv_ftl->qlc_page_wcnt[lpn];
	if (old_cnt != ~0ULL) {
		if (old_cnt == 0) {
			conv_ftl->qlc_unique_pages++;
			conv_ftl->qlc_resident_page_cnt++;
			conv_ftl->qlc_resident_read_sum += read_cnt;
		}
		conv_ftl->qlc_page_wcnt[lpn] = old_cnt + 1;
	}

	new_cnt = conv_ftl->qlc_page_wcnt[lpn];
	avg_reads = conv_ftl->qlc_resident_page_cnt ?
		    div64_u64(conv_ftl->qlc_resident_read_sum, conv_ftl->qlc_resident_page_cnt) :
		    read_cnt;

	zone = pick_locked_qlc_page_type(conv_ftl, read_cnt >= avg_reads);
	pg->qlc_latency_zone = zone;

	if (qlc_zone_is_fast(zone))
		conv_ftl->qlc_fast_count++;
	else
		conv_ftl->qlc_slow_count++;

	NVMEV_DEBUG("[HLFA] lpn=%llu read_cnt=%llu avg=%llu zone=%u writes=%llu",
		    lpn, read_cnt, avg_reads, zone, new_cnt);

	spin_unlock(&conv_ftl->qlc_zone_lock);
}

static int access_count_show(struct seq_file *m, void *v)
{
	    struct conv_ftl *conv_ftl = m->private;
	        struct heat_tracking *ht = &conv_ftl->heat_track;
		    uint64_t tt_pgs, lpn;

		        if (!ht->access_count)
				        return 0;

			    tt_pgs = conv_ftl->ssd->sp.tt_pgs;
			        for (lpn = 0; lpn < tt_pgs; lpn++)
					        seq_printf(m, "%llu %llu\n", lpn, ht->access_count[lpn]);

				    return 0;
}

static int access_count_open(struct inode *inode, struct file *file)
{
	    return single_open(file, access_count_show, inode->i_private);
}

static const struct file_operations access_count_fops = {
	.owner = THIS_MODULE,
	.open = access_count_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int page_tier_show(struct seq_file *m, void *v)
{
	struct conv_ftl *conv_ftl = m->private;
	struct ssdparams *spp;
	uint64_t lpn;

	(void)v;

	if (!conv_ftl || !conv_ftl->ssd || !conv_ftl->page_in_slc)
		return 0;

	spp = &conv_ftl->ssd->sp;
	for (lpn = 0; lpn < spp->tt_pgs; lpn++) {
		struct ppa ppa = get_maptbl_ent(conv_ftl, lpn);
		bool in_slc;
		if (!mapped_ppa(&ppa) || !valid_ppa(conv_ftl, &ppa))
			continue;

		in_slc = conv_ftl->page_in_slc[lpn] || is_slc_block(conv_ftl, ppa.g.blk);
		if (in_slc)
			seq_printf(m, "%llu 1 -1\n", lpn);
		else
			seq_printf(m, "%llu 0 %u\n", lpn,
				   (unsigned int)get_qlc_zone_for_read(conv_ftl, &ppa));
	}

	return 0;
}

static int page_tier_open(struct inode *inode, struct file *file)
{
	return single_open(file, page_tier_show, inode->i_private);
}

static const struct file_operations page_tier_fops = {
	.owner = THIS_MODULE,
	.open = page_tier_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int page_die_show(struct seq_file *m, void *v)
{
	struct conv_ftl *conv_ftl = m->private;
	struct ssdparams *spp;
	uint64_t lpn;

	(void)v;

	if (!conv_ftl || !conv_ftl->ssd)
		return 0;

	spp = &conv_ftl->ssd->sp;
	for (lpn = 0; lpn < spp->tt_pgs; lpn++) {
		struct ppa ppa = get_maptbl_ent(conv_ftl, lpn);

		if (!mapped_ppa(&ppa) || !valid_ppa(conv_ftl, &ppa))
			continue;

		seq_printf(m, "%llu %u\n", lpn,
			   (unsigned int)encode_die(spp, &ppa));
	}

	return 0;
}

static int page_die_open(struct inode *inode, struct file *file)
{
	return single_open(file, page_die_show, inode->i_private);
}

static const struct file_operations page_die_fops = {
	.owner = THIS_MODULE,
	.open = page_die_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int page_chain_show(struct seq_file *m, void *v)
{
	struct conv_ftl *conv_ftl = m->private;
	struct ssdparams *spp;
	uint64_t lpn;

	(void)v;

	if (!conv_ftl || !conv_ftl->ssd || !conv_ftl->lpn_chain_id)
		return 0;

	spp = &conv_ftl->ssd->sp;
	for (lpn = 0; lpn < spp->tt_pgs; lpn++) {
		struct ppa ppa = get_maptbl_ent(conv_ftl, lpn);
		uint32_t chain_id;

		if (!mapped_ppa(&ppa) || !valid_ppa(conv_ftl, &ppa))
			continue;
		chain_id = conv_ftl->lpn_chain_id[lpn];
		if (!chain_id_valid(conv_ftl, chain_id))
			continue;

		seq_printf(m, "%llu %u\n", lpn, chain_id);
	}

	return 0;
}

static int page_chain_open(struct inode *inode, struct file *file)
{
	return single_open(file, page_chain_show, inode->i_private);
}

static const struct file_operations page_chain_fops = {
	.owner = THIS_MODULE,
	.open = page_chain_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int chain_alloc_events_show(struct seq_file *m, void *v)
{
	struct conv_ftl *conv_ftl = m->private;
	unsigned long flags;
	uint32_t count, start, i;

	(void)v;
	if (!conv_ftl)
		return 0;

	seq_printf(m,
		   "# next_chain_id=%u no_prev=%llu prev_chain_invalid=%llu capacity_fail=%llu\n",
		   conv_ftl->next_chain_id,
		   (unsigned long long)conv_ftl->chain_alloc_no_prev,
		   (unsigned long long)conv_ftl->chain_alloc_prev_chain_invalid,
		   (unsigned long long)conv_ftl->chain_alloc_capacity_fail);
	seq_puts(m, "seq chain_id lpn prev_link_lpn prev_chain_id seed_die append overwrite reason\n");
	if (!conv_ftl->chain_alloc_events)
		return 0;

	spin_lock_irqsave(&conv_ftl->event_log_lock, flags);
	count = conv_ftl->chain_alloc_event_count;
	start = (conv_ftl->chain_alloc_event_head + NVMEV_EVENT_LOG_CAP - count) %
		NVMEV_EVENT_LOG_CAP;
	for (i = 0; i < count; i++) {
		const struct nvmev_chain_alloc_event *evt =
			&conv_ftl->chain_alloc_events[(start + i) % NVMEV_EVENT_LOG_CAP];
		seq_printf(m, "%llu %u %llu %llu %u %u %u %u %s\n",
			   (unsigned long long)evt->seq,
			   evt->chain_id,
			   (unsigned long long)evt->lpn,
			   (unsigned long long)evt->prev_link_lpn,
			   evt->prev_chain_id,
			   evt->seed_die,
			   evt->is_append,
			   evt->is_overwrite,
			   chain_alloc_reason_name(evt->reason));
	}
	spin_unlock_irqrestore(&conv_ftl->event_log_lock, flags);
	return 0;
}

static int chain_alloc_events_open(struct inode *inode, struct file *file)
{
	return single_open(file, chain_alloc_events_show, inode->i_private);
}

static const struct file_operations chain_alloc_events_fops = {
	.owner = THIS_MODULE,
	.open = chain_alloc_events_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int chain_cold_events_show(struct seq_file *m, void *v)
{
	struct conv_ftl *conv_ftl = m->private;
	unsigned long flags;
	uint32_t count, start, i;

	(void)v;
	if (!conv_ftl)
		return 0;

	seq_puts(m,
		 "seq chain_id die ch lun pl blk method pages_moved heat_sum heat_pages cold_pages owned_pages\n");
	if (!conv_ftl->chain_cold_events)
		return 0;

	spin_lock_irqsave(&conv_ftl->event_log_lock, flags);
	count = conv_ftl->chain_cold_event_count;
	start = (conv_ftl->chain_cold_event_head + NVMEV_EVENT_LOG_CAP - count) %
		NVMEV_EVENT_LOG_CAP;
	for (i = 0; i < count; i++) {
		const struct nvmev_chain_cold_event *evt =
			&conv_ftl->chain_cold_events[(start + i) % NVMEV_EVENT_LOG_CAP];
		seq_printf(m, "%llu %u %u %u %u %u %u %s %u %llu %u %u %u\n",
			   (unsigned long long)evt->seq,
			   evt->chain_id,
			   evt->die,
			   evt->ch,
			   evt->lun,
			   evt->pl,
			   evt->blk,
			   chain_cold_method_name(evt->method),
			   evt->pages_moved,
			   (unsigned long long)evt->heat_sum,
			   evt->heat_pages,
			   evt->cold_pages,
			   evt->owned_pages);
	}
	spin_unlock_irqrestore(&conv_ftl->event_log_lock, flags);
	return 0;
}

static int chain_cold_events_open(struct inode *inode, struct file *file)
{
	return single_open(file, chain_cold_events_show, inode->i_private);
}

static const struct file_operations chain_cold_events_fops = {
	.owner = THIS_MODULE,
	.open = chain_cold_events_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int gc_victim_events_show(struct seq_file *m, void *v)
{
	struct conv_ftl *conv_ftl = m->private;
	unsigned long flags;
	uint32_t count, start, i;

	(void)v;
	if (!conv_ftl)
		return 0;

	seq_puts(m, "seq tier die blk owner_chain vpc ipc high_purity\n");
	if (!conv_ftl->gc_victim_events)
		return 0;

	spin_lock_irqsave(&conv_ftl->event_log_lock, flags);
	count = conv_ftl->gc_victim_event_count;
	start = (conv_ftl->gc_victim_event_head + NVMEV_EVENT_LOG_CAP - count) %
		NVMEV_EVENT_LOG_CAP;
	for (i = 0; i < count; i++) {
		const struct nvmev_gc_victim_event *evt =
			&conv_ftl->gc_victim_events[(start + i) % NVMEV_EVENT_LOG_CAP];
		seq_printf(m, "%llu %s %u %u %u %u %u %u\n",
			   (unsigned long long)evt->seq,
			   evt->is_slc ? "slc" : "qlc",
			   evt->die,
			   evt->blk,
			   evt->owner_chain,
			   evt->vpc,
			   evt->ipc,
			   evt->high_purity);
	}
	spin_unlock_irqrestore(&conv_ftl->event_log_lock, flags);
	return 0;
}

static int gc_victim_events_open(struct inode *inode, struct file *file)
{
	return single_open(file, gc_victim_events_show, inode->i_private);
}

static const struct file_operations gc_victim_events_fops = {
	.owner = THIS_MODULE,
	.open = gc_victim_events_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int lpn_die_change_stats_show(struct seq_file *m, void *v)
{
	struct conv_ftl *conv_ftl = m->private;

	(void)v;

	if (!conv_ftl)
		return 0;

	seq_printf(m, "tracked_lpns %llu\n",
		   (unsigned long long)conv_ftl->lpn_initial_die_tracked);
	seq_printf(m, "current_changed_lpns %llu\n",
		   (unsigned long long)conv_ftl->lpn_current_die_changed);
	seq_printf(m, "host_append_die_changed %llu\n",
		   (unsigned long long)conv_ftl->lpn_changed_host_append);
	seq_printf(m, "host_overwrite_die_changed %llu\n",
		   (unsigned long long)conv_ftl->lpn_changed_host_overwrite);
	seq_printf(m, "gc_die_changed %llu\n",
		   (unsigned long long)conv_ftl->lpn_changed_gc);
	seq_printf(m, "slc_to_qlc_die_changed %llu\n",
		   (unsigned long long)conv_ftl->lpn_changed_slc_to_qlc);
	seq_printf(m, "repromote_die_changed %llu\n",
		   (unsigned long long)conv_ftl->lpn_changed_repromote);
	seq_printf(m, "qlc_rebalance_die_changed %llu\n",
		   (unsigned long long)conv_ftl->lpn_changed_qlc_rebalance);
	return 0;
}

static int lpn_die_change_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, lpn_die_change_stats_show, inode->i_private);
}

static const struct file_operations lpn_die_change_stats_fops = {
	.owner = THIS_MODULE,
	.open = lpn_die_change_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int die_affinity_stats_show(struct seq_file *m, void *v)
{
	struct conv_ftl *conv_ftl = m->private;

	(void)v;

	if (!conv_ftl)
		return 0;

	seq_printf(m, "append_requests %llu\n",
		   (unsigned long long)conv_ftl->die_aff_append_requests);
	seq_printf(m, "append_effective %llu\n",
		   (unsigned long long)conv_ftl->die_aff_append_effective);
	seq_printf(m, "overwrite_requests %llu\n",
		   (unsigned long long)conv_ftl->die_aff_overwrite_requests);
	seq_printf(m, "overwrite_effective %llu\n",
		   (unsigned long long)conv_ftl->die_aff_overwrite_effective);
	seq_printf(m, "chains_allocated %u\n", conv_ftl->next_chain_id);
	seq_printf(m, "chain_block_migration_attempts %llu\n",
		   (unsigned long long)conv_ftl->chain_block_migration_attempts);
	seq_printf(m, "chain_block_migration_pages %llu\n",
		   (unsigned long long)conv_ftl->chain_block_migration_pages);
	seq_printf(m, "chain_block_migration_skip_budget %llu\n",
		   (unsigned long long)conv_ftl->chain_block_migration_skip_budget);
	seq_printf(m, "chain_chunk_migration_attempts %llu\n",
		   (unsigned long long)conv_ftl->chain_chunk_migration_attempts);
	seq_printf(m, "chain_chunk_migration_pages %llu\n",
		   (unsigned long long)conv_ftl->chain_chunk_migration_pages);
	seq_printf(m, "chain_gc_to_qlc_pages %llu\n",
		   (unsigned long long)conv_ftl->chain_gc_to_qlc_pages);
	return 0;
}

static int die_affinity_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, die_affinity_stats_show, inode->i_private);
}

static const struct file_operations die_affinity_stats_fops = {
	.owner = THIS_MODULE,
	.open = die_affinity_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static inline bool test_phase_enabled(const struct conv_ftl *conv_ftl)
{
	return conv_ftl && READ_ONCE(conv_ftl->test_phase_active);
}

static void test_phase_reset_stats(struct conv_ftl *conv_ftl)
{
	if (!conv_ftl)
		return;

	atomic64_set(&conv_ftl->test_phase_read_reqs, 0);
	atomic64_set(&conv_ftl->test_phase_overwrite_reqs, 0);
	atomic64_set(&conv_ftl->test_phase_bg_repromote_ops, 0);
	atomic64_set(&conv_ftl->test_phase_bg_qlc_rebalance_ops, 0);
	atomic64_set(&conv_ftl->test_phase_read_bg_conflicts, 0);
	atomic64_set(&conv_ftl->test_phase_read_overwrite_conflicts, 0);
	atomic64_set(&conv_ftl->test_phase_read_die_conflicts, 0);
	atomic64_set(&conv_ftl->test_phase_read_die_wait_ns, 0);
	atomic_set(&conv_ftl->test_phase_active_reads, 0);
	atomic_set(&conv_ftl->test_phase_active_overwrites, 0);
	atomic_set(&conv_ftl->test_phase_active_bg_ops, 0);
}

static void test_phase_log_summary(struct conv_ftl *conv_ftl, const char *phase)
{
	if (!conv_ftl)
		return;

	NVMEV_INFO("[TEST_PHASE] %s reads=%lld overwrites=%lld bg_repromote=%lld "
		   "bg_qlc_rebalance=%lld read_bg_conflicts=%lld read_overwrite_conflicts=%lld "
		   "read_die_conflicts=%lld read_die_wait_ns=%lld\n",
		   phase ? phase : "summary",
		   atomic64_read(&conv_ftl->test_phase_read_reqs),
		   atomic64_read(&conv_ftl->test_phase_overwrite_reqs),
		   atomic64_read(&conv_ftl->test_phase_bg_repromote_ops),
		   atomic64_read(&conv_ftl->test_phase_bg_qlc_rebalance_ops),
		   atomic64_read(&conv_ftl->test_phase_read_bg_conflicts),
		   atomic64_read(&conv_ftl->test_phase_read_overwrite_conflicts),
		   atomic64_read(&conv_ftl->test_phase_read_die_conflicts),
		   atomic64_read(&conv_ftl->test_phase_read_die_wait_ns));
	superblock_stats_log_summary(conv_ftl, phase);
}

static void test_phase_note_read_begin(struct conv_ftl *conv_ftl, bool *tracked)
{
	if (tracked)
		*tracked = false;
	if (!test_phase_enabled(conv_ftl))
		return;

	atomic64_inc(&conv_ftl->test_phase_read_reqs);
	atomic_inc(&conv_ftl->test_phase_active_reads);
	if (atomic_read(&conv_ftl->test_phase_active_bg_ops) > 0)
		atomic64_inc(&conv_ftl->test_phase_read_bg_conflicts);
	if (atomic_read(&conv_ftl->test_phase_active_overwrites) > 0)
		atomic64_inc(&conv_ftl->test_phase_read_overwrite_conflicts);
	if (tracked)
		*tracked = true;
}

static void test_phase_note_read_end(struct conv_ftl *conv_ftl, bool tracked)
{
	if (!tracked || !conv_ftl)
		return;
	atomic_dec_if_positive(&conv_ftl->test_phase_active_reads);
}

static void test_phase_note_overwrite_begin(struct conv_ftl *conv_ftl, bool *tracked)
{
	if (tracked)
		*tracked = false;
	if (!test_phase_enabled(conv_ftl))
		return;

	atomic64_inc(&conv_ftl->test_phase_overwrite_reqs);
	atomic_inc(&conv_ftl->test_phase_active_overwrites);
	if (atomic_read(&conv_ftl->test_phase_active_reads) > 0)
		atomic64_inc(&conv_ftl->test_phase_read_overwrite_conflicts);
	if (tracked)
		*tracked = true;
}

static void test_phase_note_overwrite_end(struct conv_ftl *conv_ftl, bool tracked)
{
	if (!tracked || !conv_ftl)
		return;
	atomic_dec_if_positive(&conv_ftl->test_phase_active_overwrites);
}

static void test_phase_note_bg_begin(struct conv_ftl *conv_ftl,
				     atomic64_t *specific_counter,
				     bool *tracked)
{
	if (tracked)
		*tracked = false;
	if (!test_phase_enabled(conv_ftl))
		return;

	if (specific_counter)
		atomic64_inc(specific_counter);
	atomic_inc(&conv_ftl->test_phase_active_bg_ops);
	if (atomic_read(&conv_ftl->test_phase_active_reads) > 0)
		atomic64_inc(&conv_ftl->test_phase_read_bg_conflicts);
	if (tracked)
		*tracked = true;
}

static void test_phase_note_bg_end(struct conv_ftl *conv_ftl, bool tracked)
{
	if (!tracked || !conv_ftl)
		return;
	atomic_dec_if_positive(&conv_ftl->test_phase_active_bg_ops);
}

static ssize_t test_phase_read(struct file *file, char __user *user_buf,
			       size_t len, loff_t *ppos)
{
	struct conv_ftl *conv_ftl = file->private_data;
	char buf[32];
	int out_len;

	if (!conv_ftl)
		return -EINVAL;

	out_len = scnprintf(buf, sizeof(buf), "%u\n",
			    test_phase_enabled(conv_ftl) ? 1U : 0U);
	return simple_read_from_buffer(user_buf, len, ppos, buf, out_len);
}

static ssize_t test_phase_write(struct file *file, const char __user *user_buf,
				size_t len, loff_t *ppos)
{
	struct conv_ftl *conv_ftl = file->private_data;
	char kbuf[16];
	size_t copy;
	bool enabled;
	int rc;

	(void)ppos;

	if (!conv_ftl)
		return -EINVAL;
	if (len == 0)
		return 0;

	copy = min(len, sizeof(kbuf) - 1);
	if (copy_from_user(kbuf, user_buf, copy))
		return -EFAULT;
	kbuf[copy] = '\0';

	rc = kstrtobool(skip_spaces(kbuf), &enabled);
	if (rc)
		return rc;

	if (enabled) {
		test_phase_reset_stats(conv_ftl);
		WRITE_ONCE(conv_ftl->test_phase_active, true);
		NVMEV_INFO("[TEST_PHASE] enter\n");
	} else {
		WRITE_ONCE(conv_ftl->test_phase_active, false);
		test_phase_log_summary(conv_ftl, "exit");
	}

	return len;
}

static int test_phase_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return nonseekable_open(inode, file);
}

static const struct file_operations test_phase_fops = {
	.owner = THIS_MODULE,
	.open = test_phase_open,
	.read = test_phase_read,
	.write = test_phase_write,
	.llseek = no_llseek,
};

static int test_phase_stats_show(struct seq_file *m, void *v)
{
	struct conv_ftl *conv_ftl = m->private;

	(void)v;

	if (!conv_ftl)
		return 0;

	seq_printf(m, "active %u\n", test_phase_enabled(conv_ftl) ? 1U : 0U);
	seq_printf(m, "read_requests %lld\n",
		   atomic64_read(&conv_ftl->test_phase_read_reqs));
	seq_printf(m, "overwrite_requests %lld\n",
		   atomic64_read(&conv_ftl->test_phase_overwrite_reqs));
	seq_printf(m, "bg_repromote_ops %lld\n",
		   atomic64_read(&conv_ftl->test_phase_bg_repromote_ops));
	seq_printf(m, "bg_qlc_rebalance_ops %lld\n",
		   atomic64_read(&conv_ftl->test_phase_bg_qlc_rebalance_ops));
	seq_printf(m, "read_bg_conflicts %lld\n",
		   atomic64_read(&conv_ftl->test_phase_read_bg_conflicts));
	seq_printf(m, "read_overwrite_conflicts %lld\n",
		   atomic64_read(&conv_ftl->test_phase_read_overwrite_conflicts));
	seq_printf(m, "read_die_conflicts %lld\n",
		   atomic64_read(&conv_ftl->test_phase_read_die_conflicts));
	seq_printf(m, "read_die_wait_ns %lld\n",
		   atomic64_read(&conv_ftl->test_phase_read_die_wait_ns));
	seq_printf(m, "active_reads %d\n",
		   atomic_read(&conv_ftl->test_phase_active_reads));
	seq_printf(m, "active_overwrites %d\n",
		   atomic_read(&conv_ftl->test_phase_active_overwrites));
	seq_printf(m, "active_bg_ops %d\n",
		   atomic_read(&conv_ftl->test_phase_active_bg_ops));
	return 0;
}

static int test_phase_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, test_phase_stats_show, inode->i_private);
}

static const struct file_operations test_phase_stats_fops = {
	.owner = THIS_MODULE,
	.open = test_phase_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

struct superblock_health_snapshot {
	struct line_pool_stats slc;
	struct line_pool_stats qlc;
	uint32_t sb_total;
	uint32_t sb_state_free;
	uint32_t sb_state_active;
	uint32_t sb_state_closed;
	uint32_t sb_state_unknown;
	uint32_t sb_state_active_counted;
	uint32_t sb_used;
	uint32_t sb_open_writer;
	uint32_t sb_pure90;
	uint32_t sb_pure60_89;
	uint32_t sb_mixed_lt60;
	uint32_t sb_pure_cold_candidates;
	uint32_t slc_closed_migrated_victim;
	uint32_t slc_closed_recent_guard;
	uint32_t slc_closed_empty_lines;
	uint32_t slc_closed_full_valid_lines;
	uint32_t slc_closed_invalid_only_lines;
	uint32_t slc_closed_mixed_lines;
	uint32_t slc_closed_partial_valid_only_lines;
	uint32_t slc_closed_heat_zero_lines;
	uint32_t slc_closed_heat_le_global_lines;
	uint64_t slc_closed_valid_pages;
	uint64_t slc_closed_invalid_pages;
	uint64_t slc_closed_heat_sum;
	uint64_t slc_closed_heat_min;
	uint64_t slc_closed_heat_max;
	uint32_t slc_closed_heat_count;
	uint64_t owner_ratio_sum;
	uint64_t owner_ratio_weighted_sum;
	uint64_t owner_ratio_weight;
	uint64_t heat_sum;
	uint32_t heat_sb_count;
	uint64_t global_avg_heat;
};

static uint64_t pct_u64(uint64_t numerator, uint64_t denominator)
{
	if (!denominator)
		return 0;
	return div64_u64(numerator * 100ULL, denominator);
}

static void superblock_collect_health(struct conv_ftl *conv_ftl,
				      struct superblock_health_snapshot *snap)
{
	struct ssdparams *spp;
	uint32_t blk_id;

	memset(snap, 0, sizeof(*snap));
	if (!conv_ftl || !conv_ftl->ssd)
		return;

	spp = &conv_ftl->ssd->sp;
	collect_slc_stats(conv_ftl, &snap->slc);
	collect_qlc_stats(conv_ftl, &snap->qlc);
	snap->sb_total = conv_ftl->slc_blks_per_pl;
	(void)calc_global_avg_reads(conv_ftl, &snap->global_avg_heat);

	for (blk_id = 0; blk_id < conv_ftl->slc_blks_per_pl; blk_id++) {
		struct slc_sb_summary_no1 sum;
		uint64_t owner_ratio;
		bool state_closed = false;

		if (conv_ftl->slc_sb_state && blk_id < spp->blks_per_pl) {
			switch (conv_ftl->slc_sb_state[blk_id]) {
			case NVMEV_SB_FREE:
				snap->sb_state_free++;
				break;
			case NVMEV_SB_ACTIVE:
				snap->sb_state_active++;
				break;
			case NVMEV_SB_CLOSED:
				snap->sb_state_closed++;
				state_closed = true;
				break;
			default:
				snap->sb_state_unknown++;
				break;
			}
		} else {
			snap->sb_state_unknown++;
		}
		if (conv_ftl->slc_sb_active_counted &&
		    blk_id < spp->blks_per_pl &&
		    conv_ftl->slc_sb_active_counted[blk_id])
			snap->sb_state_active_counted++;
		if (state_closed) {
			if (conv_ftl->slc_sb_migrated_victim &&
			    conv_ftl->slc_sb_migrated_victim[blk_id])
				snap->slc_closed_migrated_victim++;
			if (slc_sb_recent_guarded(conv_ftl, blk_id))
				snap->slc_closed_recent_guard++;
		}

		if (!slc_sb_collect_summary(conv_ftl, blk_id, &sum)) {
			if (state_closed)
				snap->slc_closed_empty_lines++;
			continue;
		}
		if (state_closed) {
			uint32_t sb_pages = slc_pages_per_superblock(conv_ftl);

			snap->slc_closed_valid_pages += sum.total_vpc;
			snap->slc_closed_invalid_pages += sum.total_ipc;
			if (sum.total_vpc == sb_pages && sum.total_ipc == 0)
				snap->slc_closed_full_valid_lines++;
			else if (sum.total_vpc == 0 && sum.total_ipc > 0)
				snap->slc_closed_invalid_only_lines++;
			else if (sum.total_vpc > 0 && sum.total_ipc > 0)
				snap->slc_closed_mixed_lines++;
			else if (sum.total_vpc > 0)
				snap->slc_closed_partial_valid_only_lines++;
			if (sum.heat_count) {
				if (!snap->slc_closed_heat_count ||
				    sum.avg_heat < snap->slc_closed_heat_min)
					snap->slc_closed_heat_min = sum.avg_heat;
				if (!snap->slc_closed_heat_count ||
				    sum.avg_heat > snap->slc_closed_heat_max)
					snap->slc_closed_heat_max = sum.avg_heat;
				snap->slc_closed_heat_sum += sum.avg_heat;
				snap->slc_closed_heat_count++;
				if (sum.avg_heat == 0)
					snap->slc_closed_heat_zero_lines++;
				if (sum.avg_heat <= snap->global_avg_heat)
					snap->slc_closed_heat_le_global_lines++;
			}
		}
		if (!sum.total_vpc)
			continue;

		snap->sb_used++;
		if (sum.open_writer)
			snap->sb_open_writer++;
		owner_ratio = pct_u64(sum.owner_pages, sum.total_vpc);
		snap->owner_ratio_sum += owner_ratio;
		snap->owner_ratio_weighted_sum += owner_ratio * sum.total_vpc;
		snap->owner_ratio_weight += sum.total_vpc;
		if (sum.heat_count) {
			snap->heat_sum += sum.avg_heat;
			snap->heat_sb_count++;
		}

		if (owner_ratio >= 90ULL)
			snap->sb_pure90++;
		else if (owner_ratio >= 60ULL)
			snap->sb_pure60_89++;
		else
			snap->sb_mixed_lt60++;
		if (slc_sb_summary_tier(&sum, snap->global_avg_heat) == 1)
			snap->sb_pure_cold_candidates++;
	}
}

static void superblock_stats_print(struct seq_file *m, struct conv_ftl *conv_ftl,
				   const struct superblock_health_snapshot *snap)
{
	uint64_t gc_total = conv_ftl->slc_sb_gc_invalid_pages +
			    conv_ftl->slc_sb_gc_valid_pages;
	uint64_t qlc_resident = conv_ftl->qlc_fast_count + conv_ftl->qlc_slow_count;
	uint64_t avg_owner_ratio = snap->sb_used ?
		div64_u64(snap->owner_ratio_sum, snap->sb_used) : 0;
	uint64_t weighted_owner_ratio = snap->owner_ratio_weight ?
		div64_u64(snap->owner_ratio_weighted_sum, snap->owner_ratio_weight) : 0;
	uint64_t avg_sb_heat = snap->heat_sb_count ?
		div64_u64(snap->heat_sum, snap->heat_sb_count) : 0;
	uint64_t avg_closed_heat = snap->slc_closed_heat_count ?
		div64_u64(snap->slc_closed_heat_sum, snap->slc_closed_heat_count) : 0;
	uint64_t avg_erase_ns = conv_ftl->slc_sb_gc_erase_ops ?
		div64_u64(conv_ftl->slc_sb_gc_erase_time_ns,
			  conv_ftl->slc_sb_gc_erase_ops) : 0;
	uint32_t recent_guarded = 0;
	uint32_t i;

	if (conv_ftl->slc_sb_recent_guard) {
		for (i = 0; i < conv_ftl->slc_blks_per_pl; i++) {
			if (conv_ftl->slc_sb_recent_guard[i])
				recent_guarded++;
		}
	}

	seq_puts(m, "# superblock_stats: point-in-time counters; *_pct is integer percent\n");
	seq_printf(m, "die_count %u\n", superblock_die_count(conv_ftl));
	seq_printf(m, "active_sb_limit %u\n", NVMEV_SUPERBLOCK_ACTIVE_LIMIT);
	seq_printf(m, "active_sb_count %u\n", conv_ftl->active_sb_count);
	seq_printf(m, "qlc_active_sb_count %u\n", conv_ftl->qlc_active_sb_count);
	seq_printf(m, "slc_pages_per_superblock %u\n", slc_pages_per_superblock(conv_ftl));
	seq_printf(m, "qlc_pages_per_superblock %u\n", qlc_pages_per_superblock(conv_ftl));
	seq_printf(m, "slc_pages_per_block %u\n", conv_ftl->slc_pgs_per_blk);
	seq_printf(m, "qlc_pages_per_block %u\n", conv_ftl->qlc_pgs_per_blk);

	seq_printf(m, "slc_total_lines %u\n", snap->slc.total);
	seq_printf(m, "slc_free_lines %u\n", snap->slc.free);
	seq_printf(m, "slc_victim_lines %u\n", snap->slc.victim);
	seq_printf(m, "slc_full_lines %u\n", snap->slc.full);
	seq_printf(m, "slc_free_line_pct %llu\n",
		   (unsigned long long)pct_u64(snap->slc.free, snap->slc.total));
	seq_printf(m, "qlc_total_lines %u\n", snap->qlc.total);
	seq_printf(m, "qlc_free_lines %u\n", snap->qlc.free);
	seq_printf(m, "qlc_victim_lines %u\n", snap->qlc.victim);
	seq_printf(m, "qlc_full_lines %u\n", snap->qlc.full);
	seq_printf(m, "qlc_free_line_pct %llu\n",
		   (unsigned long long)pct_u64(snap->qlc.free, snap->qlc.total));
	seq_printf(m, "qlc_fast_pages %llu\n",
		   (unsigned long long)conv_ftl->qlc_fast_count);
	seq_printf(m, "qlc_slow_pages %llu\n",
		   (unsigned long long)conv_ftl->qlc_slow_count);
	seq_printf(m, "qlc_fast_page_pct %llu\n",
		   (unsigned long long)pct_u64(conv_ftl->qlc_fast_count, qlc_resident));

	seq_printf(m, "sb_total %u\n", snap->sb_total);
	seq_printf(m, "sb_state_free %u\n", snap->sb_state_free);
	seq_printf(m, "sb_state_active %u\n", snap->sb_state_active);
	seq_printf(m, "sb_state_closed %u\n", snap->sb_state_closed);
	seq_printf(m, "sb_state_unknown %u\n", snap->sb_state_unknown);
	seq_printf(m, "sb_state_active_counted %u\n", snap->sb_state_active_counted);
	seq_printf(m, "sb_used %u\n", snap->sb_used);
	seq_printf(m, "sb_open_writer %u\n", snap->sb_open_writer);
	seq_printf(m, "sb_pure_ge90 %u\n", snap->sb_pure90);
	seq_printf(m, "sb_pure_60_89 %u\n", snap->sb_pure60_89);
	seq_printf(m, "sb_mixed_lt60 %u\n", snap->sb_mixed_lt60);
	seq_printf(m, "sb_pure_cold_candidates %u\n", snap->sb_pure_cold_candidates);
	seq_printf(m, "sb_avg_owner_ratio_pct %llu\n",
		   (unsigned long long)avg_owner_ratio);
	seq_printf(m, "sb_weighted_owner_ratio_pct %llu\n",
		   (unsigned long long)weighted_owner_ratio);
	seq_printf(m, "sb_avg_heat %llu\n", (unsigned long long)avg_sb_heat);
	seq_printf(m, "global_avg_heat %llu\n",
		   (unsigned long long)snap->global_avg_heat);
	seq_printf(m, "slc_closed_migrated_victim_lines %u\n",
		   snap->slc_closed_migrated_victim);
	seq_printf(m, "slc_closed_recent_guard_lines %u\n",
		   snap->slc_closed_recent_guard);
	seq_printf(m, "slc_closed_empty_lines %u\n",
		   snap->slc_closed_empty_lines);
	seq_printf(m, "slc_closed_full_valid_lines %u\n",
		   snap->slc_closed_full_valid_lines);
	seq_printf(m, "slc_closed_invalid_only_lines %u\n",
		   snap->slc_closed_invalid_only_lines);
	seq_printf(m, "slc_closed_mixed_lines %u\n",
		   snap->slc_closed_mixed_lines);
	seq_printf(m, "slc_closed_partial_valid_only_lines %u\n",
		   snap->slc_closed_partial_valid_only_lines);
	seq_printf(m, "slc_closed_valid_pages %llu\n",
		   (unsigned long long)snap->slc_closed_valid_pages);
	seq_printf(m, "slc_closed_invalid_pages %llu\n",
		   (unsigned long long)snap->slc_closed_invalid_pages);
	seq_printf(m, "slc_closed_avg_heat %llu\n",
		   (unsigned long long)avg_closed_heat);
	seq_printf(m, "slc_closed_min_heat %llu\n",
		   (unsigned long long)snap->slc_closed_heat_min);
	seq_printf(m, "slc_closed_max_heat %llu\n",
		   (unsigned long long)snap->slc_closed_heat_max);
	seq_printf(m, "slc_closed_heat_zero_lines %u\n",
		   snap->slc_closed_heat_zero_lines);
	seq_printf(m, "slc_closed_heat_le_global_lines %u\n",
		   snap->slc_closed_heat_le_global_lines);

	seq_printf(m, "slc_sb_migration_attempts %llu\n",
		   (unsigned long long)conv_ftl->slc_sb_migration_attempts);
	seq_printf(m, "slc_sb_migration_pages %llu\n",
		   (unsigned long long)conv_ftl->slc_sb_migration_pages);
	seq_printf(m, "slc_sb_migrated_victim_queued %u\n",
		   conv_ftl->slc_sb_migrated_victim_count);
	seq_printf(m, "slc_sb_migrated_victim_enqueues %llu\n",
		   (unsigned long long)conv_ftl->slc_sb_migration_victim_enqueues);
	seq_printf(m, "slc_sb_migrated_victim_dequeues %llu\n",
		   (unsigned long long)conv_ftl->slc_sb_migration_victim_dequeues);
		seq_printf(m, "slc_sb_migrated_victim_stale %llu\n",
			   (unsigned long long)conv_ftl->slc_sb_migration_victim_stale);
		seq_printf(m, "slc_sb_recent_guarded %u\n", recent_guarded);
		seq_printf(m, "slc_sb_recent_guard_ring_size %u\n",
			   conv_ftl->slc_recent_guard_ring_size);
		seq_printf(m, "slc_sb_recent_guard_skips %llu\n",
			   (unsigned long long)conv_ftl->slc_sb_recent_guard_skips);
		seq_printf(m, "slc_sb_recent_guard_forced %llu\n",
			   (unsigned long long)conv_ftl->slc_sb_recent_guard_forced);
		seq_printf(m, "chain_block_migration_attempts %llu\n",
			   (unsigned long long)conv_ftl->chain_block_migration_attempts);
	seq_printf(m, "chain_block_migration_pages %llu\n",
		   (unsigned long long)conv_ftl->chain_block_migration_pages);
	seq_printf(m, "chain_chunk_migration_attempts %llu\n",
		   (unsigned long long)conv_ftl->chain_chunk_migration_attempts);
	seq_printf(m, "chain_chunk_migration_pages %llu\n",
		   (unsigned long long)conv_ftl->chain_chunk_migration_pages);
	seq_printf(m, "chain_gc_to_qlc_pages %llu\n",
		   (unsigned long long)conv_ftl->chain_gc_to_qlc_pages);
	seq_printf(m, "repromote_chain_alloc_pages %llu\n",
		   (unsigned long long)conv_ftl->repromote_chain_alloc_pages);
		seq_printf(m, "repromote_gc_pool_pages %llu\n",
			   (unsigned long long)conv_ftl->repromote_gc_pool_pages);
		seq_printf(m, "repromote_skip_active_cap %llu\n",
			   (unsigned long long)conv_ftl->repromote_skip_active_cap);
		seq_printf(m, "qlc_closed_repromote_queued %u\n",
			   conv_ftl->qlc_closed_repromote_count);
		seq_printf(m, "qlc_closed_repromote_enqueues %llu\n",
			   (unsigned long long)conv_ftl->qlc_closed_repromote_enqueues);
		seq_printf(m, "qlc_closed_repromote_dequeues %llu\n",
			   (unsigned long long)conv_ftl->qlc_closed_repromote_dequeues);
		seq_printf(m, "qlc_closed_repromote_drops %llu\n",
			   (unsigned long long)conv_ftl->qlc_closed_repromote_drops);
		seq_printf(m, "qlc_closed_repromote_scans %llu\n",
			   (unsigned long long)conv_ftl->qlc_closed_repromote_scans);
		seq_printf(m, "qlc_closed_repromote_pages %llu\n",
			   (unsigned long long)conv_ftl->qlc_closed_repromote_pages);
		seq_printf(m, "chain_slc_die_reroute_count %llu\n",
			   (unsigned long long)conv_ftl->chain_slc_die_reroute_count);

	seq_printf(m, "slc_sb_gc_count %llu\n",
		   (unsigned long long)conv_ftl->slc_sb_gc_count);
	seq_printf(m, "slc_sb_gc_valid_pages %llu\n",
		   (unsigned long long)conv_ftl->slc_sb_gc_valid_pages);
	seq_printf(m, "slc_sb_gc_invalid_pages %llu\n",
		   (unsigned long long)conv_ftl->slc_sb_gc_invalid_pages);
	seq_printf(m, "slc_sb_gc_reclaim_eff_pct %llu\n",
		   (unsigned long long)pct_u64(conv_ftl->slc_sb_gc_invalid_pages,
					       gc_total));
	seq_printf(m, "slc_sb_gc_erase_ops %llu\n",
		   (unsigned long long)conv_ftl->slc_sb_gc_erase_ops);
	seq_printf(m, "slc_sb_gc_erase_time_ns %llu\n",
		   (unsigned long long)conv_ftl->slc_sb_gc_erase_time_ns);
	seq_printf(m, "slc_sb_gc_avg_erase_time_ns %llu\n",
		   (unsigned long long)avg_erase_ns);
	seq_printf(m, "slc_sb_gc_tier_empty %llu\n",
		   (unsigned long long)conv_ftl->slc_sb_gc_tier_empty);
	seq_printf(m, "slc_sb_gc_tier_pure_cold %llu\n",
		   (unsigned long long)conv_ftl->slc_sb_gc_tier_pure_cold);
	seq_printf(m, "slc_sb_gc_tier_mixed %llu\n",
		   (unsigned long long)conv_ftl->slc_sb_gc_tier_mixed);
	seq_printf(m, "slc_sb_gc_tier_fallback %llu\n",
		   (unsigned long long)conv_ftl->slc_sb_gc_tier_fallback);

	seq_printf(m, "test_phase_read_die_wait_ns %lld\n",
		   atomic64_read(&conv_ftl->test_phase_read_die_wait_ns));
	seq_printf(m, "migration_read_path_time_ns %llu\n",
		   (unsigned long long)conv_ftl->migration_read_path_time_ns);
}

static void superblock_stats_log_summary(struct conv_ftl *conv_ftl, const char *phase)
{
	struct superblock_health_snapshot snap;
	uint64_t gc_total;
	uint64_t gc_eff;
	uint64_t weighted_owner_ratio;

	if (!conv_ftl || !conv_ftl->ssd)
		return;

	superblock_collect_health(conv_ftl, &snap);
	gc_total = conv_ftl->slc_sb_gc_invalid_pages +
		   conv_ftl->slc_sb_gc_valid_pages;
	gc_eff = pct_u64(conv_ftl->slc_sb_gc_invalid_pages, gc_total);
	weighted_owner_ratio = snap.owner_ratio_weight ?
		div64_u64(snap.owner_ratio_weighted_sum, snap.owner_ratio_weight) : 0;

	NVMEV_INFO("[SB_STATS] %s slc_free=%u/%u qlc_free=%u/%u active=%u/%u "
		   "pure90=%u mixed_lt60=%u owner_ratio=%llu gc=%llu gc_eff=%llu "
		   "sb_mig_pages=%llu repromote_chain=%llu repromote_gc=%llu "
		   "skip_cap=%llu reroute=%llu erase_ops=%llu erase_ns=%llu\n",
		   phase ? phase : "summary",
		   snap.slc.free, snap.slc.total, snap.qlc.free, snap.qlc.total,
		   conv_ftl->active_sb_count, NVMEV_SUPERBLOCK_ACTIVE_LIMIT,
		   snap.sb_pure90, snap.sb_mixed_lt60,
		   (unsigned long long)weighted_owner_ratio,
		   (unsigned long long)conv_ftl->slc_sb_gc_count,
		   (unsigned long long)gc_eff,
		   (unsigned long long)conv_ftl->slc_sb_migration_pages,
		   (unsigned long long)conv_ftl->repromote_chain_alloc_pages,
		   (unsigned long long)conv_ftl->repromote_gc_pool_pages,
		   (unsigned long long)conv_ftl->repromote_skip_active_cap,
		   (unsigned long long)conv_ftl->chain_slc_die_reroute_count,
		   (unsigned long long)conv_ftl->slc_sb_gc_erase_ops,
		   (unsigned long long)conv_ftl->slc_sb_gc_erase_time_ns);
}

static int superblock_stats_show(struct seq_file *m, void *v)
{
	struct conv_ftl *conv_ftl = m->private;
	struct superblock_health_snapshot snap;

	(void)v;
	if (!conv_ftl)
		return 0;

	superblock_collect_health(conv_ftl, &snap);
	superblock_stats_print(m, conv_ftl, &snap);
	return 0;
}

static int superblock_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, superblock_stats_show, inode->i_private);
}

static const struct file_operations superblock_stats_fops = {
	.owner = THIS_MODULE,
	.open = superblock_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int access_inject_open(struct inode *inode, struct file *file)
{
	pr_info("access_inject_open: inode=%p private=%p\n", inode, inode->i_private);
	file->private_data = inode->i_private;
	return nonseekable_open(inode, file);
}

static ssize_t access_inject_write(struct file *file, const char __user *user_buf,
				   size_t len, loff_t *ppos)
{
	struct conv_ftl *conv_ftl = file->private_data;
	struct heat_tracking *ht;
	struct ssdparams *spp;
	char kbuf[256];
	size_t copy;
	char *cursor, *token_end;
	char *count_str = NULL;
	unsigned long long lpn, count;
	int ret;

	if (!conv_ftl || !conv_ftl->ssd) {
		NVMEV_ERROR("access_inject_write: missing conv_ftl (%p) or ssd (%p)\n",
			    conv_ftl, conv_ftl ? conv_ftl->ssd : NULL);
		return -EINVAL;
	}

	ht = &conv_ftl->heat_track;
	spp = &conv_ftl->ssd->sp;
	if (!ht->access_count) {
		NVMEV_ERROR("access_inject_write: access_count not initialized\n");
		return -EINVAL;
	}

	if (len == 0)
		return 0;

	copy = min(len, sizeof(kbuf) - 1);
	if (copy_from_user(kbuf, user_buf, copy))
		return -EFAULT;
	kbuf[copy] = '\0';

	cursor = skip_spaces(kbuf);
	if (!*cursor) {
		NVMEV_ERROR("access_inject_write: missing LPN token in '%s'\n", kbuf);
		return -EINVAL;
	}

	token_end = cursor;
	while (*token_end && !isspace(*token_end))
		token_end++;
	if (*token_end) {
		*token_end = '\0';
		count_str = token_end + 1;
	} else {
		count_str = token_end;
	}

	ret = kstrtoull(cursor, 10, &lpn);
	if (ret) {
		NVMEV_ERROR("access_inject_write: invalid LPN token '%s' (ret=%d)\n", cursor, ret);
		return ret;
	}

	count_str = skip_spaces(count_str);
	if (!count_str || !*count_str) {
		NVMEV_ERROR("access_inject_write: missing count token after LPN=%llu\n", lpn);
		return -EINVAL;
	}

	token_end = count_str;
	while (*token_end && !isspace(*token_end))
		token_end++;
	if (*token_end)
		*token_end = '\0';

	ret = kstrtoull(count_str, 10, &count);
	if (ret) {
		NVMEV_ERROR("access_inject_write: invalid count token '%s' for LPN=%llu (ret=%d)\n",
			    count_str, lpn, ret);
		return ret;
	}

	if (lpn >= spp->tt_pgs) {
		NVMEV_ERROR("access_inject_write: LPN=%llu out of range (tt_pgs=%lu)\n",
			    lpn, spp->tt_pgs);
		return -ERANGE;
	}

	ht->access_count[lpn] = count;
	pr_debug("access_inject_write: LPN=%llu heat=%llu\n", lpn, count);

	return len;
}

static const struct file_operations access_inject_fops = {
	.owner = THIS_MODULE,
	.open = access_inject_open,
	.write = access_inject_write,
	.llseek = no_llseek,
};


static inline uint8_t get_qlc_zone_for_read(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct nand_page *pg = get_pg(conv_ftl->ssd, ppa);
	uint8_t zone = 0;

	if (pg)
		zone = pg->qlc_latency_zone;
	if (zone >= QLC_ZONE_COUNT)
		zone = QLC_ZONE_COUNT - 1;
	return zone;
}

static inline uint32_t encode_die(struct ssdparams *spp, const struct ppa *ppa)
{
	return ppa->g.lun * spp->nchs + ppa->g.ch;
}

static inline void decode_die(struct ssdparams *spp, uint32_t die,
				 uint32_t *ch, uint32_t *lun)
{
	uint32_t total_ch = spp->nchs;
	*ch = die % total_ch;
	*lun = die / total_ch;
}

static inline uint32_t total_dies(struct ssdparams *spp)
{
	return spp->nchs * spp->luns_per_ch;
}

static inline uint32_t next_adjacent_die(struct ssdparams *spp, uint32_t die)
{
	uint32_t total = total_dies(spp);
	if (!total)
		return 0;
	return (die + 1) % total;
}

static inline struct line_mgmt *get_slc_die_lm(struct conv_ftl *conv_ftl, uint32_t die)
{
	if (!conv_ftl->slc_lunlm || !conv_ftl->die_count)
		return NULL;
	return &conv_ftl->slc_lunlm[die % conv_ftl->die_count];
}

static inline struct line_mgmt *get_qlc_die_lm(struct conv_ftl *conv_ftl, uint32_t die)
{
	if (!conv_ftl->qlc_lunlm || !conv_ftl->die_count)
		return NULL;
	return &conv_ftl->qlc_lunlm[die % conv_ftl->die_count];
}

static inline struct write_pointer *get_qlc_die_wp(struct conv_ftl *conv_ftl, uint32_t die,
						   bool gc)
{
	struct write_pointer **arr = gc ? &conv_ftl->gc_qlc_lunwp : &conv_ftl->qlc_lunwp;

	if (!conv_ftl->die_count || !*arr)
		return NULL;

	return &(*arr)[die % conv_ftl->die_count];
}

static inline uint64_t slc_block_index_for_die(struct conv_ftl *conv_ftl, uint32_t die,
					       uint32_t blk)
{
	struct ppa ppa = { .ppa = 0 };
	struct ssdparams *spp;
	uint32_t ch, lun;

	if (!conv_ftl || !conv_ftl->ssd)
		return U64_MAX;

	spp = &conv_ftl->ssd->sp;
	decode_die(spp, die % (conv_ftl->die_count ? conv_ftl->die_count : 1), &ch, &lun);
	ppa.g.ch = ch;
	ppa.g.lun = lun;
	ppa.g.pl = 0;
	ppa.g.blk = blk;
	ppa.g.pg = 0;
	return block_meta_index(conv_ftl, &ppa);
}

static inline void slc_active_block_ref_change(struct conv_ftl *conv_ftl, uint32_t die,
					       uint32_t blk, int delta)
{
	uint64_t idx;

	if (!conv_ftl || !conv_ftl->blk_active_wp_refs || delta == 0)
		return;

	idx = slc_block_index_for_die(conv_ftl, die, blk);
	if (idx == U64_MAX || idx >= conv_ftl->ssd->sp.tt_blks)
		return;

	if (delta > 0) {
		uint32_t value = conv_ftl->blk_active_wp_refs[idx];
		if (value < U16_MAX)
			conv_ftl->blk_active_wp_refs[idx] = value + 1;
		return;
	}

	if (conv_ftl->blk_active_wp_refs[idx] > 0)
		conv_ftl->blk_active_wp_refs[idx]--;
}

static inline bool slc_block_has_active_writer(struct conv_ftl *conv_ftl, const struct ppa *ppa)
{
	uint64_t idx;

	if (!conv_ftl || !ppa || !conv_ftl->blk_active_wp_refs)
		return false;

	idx = block_meta_index(conv_ftl, ppa);
	if (idx == U64_MAX || idx >= conv_ftl->ssd->sp.tt_blks)
		return false;
	return conv_ftl->blk_active_wp_refs[idx] != 0;
}

#define SB_OWNER_CANDIDATE_MAX 16U

static bool slc_sb_collect_summary(struct conv_ftl *conv_ftl, uint32_t blk_id,
				   struct slc_sb_summary_no1 *sum)
{
	struct ssdparams *spp;
	uint32_t die_count;
	uint32_t die;
	uint32_t owner_ids[SB_OWNER_CANDIDATE_MAX];
	uint32_t owner_pages[SB_OWNER_CANDIDATE_MAX];
	uint32_t owner_cnt = 0;
	uint32_t best_owner_pages = 0;
	uint32_t best_owner = INVALID_CHAIN_ID;

	if (!conv_ftl || !conv_ftl->ssd || !sum)
		return false;
	spp = &conv_ftl->ssd->sp;
	if (blk_id >= conv_ftl->slc_blks_per_pl)
		return false;

	memset(sum, 0, sizeof(*sum));
	sum->blk_id = blk_id;
	sum->owner_chain = INVALID_CHAIN_ID;
	die_count = conv_ftl->die_count ? conv_ftl->die_count : 1;

	if (conv_ftl->slc_sb_state && blk_id < spp->blks_per_pl &&
	    conv_ftl->slc_sb_state[blk_id] == NVMEV_SB_ACTIVE)
		sum->active = true;

	for (die = 0; die < die_count; die++) {
		struct line_mgmt *lm = get_slc_die_lm(conv_ftl, die);
		struct write_pointer *host_wp = conv_ftl->slc_lunwp ?
			&conv_ftl->slc_lunwp[die] : NULL;
		struct write_pointer *gc_wp = conv_ftl->gc_slc_lunwp ?
			&conv_ftl->gc_slc_lunwp[die] : NULL;
		struct ppa probe = { .ppa = 0 };
		struct nand_block *blk;
		struct line *line;
		uint32_t ch, lun;
		uint64_t meta_idx;
		uint32_t pg;

		if (!lm || !lm->lines || blk_id >= lm->tt_lines)
			continue;
		line = &lm->lines[blk_id];

		if ((host_wp && host_wp->curline == line) ||
		    (gc_wp && gc_wp->curline == line))
			sum->open_writer = true;

		decode_die(spp, die, &ch, &lun);
		probe.g.ch = ch;
		probe.g.lun = lun;
		probe.g.pl = 0;
		probe.g.blk = blk_id;
		probe.g.pg = 0;
		if (slc_block_has_active_writer(conv_ftl, &probe))
			sum->open_writer = true;

		if (line->vpc || line->ipc) {
			sum->eligible_mask |= (uint16_t)(1u << die);
			sum->total_vpc += line->vpc;
			sum->total_ipc += line->ipc;
			if (line->vpc == spp->pgs_per_lun_line)
				sum->full_mask |= (uint16_t)(1u << die);
		}

		meta_idx = slc_block_index_for_die(conv_ftl, die, blk_id);
		if (meta_idx != U64_MAX && meta_idx < spp->tt_blks &&
		    conv_ftl->blk_owner_chain && conv_ftl->blk_owner_pages &&
		    conv_ftl->blk_valid_pages) {
			uint32_t owner = conv_ftl->blk_owner_chain[meta_idx];
			uint32_t pages = conv_ftl->blk_owner_pages[meta_idx];
			uint32_t c;

			sum->valid_pages += conv_ftl->blk_valid_pages[meta_idx];
			if (chain_id_valid(conv_ftl, owner) && pages) {
				for (c = 0; c < owner_cnt; c++) {
					if (owner_ids[c] == owner) {
						owner_pages[c] += pages;
						break;
					}
				}
				if (c == owner_cnt && owner_cnt < SB_OWNER_CANDIDATE_MAX) {
					owner_ids[owner_cnt] = owner;
					owner_pages[owner_cnt] = pages;
					owner_cnt++;
				}
			}
		}

		blk = get_blk(conv_ftl->ssd, &probe);
		if (!blk)
			continue;
		for (pg = 0; pg < (uint32_t)blk->npgs; pg++) {
			uint64_t lpn;

			if (blk->pg[pg].status != PG_VALID)
				continue;
			probe.g.pg = pg;
			lpn = get_rmap_ent(conv_ftl, &probe);
			if (lpn == INVALID_LPN || lpn >= spp->tt_pgs)
				continue;
			if (conv_ftl->heat_track.access_count)
				sum->heat_sum += conv_ftl->heat_track.access_count[lpn];
			sum->heat_count++;
		}
	}

	for (die = 0; die < owner_cnt; die++) {
		if (owner_pages[die] > best_owner_pages) {
			best_owner_pages = owner_pages[die];
			best_owner = owner_ids[die];
		}
	}
	sum->owner_chain = best_owner;
	sum->owner_pages = best_owner_pages;
	if (sum->heat_count)
		sum->avg_heat = div64_u64(sum->heat_sum, sum->heat_count);

	return sum->eligible_mask != 0;
}

static uint8_t slc_sb_summary_tier(const struct slc_sb_summary_no1 *sum,
				   uint64_t global_avg)
{
	uint32_t owner_ratio = 0;

	if (!sum)
		return 3;
	if (sum->total_vpc == 0)
		return 0;
	if (sum->total_vpc)
		owner_ratio = sum->owner_pages * 100U / sum->total_vpc;
	if (owner_ratio >= 90U && sum->avg_heat <= global_avg)
		return 1;
	if (owner_ratio < 60U)
		return 2;
	return 3;
}

static bool __maybe_unused slc_sb_summary_better(const struct slc_sb_summary_no1 *cand,
						 uint8_t cand_tier,
						 const struct slc_sb_summary_no1 *best,
						 uint8_t best_tier)
{
	if (!best)
		return true;
	if (cand->total_ipc != best->total_ipc)
		return cand->total_ipc > best->total_ipc;
	if (cand->total_vpc != best->total_vpc)
		return cand->total_vpc < best->total_vpc;
	if (cand_tier != best_tier)
		return cand_tier < best_tier;
	if (cand->avg_heat != best->avg_heat)
		return cand->avg_heat < best->avg_heat;
	return hweight16(cand->eligible_mask) > hweight16(best->eligible_mask);
}

static uint32_t migrate_chain_superblock_from_slc(struct conv_ftl *conv_ftl,
						  uint32_t blk_id,
						  uint32_t budget,
						  uint64_t dyn_thresh)
{
	struct ssdparams *spp;
	uint32_t die_count;
	uint32_t die;
	uint32_t eligible = 0;
	uint32_t moved = 0;

	if (!conv_ftl || !conv_ftl->ssd || !budget)
		return 0;
	spp = &conv_ftl->ssd->sp;
	die_count = conv_ftl->die_count ? conv_ftl->die_count : 1;
	conv_ftl->slc_sb_migration_attempts++;

	for (die = 0; die < die_count; die++) {
		struct ppa ppa = { .ppa = 0 };
		struct nand_block *blk;
		uint32_t ch, lun, pg;

		decode_die(spp, die, &ch, &lun);
		ppa.g.ch = ch;
		ppa.g.lun = lun;
		ppa.g.pl = 0;
		ppa.g.blk = blk_id;
		blk = get_blk(conv_ftl->ssd, &ppa);
		if (!blk)
			continue;

		for (pg = 0; pg < (uint32_t)blk->npgs; pg++) {
			uint64_t lpn;

			ppa.g.pg = pg;
			if (blk->pg[pg].status != PG_VALID)
				continue;
			lpn = get_rmap_ent(conv_ftl, &ppa);
			if (lpn == INVALID_LPN || lpn >= spp->tt_pgs)
				continue;
			if (!conv_ftl->page_in_slc || !conv_ftl->page_in_slc[lpn])
				continue;
			if (!slc_chain_page_is_cold(conv_ftl, lpn, dyn_thresh))
				return 0;
			eligible++;
		}
	}

	if (!eligible || eligible > budget)
		return 0;

	for (die = 0; die < die_count && moved < budget; die++) {
		struct ppa ppa = { .ppa = 0 };
		struct nand_block *blk;
		uint32_t ch, lun, pg;

		decode_die(spp, die, &ch, &lun);
		ppa.g.ch = ch;
		ppa.g.lun = lun;
		ppa.g.pl = 0;
		ppa.g.blk = blk_id;
		blk = get_blk(conv_ftl->ssd, &ppa);
		if (!blk)
			continue;

		for (pg = 0; pg < (uint32_t)blk->npgs && moved < budget; pg++) {
			uint64_t lpn;

			ppa.g.pg = pg;
			if (blk->pg[pg].status != PG_VALID)
				continue;
			lpn = get_rmap_ent(conv_ftl, &ppa);
			if (lpn == INVALID_LPN || lpn >= spp->tt_pgs)
				continue;
			if (!conv_ftl->page_in_slc || !conv_ftl->page_in_slc[lpn])
				continue;
			if (migrate_page_to_qlc(conv_ftl, lpn, &ppa) == 0)
				moved++;
		}
	}

	if (moved)
		conv_ftl->slc_sb_migration_pages += moved;
	return moved;
}

static uint32_t migrate_superblock_valid_pages_from_slc(struct conv_ftl *conv_ftl,
							uint32_t blk_id,
							uint32_t budget)
{
	struct ssdparams *spp;
	uint32_t die_count;
	uint32_t die;
	uint32_t moved = 0;

	if (!conv_ftl || !conv_ftl->ssd || !budget)
		return 0;
	spp = &conv_ftl->ssd->sp;
	die_count = conv_ftl->die_count ? conv_ftl->die_count : 1;
	conv_ftl->slc_sb_migration_attempts++;

	for (die = 0; die < die_count && moved < budget; die++) {
		struct ppa ppa = { .ppa = 0 };
		struct nand_block *blk;
		uint32_t ch, lun, pg;

		decode_die(spp, die, &ch, &lun);
		ppa.g.ch = ch;
		ppa.g.lun = lun;
		ppa.g.pl = 0;
		ppa.g.blk = blk_id;
		blk = get_blk(conv_ftl->ssd, &ppa);
		if (!blk)
			continue;

		for (pg = 0; pg < (uint32_t)blk->npgs && moved < budget; pg++) {
			uint64_t lpn;

			ppa.g.pg = pg;
			if (blk->pg[pg].status != PG_VALID)
				continue;
			lpn = get_rmap_ent(conv_ftl, &ppa);
			if (lpn == INVALID_LPN || lpn >= spp->tt_pgs)
				continue;
			if (!conv_ftl->page_in_slc || !conv_ftl->page_in_slc[lpn])
				continue;
			if (migrate_page_to_qlc(conv_ftl, lpn, &ppa) == 0)
				moved++;
		}
	}

	if (moved)
		conv_ftl->slc_sb_migration_pages += moved;
	return moved;
}

static bool slc_sb_summary_is_mixed(struct conv_ftl *conv_ftl,
				    const struct slc_sb_summary_no1 *sum)
{
	uint32_t owner_ratio = 0;

	if (!sum || !sum->total_vpc)
		return false;
	if (!chain_id_valid(conv_ftl, sum->owner_chain))
		return true;
	owner_ratio = sum->owner_pages * 100U / sum->total_vpc;
	return owner_ratio < 90U;
}

static bool slc_mixed_sb_summary_better(const struct slc_sb_summary_no1 *cand,
					const struct slc_sb_summary_no1 *best,
					bool have_best)
{
	if (!cand)
		return false;
	if (!have_best || !best)
		return true;
	if (cand->avg_heat != best->avg_heat)
		return cand->avg_heat < best->avg_heat;
	if (cand->heat_sum != best->heat_sum)
		return cand->heat_sum < best->heat_sum;
	return cand->blk_id < best->blk_id;
}

static void slc_migrated_victim_remove_locked(struct conv_ftl *conv_ftl, uint32_t blk_id)
{
	if (!conv_ftl || !conv_ftl->slc_sb_migrated_victim ||
	    blk_id >= conv_ftl->slc_blks_per_pl)
		return;
	if (!conv_ftl->slc_sb_migrated_victim[blk_id])
		return;
	conv_ftl->slc_sb_migrated_victim[blk_id] = 0;
	if (conv_ftl->slc_sb_migrated_victim_count)
		conv_ftl->slc_sb_migrated_victim_count--;
}

static bool slc_migrated_victim_enqueue_locked(struct conv_ftl *conv_ftl,
					       uint32_t blk_id)
{
	if (!conv_ftl || !conv_ftl->slc_sb_migrated_victim ||
	    blk_id >= conv_ftl->slc_blks_per_pl)
		return false;
	if (conv_ftl->slc_sb_migrated_victim[blk_id])
		return true;
	if (conv_ftl->slc_sb_recent_guard)
		conv_ftl->slc_sb_recent_guard[blk_id] = 0;
	conv_ftl->slc_sb_migrated_victim[blk_id] = 1;
	conv_ftl->slc_sb_migrated_victim_count++;
	conv_ftl->slc_sb_migration_victim_enqueues++;
	return true;
}

static bool slc_migrated_victim_enqueue(struct conv_ftl *conv_ftl,
					uint32_t blk_id)
{
	bool queued;

	if (!conv_ftl)
		return false;
	spin_lock(&conv_ftl->slc_lock);
	queued = slc_migrated_victim_enqueue_locked(conv_ftl, blk_id);
	spin_unlock(&conv_ftl->slc_lock);
	return queued;
}

static bool slc_sb_recent_guarded(struct conv_ftl *conv_ftl, uint32_t blk_id)
{
	if (!conv_ftl || !conv_ftl->slc_sb_recent_guard ||
	    blk_id >= conv_ftl->slc_blks_per_pl)
		return false;
	return !!conv_ftl->slc_sb_recent_guard[blk_id];
}

static void slc_sb_recent_guard_clear_locked(struct conv_ftl *conv_ftl,
					     uint32_t blk_id)
{
	if (!conv_ftl || blk_id >= conv_ftl->slc_blks_per_pl)
		return;
	if (conv_ftl->slc_sb_recent_guard)
		conv_ftl->slc_sb_recent_guard[blk_id] = 0;
	if (conv_ftl->slc_sb_generation) {
		uint32_t gen = conv_ftl->slc_sb_generation[blk_id] + 1;

		if (!gen)
			gen = 1;
		conv_ftl->slc_sb_generation[blk_id] = gen;
	}
}

static void slc_sb_recent_guard_note_closed_locked(struct conv_ftl *conv_ftl,
						   uint32_t blk_id)
{
	uint32_t slot;
	uint32_t old_blk;
	uint32_t old_gen;
	uint32_t gen = 0;

	if (!conv_ftl || !conv_ftl->slc_sb_recent_guard ||
	    !conv_ftl->slc_recent_guard_ring_blk ||
	    !conv_ftl->slc_recent_guard_ring_gen ||
	    !conv_ftl->slc_recent_guard_ring_size ||
	    blk_id >= conv_ftl->slc_blks_per_pl)
		return;
	if (conv_ftl->slc_sb_recent_guard[blk_id])
		return;

	slot = conv_ftl->slc_recent_guard_ring_head %
		conv_ftl->slc_recent_guard_ring_size;
	old_blk = conv_ftl->slc_recent_guard_ring_blk[slot];
	old_gen = conv_ftl->slc_recent_guard_ring_gen[slot];
	if (old_blk < conv_ftl->slc_blks_per_pl &&
	    conv_ftl->slc_sb_generation &&
	    old_gen == conv_ftl->slc_sb_generation[old_blk])
		conv_ftl->slc_sb_recent_guard[old_blk] = 0;

	if (conv_ftl->slc_sb_generation)
		gen = conv_ftl->slc_sb_generation[blk_id];
	conv_ftl->slc_sb_recent_guard[blk_id] = 1;
	conv_ftl->slc_recent_guard_ring_blk[slot] = blk_id;
	conv_ftl->slc_recent_guard_ring_gen[slot] = gen;
	conv_ftl->slc_recent_guard_ring_head =
		(slot + 1) % conv_ftl->slc_recent_guard_ring_size;
}

static bool slc_migration_sb_better(const struct slc_sb_summary_no1 *cand,
				    const struct slc_sb_summary_no1 *best,
				    bool have_best)
{
	if (!cand)
		return false;
	if (!have_best || !best)
		return true;
	if (cand->avg_heat != best->avg_heat)
		return cand->avg_heat < best->avg_heat;
	if (cand->heat_sum != best->heat_sum)
		return cand->heat_sum < best->heat_sum;
	return cand->blk_id < best->blk_id;
}

static uint32_t __maybe_unused migrate_pure_cold_superblocks_from_slc(struct conv_ftl *conv_ftl,
						       uint32_t max_pages,
						       uint64_t dyn_thresh,
						       uint32_t *sampled_out,
						       uint32_t *blocks_out)
{
	uint64_t global_avg = 0;
	uint32_t migrated = 0;
	uint32_t blk_id;

	if (sampled_out)
		*sampled_out = 0;
	if (blocks_out)
		*blocks_out = 0;
	if (!conv_ftl || !conv_ftl->ssd || !max_pages)
		return 0;

	(void)calc_global_avg_reads(conv_ftl, &global_avg);
	for (blk_id = 0; blk_id < conv_ftl->slc_blks_per_pl && migrated < max_pages; blk_id++) {
		struct slc_sb_summary_no1 sum;
		uint32_t moved;

		if (blocks_out)
			(*blocks_out)++;
		if (!slc_sb_collect_summary(conv_ftl, blk_id, &sum))
			continue;
		if (sampled_out)
			*sampled_out += sum.heat_count;
		if (sum.active || sum.open_writer || sum.total_vpc == 0)
			continue;
		if (slc_sb_summary_tier(&sum, global_avg) != 1)
			continue;
		if (sum.avg_heat > dyn_thresh)
			continue;
		if (!chain_id_valid(conv_ftl, sum.owner_chain))
			continue;

		moved = migrate_chain_superblock_from_slc(conv_ftl, blk_id,
							  max_pages - migrated,
							  dyn_thresh);
		migrated += moved;
	}

	return migrated;
}

static uint32_t __maybe_unused migrate_coldest_mixed_superblock_from_slc(struct conv_ftl *conv_ftl,
							  uint32_t max_pages,
							  uint32_t *sampled_out,
							  uint32_t *blocks_out)
{
	struct slc_sb_summary_no1 best;
	bool have_best = false;
	uint32_t blk_id;

	if (sampled_out)
		*sampled_out = 0;
	if (blocks_out)
		*blocks_out = 0;
	if (!conv_ftl || !conv_ftl->ssd || !max_pages)
		return 0;

	memset(&best, 0, sizeof(best));
	for (blk_id = 0; blk_id < conv_ftl->slc_blks_per_pl; blk_id++) {
		struct slc_sb_summary_no1 sum;

		if (blocks_out)
			(*blocks_out)++;
		if (!slc_sb_collect_summary(conv_ftl, blk_id, &sum))
			continue;
		if (sampled_out)
			*sampled_out += sum.heat_count;
		if (sum.active || sum.open_writer || sum.total_vpc == 0)
			continue;
		if (!slc_sb_summary_is_mixed(conv_ftl, &sum))
			continue;
		if (slc_mixed_sb_summary_better(&sum, &best, have_best)) {
			best = sum;
			have_best = true;
		}
	}

	if (!have_best)
		return 0;
	return migrate_superblock_valid_pages_from_slc(conv_ftl, best.blk_id, max_pages);
}

static uint32_t migrate_coldest_superblock_to_victim_queue_from_slc(struct conv_ftl *conv_ftl,
								    uint32_t max_pages,
								    uint32_t *sampled_out,
								    uint32_t *blocks_out)
{
	struct slc_sb_summary_no1 best;
	bool have_best = false;
	bool force_recent = false;
	uint32_t blk_id;
	uint32_t moved = 0;
	uint32_t guard_skipped = 0;
	uint32_t pass;

	if (sampled_out)
		*sampled_out = 0;
	if (blocks_out)
		*blocks_out = 0;
	if (!conv_ftl || !conv_ftl->ssd || !max_pages)
		return 0;

	memset(&best, 0, sizeof(best));
	for (pass = 0; pass < 2 && !have_best; pass++) {
		bool allow_recent = pass != 0;

		if (allow_recent) {
			struct line_pool_stats slc_stats;

			if (!guard_skipped)
				break;
			collect_slc_stats(conv_ftl, &slc_stats);
			if (slc_stats.free > conv_ftl->slc_gc_free_thres_low)
				break;
			force_recent = true;
			conv_ftl->slc_sb_recent_guard_forced++;
		}

		for (blk_id = 0; blk_id < conv_ftl->slc_blks_per_pl; blk_id++) {
			struct slc_sb_summary_no1 sum;

			if (blocks_out)
				(*blocks_out)++;
			if (conv_ftl->slc_sb_migrated_victim &&
			    conv_ftl->slc_sb_migrated_victim[blk_id])
				continue;
			if (!allow_recent && slc_sb_recent_guarded(conv_ftl, blk_id)) {
				guard_skipped++;
				continue;
			}
			if (!slc_sb_collect_summary(conv_ftl, blk_id, &sum))
				continue;
			if (sampled_out)
				*sampled_out += sum.heat_count;
			if (sum.active || sum.open_writer || sum.total_vpc == 0)
				continue;
			if (slc_migration_sb_better(&sum, &best, have_best)) {
				best = sum;
				have_best = true;
			}
		}
	}
	if (guard_skipped)
		conv_ftl->slc_sb_recent_guard_skips += guard_skipped;

	if (!have_best)
		return 0;
	if (force_recent)
		NVMEV_DEBUG("SLC cold migration: forcing recently closed SB blk=%u under low-free pressure\n",
			    best.blk_id);
	if (best.total_vpc)
		moved = migrate_superblock_valid_pages_from_slc(conv_ftl, best.blk_id, max_pages);
	if (!slc_migrated_victim_enqueue(conv_ftl, best.blk_id))
		return moved;
	return moved;
}

static struct write_pointer *get_chain_host_slc_wp(struct conv_ftl *conv_ftl, uint32_t chain_id,
						   uint32_t die, bool alloc)
{
	struct write_pointer *wps;

	if (!chain_id_valid(conv_ftl, chain_id) || !conv_ftl->chain_host_slc_wps ||
	    !conv_ftl->die_count)
		return NULL;

	wps = READ_ONCE(conv_ftl->chain_host_slc_wps[chain_id]);
	if (!wps && alloc) {
		struct write_pointer *new_wps;

		new_wps = kcalloc(conv_ftl->die_count, sizeof(*new_wps), GFP_KERNEL);
		if (!new_wps)
			return NULL;

		spin_lock(&conv_ftl->slc_lock);
		if (!conv_ftl->chain_host_slc_wps[chain_id])
			conv_ftl->chain_host_slc_wps[chain_id] = new_wps;
		spin_unlock(&conv_ftl->slc_lock);

		wps = READ_ONCE(conv_ftl->chain_host_slc_wps[chain_id]);
		if (wps != new_wps)
			kfree(new_wps);
	}

	if (!wps)
		return NULL;
	return &wps[die % conv_ftl->die_count];
}

static struct write_pointer *get_chain_host_qlc_wp(struct conv_ftl *conv_ftl,
						   uint32_t chain_id,
						   uint32_t die, bool alloc)
{
	struct write_pointer *wps;

	if (!chain_id_valid(conv_ftl, chain_id) || !conv_ftl->chain_host_qlc_wps ||
	    !conv_ftl->die_count)
		return NULL;

	wps = READ_ONCE(conv_ftl->chain_host_qlc_wps[chain_id]);
	if (!wps && alloc) {
		struct write_pointer *new_wps;

		new_wps = kcalloc(conv_ftl->die_count, sizeof(*new_wps), GFP_KERNEL);
		if (!new_wps)
			return NULL;

		spin_lock(&conv_ftl->qlc_lock);
		if (!conv_ftl->chain_host_qlc_wps[chain_id])
			conv_ftl->chain_host_qlc_wps[chain_id] = new_wps;
		spin_unlock(&conv_ftl->qlc_lock);

		wps = READ_ONCE(conv_ftl->chain_host_qlc_wps[chain_id]);
		if (wps != new_wps)
			kfree(new_wps);
	}

	if (!wps)
		return NULL;
	return &wps[die % conv_ftl->die_count];
}

/* forward declarations for the SB state-machine helpers defined further down */
static int slc_find_free_sb_blk_locked(struct conv_ftl *conv_ftl);
static bool slc_open_chain_active_sb_locked(struct conv_ftl *conv_ftl,
					    uint32_t chain_id, uint32_t blk);
static uint32_t slc_pick_share_target_locked(struct conv_ftl *conv_ftl,
					     uint32_t self_chain_id);
static void slc_sb_note_die_full_locked(struct conv_ftl *conv_ftl,
					uint32_t blk, uint32_t die);
static void slc_sb_note_freed_locked(struct conv_ftl *conv_ftl, uint32_t blk);

static inline bool slc_wp_open_line_locked(struct conv_ftl *conv_ftl, struct line_mgmt *lm,
					   struct write_pointer *wp, uint32_t die)
{
	struct line *curline;
	struct ssdparams *spp;

	if (!conv_ftl || !lm || !wp || !conv_ftl->ssd)
		return false;

	curline = list_first_entry_or_null(&lm->free_line_list, struct line, entry);
	if (!curline)
		return false;

	spp = &conv_ftl->ssd->sp;
	list_del_init(&curline->entry);
	lm->free_line_cnt--;
	decode_die(spp, die, &wp->ch, &wp->lun);
	wp->curline = curline;
	wp->blk = curline->id;
	wp->pg = 0;
	wp->pl = 0;
	slc_active_block_ref_change(conv_ftl, die, wp->blk, +1);
	return true;
}

static inline void slc_wp_close_line_locked(struct conv_ftl *conv_ftl, struct line_mgmt *lm,
					    struct write_pointer *wp, uint32_t die)
{
	struct ssdparams *spp;
	uint32_t closing_blk;

	if (!conv_ftl || !lm || !wp || !wp->curline || !conv_ftl->ssd)
		return;

	spp = &conv_ftl->ssd->sp;
	closing_blk = wp->blk;
	slc_active_block_ref_change(conv_ftl, die, wp->blk, -1);
	if (wp->curline->vpc == 0 && wp->curline->ipc == 0) {
		list_add_tail(&wp->curline->entry, &lm->free_line_list);
		lm->free_line_cnt++;
		/* edge case: line returned directly to free pool (no valid pages
		 * ever stuck). Clear this die's bit; if SB now has no filled dies
		 * and was CLOSED, transition to FREE. */
		if (conv_ftl->slc_sb_die_full_mask &&
		    closing_blk < spp->blks_per_pl) {
			conv_ftl->slc_sb_die_full_mask[closing_blk] &=
				~(uint16_t)(1u << die);
			if (conv_ftl->slc_sb_state &&
			    conv_ftl->slc_sb_state[closing_blk] == NVMEV_SB_CLOSED &&
			    conv_ftl->slc_sb_die_full_mask[closing_blk] == 0)
				slc_sb_note_freed_locked(conv_ftl, closing_blk);
		}
	} else if (wp->curline->vpc == spp->pgs_per_lun_line) {
		list_add_tail(&wp->curline->entry, &lm->full_line_list);
		lm->full_line_cnt++;
	} else {
		wp->curline->pos = 0;
	}

	wp->curline = NULL;
	wp->blk = 0;
	wp->pg = 0;
	wp->pl = 0;
}

static bool chain_has_open_slc_superblock_locked(struct conv_ftl *conv_ftl,
						 uint32_t chain_id)
{
	struct write_pointer *wps;
	uint32_t die;

	if (!chain_id_valid(conv_ftl, chain_id) || !conv_ftl->chain_host_slc_wps)
		return false;

	wps = conv_ftl->chain_host_slc_wps[chain_id];
	if (!wps)
		return false;

	for (die = 0; die < conv_ftl->die_count; die++) {
		if (wps[die].curline)
			return true;
	}
	return false;
}

/* Find a blk_id that is free (in free_line_list) on every die.
 * Returns -1 if no such SB-aligned free blk exists. Caller holds slc_lock.
 *
 * Scan strategy: walk die0's free list; for each candidate blk, verify it
 * appears in every other die's free list. Cost is bounded by free_lines * dies.
 */
static int slc_find_free_sb_blk_locked(struct conv_ftl *conv_ftl)
{
	struct line_mgmt *lm0;
	struct line *line0;
	uint32_t die_count = conv_ftl ? conv_ftl->die_count : 0;
	uint32_t die;

	if (!conv_ftl || !die_count || !conv_ftl->slc_lunlm)
		return -1;

	lm0 = get_slc_die_lm(conv_ftl, 0);
	if (!lm0)
		return -1;

	list_for_each_entry(line0, &lm0->free_line_list, entry) {
		uint32_t blk = line0->id;
		bool all_free = true;

		/* skip QLC blks (in mixed mode the same blk space holds both) */
		if (conv_ftl->is_slc_block && !conv_ftl->is_slc_block[blk])
			continue;
		/* Skip ACTIVE — its line wouldn't be in free pool anyway, but be
		 * defensive. CLOSED with all dies erased is fine: presence in every
		 * die's free pool is the source of truth, and the open path will
		 * lazily reset state to ACTIVE. */
		if (conv_ftl->slc_sb_state &&
		    blk < conv_ftl->ssd->sp.blks_per_pl &&
		    conv_ftl->slc_sb_state[blk] == NVMEV_SB_ACTIVE)
			continue;

		for (die = 1; die < die_count && all_free; die++) {
			struct line_mgmt *lm = get_slc_die_lm(conv_ftl, die);
			struct line *l;
			bool found = false;

			if (!lm) { all_free = false; break; }
			list_for_each_entry(l, &lm->free_line_list, entry) {
				if (l->id == blk) { found = true; break; }
			}
			if (!found) all_free = false;
		}

		if (all_free)
			return (int)blk;
	}
	return -1;
}

static bool slc_open_aligned_wps_locked(struct conv_ftl *conv_ftl,
					struct write_pointer *wps,
					bool count_active)
{
	struct ssdparams *spp;
	uint32_t die_count;
	uint32_t die;
	int free_blk;
	uint32_t blk;

	if (!conv_ftl || !conv_ftl->ssd || !wps || !conv_ftl->slc_lunlm)
		return false;
	spp = &conv_ftl->ssd->sp;
	die_count = conv_ftl->die_count ? conv_ftl->die_count : 1;

	for (die = 0; die < die_count; die++) {
		if (wps[die].curline)
			return false;
	}
	if (count_active &&
	    conv_ftl->active_sb_count >= NVMEV_SUPERBLOCK_ACTIVE_LIMIT)
		return false;

	free_blk = slc_find_free_sb_blk_locked(conv_ftl);
	if (free_blk < 0)
		return false;
	blk = (uint32_t)free_blk;

	for (die = 0; die < die_count; die++) {
		struct line_mgmt *lm = get_slc_die_lm(conv_ftl, die);
		struct line *line;
		bool found = false;

		if (!lm)
			goto rollback;
		list_for_each_entry(line, &lm->free_line_list, entry) {
			if (line->id == blk) {
				found = true;
				break;
			}
		}
		if (!found)
			goto rollback;

		list_del_init(&line->entry);
		lm->free_line_cnt--;
		decode_die(spp, die, &wps[die].ch, &wps[die].lun);
		wps[die].curline = line;
		wps[die].blk = blk;
		wps[die].pg = 0;
		wps[die].pl = 0;
	}

	if (conv_ftl->slc_sb_state && blk < spp->blks_per_pl) {
		slc_migrated_victim_remove_locked(conv_ftl, blk);
		slc_sb_recent_guard_clear_locked(conv_ftl, blk);
		conv_ftl->slc_sb_state[blk] = NVMEV_SB_ACTIVE;
		if (conv_ftl->slc_sb_owner_chain)
			conv_ftl->slc_sb_owner_chain[blk] = INVALID_CHAIN_ID;
		if (conv_ftl->slc_sb_die_full_mask)
			conv_ftl->slc_sb_die_full_mask[blk] = 0;
		if (conv_ftl->slc_sb_active_counted) {
			conv_ftl->slc_sb_active_counted[blk] = count_active ? 1 : 0;
			if (count_active)
				conv_ftl->active_sb_count++;
		} else if (count_active) {
			conv_ftl->active_sb_count++;
		}
	}
	return true;

rollback:
	while (die--) {
		struct line_mgmt *lm = get_slc_die_lm(conv_ftl, die);

		if (!lm || !wps[die].curline)
			continue;
		list_add_tail(&wps[die].curline->entry, &lm->free_line_list);
		lm->free_line_cnt++;
		wps[die].curline = NULL;
		wps[die].blk = 0;
		wps[die].pg = 0;
		wps[die].pl = 0;
	}
	return false;
}

/* Reserve line[blk] from each die's free_line_list and install it as the
 * curline of chain_id's per-die wp. Caller holds slc_lock and has already
 * verified the blk is free on every die.
 */
static bool slc_open_chain_active_sb_locked(struct conv_ftl *conv_ftl,
					    uint32_t chain_id, uint32_t blk)
{
	struct write_pointer *wps;
	struct ssdparams *spp;
	uint32_t die;
	uint32_t die_count;

	if (!conv_ftl || !chain_id_valid(conv_ftl, chain_id))
		return false;
	if (!conv_ftl->chain_host_slc_wps || !conv_ftl->ssd)
		return false;

	wps = conv_ftl->chain_host_slc_wps[chain_id];
	if (!wps)
		return false;
	spp = &conv_ftl->ssd->sp;
	die_count = conv_ftl->die_count;
	if (!die_count)
		return false;

	for (die = 0; die < die_count; die++) {
		struct line_mgmt *lm = get_slc_die_lm(conv_ftl, die);
		struct line *line;
		bool found = false;

		if (!lm)
			goto rollback;
		list_for_each_entry(line, &lm->free_line_list, entry) {
			if (line->id == blk) { found = true; break; }
		}
		if (!found)
			goto rollback;

		list_del_init(&line->entry);
		lm->free_line_cnt--;
		decode_die(spp, die, &wps[die].ch, &wps[die].lun);
		wps[die].curline = line;
		wps[die].blk = blk;
		wps[die].pg = 0;
		wps[die].pl = 0;
		slc_active_block_ref_change(conv_ftl, die, blk, +1);
	}

	if (conv_ftl->slc_sb_state && blk < spp->blks_per_pl) {
		bool was_active = conv_ftl->slc_sb_state[blk] == NVMEV_SB_ACTIVE;

		/* If state was stale CLOSED (all dies got erased without explicit
		 * transition), reset cleanly before going ACTIVE. */
		if (!was_active) {
			slc_migrated_victim_remove_locked(conv_ftl, blk);
			slc_sb_recent_guard_clear_locked(conv_ftl, blk);
			conv_ftl->slc_sb_state[blk] = NVMEV_SB_ACTIVE;
			if (conv_ftl->slc_sb_owner_chain)
				conv_ftl->slc_sb_owner_chain[blk] = chain_id;
			if (conv_ftl->slc_sb_die_full_mask)
				conv_ftl->slc_sb_die_full_mask[blk] = 0;
		}
		if (conv_ftl->slc_sb_active_counted &&
		    !conv_ftl->slc_sb_active_counted[blk]) {
			conv_ftl->slc_sb_active_counted[blk] = 1;
			conv_ftl->active_sb_count++;
		} else if (!conv_ftl->slc_sb_active_counted && !was_active) {
			conv_ftl->active_sb_count++;
		}
	}
	if (conv_ftl->chain_cur_active_sb)
		conv_ftl->chain_cur_active_sb[chain_id] = blk;
	return true;

rollback:
	{
		uint32_t r;
		for (r = 0; r < die; r++) {
			struct line_mgmt *lm = get_slc_die_lm(conv_ftl, r);
			if (!lm || !wps[r].curline)
				continue;
			list_add_tail(&wps[r].curline->entry, &lm->free_line_list);
			lm->free_line_cnt++;
			slc_active_block_ref_change(conv_ftl, r, wps[r].blk, -1);
			wps[r].curline = NULL;
			wps[r].blk = 0;
			wps[r].pg = 0;
			wps[r].pl = 0;
		}
	}
	return false;
}

/* Pick the most-full ACTIVE SB to share, excluding any SB owned by self_chain.
 * Returns blk_id or U32_MAX. Caller holds slc_lock.
 *
 * "Most full" = sum of per-die wp->pg across all dies. The intuition: a nearly
 * full SB will close soonest, minimizing how long self_chain's pages stay
 * trapped in a mixed SB.
 */
static uint32_t slc_pick_share_target_locked(struct conv_ftl *conv_ftl,
					     uint32_t self_chain_id)
{
	struct ssdparams *spp;
	uint32_t blk;
	uint32_t best_blk = U32_MAX;
	uint64_t best_fill = 0;

	if (!conv_ftl || !conv_ftl->ssd || !conv_ftl->slc_sb_state ||
	    !conv_ftl->slc_sb_owner_chain || !conv_ftl->chain_host_slc_wps)
		return U32_MAX;

	spp = &conv_ftl->ssd->sp;
	for (blk = 0; blk < spp->blks_per_pl; blk++) {
		uint32_t owner;
		uint64_t fill = 0;
		uint32_t die;
		struct write_pointer *wps;

		if (conv_ftl->slc_sb_state[blk] != NVMEV_SB_ACTIVE)
			continue;
		owner = conv_ftl->slc_sb_owner_chain[blk];
		if (owner == self_chain_id)
			continue;
		if (!chain_id_valid(conv_ftl, owner))
			continue;
		wps = conv_ftl->chain_host_slc_wps[owner];
		if (!wps)
			continue;
		for (die = 0; die < conv_ftl->die_count; die++) {
			if (wps[die].curline && wps[die].blk == blk)
				fill += wps[die].pg;
			else
				fill += spp->pgs_per_blk;
		}
		if (fill > best_fill) {
			best_fill = fill;
			best_blk = blk;
		}
	}
	return best_blk;
}

/* Mark SB blk's portion on `die` as full. When all dies are full, transition
 * NVMEV_SB_ACTIVE -> NVMEV_SB_CLOSED, drop any counted active-cap reference,
 * and clear any chain's chain_cur_active_sb that points to this SB. Caller
 * holds slc_lock.
 */
static void slc_sb_note_die_full_locked(struct conv_ftl *conv_ftl,
					uint32_t blk, uint32_t die)
{
	struct ssdparams *spp;
	uint32_t die_count;
	uint16_t mask;
	uint16_t full_mask;

	if (!conv_ftl || !conv_ftl->ssd || !conv_ftl->slc_sb_state ||
	    !conv_ftl->slc_sb_die_full_mask)
		return;
	spp = &conv_ftl->ssd->sp;
	if (blk >= spp->blks_per_pl)
		return;
	die_count = conv_ftl->die_count;
	if (!die_count || die >= die_count)
		return;

	mask = conv_ftl->slc_sb_die_full_mask[blk];
	mask |= (uint16_t)(1u << die);
	conv_ftl->slc_sb_die_full_mask[blk] = mask;

	full_mask = (die_count >= 16) ? 0xFFFFu : (uint16_t)((1u << die_count) - 1u);
	if ((mask & full_mask) != full_mask)
		return;

	/* SB fully written across all dies: ACTIVE -> CLOSED */
		if (conv_ftl->slc_sb_state[blk] == NVMEV_SB_ACTIVE) {
			conv_ftl->slc_sb_state[blk] = NVMEV_SB_CLOSED;
			slc_sb_recent_guard_note_closed_locked(conv_ftl, blk);
			if (conv_ftl->slc_sb_active_counted &&
			    conv_ftl->slc_sb_active_counted[blk]) {
				conv_ftl->slc_sb_active_counted[blk] = 0;
			if (conv_ftl->active_sb_count)
				conv_ftl->active_sb_count--;
		} else if (!conv_ftl->slc_sb_active_counted &&
			   conv_ftl->active_sb_count) {
			conv_ftl->active_sb_count--;
		}
		/* Clear chain_cur_active_sb for any chain whose pointer was this SB
		 * (owner + parasitic writers). Bounded by next_chain_id. */
		if (conv_ftl->chain_cur_active_sb) {
			uint32_t c;
			for (c = 0; c < conv_ftl->next_chain_id; c++) {
				if (conv_ftl->chain_cur_active_sb[c] == blk)
					conv_ftl->chain_cur_active_sb[c] = U32_MAX;
			}
		}
	}
}

/* Mark SB blk as FREE. Caller holds slc_lock. Called from the GC erase path
 * when a closed SB is fully erased back into the free pool. */
static void slc_sb_note_freed_locked(struct conv_ftl *conv_ftl, uint32_t blk)
{
	struct ssdparams *spp;

	if (!conv_ftl || !conv_ftl->ssd || !conv_ftl->slc_sb_state)
		return;
	spp = &conv_ftl->ssd->sp;
	if (blk >= spp->blks_per_pl)
		return;
	if (conv_ftl->slc_sb_active_counted && conv_ftl->slc_sb_active_counted[blk]) {
		conv_ftl->slc_sb_active_counted[blk] = 0;
		if (conv_ftl->active_sb_count)
			conv_ftl->active_sb_count--;
	} else if (!conv_ftl->slc_sb_active_counted &&
		   conv_ftl->slc_sb_state[blk] == NVMEV_SB_ACTIVE &&
		   conv_ftl->active_sb_count) {
		conv_ftl->active_sb_count--;
	}
	slc_migrated_victim_remove_locked(conv_ftl, blk);
	slc_sb_recent_guard_clear_locked(conv_ftl, blk);
	conv_ftl->slc_sb_state[blk] = NVMEV_SB_FREE;
	if (conv_ftl->slc_sb_owner_chain)
		conv_ftl->slc_sb_owner_chain[blk] = INVALID_CHAIN_ID;
	if (conv_ftl->slc_sb_die_full_mask)
		conv_ftl->slc_sb_die_full_mask[blk] = 0;
}

static __maybe_unused uint32_t chain_active_slc_superblocks_locked(struct conv_ftl *conv_ftl,
						    uint32_t skip_chain,
						    uint32_t *cold_chain_out)
{
	uint32_t chain_id;
	uint32_t active = 0;
	uint32_t cold_chain = INVALID_CHAIN_ID;
	uint64_t cold_touch = U64_MAX;

	if (cold_chain_out)
		*cold_chain_out = INVALID_CHAIN_ID;
	if (!conv_ftl || !conv_ftl->chain_host_slc_wps)
		return 0;

	for (chain_id = 0; chain_id < conv_ftl->next_chain_id; chain_id++) {
		uint64_t touch = 0;

		if (!chain_has_open_slc_superblock_locked(conv_ftl, chain_id))
			continue;

		active++;
		if (chain_id == skip_chain)
			continue;

		if (conv_ftl->chain_last_slc_touch)
			touch = READ_ONCE(conv_ftl->chain_last_slc_touch[chain_id]);
		if (cold_chain == INVALID_CHAIN_ID || touch < cold_touch) {
			cold_chain = chain_id;
			cold_touch = touch;
		}
	}

	if (cold_chain_out)
		*cold_chain_out = cold_chain;
	return active;
}

static __maybe_unused bool chain_close_open_slc_superblock_locked(struct conv_ftl *conv_ftl,
						   uint32_t chain_id)
{
	struct write_pointer *wps;
	uint32_t die;
	bool closed = false;

	if (!chain_id_valid(conv_ftl, chain_id) || !conv_ftl->chain_host_slc_wps)
		return false;

	wps = conv_ftl->chain_host_slc_wps[chain_id];
	if (!wps)
		return false;

	for (die = 0; die < conv_ftl->die_count; die++) {
		struct line_mgmt *lm;

		if (!wps[die].curline)
			continue;

		lm = get_slc_die_lm(conv_ftl, die);
		if (!lm)
			continue;

		slc_wp_close_line_locked(conv_ftl, lm, &wps[die], die);
		closed = true;
	}

	return closed;
}

/* Ensure chain_id has a writable ACTIVE SB. Caller holds slc_lock.
 *
 * State machine (per design):
 *   1) chain already points at an ACTIVE SB (owner OR parasitic) -> done
 *   2) active_sb_count < 14 -> open a fresh SB strict-cross-die for this chain
 *   3) active_sb_count == 14 -> parasitic share into the most-full ACTIVE SB
 *
 * SBs are NEVER closed-by-eviction here. They only close when fully written
 * (handled in slc_sb_note_die_full_locked). This matches the design rule:
 * "active 同时可以写入的最多开14条；当写满就完成这条sb，开新的active sb写入".
 */
static bool chain_ensure_slc_superblock_slot_locked(struct conv_ftl *conv_ftl,
						    uint32_t chain_id)
{
	uint32_t cur_blk;
	int free_blk;
	uint32_t share_blk;

	if (!conv_ftl || !chain_id_valid(conv_ftl, chain_id))
		return false;

	/* Path 1: chain already has an ACTIVE SB (its own or parasitic). */
	if (conv_ftl->chain_cur_active_sb) {
		cur_blk = conv_ftl->chain_cur_active_sb[chain_id];
		if (cur_blk != U32_MAX && conv_ftl->slc_sb_state &&
		    cur_blk < conv_ftl->ssd->sp.blks_per_pl &&
		    conv_ftl->slc_sb_state[cur_blk] == NVMEV_SB_ACTIVE)
			return true;
		/* stale pointer (SB closed under us) — clear and reselect */
		if (cur_blk != U32_MAX)
			conv_ftl->chain_cur_active_sb[chain_id] = U32_MAX;
	}

	/* Path 2: capacity allows a fresh dedicated SB. */
	if (conv_ftl->active_sb_count < NVMEV_SUPERBLOCK_ACTIVE_LIMIT) {
		free_blk = slc_find_free_sb_blk_locked(conv_ftl);
		if (free_blk >= 0) {
			if (slc_open_chain_active_sb_locked(conv_ftl, chain_id,
							    (uint32_t)free_blk))
				return true;
		}
	}

	/* Path 3: cap hit (or no SB-aligned free blk). Parasitic share into the
	 * most-full ACTIVE SB. The owner's wp continues to advance; the chain
	 * just routes its writes through the same SB until it fills.
	 */
	share_blk = slc_pick_share_target_locked(conv_ftl, chain_id);
	if (share_blk != U32_MAX && conv_ftl->chain_cur_active_sb) {
		conv_ftl->chain_cur_active_sb[chain_id] = share_blk;
		return true;
	}

	/* Last-resort: no dedicated SB and no shareable ACTIVE SB. Refuse the
	 * allocation so the caller can migrate/GC and retry; do not open a
	 * drifting per-die SLC block here. */
	return false;
}

static inline void __maybe_unused qlc_wp_set_die_hint(struct conv_ftl *conv_ftl,
						      struct write_pointer *wp,
						      uint32_t ch, uint32_t lun)
{
	if (!conv_ftl || !wp)
		return;

	wp->ch = ch % conv_ftl->ssd->sp.nchs;
	wp->lun = lun % conv_ftl->ssd->sp.luns_per_ch;
}

static void qlc_prepare_die_wp(struct conv_ftl *conv_ftl, struct write_pointer *wp,
			       struct line_mgmt *lm, uint32_t die)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	if (!conv_ftl || !wp || !lm)
		return;

	if (wp->curline &&
	    (wp->curline < lm->lines || wp->curline >= (lm->lines + lm->tt_lines)))
		wp->curline = NULL;

	decode_die(spp, die, &wp->ch, &wp->lun);
}

static inline uint32_t total_slc_lines(const struct conv_ftl *conv_ftl)
{
	return conv_ftl->slc_blks_per_pl;
}

static inline uint32_t total_qlc_lines(const struct conv_ftl *conv_ftl)
{
	return conv_ftl->qlc_blks_per_pl;
}

static inline uint32_t superblock_die_count(const struct conv_ftl *conv_ftl)
{
	return conv_ftl && conv_ftl->die_count ? conv_ftl->die_count : 1;
}

static inline uint32_t slc_pages_per_superblock(const struct conv_ftl *conv_ftl)
{
	return conv_ftl->slc_pgs_per_blk * superblock_die_count(conv_ftl);
}

static inline uint32_t qlc_pages_per_superblock(const struct conv_ftl *conv_ftl)
{
	return conv_ftl->qlc_pgs_per_blk * superblock_die_count(conv_ftl);
}

static inline uint64_t total_slc_pages(const struct conv_ftl *conv_ftl)
{
	return (uint64_t)total_slc_lines(conv_ftl) *
	       slc_pages_per_superblock(conv_ftl);
}

static inline uint64_t total_qlc_pages(const struct conv_ftl *conv_ftl)
{
	return (uint64_t)total_qlc_lines(conv_ftl) *
	       qlc_pages_per_superblock(conv_ftl);
}

static void slc_resident_untrack_page(struct conv_ftl *conv_ftl, uint64_t lpn)
{
	uint32_t slot, cap, die, count, base, last_slot;
	uint32_t chain_id;
	uint64_t moved_lpn;

	if (!conv_ftl || !conv_ftl->slc_resident_slot || !conv_ftl->slc_resident_lpns ||
	    !conv_ftl->slc_die_resident_count || !conv_ftl->slc_resident_capacity_per_die)
		return;
	if (lpn >= conv_ftl->ssd->sp.tt_pgs)
		return;

	cap = conv_ftl->slc_resident_capacity_per_die;

	spin_lock(&conv_ftl->slc_lock);
	slot = conv_ftl->slc_resident_slot[lpn];
	if (slot == U32_MAX) {
		spin_unlock(&conv_ftl->slc_lock);
		return;
	}

	die = slot / cap;
	base = die * cap;
	count = conv_ftl->slc_die_resident_count[die];
	if (!count) {
		conv_ftl->slc_resident_slot[lpn] = U32_MAX;
		spin_unlock(&conv_ftl->slc_lock);
		return;
	}

	chain_id = chain_id_for_lpn(conv_ftl, lpn);
	last_slot = base + count - 1;
	if (slot != last_slot) {
		moved_lpn = conv_ftl->slc_resident_lpns[last_slot];
		conv_ftl->slc_resident_lpns[slot] = moved_lpn;
		if (moved_lpn < conv_ftl->ssd->sp.tt_pgs)
			conv_ftl->slc_resident_slot[moved_lpn] = slot;
	}

	conv_ftl->slc_resident_slot[lpn] = U32_MAX;
	conv_ftl->slc_die_resident_count[die]--;
	chain_slc_page_count_dec(conv_ftl, chain_id);
	if (conv_ftl->slc_die_resident_count[die] == 0) {
		conv_ftl->slc_die_resident_cursor[die] = 0;
	} else if (conv_ftl->slc_die_resident_cursor[die] >=
		   conv_ftl->slc_die_resident_count[die]) {
		conv_ftl->slc_die_resident_cursor[die] %=
			conv_ftl->slc_die_resident_count[die];
	}
	spin_unlock(&conv_ftl->slc_lock);
}

static void slc_resident_track_page(struct conv_ftl *conv_ftl, uint64_t lpn, uint32_t die)
{
	uint32_t cap, slot, old_slot, old_die, base, count;
	uint32_t chain_id;

	if (!conv_ftl || !conv_ftl->slc_resident_slot || !conv_ftl->slc_resident_lpns ||
	    !conv_ftl->slc_die_resident_count || !conv_ftl->slc_resident_capacity_per_die)
		return;
	if (lpn >= conv_ftl->ssd->sp.tt_pgs || !conv_ftl->die_count)
		return;

	cap = conv_ftl->slc_resident_capacity_per_die;
	die %= conv_ftl->die_count;

	spin_lock(&conv_ftl->slc_lock);
	old_slot = conv_ftl->slc_resident_slot[lpn];
	if (old_slot != U32_MAX) {
		old_die = old_slot / cap;
		if (old_die == die) {
			spin_unlock(&conv_ftl->slc_lock);
			return;
		}
		spin_unlock(&conv_ftl->slc_lock);
		slc_resident_untrack_page(conv_ftl, lpn);
		spin_lock(&conv_ftl->slc_lock);
	}

	count = conv_ftl->slc_die_resident_count[die];
	if (count >= cap) {
		spin_unlock(&conv_ftl->slc_lock);
		NVMEV_WARN("slc_resident_track_page: die %u resident set full, lpn=%llu\n",
			   die, lpn);
		return;
	}

	base = die * cap;
	slot = base + count;
	conv_ftl->slc_resident_lpns[slot] = lpn;
	conv_ftl->slc_resident_slot[lpn] = slot;
	conv_ftl->slc_die_resident_count[die]++;
	chain_id = chain_id_for_lpn(conv_ftl, lpn);
	chain_slc_page_count_inc(conv_ftl, chain_id);
	spin_unlock(&conv_ftl->slc_lock);
}

static bool slc_has_any_victim(struct conv_ftl *conv_ftl)
{
	uint32_t blk;
	bool found = false;

	if (!conv_ftl || !conv_ftl->ssd || !conv_ftl->slc_lunlm ||
	    !conv_ftl->slc_sb_migrated_victim)
		return false;

	spin_lock(&conv_ftl->slc_lock);
	for (blk = 0; blk < conv_ftl->slc_blks_per_pl; blk++) {
		struct slc_sb_summary_no1 sum;

		if (!conv_ftl->slc_sb_migrated_victim[blk])
			continue;
		if (!slc_sb_collect_summary(conv_ftl, blk, &sum))
			continue;
		if (sum.active || sum.open_writer ||
		    (sum.total_vpc == 0 && sum.total_ipc == 0)) {
			slc_migrated_victim_remove_locked(conv_ftl, blk);
			conv_ftl->slc_sb_migration_victim_stale++;
			continue;
		}
		if (sum.total_ipc || sum.total_vpc) {
			found = true;
			break;
		}
	}
	spin_unlock(&conv_ftl->slc_lock);
	return found;
}

static bool qlc_has_any_victim(struct conv_ftl *conv_ftl)
{
	uint32_t die;
	bool found = false;

	if (!conv_ftl || !conv_ftl->qlc_lunlm)
		return false;

	spin_lock(&conv_ftl->qlc_lock);
	for (die = 0; die < conv_ftl->die_count; die++) {
		if (conv_ftl->qlc_lunlm[die].victim_line_cnt) {
			found = true;
			break;
		}
	}
	spin_unlock(&conv_ftl->qlc_lock);
	return found;
}

static inline uint32_t pages_to_lines(uint64_t pages, uint32_t pgs_per_blk)
{
	return pgs_per_blk ? (uint32_t)div_u64(pages, pgs_per_blk) : 0;
}

struct victim_candidate {
	struct line *line;
	uint32_t die;
	bool is_slc;
	uint32_t vpc;
};

static bool find_best_victim(struct conv_ftl *conv_ftl, bool slc_pool,
			     bool force, struct victim_candidate *cand)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	if (slc_pool) {
		struct line_mgmt *array = conv_ftl->slc_lunlm;
		spinlock_t *lock = &conv_ftl->slc_lock;
		uint32_t die_count = conv_ftl->die_count ? conv_ftl->die_count : 1;
		struct line *best = NULL;
		uint32_t best_die = 0;
		bool best_high_purity = false;
		uint32_t die;

		if (!array)
			return false;

		spin_lock(lock);
		for (die = 0; die < die_count; die++) {
			struct line_mgmt *lm = &array[die];
			struct line *candidate = pqueue_peek(lm->victim_line_pq);
			uint32_t slc_line_pages = spp->pgs_per_lun_line ?
				spp->pgs_per_lun_line : spp->pgs_per_blk;
			bool candidate_high_purity = false;
			struct ppa candidate_ppa = { .ppa = 0 };

			if (!candidate)
				continue;
			if (!force) {
				uint32_t ch = 0, lun = 0;

				candidate_ppa.g.blk = blk_from_line(candidate->id);
				candidate_ppa.g.pl = 0;
				decode_die(spp, die, &ch, &lun);
				candidate_ppa.g.ch = ch;
				candidate_ppa.g.lun = lun;
				candidate_high_purity =
					block_meta_high_purity(conv_ftl, &candidate_ppa, NULL);
				if (candidate->vpc > max_t(uint32_t, 1, slc_line_pages / 8) &&
				    !(candidate_high_purity &&
				      candidate->vpc <= max_t(uint32_t, 1, slc_line_pages / 2)))
					continue;
			}
			if (!best || (!force && candidate_high_purity && !best_high_purity) ||
			    (candidate_high_purity == best_high_purity &&
			     candidate->vpc < best->vpc)) {
				best = candidate;
				best_die = die;
				best_high_purity = candidate_high_purity;
			}
		}

		if (best) {
			cand->line = best;
			cand->die = best_die;
			cand->is_slc = true;
			cand->vpc = best->vpc;
		}
		spin_unlock(lock);
		return best != NULL;
	}

	/* QLC: per-die pools */
	{
		struct line *best = NULL;
		uint32_t best_die = 0;
		uint32_t die_count = conv_ftl->die_count ? conv_ftl->die_count : 1;
		uint32_t die;
		uint32_t qlc_line_pages = conv_ftl->qlc_pgs_per_blk ?
					  conv_ftl->qlc_pgs_per_blk : spp->pgs_per_blk;

		if (!conv_ftl->qlc_lunlm)
			return false;

		spin_lock(&conv_ftl->qlc_lock);
		for (die = 0; die < die_count; die++) {
			struct line_mgmt *lm = &conv_ftl->qlc_lunlm[die];
			struct line *candidate = pqueue_peek(lm->victim_line_pq);

			if (!candidate)
				continue;
			if (!force && candidate->vpc > (qlc_line_pages / 8))
				continue;
			if (!best || candidate->vpc < best->vpc) {
				best = candidate;
				best_die = die;
			}
		}

		if (best) {
			cand->line = best;
			cand->die = best_die;
			cand->is_slc = false;
			cand->vpc = best->vpc;
		}
		spin_unlock(&conv_ftl->qlc_lock);
		return best != NULL;
	}
}

static bool pop_victim_from_pool(struct conv_ftl *conv_ftl, struct victim_candidate *cand)
{
	if (cand->is_slc) {
		struct line_mgmt *array = conv_ftl->slc_lunlm;
		spinlock_t *lock = &conv_ftl->slc_lock;
		uint32_t die_count = conv_ftl->die_count ? conv_ftl->die_count : 1;
		uint32_t die;
		struct line_mgmt *lm;

		if (!array || die_count == 0)
			return false;

		die = cand->die % die_count;
		lm = &array[die];

		spin_lock(lock);
		if (!lm->victim_line_cnt || pqueue_peek(lm->victim_line_pq) != cand->line) {
			spin_unlock(lock);
			return false;
		}

		pqueue_pop(lm->victim_line_pq);
		cand->line->pos = 0;
		lm->victim_line_cnt--;
		spin_unlock(lock);
		return true;
	}

	if (!conv_ftl->qlc_lunlm || !conv_ftl->die_count)
		return false;

	spin_lock(&conv_ftl->qlc_lock);
	{
		uint32_t die = cand->die % conv_ftl->die_count;
		struct line_mgmt *lm = &conv_ftl->qlc_lunlm[die];

		if (!lm->victim_line_cnt || pqueue_peek(lm->victim_line_pq) != cand->line) {
			spin_unlock(&conv_ftl->qlc_lock);
			return false;
		}

		pqueue_pop(lm->victim_line_pq);
		cand->line->pos = 0;
		lm->victim_line_cnt--;
	}
	spin_unlock(&conv_ftl->qlc_lock);
	return true;
}

static void slc_apply_line_valid(struct line_mgmt *lm, uint32_t blk, struct ssdparams *spp)
{
	struct line *line = &lm->lines[blk];

	line->vpc++;
	if (line->vpc > spp->pgs_per_lun_line)
		line->vpc = spp->pgs_per_lun_line;
}

static void slc_apply_line_invalid(struct line_mgmt *lm, uint32_t blk, struct ssdparams *spp)
{
	struct line *line = &lm->lines[blk];
	bool was_full_line = (line->vpc == spp->pgs_per_lun_line);

	line->ipc++;
	if (line->vpc > 0)
		line->vpc--;

	if (was_full_line) {
		list_del_init(&line->entry);
		if (lm->full_line_cnt)
			lm->full_line_cnt--;
	}
	line->pos = 0;
}

static void set_page_prev_link(struct conv_ftl *conv_ftl, uint64_t lpn,
			 struct ppa *ppa, uint64_t prev_lpn)
{
	struct nand_page *pg;
	(void)lpn;

	pg = get_pg(conv_ftl->ssd, ppa);
	if (!pg)
		return;
	pg->oob_prev_lpn = prev_lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
	return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
	return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
	((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
	return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
	((struct line *)a)->pos = pos;
}

static inline void consume_write_credit(struct conv_ftl *conv_ftl)
{
	conv_ftl->wfc.write_credits--;
}

static void forground_gc(struct conv_ftl *conv_ftl, enum foreground_gc_mode mode);

static inline bool check_and_refill_write_credit(struct conv_ftl *conv_ftl)
{
	struct write_flow_control *wfc = &(conv_ftl->wfc);
	uint32_t refill_pages;

	if (wfc->write_credits <= 0) {
		/*
		 * Refill credits in oneshot-sized chunks so sustained writes can
		 * continue making progress.  Actual GC is now gated by explicit
		 * free-line thresholds in the control-tick path.
		 */
		refill_pages = conv_ftl->ssd->sp.pgs_per_oneshotpg * 8;
		if (refill_pages < 10)
			refill_pages = 10;
		wfc->write_credits += refill_pages;
		return true;
	}

	return false;
}

static void init_write_flow_control(struct conv_ftl *conv_ftl)
{
	struct write_flow_control *wfc = &(conv_ftl->wfc);
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	wfc->write_credits = spp->pgs_per_line;
	wfc->credits_to_refill = spp->pgs_per_line;
}

static void init_maptbl(struct conv_ftl *conv_ftl)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	int i;
	uint32_t guard_i;

	conv_ftl->maptbl = vmalloc(sizeof(struct ppa) * spp->tt_pgs);
	if (!conv_ftl->maptbl) {
		NVMEV_ERROR("Failed to allocate mapping table memory\n");
		conv_ftl->maptbl_initialized = false;
		return;
	}
	conv_ftl->lpn_initial_die = vmalloc(sizeof(*conv_ftl->lpn_initial_die) * spp->tt_pgs);
	conv_ftl->lpn_die_changed = vzalloc(sizeof(*conv_ftl->lpn_die_changed) * spp->tt_pgs);
	conv_ftl->lpn_die_change_reason = vzalloc(sizeof(*conv_ftl->lpn_die_change_reason) * spp->tt_pgs);
#if NVMEV_ENABLE_CHAIN_AGGREGATION
	conv_ftl->lpn_chain_id = vmalloc(sizeof(*conv_ftl->lpn_chain_id) * spp->tt_pgs);
	conv_ftl->chain_slc_next_die = vmalloc(sizeof(*conv_ftl->chain_slc_next_die) * spp->tt_pgs);
	conv_ftl->chain_qlc_next_die = vmalloc(sizeof(*conv_ftl->chain_qlc_next_die) * spp->tt_pgs);
	conv_ftl->chain_slc_rr_pages = vzalloc(sizeof(*conv_ftl->chain_slc_rr_pages) * spp->tt_pgs);
	conv_ftl->chain_qlc_rr_pages = vzalloc(sizeof(*conv_ftl->chain_qlc_rr_pages) * spp->tt_pgs);
	conv_ftl->chain_slc_page_count = vzalloc(sizeof(*conv_ftl->chain_slc_page_count) * spp->tt_pgs);
	conv_ftl->chain_last_slc_touch = vzalloc(sizeof(*conv_ftl->chain_last_slc_touch) * spp->tt_pgs);
	conv_ftl->chain_host_slc_wps = vzalloc(sizeof(*conv_ftl->chain_host_slc_wps) * spp->tt_pgs);
	conv_ftl->blk_owner_chain = vmalloc(sizeof(*conv_ftl->blk_owner_chain) * spp->tt_blks);
	conv_ftl->blk_owner_pages = vzalloc(sizeof(*conv_ftl->blk_owner_pages) * spp->tt_blks);
	conv_ftl->blk_valid_pages = vzalloc(sizeof(*conv_ftl->blk_valid_pages) * spp->tt_blks);
	conv_ftl->blk_mixed_pages = vzalloc(sizeof(*conv_ftl->blk_mixed_pages) * spp->tt_blks);
	conv_ftl->blk_active_wp_refs = vzalloc(sizeof(*conv_ftl->blk_active_wp_refs) * spp->tt_blks);
	/* superblock state machine: indexed by blk_id (0..blks_per_pl-1).
	 * SB == same blk_id across all dies, so we only need one entry per blk_id. */
	conv_ftl->slc_sb_state = vzalloc(sizeof(*conv_ftl->slc_sb_state) * spp->blks_per_pl);
	conv_ftl->slc_sb_active_counted = vzalloc(sizeof(*conv_ftl->slc_sb_active_counted) * spp->blks_per_pl);
	conv_ftl->slc_sb_owner_chain = vmalloc(sizeof(*conv_ftl->slc_sb_owner_chain) * spp->blks_per_pl);
	conv_ftl->slc_sb_die_full_mask = vzalloc(sizeof(*conv_ftl->slc_sb_die_full_mask) * spp->blks_per_pl);
	conv_ftl->slc_sb_migrated_victim = vzalloc(sizeof(*conv_ftl->slc_sb_migrated_victim) * spp->blks_per_pl);
	conv_ftl->slc_sb_recent_guard = vzalloc(sizeof(*conv_ftl->slc_sb_recent_guard) * spp->blks_per_pl);
	conv_ftl->slc_sb_generation = vzalloc(sizeof(*conv_ftl->slc_sb_generation) * spp->blks_per_pl);
	conv_ftl->qlc_sb_state = vzalloc(sizeof(*conv_ftl->qlc_sb_state) *
					 conv_ftl->qlc_blks_per_pl);
	conv_ftl->qlc_sb_active_counted =
		vzalloc(sizeof(*conv_ftl->qlc_sb_active_counted) *
			conv_ftl->qlc_blks_per_pl);
	conv_ftl->qlc_sb_owner_chain =
		vmalloc(sizeof(*conv_ftl->qlc_sb_owner_chain) *
			conv_ftl->qlc_blks_per_pl);
	conv_ftl->qlc_sb_die_closed_mask =
		vzalloc(sizeof(*conv_ftl->qlc_sb_die_closed_mask) *
			conv_ftl->qlc_blks_per_pl);
	conv_ftl->slc_recent_guard_ring_size =
		(conv_ftl->slc_blks_per_pl * SLC_SB_RECENT_GUARD_PCT + 99U) / 100U;
	if (!conv_ftl->slc_recent_guard_ring_size)
		conv_ftl->slc_recent_guard_ring_size = 1;
	if (conv_ftl->slc_recent_guard_ring_size > conv_ftl->slc_blks_per_pl)
		conv_ftl->slc_recent_guard_ring_size = conv_ftl->slc_blks_per_pl;
	conv_ftl->slc_recent_guard_ring_blk =
		vmalloc(sizeof(*conv_ftl->slc_recent_guard_ring_blk) *
			conv_ftl->slc_recent_guard_ring_size);
	conv_ftl->slc_recent_guard_ring_gen =
		vzalloc(sizeof(*conv_ftl->slc_recent_guard_ring_gen) *
			conv_ftl->slc_recent_guard_ring_size);
	conv_ftl->chain_cur_active_sb = vmalloc(sizeof(*conv_ftl->chain_cur_active_sb) * spp->tt_pgs);
	conv_ftl->chain_cur_active_qlc_sb =
		vmalloc(sizeof(*conv_ftl->chain_cur_active_qlc_sb) * spp->tt_pgs);
	conv_ftl->chain_host_qlc_wps =
		vzalloc(sizeof(*conv_ftl->chain_host_qlc_wps) * spp->tt_pgs);
	conv_ftl->chain_host_read_count = vzalloc(sizeof(*conv_ftl->chain_host_read_count) * spp->tt_pgs);
	conv_ftl->chain_repromote_cursor = vzalloc(sizeof(*conv_ftl->chain_repromote_cursor) * spp->tt_pgs);
	conv_ftl->active_sb_count = 0;
	conv_ftl->qlc_active_sb_count = 0;
	conv_ftl->slc_sb_migrated_victim_count = 0;
	conv_ftl->slc_recent_guard_ring_head = 0;
#else
	conv_ftl->lpn_chain_id = NULL;
	conv_ftl->chain_slc_next_die = NULL;
	conv_ftl->chain_qlc_next_die = NULL;
	conv_ftl->chain_slc_rr_pages = NULL;
	conv_ftl->chain_qlc_rr_pages = NULL;
	conv_ftl->chain_slc_page_count = NULL;
	conv_ftl->chain_last_slc_touch = NULL;
	conv_ftl->chain_host_slc_wps = NULL;
	conv_ftl->blk_owner_chain = NULL;
	conv_ftl->blk_owner_pages = NULL;
	conv_ftl->blk_valid_pages = NULL;
	conv_ftl->blk_mixed_pages = NULL;
	conv_ftl->blk_active_wp_refs = NULL;
	conv_ftl->slc_sb_state = NULL;
	conv_ftl->slc_sb_active_counted = NULL;
	conv_ftl->slc_sb_owner_chain = NULL;
	conv_ftl->slc_sb_die_full_mask = NULL;
	conv_ftl->slc_sb_migrated_victim = NULL;
	conv_ftl->slc_sb_recent_guard = NULL;
	conv_ftl->slc_sb_generation = NULL;
	conv_ftl->qlc_sb_state = NULL;
	conv_ftl->qlc_sb_active_counted = NULL;
	conv_ftl->qlc_sb_owner_chain = NULL;
	conv_ftl->qlc_sb_die_closed_mask = NULL;
	conv_ftl->slc_recent_guard_ring_blk = NULL;
	conv_ftl->slc_recent_guard_ring_gen = NULL;
	conv_ftl->chain_cur_active_sb = NULL;
	conv_ftl->chain_cur_active_qlc_sb = NULL;
	conv_ftl->chain_host_qlc_wps = NULL;
	conv_ftl->chain_host_read_count = NULL;
	conv_ftl->chain_repromote_cursor = NULL;
	conv_ftl->active_sb_count = 0;
	conv_ftl->qlc_active_sb_count = 0;
	conv_ftl->slc_sb_migrated_victim_count = 0;
	conv_ftl->slc_recent_guard_ring_size = 0;
	conv_ftl->slc_recent_guard_ring_head = 0;
#endif
	if (!conv_ftl->lpn_initial_die || !conv_ftl->lpn_die_changed ||
	    !conv_ftl->lpn_die_change_reason
#if NVMEV_ENABLE_CHAIN_AGGREGATION
	    || !conv_ftl->lpn_chain_id || !conv_ftl->chain_slc_next_die ||
	    !conv_ftl->chain_qlc_next_die || !conv_ftl->chain_slc_rr_pages ||
	    !conv_ftl->chain_qlc_rr_pages || !conv_ftl->chain_slc_page_count ||
	    !conv_ftl->chain_last_slc_touch || !conv_ftl->chain_host_slc_wps ||
	    !conv_ftl->blk_owner_chain ||
	    !conv_ftl->blk_owner_pages || !conv_ftl->blk_valid_pages ||
	    !conv_ftl->blk_mixed_pages || !conv_ftl->blk_active_wp_refs ||
	    !conv_ftl->slc_sb_state || !conv_ftl->slc_sb_active_counted ||
	    !conv_ftl->slc_sb_owner_chain ||
	    !conv_ftl->slc_sb_die_full_mask || !conv_ftl->slc_sb_migrated_victim ||
	    !conv_ftl->slc_sb_recent_guard || !conv_ftl->slc_sb_generation ||
	    !conv_ftl->qlc_sb_state || !conv_ftl->qlc_sb_active_counted ||
	    !conv_ftl->qlc_sb_owner_chain || !conv_ftl->qlc_sb_die_closed_mask ||
	    !conv_ftl->slc_recent_guard_ring_blk ||
	    !conv_ftl->slc_recent_guard_ring_gen ||
	    !conv_ftl->chain_cur_active_sb || !conv_ftl->chain_cur_active_qlc_sb ||
	    !conv_ftl->chain_host_qlc_wps ||
	    !conv_ftl->chain_host_read_count || !conv_ftl->chain_repromote_cursor
#endif
	    ) {
		NVMEV_ERROR("Failed to allocate LPN die-change tracking memory\n");
		vfree(conv_ftl->maptbl);
		conv_ftl->maptbl = NULL;
		if (conv_ftl->lpn_initial_die)
			vfree(conv_ftl->lpn_initial_die);
		if (conv_ftl->lpn_die_changed)
			vfree(conv_ftl->lpn_die_changed);
		if (conv_ftl->lpn_die_change_reason)
			vfree(conv_ftl->lpn_die_change_reason);
		if (conv_ftl->lpn_chain_id)
			vfree(conv_ftl->lpn_chain_id);
		if (conv_ftl->chain_slc_next_die)
			vfree(conv_ftl->chain_slc_next_die);
		if (conv_ftl->chain_qlc_next_die)
			vfree(conv_ftl->chain_qlc_next_die);
		if (conv_ftl->chain_slc_rr_pages)
			vfree(conv_ftl->chain_slc_rr_pages);
		if (conv_ftl->chain_qlc_rr_pages)
			vfree(conv_ftl->chain_qlc_rr_pages);
		if (conv_ftl->chain_slc_page_count)
			vfree(conv_ftl->chain_slc_page_count);
		if (conv_ftl->chain_last_slc_touch)
			vfree(conv_ftl->chain_last_slc_touch);
		if (conv_ftl->chain_host_slc_wps)
			vfree(conv_ftl->chain_host_slc_wps);
		if (conv_ftl->blk_owner_chain)
			vfree(conv_ftl->blk_owner_chain);
		if (conv_ftl->blk_owner_pages)
			vfree(conv_ftl->blk_owner_pages);
		if (conv_ftl->blk_valid_pages)
			vfree(conv_ftl->blk_valid_pages);
		if (conv_ftl->blk_mixed_pages)
			vfree(conv_ftl->blk_mixed_pages);
		if (conv_ftl->blk_active_wp_refs)
			vfree(conv_ftl->blk_active_wp_refs);
		if (conv_ftl->slc_sb_state)
			vfree(conv_ftl->slc_sb_state);
		if (conv_ftl->slc_sb_active_counted)
			vfree(conv_ftl->slc_sb_active_counted);
		if (conv_ftl->slc_sb_owner_chain)
			vfree(conv_ftl->slc_sb_owner_chain);
		if (conv_ftl->slc_sb_die_full_mask)
			vfree(conv_ftl->slc_sb_die_full_mask);
		if (conv_ftl->slc_sb_migrated_victim)
			vfree(conv_ftl->slc_sb_migrated_victim);
		if (conv_ftl->slc_sb_recent_guard)
			vfree(conv_ftl->slc_sb_recent_guard);
		if (conv_ftl->slc_sb_generation)
			vfree(conv_ftl->slc_sb_generation);
		if (conv_ftl->qlc_sb_state)
			vfree(conv_ftl->qlc_sb_state);
		if (conv_ftl->qlc_sb_active_counted)
			vfree(conv_ftl->qlc_sb_active_counted);
		if (conv_ftl->qlc_sb_owner_chain)
			vfree(conv_ftl->qlc_sb_owner_chain);
		if (conv_ftl->qlc_sb_die_closed_mask)
			vfree(conv_ftl->qlc_sb_die_closed_mask);
		if (conv_ftl->slc_recent_guard_ring_blk)
			vfree(conv_ftl->slc_recent_guard_ring_blk);
		if (conv_ftl->slc_recent_guard_ring_gen)
			vfree(conv_ftl->slc_recent_guard_ring_gen);
		if (conv_ftl->chain_cur_active_sb)
			vfree(conv_ftl->chain_cur_active_sb);
		if (conv_ftl->chain_cur_active_qlc_sb)
			vfree(conv_ftl->chain_cur_active_qlc_sb);
		if (conv_ftl->chain_host_qlc_wps)
			vfree(conv_ftl->chain_host_qlc_wps);
		if (conv_ftl->chain_host_read_count)
			vfree(conv_ftl->chain_host_read_count);
		if (conv_ftl->chain_repromote_cursor)
			vfree(conv_ftl->chain_repromote_cursor);
		conv_ftl->lpn_initial_die = NULL;
		conv_ftl->lpn_die_changed = NULL;
		conv_ftl->lpn_die_change_reason = NULL;
		conv_ftl->lpn_chain_id = NULL;
		conv_ftl->chain_slc_next_die = NULL;
		conv_ftl->chain_qlc_next_die = NULL;
		conv_ftl->chain_slc_rr_pages = NULL;
		conv_ftl->chain_qlc_rr_pages = NULL;
		conv_ftl->chain_slc_page_count = NULL;
		conv_ftl->chain_last_slc_touch = NULL;
		conv_ftl->chain_host_slc_wps = NULL;
		conv_ftl->blk_owner_chain = NULL;
		conv_ftl->blk_owner_pages = NULL;
		conv_ftl->blk_valid_pages = NULL;
		conv_ftl->blk_mixed_pages = NULL;
		conv_ftl->blk_active_wp_refs = NULL;
		conv_ftl->slc_sb_state = NULL;
		conv_ftl->slc_sb_active_counted = NULL;
		conv_ftl->slc_sb_owner_chain = NULL;
		conv_ftl->slc_sb_die_full_mask = NULL;
		conv_ftl->slc_sb_migrated_victim = NULL;
		conv_ftl->slc_sb_recent_guard = NULL;
		conv_ftl->slc_sb_generation = NULL;
		conv_ftl->qlc_sb_state = NULL;
		conv_ftl->qlc_sb_active_counted = NULL;
		conv_ftl->qlc_sb_owner_chain = NULL;
		conv_ftl->qlc_sb_die_closed_mask = NULL;
		conv_ftl->slc_recent_guard_ring_blk = NULL;
		conv_ftl->slc_recent_guard_ring_gen = NULL;
		conv_ftl->chain_cur_active_sb = NULL;
		conv_ftl->chain_cur_active_qlc_sb = NULL;
		conv_ftl->chain_host_qlc_wps = NULL;
		conv_ftl->chain_host_read_count = NULL;
		conv_ftl->chain_repromote_cursor = NULL;
		conv_ftl->slc_recent_guard_ring_size = 0;
		conv_ftl->slc_recent_guard_ring_head = 0;
		conv_ftl->maptbl_initialized = false;
		return;
	}

	for (i = 0; i < spp->tt_pgs; i++) {
		conv_ftl->maptbl[i].ppa = UNMAPPED_PPA;
		conv_ftl->lpn_initial_die[i] = U16_MAX;
		if (conv_ftl->lpn_chain_id)
			conv_ftl->lpn_chain_id[i] = INVALID_CHAIN_ID;
		if (conv_ftl->chain_slc_next_die)
			conv_ftl->chain_slc_next_die[i] = INVALID_CHAIN_DIE;
		if (conv_ftl->chain_qlc_next_die)
			conv_ftl->chain_qlc_next_die[i] = INVALID_CHAIN_DIE;
	}
	if (conv_ftl->blk_owner_chain) {
		for (i = 0; i < spp->tt_blks; i++)
			conv_ftl->blk_owner_chain[i] = INVALID_CHAIN_ID;
	}
	if (conv_ftl->slc_sb_owner_chain) {
		for (i = 0; i < spp->blks_per_pl; i++)
			conv_ftl->slc_sb_owner_chain[i] = INVALID_CHAIN_ID;
	}
	if (conv_ftl->qlc_sb_owner_chain) {
		for (i = 0; i < conv_ftl->qlc_blks_per_pl; i++)
			conv_ftl->qlc_sb_owner_chain[i] = INVALID_CHAIN_ID;
	}
	if (conv_ftl->chain_cur_active_sb) {
		for (i = 0; i < spp->tt_pgs; i++)
			conv_ftl->chain_cur_active_sb[i] = U32_MAX;
	}
	if (conv_ftl->chain_cur_active_qlc_sb) {
		for (i = 0; i < spp->tt_pgs; i++)
			conv_ftl->chain_cur_active_qlc_sb[i] = U32_MAX;
	}
	if (conv_ftl->slc_recent_guard_ring_blk) {
		for (guard_i = 0; guard_i < conv_ftl->slc_recent_guard_ring_size; guard_i++)
			conv_ftl->slc_recent_guard_ring_blk[guard_i] = U32_MAX;
	}
	conv_ftl->lpn_initial_die_tracked = 0;
	conv_ftl->lpn_current_die_changed = 0;
	conv_ftl->lpn_changed_host_append = 0;
	conv_ftl->lpn_changed_host_overwrite = 0;
	conv_ftl->lpn_changed_gc = 0;
	conv_ftl->lpn_changed_slc_to_qlc = 0;
	conv_ftl->lpn_changed_repromote = 0;
	conv_ftl->lpn_changed_qlc_rebalance = 0;
	conv_ftl->chain_chunk_migration_attempts = 0;
	conv_ftl->chain_chunk_migration_pages = 0;
	conv_ftl->chain_block_migration_attempts = 0;
	conv_ftl->chain_block_migration_pages = 0;
	conv_ftl->chain_block_migration_skip_budget = 0;
	conv_ftl->chain_gc_to_qlc_pages = 0;
	conv_ftl->slc_sb_migration_attempts = 0;
	conv_ftl->slc_sb_migration_pages = 0;
	conv_ftl->slc_sb_migration_victim_enqueues = 0;
	conv_ftl->slc_sb_migration_victim_dequeues = 0;
	conv_ftl->slc_sb_migration_victim_stale = 0;
	conv_ftl->slc_sb_recent_guard_skips = 0;
	conv_ftl->slc_sb_recent_guard_forced = 0;
	conv_ftl->slc_sb_gc_count = 0;
	conv_ftl->slc_sb_gc_valid_pages = 0;
	conv_ftl->slc_sb_gc_invalid_pages = 0;
	conv_ftl->slc_sb_gc_erase_ops = 0;
	conv_ftl->slc_sb_gc_erase_time_ns = 0;
	conv_ftl->slc_sb_gc_tier_empty = 0;
	conv_ftl->slc_sb_gc_tier_pure_cold = 0;
	conv_ftl->slc_sb_gc_tier_mixed = 0;
	conv_ftl->slc_sb_gc_tier_fallback = 0;
	conv_ftl->repromote_chain_alloc_pages = 0;
	conv_ftl->repromote_gc_pool_pages = 0;
	conv_ftl->repromote_skip_active_cap = 0;
	conv_ftl->chain_slc_die_reroute_count = 0;
	conv_ftl->chain_alloc_no_prev = 0;
	conv_ftl->chain_alloc_prev_chain_invalid = 0;
	conv_ftl->chain_alloc_capacity_fail = 0;
	conv_ftl->chain_capacity =
#if NVMEV_ENABLE_CHAIN_AGGREGATION
		spp->tt_pgs;
#else
		0;
#endif
	conv_ftl->next_chain_id = 0;
	conv_ftl->maptbl_initialized = true;

	spin_lock_init(&conv_ftl->event_log_lock);
	conv_ftl->chain_alloc_event_head = 0;
	conv_ftl->chain_alloc_event_count = 0;
	conv_ftl->chain_cold_event_head = 0;
	conv_ftl->chain_cold_event_count = 0;
	conv_ftl->gc_victim_event_head = 0;
	conv_ftl->gc_victim_event_count = 0;
	conv_ftl->chain_alloc_event_seq = 0;
	conv_ftl->chain_cold_event_seq = 0;
	conv_ftl->gc_victim_event_seq = 0;
	conv_ftl->chain_alloc_events =
		vzalloc(sizeof(*conv_ftl->chain_alloc_events) * NVMEV_EVENT_LOG_CAP);
	conv_ftl->chain_cold_events =
		vzalloc(sizeof(*conv_ftl->chain_cold_events) * NVMEV_EVENT_LOG_CAP);
	conv_ftl->gc_victim_events =
		vzalloc(sizeof(*conv_ftl->gc_victim_events) * NVMEV_EVENT_LOG_CAP);
	if (!conv_ftl->chain_alloc_events || !conv_ftl->chain_cold_events ||
	    !conv_ftl->gc_victim_events)
		NVMEV_WARN("event logs unavailable (alloc failed)\n");
}

static void remove_maptbl(struct conv_ftl *conv_ftl)
{
	uint32_t chain_id;

	if (conv_ftl->chain_host_slc_wps) {
		for (chain_id = 0; chain_id < conv_ftl->next_chain_id; chain_id++)
			kfree(conv_ftl->chain_host_slc_wps[chain_id]);
	}
	if (conv_ftl->chain_host_qlc_wps) {
		for (chain_id = 0; chain_id < conv_ftl->next_chain_id; chain_id++)
			kfree(conv_ftl->chain_host_qlc_wps[chain_id]);
	}
	vfree(conv_ftl->maptbl);
	vfree(conv_ftl->lpn_initial_die);
	vfree(conv_ftl->lpn_die_changed);
	vfree(conv_ftl->lpn_die_change_reason);
	vfree(conv_ftl->lpn_chain_id);
	vfree(conv_ftl->chain_slc_next_die);
	vfree(conv_ftl->chain_qlc_next_die);
	vfree(conv_ftl->chain_slc_rr_pages);
	vfree(conv_ftl->chain_qlc_rr_pages);
	vfree(conv_ftl->chain_slc_page_count);
	vfree(conv_ftl->chain_last_slc_touch);
	vfree(conv_ftl->chain_host_slc_wps);
	vfree(conv_ftl->blk_owner_chain);
	vfree(conv_ftl->blk_owner_pages);
	vfree(conv_ftl->blk_valid_pages);
	vfree(conv_ftl->blk_mixed_pages);
	vfree(conv_ftl->blk_active_wp_refs);
	vfree(conv_ftl->slc_sb_state);
	vfree(conv_ftl->slc_sb_active_counted);
	vfree(conv_ftl->slc_sb_owner_chain);
	vfree(conv_ftl->slc_sb_die_full_mask);
	vfree(conv_ftl->slc_sb_migrated_victim);
	vfree(conv_ftl->slc_sb_recent_guard);
	vfree(conv_ftl->slc_sb_generation);
	vfree(conv_ftl->qlc_sb_state);
	vfree(conv_ftl->qlc_sb_active_counted);
	vfree(conv_ftl->qlc_sb_owner_chain);
	vfree(conv_ftl->qlc_sb_die_closed_mask);
	vfree(conv_ftl->slc_recent_guard_ring_blk);
	vfree(conv_ftl->slc_recent_guard_ring_gen);
	vfree(conv_ftl->chain_cur_active_sb);
	vfree(conv_ftl->chain_cur_active_qlc_sb);
	vfree(conv_ftl->chain_host_qlc_wps);
	vfree(conv_ftl->chain_host_read_count);
	vfree(conv_ftl->chain_repromote_cursor);
	vfree(conv_ftl->chain_alloc_events);
	vfree(conv_ftl->chain_cold_events);
	vfree(conv_ftl->gc_victim_events);
	conv_ftl->maptbl = NULL;
	conv_ftl->lpn_initial_die = NULL;
	conv_ftl->lpn_die_changed = NULL;
	conv_ftl->lpn_die_change_reason = NULL;
	conv_ftl->lpn_chain_id = NULL;
	conv_ftl->chain_slc_next_die = NULL;
	conv_ftl->chain_qlc_next_die = NULL;
	conv_ftl->chain_slc_rr_pages = NULL;
	conv_ftl->chain_qlc_rr_pages = NULL;
	conv_ftl->chain_slc_page_count = NULL;
	conv_ftl->chain_last_slc_touch = NULL;
	conv_ftl->chain_alloc_events = NULL;
	conv_ftl->chain_cold_events = NULL;
	conv_ftl->gc_victim_events = NULL;
	conv_ftl->chain_host_slc_wps = NULL;
	conv_ftl->blk_owner_chain = NULL;
	conv_ftl->blk_owner_pages = NULL;
	conv_ftl->blk_valid_pages = NULL;
	conv_ftl->blk_mixed_pages = NULL;
	conv_ftl->blk_active_wp_refs = NULL;
	conv_ftl->slc_sb_state = NULL;
	conv_ftl->slc_sb_active_counted = NULL;
	conv_ftl->slc_sb_owner_chain = NULL;
	conv_ftl->slc_sb_die_full_mask = NULL;
	conv_ftl->slc_sb_migrated_victim = NULL;
	conv_ftl->slc_sb_recent_guard = NULL;
	conv_ftl->slc_sb_generation = NULL;
	conv_ftl->qlc_sb_state = NULL;
	conv_ftl->qlc_sb_active_counted = NULL;
	conv_ftl->qlc_sb_owner_chain = NULL;
	conv_ftl->qlc_sb_die_closed_mask = NULL;
	conv_ftl->slc_recent_guard_ring_blk = NULL;
	conv_ftl->slc_recent_guard_ring_gen = NULL;
	conv_ftl->chain_cur_active_sb = NULL;
	conv_ftl->chain_cur_active_qlc_sb = NULL;
	conv_ftl->chain_host_qlc_wps = NULL;
	conv_ftl->chain_capacity = 0;
	conv_ftl->next_chain_id = 0;
	conv_ftl->slc_sb_migrated_victim_count = 0;
	conv_ftl->slc_recent_guard_ring_size = 0;
	conv_ftl->slc_recent_guard_ring_head = 0;
	conv_ftl->qlc_active_sb_count = 0;
}

static void init_rmap(struct conv_ftl *conv_ftl)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
    int i;

	conv_ftl->rmap = vmalloc(sizeof(uint64_t) * spp->tt_pgs);
	if (!conv_ftl->rmap) {
		NVMEV_ERROR("Failed to allocate reverse mapping table memory\n");
        conv_ftl->rmap_initialized = false;
		return;
	}

    for (i = 0; i < spp->tt_pgs; i++) {
		conv_ftl->rmap[i] = INVALID_LPN;
	}
    conv_ftl->rmap_initialized = true;
}

static void remove_rmap(struct conv_ftl *conv_ftl)
{
	vfree(conv_ftl->rmap);
}

/* forward declaration to satisfy C90 before first use */
static int init_slc_qlc_blocks_fallback(struct conv_ftl *conv_ftl);
/* forward declare to avoid implicit declaration before first use */
static bool is_slc_block(struct conv_ftl *conv_ftl, uint32_t blk_id);

static int init_slc_qlc_blocks_with_retry(struct conv_ftl *conv_ftl, int max_retries)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint32_t total_blks_per_pl = spp->blks_per_pl;
	int i, retry_count = 0;
	
	/* 重试分配内存 */
	while (retry_count < max_retries) {
		conv_ftl->is_slc_block = vmalloc(sizeof(bool) * total_blks_per_pl);
			if (conv_ftl->is_slc_block) {
				uint32_t slc_lines, qlc_lines;

				/* 分配成功，按 line 比例初始化标记 */
				compute_line_distribution(total_blks_per_pl, &slc_lines, &qlc_lines);
				conv_ftl->slc_blks_per_pl = slc_lines;
				conv_ftl->qlc_blks_per_pl = qlc_lines;
				
				for (i = 0; i < total_blks_per_pl; i++) {
					if (i < slc_lines) {
						conv_ftl->is_slc_block[i] = true;  /* SLC line */
					} else {
						conv_ftl->is_slc_block[i] = false; /* QLC line */
					}
				}
				
			conv_ftl->slc_initialized = true;
			NVMEV_INFO("SLC blocks: %d, QLC blocks: %d\n",
				   conv_ftl->slc_blks_per_pl, conv_ftl->qlc_blks_per_pl);
			return 0;
		}
		
		/* 分配失败，等待后重试 */
		NVMEV_ERROR("Failed to allocate SLC block marker memory, retry %d/%d\n", 
			   retry_count + 1, max_retries);
		msleep(100);  /* 等待100ms */
		retry_count++;
	}
	
	/* 所有重试都失败，尝试降级处理 */
	NVMEV_ERROR("All retries failed, trying fallback allocation\n");
	return init_slc_qlc_blocks_fallback(conv_ftl);
}

static int init_slc_qlc_blocks_fallback(struct conv_ftl *conv_ftl)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint32_t total_blks_per_pl = spp->blks_per_pl;
	uint32_t reduced_size;
	int i;
	
	/* 降级：使用较小的分配 */
	reduced_size = total_blks_per_pl / 2;
	conv_ftl->is_slc_block = vmalloc(sizeof(bool) * reduced_size);
	if (!conv_ftl->is_slc_block) {
		NVMEV_ERROR("Fallback allocation also failed\n");
		return -ENOMEM;
	}
	
	/* 调整配置以适应较小的分配（仍按 line 比例拆分） */
	compute_line_distribution(reduced_size, &conv_ftl->slc_blks_per_pl,
				  &conv_ftl->qlc_blks_per_pl);
	
	/* 初始化标记 */
	for (i = 0; i < reduced_size; i++) {
		if (i < conv_ftl->slc_blks_per_pl) {
			conv_ftl->is_slc_block[i] = true;
		} else {
			conv_ftl->is_slc_block[i] = false;
		}
	}
	
	conv_ftl->slc_initialized = true;
	NVMEV_INFO("Using reduced allocation: %d blocks (SLC: %d, QLC: %d)\n", 
		   reduced_size, conv_ftl->slc_blks_per_pl, conv_ftl->qlc_blks_per_pl);
	return 0;
}

/* 保持原有函数名兼容性 */
static void init_slc_qlc_blocks(struct conv_ftl *conv_ftl)
{
	int ret = init_slc_qlc_blocks_with_retry(conv_ftl, 3);  /* 重试3次 */
	if (ret != 0) {
		NVMEV_ERROR("SLC/QLC blocks initialization failed: %d\n", ret);
		/* 设置初始化失败标志 */
		conv_ftl->slc_initialized = false;
	}
}

static int init_heat_tracking_with_retry(struct conv_ftl *conv_ftl, int max_retries)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct heat_tracking *ht = &conv_ftl->heat_track;
	int i, retry_count = 0;
	
	/* 重试分配 access_count */
	while (retry_count < max_retries) {
		ht->access_count = vmalloc(sizeof(uint64_t) * spp->tt_pgs);
		if (ht->access_count) {
			break;
		}
		NVMEV_ERROR("Failed to allocate access count memory, retry %d/%d\n", 
			   retry_count + 1, max_retries);
		msleep(50);
		retry_count++;
	}
	
	if (!ht->access_count) {
		NVMEV_ERROR("Failed to allocate access count memory after %d retries\n", max_retries);
		return -ENOMEM;
	}
	
	/* 重试分配 last_access_time */
	retry_count = 0;
	while (retry_count < max_retries) {
		ht->last_access_time = vmalloc(sizeof(uint64_t) * spp->tt_pgs);
		if (ht->last_access_time) {
			break;
		}
		NVMEV_ERROR("Failed to allocate last access time memory, retry %d/%d\n", 
			   retry_count + 1, max_retries);
		msleep(50);
		retry_count++;
	}
	
	if (!ht->last_access_time) {
		NVMEV_ERROR("Failed to allocate last access time memory after %d retries\n", max_retries);
		vfree(ht->access_count);
		ht->access_count = NULL;
		return -ENOMEM;
	}
	
	/* 重试分配 write_epoch */
	retry_count = 0;
	while (retry_count < max_retries) {
		ht->write_epoch = vmalloc(sizeof(uint64_t) * spp->tt_pgs);
		if (ht->write_epoch)
			break;
		NVMEV_ERROR("Failed to allocate write epoch memory, retry %d/%d\n",
			   retry_count + 1, max_retries);
		msleep(50);
		retry_count++;
	}
	
	if (!ht->write_epoch) {
		NVMEV_ERROR("Failed to allocate write epoch memory after %d retries\n", max_retries);
		vfree(ht->access_count);
		vfree(ht->last_access_time);
		ht->access_count = NULL;
		ht->last_access_time = NULL;
		return -ENOMEM;
	}
	
	/* 重试分配 page_in_slc */
	retry_count = 0;
	while (retry_count < max_retries) {
		conv_ftl->page_in_slc = vmalloc(sizeof(bool) * spp->tt_pgs);
		if (conv_ftl->page_in_slc) {
			break;
		}
		NVMEV_ERROR("Failed to allocate page in SLC marker memory, retry %d/%d\n", 
			   retry_count + 1, max_retries);
		msleep(50);
		retry_count++;
	}
	
	if (!conv_ftl->page_in_slc) {
		NVMEV_ERROR("Failed to allocate page in SLC marker memory after %d retries\n", max_retries);
		vfree(ht->access_count);
		vfree(ht->last_access_time);
		vfree(ht->write_epoch);
		ht->access_count = NULL;
		ht->last_access_time = NULL;
		ht->write_epoch = NULL;
		return -ENOMEM;
	}

	conv_ftl->slc_resident_capacity_per_die =
		conv_ftl->slc_blks_per_pl * conv_ftl->slc_pgs_per_blk;
	if (conv_ftl->slc_resident_capacity_per_die) {
		conv_ftl->slc_resident_lpns =
			vmalloc(sizeof(uint64_t) * total_slc_pages(conv_ftl));
		conv_ftl->slc_resident_slot =
			vmalloc(sizeof(uint32_t) * spp->tt_pgs);
		conv_ftl->slc_die_resident_count =
			kcalloc(conv_ftl->die_count, sizeof(uint32_t), GFP_KERNEL);
		conv_ftl->slc_die_resident_cursor =
			kcalloc(conv_ftl->die_count, sizeof(uint32_t), GFP_KERNEL);
		if (!conv_ftl->slc_resident_lpns || !conv_ftl->slc_resident_slot ||
		    !conv_ftl->slc_die_resident_count || !conv_ftl->slc_die_resident_cursor) {
			NVMEV_ERROR("Failed to allocate SLC resident candidate tracking\n");
			vfree(ht->access_count);
			vfree(ht->last_access_time);
			vfree(ht->write_epoch);
			vfree(conv_ftl->page_in_slc);
			vfree(conv_ftl->slc_resident_lpns);
			vfree(conv_ftl->slc_resident_slot);
			kfree(conv_ftl->slc_die_resident_count);
			kfree(conv_ftl->slc_die_resident_cursor);
			ht->access_count = NULL;
			ht->last_access_time = NULL;
			ht->write_epoch = NULL;
			conv_ftl->page_in_slc = NULL;
			conv_ftl->slc_resident_lpns = NULL;
			conv_ftl->slc_resident_slot = NULL;
			conv_ftl->slc_die_resident_count = NULL;
			conv_ftl->slc_die_resident_cursor = NULL;
			conv_ftl->slc_resident_capacity_per_die = 0;
			return -ENOMEM;
		}
	}
	
	/* 初始化所有数组 */
	for (i = 0; i < spp->tt_pgs; i++) {
		ht->access_count[i] = 0;
		ht->last_access_time[i] = 0;
		ht->write_epoch[i] = 0;
		conv_ftl->page_in_slc[i] = false;
		if (conv_ftl->slc_resident_slot)
			conv_ftl->slc_resident_slot[i] = U32_MAX;
	}
	if (conv_ftl->slc_die_resident_count)
		memset(conv_ftl->slc_die_resident_count, 0,
		       sizeof(uint32_t) * conv_ftl->die_count);
	if (conv_ftl->slc_die_resident_cursor)
		memset(conv_ftl->slc_die_resident_cursor, 0,
		       sizeof(uint32_t) * conv_ftl->die_count);
	
	ht->migration_threshold = MIGRATION_THRESHOLD;
	INIT_LIST_HEAD(&conv_ftl->migration.migration_queue);
	conv_ftl->migration.pending_migrations = 0;
	
	conv_ftl->heat_track_initialized = true;
	conv_ftl->total_host_writes = 0;
	return 0;
}

/* 保持原有函数名兼容性 */
static void init_heat_tracking(struct conv_ftl *conv_ftl)
{
	int ret = init_heat_tracking_with_retry(conv_ftl, 3);  /* 重试3次 */
	if (ret != 0) {
		NVMEV_ERROR("Heat tracking initialization failed: %d\n", ret);
		conv_ftl->heat_track_initialized = false;
	}
}

/* 初始化迁移管理 */
static void init_migration_mgmt(struct conv_ftl *conv_ftl)
{
	INIT_LIST_HEAD(&conv_ftl->migration.migration_queue);
	conv_ftl->migration.pending_migrations = 0;
	conv_ftl->migration.migration_in_progress = false;
	
	/* 初始化统计计数器 */
    conv_ftl->slc_write_cnt = 0;
    conv_ftl->qlc_write_cnt = 0;
    conv_ftl->migration_cnt = 0;
    conv_ftl->total_host_writes = 0;

    /* 初始化账本并发保护锁 */
    spin_lock_init(&conv_ftl->slc_lock);
    spin_lock_init(&conv_ftl->qlc_lock);
}
/* 初始化 SLC line 管理 */
static void cleanup_line_pool(struct line_mgmt *lm)
{
	if (!lm)
		return;

	if (lm->victim_line_pq) {
		pqueue_free(lm->victim_line_pq);
		lm->victim_line_pq = NULL;
	}

	if (lm->lines) {
		vfree(lm->lines);
		lm->lines = NULL;
	}

	INIT_LIST_HEAD(&lm->free_line_list);
	INIT_LIST_HEAD(&lm->full_line_list);
	lm->tt_lines = 0;
	lm->free_line_cnt = 0;
	lm->victim_line_cnt = 0;
	lm->full_line_cnt = 0;
}

static int init_line_pool(struct conv_ftl *conv_ftl, struct line_mgmt *lm,
			  uint32_t line_base, uint32_t line_cnt, const char *tag,
			  uint32_t die)
{
	uint32_t i;

	lm->tt_lines = line_cnt;
	lm->lines = vmalloc(sizeof(struct line) * lm->tt_lines);
	if (!lm->lines) {
		NVMEV_ERROR("Failed to allocate %s die=%u line array (cnt=%u)\n", tag, die,
			    line_cnt);
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&lm->free_line_list);
	INIT_LIST_HEAD(&lm->full_line_list);

	lm->victim_line_pq = pqueue_init(lm->tt_lines, victim_line_cmp_pri, victim_line_get_pri,
					 victim_line_set_pri, victim_line_get_pos,
					 victim_line_set_pos);
	if (!lm->victim_line_pq) {
		NVMEV_ERROR("Failed to init %s die=%u victim PQ\n", tag, die);
		vfree(lm->lines);
		lm->lines = NULL;
		return -ENOMEM;
	}

	lm->free_line_cnt = 0;
	for (i = 0; i < lm->tt_lines; i++) {
		lm->lines[i] = (struct line){
			.id = line_base + i,
			.ipc = 0,
			.vpc = 0,
			.entry = LIST_HEAD_INIT(lm->lines[i].entry),
			.pos = 0,
			.zone_written = { 0 },
		};
		list_add_tail(&lm->lines[i].entry, &lm->free_line_list);
		lm->free_line_cnt++;
	}

	lm->victim_line_cnt = 0;
	lm->full_line_cnt = 0;
	NVMEV_DEBUG("[LINE_POOL] %s die=%u initialized: base=%u cnt=%u\n", tag, die,
		    line_base, line_cnt);
	return 0;
}

static int init_per_die_line_mgmt(struct conv_ftl *conv_ftl, bool slc_pool)
{
	uint32_t die, die_cnt = conv_ftl->die_count ? conv_ftl->die_count : 1;
	struct line_mgmt **array = slc_pool ? &conv_ftl->slc_lunlm : &conv_ftl->qlc_lunlm;
	uint32_t line_base = slc_pool ? 0 : conv_ftl->slc_blks_per_pl;
	uint32_t line_cnt = slc_pool ? conv_ftl->slc_blks_per_pl : conv_ftl->qlc_blks_per_pl;
	const char *tag = slc_pool ? "SLC" : "QLC";

	if (!line_cnt) {
		*array = NULL;
		return 0;
	}

	*array = kcalloc(die_cnt, sizeof(**array), GFP_KERNEL);
	if (!*array) {
		NVMEV_ERROR("Failed to allocate %s per-die line_mgmt array\n", tag);
		return -ENOMEM;
	}

	for (die = 0; die < die_cnt; die++) {
		if (init_line_pool(conv_ftl, &(*array)[die], line_base, line_cnt, tag, die) !=
		    0) {
			while (die--)
				cleanup_line_pool(&(*array)[die]);
			kfree(*array);
			*array = NULL;
			return -ENOMEM;
		}
	}

	return 0;
}

static int init_qlc_lines(struct conv_ftl *conv_ftl)
{
	return init_per_die_line_mgmt(conv_ftl, false);
}

static int init_qlc_closed_repromote_queue(struct conv_ftl *conv_ftl)
{
	uint32_t size;

	if (!conv_ftl)
		return -EINVAL;

	size = (conv_ftl->die_count ? conv_ftl->die_count : 1) *
		(conv_ftl->qlc_blks_per_pl ? conv_ftl->qlc_blks_per_pl : 1);
	if (size < QLC_CLOSED_REPROMOTE_TRIGGER)
		size = QLC_CLOSED_REPROMOTE_TRIGGER;

	conv_ftl->qlc_closed_repromote_blk =
		vmalloc(sizeof(*conv_ftl->qlc_closed_repromote_blk) * size);
	conv_ftl->qlc_closed_sb_die_mask =
		vzalloc(sizeof(*conv_ftl->qlc_closed_sb_die_mask) *
			conv_ftl->qlc_blks_per_pl);
	conv_ftl->qlc_closed_repromote_queued =
		vzalloc(sizeof(*conv_ftl->qlc_closed_repromote_queued) *
			conv_ftl->qlc_blks_per_pl);
	if (!conv_ftl->qlc_closed_repromote_blk ||
	    !conv_ftl->qlc_closed_sb_die_mask ||
	    !conv_ftl->qlc_closed_repromote_queued) {
		vfree(conv_ftl->qlc_closed_repromote_blk);
		vfree(conv_ftl->qlc_closed_sb_die_mask);
		vfree(conv_ftl->qlc_closed_repromote_queued);
		conv_ftl->qlc_closed_repromote_blk = NULL;
		conv_ftl->qlc_closed_sb_die_mask = NULL;
		conv_ftl->qlc_closed_repromote_queued = NULL;
		conv_ftl->qlc_closed_repromote_size = 0;
		return -ENOMEM;
	}

	conv_ftl->qlc_closed_repromote_head = 0;
	conv_ftl->qlc_closed_repromote_tail = 0;
	conv_ftl->qlc_closed_repromote_count = 0;
	conv_ftl->qlc_closed_repromote_size = size;
	conv_ftl->qlc_closed_repromote_since_scan = 0;
	conv_ftl->qlc_closed_repromote_enqueues = 0;
	conv_ftl->qlc_closed_repromote_dequeues = 0;
	conv_ftl->qlc_closed_repromote_drops = 0;
	conv_ftl->qlc_closed_repromote_scans = 0;
	conv_ftl->qlc_closed_repromote_pages = 0;
	return 0;
}

static void remove_qlc_closed_repromote_queue(struct conv_ftl *conv_ftl)
{
	if (!conv_ftl)
		return;
	vfree(conv_ftl->qlc_closed_repromote_blk);
	vfree(conv_ftl->qlc_closed_sb_die_mask);
	vfree(conv_ftl->qlc_closed_repromote_queued);
	conv_ftl->qlc_closed_repromote_blk = NULL;
	conv_ftl->qlc_closed_sb_die_mask = NULL;
	conv_ftl->qlc_closed_repromote_queued = NULL;
	conv_ftl->qlc_closed_repromote_head = 0;
	conv_ftl->qlc_closed_repromote_tail = 0;
	conv_ftl->qlc_closed_repromote_count = 0;
	conv_ftl->qlc_closed_repromote_size = 0;
	conv_ftl->qlc_closed_repromote_since_scan = 0;
}

/* 清理函数 */
static void remove_slc_qlc_blocks(struct conv_ftl *conv_ftl)
{
	vfree(conv_ftl->is_slc_block);
}

static void remove_heat_tracking(struct conv_ftl *conv_ftl)
{
	vfree(conv_ftl->heat_track.access_count);
	vfree(conv_ftl->heat_track.last_access_time);
	vfree(conv_ftl->heat_track.write_epoch);
	vfree(conv_ftl->page_in_slc);
	vfree(conv_ftl->slc_resident_lpns);
	vfree(conv_ftl->slc_resident_slot);
	kfree(conv_ftl->slc_die_resident_count);
	kfree(conv_ftl->slc_die_resident_cursor);
	conv_ftl->heat_track.access_count = NULL;
	conv_ftl->heat_track.last_access_time = NULL;
	conv_ftl->heat_track.write_epoch = NULL;
	conv_ftl->page_in_slc = NULL;
	conv_ftl->slc_resident_lpns = NULL;
	conv_ftl->slc_resident_slot = NULL;
	conv_ftl->slc_die_resident_count = NULL;
	conv_ftl->slc_die_resident_cursor = NULL;
	conv_ftl->slc_resident_capacity_per_die = 0;
}

static void destroy_per_die_lines(struct line_mgmt **lms, uint32_t die_cnt)
{
	uint32_t die;

	if (!lms || !*lms)
		return;

	for (die = 0; die < die_cnt; die++)
		cleanup_line_pool(&(*lms)[die]);

	kfree(*lms);
	*lms = NULL;
}

static void remove_slc_lines(struct conv_ftl *conv_ftl)
{
	destroy_per_die_lines(&conv_ftl->slc_lunlm, conv_ftl->die_count);
}

static void remove_qlc_lines(struct conv_ftl *conv_ftl)
{
	destroy_per_die_lines(&conv_ftl->qlc_lunlm, conv_ftl->die_count);
}

static void conv_init_ftl(struct conv_ftl *conv_ftl, struct convparams *cpp, struct ssd *ssd)
{
	/*copy convparams*/
	conv_ftl->cp = *cpp;

	conv_ftl->ssd = ssd;
	seqlock_init(&conv_ftl->maptbl_lock);
	conv_ftl->slc_pgs_per_blk = ssd->sp.pgs_per_blk;
	conv_ftl->qlc_pgs_per_blk = conv_ftl->slc_pgs_per_blk * QLC_BLOCK_CAPACITY_FACTOR;
	conv_ftl->debug_access_count = NULL;
	conv_ftl->debug_access_inject = NULL;
	conv_ftl->debug_page_tier = NULL;
	conv_ftl->debug_page_die = NULL;
	conv_ftl->debug_page_chain = NULL;
	conv_ftl->debug_die_affinity_stats = NULL;
	conv_ftl->debug_lpn_die_change_stats = NULL;
	conv_ftl->debug_test_phase = NULL;
	conv_ftl->debug_test_phase_stats = NULL;
	conv_ftl->debug_superblock_stats = NULL;

	conv_ftl->die_count = total_dies(&ssd->sp);
	if (!conv_ftl->die_count)
		conv_ftl->die_count = 1;

	conv_ftl->slc_lunwp = kcalloc(conv_ftl->die_count,
				      sizeof(*conv_ftl->slc_lunwp), GFP_KERNEL);
	conv_ftl->gc_slc_lunwp = kcalloc(conv_ftl->die_count,
					 sizeof(*conv_ftl->gc_slc_lunwp), GFP_KERNEL);
	conv_ftl->qlc_lunwp = kcalloc(conv_ftl->die_count,
				      sizeof(*conv_ftl->qlc_lunwp), GFP_KERNEL);
	conv_ftl->gc_qlc_lunwp = kcalloc(conv_ftl->die_count,
					 sizeof(*conv_ftl->gc_qlc_lunwp), GFP_KERNEL);
	if (!conv_ftl->slc_lunwp || !conv_ftl->gc_slc_lunwp) {
		NVMEV_ERROR("Failed to allocate per-die SLC write pointer arrays\n");
		kfree(conv_ftl->slc_lunwp);
		kfree(conv_ftl->gc_slc_lunwp);
		conv_ftl->slc_lunwp = NULL;
		conv_ftl->gc_slc_lunwp = NULL;
		kfree(conv_ftl->qlc_lunwp);
		kfree(conv_ftl->gc_qlc_lunwp);
		conv_ftl->qlc_lunwp = NULL;
		conv_ftl->gc_qlc_lunwp = NULL;
		return;
	}
	if (!conv_ftl->qlc_lunwp || !conv_ftl->gc_qlc_lunwp) {
		NVMEV_ERROR("Failed to allocate per-die QLC write pointer arrays\n");
		kfree(conv_ftl->slc_lunwp);
		kfree(conv_ftl->gc_slc_lunwp);
		kfree(conv_ftl->qlc_lunwp);
		kfree(conv_ftl->gc_qlc_lunwp);
		conv_ftl->slc_lunwp = NULL;
		conv_ftl->gc_slc_lunwp = NULL;
		conv_ftl->qlc_lunwp = NULL;
		conv_ftl->gc_qlc_lunwp = NULL;
		return;
	}

	/* initialize SLC/QLC line distribution before metadata arrays that
	 * are sized by slc_blks_per_pl / qlc_blks_per_pl. */
	NVMEV_INFO("initialize SLC/QLC blocks\n");
	init_slc_qlc_blocks(conv_ftl);

	/* initialize maptbl */
	NVMEV_INFO("initialize maptbl\n");
	init_maptbl(conv_ftl); // mapping table

	/* initialize rmap */
	NVMEV_INFO("initialize rmap\n");
	init_rmap(conv_ftl); // reverse mapping table (?)

	/* 删除旧的 init_lines 调用 - 使用新的 SLC/QLC 系统 */
	
	if (init_per_die_line_mgmt(conv_ftl, true) != 0) {
		NVMEV_ERROR("Failed to initialize per-die SLC line managers\n");
		return;
	}
	
	if (init_qlc_lines(conv_ftl) != 0) {
		NVMEV_ERROR("Failed to initialize per-die QLC line managers\n");
		return;
	}

	if (init_qlc_closed_repromote_queue(conv_ftl) != 0) {
		NVMEV_ERROR("Failed to initialize closed-QLC repromotion queue\n");
		return;
	}
	
	NVMEV_INFO("initialize heat tracking\n");
	init_heat_tracking(conv_ftl);
	
	NVMEV_INFO("initialize migration management\n");
	init_migration_mgmt(conv_ftl);

	/* initialize write pointer, this is how we allocate new pages for writes */
	NVMEV_INFO("initialize write pointer\n");
	/* DEPRECATED: prepare_write_pointer calls removed - using dynamic SLC/QLC allocation */
	/* SLC/QLC write pointers will be initialized on first write operation */

    /* SLC Die-Affinity uses per-instance lunpointer */
    conv_ftl->lunpointer = 0;

	/* 初始化 SLC 写指针 - 使用 Die Affinity */
	/* 注意：SLC 写指针将在第一次写入时初始化 */

	init_write_flow_control(conv_ftl);
	memset(&conv_ftl->qlc_wp, 0, sizeof(conv_ftl->qlc_wp));
	memset(&conv_ftl->qlc_gc_wp, 0, sizeof(conv_ftl->qlc_gc_wp));

	conv_ftl->qlc_page_wcnt = vzalloc(sizeof(uint64_t) * conv_ftl->ssd->sp.tt_pgs);
	if (!conv_ftl->qlc_page_wcnt)
		NVMEV_ERROR("Failed to allocate QLC page write counters\n");
	conv_ftl->qlc_total_wcnt = 0;
	conv_ftl->qlc_unique_pages = 0;
	conv_ftl->qlc_threshold_q1_q2 = 0;
	conv_ftl->qlc_threshold_q2_q3 = 0;
	conv_ftl->qlc_threshold_q3_q4 = 0;
	conv_ftl->qlc_resident_read_sum = 0;
	conv_ftl->qlc_resident_page_cnt = 0;
	conv_ftl->qlc_migration_read_sum = 0;
	conv_ftl->qlc_migration_page_cnt = 0;
	conv_ftl->global_read_sum = 0;
	conv_ftl->global_valid_pg_cnt = 0;
	conv_ftl->qlc_zone_rr_cursor = 0;
	conv_ftl->migration_read_path_count = 0;
	conv_ftl->migration_read_path_time_ns = 0;
	conv_ftl->die_aff_append_requests = 0;
	conv_ftl->die_aff_append_effective = 0;
	conv_ftl->die_aff_overwrite_requests = 0;
	conv_ftl->die_aff_overwrite_effective = 0;
	conv_ftl->test_phase_active = false;
	test_phase_reset_stats(conv_ftl);
	conv_ftl->bg_slc_rr_die = 0;
	conv_ftl->bg_slc_rr_pages = 0;
	conv_ftl->bg_qlc_rr_die = 0;
	conv_ftl->bg_qlc_rr_pages = 0;
	conv_ftl->qlc_promote_cursor = 0;
	conv_ftl->qlc_demote_cursor = 0;
	conv_ftl->qlc_promote_die_cursor = 0;
	conv_ftl->qlc_demote_die_cursor = 0;
	conv_ftl->qlc_rebalance_period_writes = 2048;
	conv_ftl->qlc_rebalance_promote_budget = 32;
	conv_ftl->qlc_rebalance_demote_budget = 16;
	conv_ftl->qlc_fast_drain_active = false;
		conv_ftl->qlc_fast_count = 0;
		conv_ftl->qlc_slow_count = 0;
		conv_ftl->enable_read_repromotion = !!NVMEV_ENABLE_READ_REPROMOTION;
		conv_ftl->repromote_period_reads = 10000;
		conv_ftl->repromote_budget_per_run = 256;
	spin_lock_init(&conv_ftl->qlc_zone_lock);

	/* 后台迁移 workqueue 初始化 */
	conv_ftl->bg_migration_wq = alloc_workqueue("nvmev_bg_mig",
						     WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	INIT_WORK(&conv_ftl->repromotion_work, bg_repromotion_worker);
	INIT_WORK(&conv_ftl->qlc_rebalance_work, bg_qlc_rebalance_worker);
	atomic64_set(&conv_ftl->total_host_reads, 0);
	spin_lock_init(&conv_ftl->repromote_queue_lock);
	conv_ftl->repromote_head = 0;
	conv_ftl->repromote_tail = 0;
	conv_ftl->repromote_die_cursor = 0;

	/* 直接初始化水位线（无后台线程） */
	{
		uint64_t slc_total_pages = total_slc_pages(conv_ftl);
		uint64_t qlc_total_pages = total_qlc_pages(conv_ftl);
		uint32_t slc_sb_pages = slc_pages_per_superblock(conv_ftl);
		uint32_t qlc_sb_pages = qlc_pages_per_superblock(conv_ftl);
		uint64_t tmp;

		tmp = div_u64(slc_total_pages * SLC_MIGRATE_FREE_PCT, 100);
		conv_ftl->slc_high_watermark =
			pages_to_lines_ceil(tmp, slc_sb_pages);

		tmp = div_u64(slc_total_pages * SLC_MIGRATE_TARGET_FREE_PCT, 100);
		conv_ftl->slc_target_watermark =
			pages_to_lines_ceil(tmp, slc_sb_pages);

		tmp = div_u64(slc_total_pages * SLC_SOFT_GC_FREE_PCT, 100);
		conv_ftl->slc_gc_free_thres_high =
			pages_to_lines_ceil(tmp, slc_sb_pages);

		tmp = div_u64(slc_total_pages * SLC_HARD_GC_FREE_PCT, 100);
		conv_ftl->slc_gc_free_thres_low =
			pages_to_lines_ceil(tmp, slc_sb_pages);

		tmp = div_u64(qlc_total_pages * QLC_GC_FREE_PCT, 100);
		conv_ftl->qlc_gc_free_thres_high =
			pages_to_lines_ceil(tmp, qlc_sb_pages);

		tmp = div_u64(slc_total_pages * SLC_REPROMOTE_GUARD_FREE_PCT, 100);
		conv_ftl->slc_repromote_guard_lines =
			pages_to_lines_ceil(tmp, slc_sb_pages);
	}

	NVMEV_INFO("Init FTL Instance with %d channels(%ld pages)\n", conv_ftl->ssd->sp.nchs,
		   conv_ftl->ssd->sp.tt_pgs);
	NVMEV_INFO("SLC/QLC Hybrid Mode: SLC %d blks, QLC %d blks, QLC zones per line=%u\n", 
		   conv_ftl->slc_blks_per_pl, conv_ftl->qlc_blks_per_pl, QLC_ZONE_COUNT);
	NVMEV_INFO("Superblock accounting: dies=%u SLC pages/sb=%u QLC pages/sb=%u active_limit=%u\n",
		   superblock_die_count(conv_ftl), slc_pages_per_superblock(conv_ftl),
		   qlc_pages_per_superblock(conv_ftl), NVMEV_SUPERBLOCK_ACTIVE_LIMIT);
	NVMEV_INFO("Per-block pages: SLC=%u, QLC=%u (pattern=%u)\n",
		   conv_ftl->slc_pgs_per_blk, conv_ftl->qlc_pgs_per_blk, QLC_PAGE_PATTERN);

	nvmev_debugfs_init_instance(conv_ftl);
	{
		struct dentry *parent = conv_ftl->debug_dir ? conv_ftl->debug_dir :
							   nvmev_debugfs_root();

		conv_ftl->debug_access_count =
			debugfs_create_file("access_count", 0440, parent,
					    conv_ftl, &access_count_fops);
		conv_ftl->debug_access_inject =
			debugfs_create_file("access_inject", 0200, parent,
					    conv_ftl, &access_inject_fops);
			conv_ftl->debug_page_tier =
				debugfs_create_file("page_tier", 0440, parent,
						    conv_ftl, &page_tier_fops);
			conv_ftl->debug_page_die =
				debugfs_create_file("page_die", 0440, parent,
						    conv_ftl, &page_die_fops);
			conv_ftl->debug_page_chain =
				debugfs_create_file("page_chain", 0440, parent,
						    conv_ftl, &page_chain_fops);
			conv_ftl->debug_die_affinity_stats =
				debugfs_create_file("die_affinity_stats", 0440, parent,
						    conv_ftl, &die_affinity_stats_fops);
		conv_ftl->debug_lpn_die_change_stats =
			debugfs_create_file("lpn_die_change_stats", 0440, parent,
					    conv_ftl, &lpn_die_change_stats_fops);
		conv_ftl->debug_test_phase =
			debugfs_create_file("test_phase", 0640, parent,
					    conv_ftl, &test_phase_fops);
		conv_ftl->debug_test_phase_stats =
			debugfs_create_file("test_phase_stats", 0440, parent,
					    conv_ftl, &test_phase_stats_fops);
		conv_ftl->debug_superblock_stats =
			debugfs_create_file("superblock_stats", 0440, parent,
					    conv_ftl, &superblock_stats_fops);
		conv_ftl->debug_chain_alloc_events =
			debugfs_create_file("chain_alloc_events", 0440, parent,
					    conv_ftl, &chain_alloc_events_fops);
		conv_ftl->debug_chain_cold_events =
			debugfs_create_file("chain_cold_events", 0440, parent,
					    conv_ftl, &chain_cold_events_fops);
		conv_ftl->debug_gc_victim_events =
			debugfs_create_file("gc_victim_events", 0440, parent,
					    conv_ftl, &gc_victim_events_fops);
		conv_ftl->debug_read_repromotion = NULL;
	}
	superblock_stats_log_summary(conv_ftl, "init");

	return;
}

static void conv_remove_ftl(struct conv_ftl *conv_ftl)
{
	if (conv_ftl->bg_migration_wq) {
		flush_workqueue(conv_ftl->bg_migration_wq);
		destroy_workqueue(conv_ftl->bg_migration_wq);
		conv_ftl->bg_migration_wq = NULL;
	}

	if (conv_ftl->debug_dir) {
		debugfs_remove_recursive(conv_ftl->debug_dir);
		conv_ftl->debug_dir = NULL;
		conv_ftl->debug_access_count = NULL;
			conv_ftl->debug_access_inject = NULL;
			conv_ftl->debug_page_tier = NULL;
			conv_ftl->debug_page_die = NULL;
			conv_ftl->debug_page_chain = NULL;
			conv_ftl->debug_die_affinity_stats = NULL;
		conv_ftl->debug_lpn_die_change_stats = NULL;
		conv_ftl->debug_test_phase = NULL;
		conv_ftl->debug_test_phase_stats = NULL;
		conv_ftl->debug_read_repromotion = NULL;
		conv_ftl->debug_superblock_stats = NULL;
	} else {
		if (conv_ftl->debug_access_count) {
			debugfs_remove(conv_ftl->debug_access_count);
			conv_ftl->debug_access_count = NULL;
		}
		if (conv_ftl->debug_access_inject) {
			debugfs_remove(conv_ftl->debug_access_inject);
			conv_ftl->debug_access_inject = NULL;
		}
		if (conv_ftl->debug_page_tier) {
			debugfs_remove(conv_ftl->debug_page_tier);
			conv_ftl->debug_page_tier = NULL;
		}
			if (conv_ftl->debug_page_die) {
				debugfs_remove(conv_ftl->debug_page_die);
				conv_ftl->debug_page_die = NULL;
			}
			if (conv_ftl->debug_page_chain) {
				debugfs_remove(conv_ftl->debug_page_chain);
				conv_ftl->debug_page_chain = NULL;
			}
			if (conv_ftl->debug_die_affinity_stats) {
			debugfs_remove(conv_ftl->debug_die_affinity_stats);
			conv_ftl->debug_die_affinity_stats = NULL;
		}
		if (conv_ftl->debug_lpn_die_change_stats) {
			debugfs_remove(conv_ftl->debug_lpn_die_change_stats);
			conv_ftl->debug_lpn_die_change_stats = NULL;
		}
		if (conv_ftl->debug_test_phase) {
			debugfs_remove(conv_ftl->debug_test_phase);
			conv_ftl->debug_test_phase = NULL;
		}
		if (conv_ftl->debug_test_phase_stats) {
			debugfs_remove(conv_ftl->debug_test_phase_stats);
			conv_ftl->debug_test_phase_stats = NULL;
		}
		if (conv_ftl->debug_superblock_stats) {
			debugfs_remove(conv_ftl->debug_superblock_stats);
			conv_ftl->debug_superblock_stats = NULL;
		}
		if (conv_ftl->debug_chain_alloc_events) {
			debugfs_remove(conv_ftl->debug_chain_alloc_events);
			conv_ftl->debug_chain_alloc_events = NULL;
		}
		if (conv_ftl->debug_chain_cold_events) {
			debugfs_remove(conv_ftl->debug_chain_cold_events);
			conv_ftl->debug_chain_cold_events = NULL;
		}
		if (conv_ftl->debug_gc_victim_events) {
			debugfs_remove(conv_ftl->debug_gc_victim_events);
			conv_ftl->debug_gc_victim_events = NULL;
		}
		if (conv_ftl->debug_read_repromotion) {
			debugfs_remove(conv_ftl->debug_read_repromotion);
			conv_ftl->debug_read_repromotion = NULL;
		}
	}
	
	/* 清理 SLC/QLC 相关资源 */
	remove_qlc_closed_repromote_queue(conv_ftl);
	remove_slc_lines(conv_ftl);
	remove_qlc_lines(conv_ftl);
	remove_heat_tracking(conv_ftl);
	remove_slc_qlc_blocks(conv_ftl);
	if (conv_ftl->qlc_page_wcnt) {
		vfree(conv_ftl->qlc_page_wcnt);
		conv_ftl->qlc_page_wcnt = NULL;
	}
	kfree(conv_ftl->slc_lunwp);
	kfree(conv_ftl->gc_slc_lunwp);
	kfree(conv_ftl->qlc_lunwp);
	kfree(conv_ftl->gc_qlc_lunwp);
	conv_ftl->slc_lunwp = NULL;
	conv_ftl->gc_slc_lunwp = NULL;
	conv_ftl->qlc_lunwp = NULL;
	conv_ftl->gc_qlc_lunwp = NULL;

	remove_rmap(conv_ftl);
	remove_maptbl(conv_ftl);
}

static void conv_init_params(struct convparams *cpp)
{
	cpp->op_area_pcent = OP_AREA_PERCENT;
	cpp->gc_thres_lines = 2; /* Need only two lines.(host write, gc)*/
	cpp->gc_thres_lines_high = 2; /* Need only two lines.(host write, gc)*/
	cpp->enable_gc_delay = 1;
	cpp->pba_pcent = (int)((1 + cpp->op_area_pcent) * 100);
}

void conv_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			 uint32_t cpu_nr_dispatcher)
{
	struct ssdparams spp;
	struct convparams cpp;
	struct conv_ftl *conv_ftls;
	struct ssd *ssd;
	uint32_t i;
	const uint32_t nr_parts = SSD_PARTITIONS;

	ssd_init_params(&spp, size, nr_parts);
	conv_init_params(&cpp);

	conv_ftls = kzalloc(sizeof(struct conv_ftl) * nr_parts, GFP_KERNEL);
	if (!conv_ftls) {
		NVMEV_ERROR("Failed to allocate conv_ftl array for %u partitions\n", nr_parts);
		return;
	}

    for (i = 0; i < nr_parts; i++) {
        ssd = kmalloc(sizeof(struct ssd), GFP_KERNEL);
		ssd_init(ssd, &spp, cpu_nr_dispatcher);
		conv_init_ftl(&conv_ftls[i], &cpp, ssd);
	}

	/* PCIe, Write buffer are shared by all instances*/
    for (i = 1; i < nr_parts; i++) {
		kfree(conv_ftls[i].ssd->pcie->perf_model);
		kfree(conv_ftls[i].ssd->pcie);
		kfree(conv_ftls[i].ssd->write_buffer);

		conv_ftls[i].ssd->pcie = conv_ftls[0].ssd->pcie;
		conv_ftls[i].ssd->write_buffer = conv_ftls[0].ssd->write_buffer;
	}

	ns->id = id;
	ns->csi = NVME_CSI_NVM;
	ns->nr_parts = nr_parts;
	ns->ftls = (void *)conv_ftls;
	ns->size = (uint64_t)((size * 100) / cpp.pba_pcent);
	ns->mapped = mapped_addr;
	/*register io command handler*/
	ns->proc_io_cmd = conv_proc_nvme_io_cmd;

	NVMEV_INFO("FTL physical space: %lld, logical space: %lld (physical/logical * 100 = %d)\n",
		   size, ns->size, cpp.pba_pcent);

	return;
}

void conv_remove_namespace(struct nvmev_ns *ns)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	const uint32_t nr_parts = SSD_PARTITIONS;
	uint32_t i;

	/* PCIe, Write buffer are shared by all instances*/
	for (i = 1; i < nr_parts; i++) {
		/*
		 * These were freed from conv_init_namespace() already.
		 * Mark these NULL so that ssd_remove() skips it.
		 */
		conv_ftls[i].ssd->pcie = NULL;
		conv_ftls[i].ssd->write_buffer = NULL;
		
        /* per-LUN state removed */
	}

	for (i = 0; i < nr_parts; i++) {
		conv_remove_ftl(&conv_ftls[i]);
		ssd_remove(conv_ftls[i].ssd);
		kfree(conv_ftls[i].ssd);
	}

	kfree(conv_ftls);
	ns->ftls = NULL;
}

static inline bool valid_ppa(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	int ch = ppa->g.ch;
	int lun = ppa->g.lun;
	int pl = ppa->g.pl;
	int blk = ppa->g.blk;
	int pg = ppa->g.pg;
	//int sec = ppa->g.sec;

	if (ch < 0 || ch >= spp->nchs)
		return false;
	if (lun < 0 || lun >= spp->luns_per_ch)
		return false;
	if (pl < 0 || pl >= spp->pls_per_lun)
		return false;
	if (blk < 0 || blk >= spp->blks_per_pl)
		return false;
	{
		uint32_t max_pg = is_slc_block(conv_ftl, blk) ?
			spp->pgs_per_blk : conv_ftl->qlc_pgs_per_blk;

		if (pg < 0 || pg >= max_pg)
			return false;
	}

	return true;
}

static inline bool valid_lpn(struct conv_ftl *conv_ftl, uint64_t lpn)
{
	return (lpn < conv_ftl->ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
	return !(ppa->ppa == UNMAPPED_PPA);
}

static inline uint32_t get_glun(struct conv_ftl *conv_ftl, struct ppa *ppa)
{	
	return (ppa->g.lun * conv_ftl->ssd->sp.nchs + ppa->g.ch);
}

static inline struct line *get_line(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	uint32_t die = encode_die(&conv_ftl->ssd->sp, ppa);
	struct line_mgmt *lm;
	uint32_t idx;

	if (is_slc_block(conv_ftl, ppa->g.blk)) {
		lm = get_slc_die_lm(conv_ftl, die);
		if (!lm || !lm->lines || ppa->g.blk >= lm->tt_lines)
			return NULL;
		return &lm->lines[ppa->g.blk];
	}

	if (ppa->g.blk < conv_ftl->slc_blks_per_pl)
		return NULL;

	lm = get_qlc_die_lm(conv_ftl, die);
	if (!lm || !lm->lines)
		return NULL;

	idx = ppa->g.blk - conv_ftl->slc_blks_per_pl;
	if (idx >= lm->tt_lines)
		return NULL;

	return &lm->lines[idx];
}

static inline struct line *get_line_DA(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	/* Die-Affinity get_line 函数已废弃 - 现在使用统一的 get_line 函数 */
	return get_line(conv_ftl, ppa);
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp;
	struct nand_block *blk;
	struct nand_page *pg;
	struct heat_tracking *ht;
	uint64_t lpn = INVALID_LPN;
	uint64_t read_cnt = 0;
	bool in_slc;
	bool invalidated = false;

    /* 1. 增加全局参数验证 */
    if (!conv_ftl || !ppa || !conv_ftl->ssd) {
        NVMEV_ERROR("[mark_page_invalid] Invalid parameters.\n");
        return;
    }

    if (!valid_ppa(conv_ftl, ppa)) {
        NVMEV_ERROR("[mark_page_invalid] Invalid PPA: ch=%d, lun=%d, blk=%d, pg=%d\n",
                   ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg);
        return;
    }

	spp = &conv_ftl->ssd->sp;
	ht = &conv_ftl->heat_track;

    /* 更新页和块的状态 (这部分不涉及共享数据结构，可以在锁外完成) */
    pg = get_pg(conv_ftl->ssd, ppa);
    if (!pg) {
        NVMEV_ERROR("[mark_page_invalid] Failed to get page for ppa ch=%d,lun=%d,blk=%d,pg=%d\n",
                   ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg);
        return;
    }
    
    /* 检查页面状态，如果已经无效则直接返回 */
    if (pg->status == PG_INVALID) {
        NVMEV_DEBUG("[mark_page_invalid] Page already invalid at ch=%d,lun=%d,blk=%d,pg=%d\n",
                   ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg);
        return;
    }
    
    /* 只有有效页面才能被标记为无效 */
    if (pg->status != PG_VALID) {
        NVMEV_ERROR("[mark_page_invalid] Invalid page status %d, expected PG_VALID at ch=%d,lun=%d,blk=%d,pg=%d\n",
                   pg->status, ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg);
        return;
    }

	lpn = get_rmap_ent(conv_ftl, ppa);

    /* 2. 根据介质类型，进入完全独立的原子操作块 */
    in_slc = is_slc_block(conv_ftl, ppa->g.blk);
    if (in_slc) {
        uint32_t die = encode_die(spp, ppa);
        struct line_mgmt *lm = get_slc_die_lm(conv_ftl, die);

        /* SLC 边界检查 (在加锁前) */
        if (!lm || !lm->lines || ppa->g.blk >= lm->tt_lines) {
            NVMEV_ERROR("[mark_page_invalid] SLC block index out of range: %u >= %u\n", 
                        ppa->g.blk, lm->tt_lines);
            return;
        }

	spin_lock(&conv_ftl->slc_lock); // --- SLC 加锁 ---
	/* 双重检查，避免并发重复失效 */
	if (pg->status == PG_INVALID) {
		spin_unlock(&conv_ftl->slc_lock);
		return;
	}
	if (pg->status != PG_VALID) {
		spin_unlock(&conv_ftl->slc_lock);
		return;
	}
	pg->status = PG_INVALID;
	pg->oob_prev_lpn = INVALID_LPN;
	invalidated = true;
	slc_apply_line_invalid(lm, ppa->g.blk, spp);
	block_meta_note_invalid(conv_ftl, ppa, lpn);
	spin_unlock(&conv_ftl->slc_lock); // --- SLC 解锁 ---

	} else { // QLC 路径
		uint32_t die = encode_die(spp, ppa);
		struct line_mgmt *lm = get_qlc_die_lm(conv_ftl, die);
		struct line *line;
		uint32_t start_blk = conv_ftl->slc_blks_per_pl;
		uint32_t idx;
		bool was_full_line = false;
        
        /* QLC 边界检查 (在加锁前) */
        if (!lm || !lm->lines) {
            NVMEV_ERROR("[mark_page_invalid] QLC line management not initialized\n");
            return;
        }
        
        if (ppa->g.blk < start_blk) {
            NVMEV_ERROR("[mark_page_invalid] QLC block ID %u is less than start block %u\n", 
                        ppa->g.blk, start_blk);
            return;
        }
        idx = ppa->g.blk - start_blk;
        if (idx >= lm->tt_lines) {
            NVMEV_ERROR("[mark_page_invalid] QLC index out of range: %u >= %u\n", 
                        idx, lm->tt_lines);
            return;
        }

        spin_lock(&conv_ftl->qlc_lock); // --- QLC 加锁 ---
		/* 双重检查，避免并发重复失效 */
		if (pg->status == PG_INVALID) {
			spin_unlock(&conv_ftl->qlc_lock);
			return;
		}
		if (pg->status != PG_VALID) {
			spin_unlock(&conv_ftl->qlc_lock);
			return;
		}
		pg->status = PG_INVALID;
		pg->oob_prev_lpn = INVALID_LPN;
		invalidated = true;

        line = &lm->lines[idx];
        
        if (line->vpc == conv_ftl->qlc_pgs_per_blk) {
            was_full_line = true;
        }
        line->ipc++;

        if (line->pos) { // 如果在victim队列中
            pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
        } else {
            if (line->vpc > 0) line->vpc--;
        }

        if (was_full_line) {
            list_del_init(&line->entry);
            lm->full_line_cnt--;
            pqueue_insert(lm->victim_line_pq, line);
            lm->victim_line_cnt++;
        }

		block_meta_note_invalid(conv_ftl, ppa, lpn);

        spin_unlock(&conv_ftl->qlc_lock); // --- QLC 解锁 ---
    }

	if (!invalidated)
		return;

	if (lpn != INVALID_LPN && ht && ht->access_count)
		read_cnt = ht->access_count[lpn];

	if (conv_ftl->global_read_sum >= read_cnt)
		conv_ftl->global_read_sum -= read_cnt;
	else
		conv_ftl->global_read_sum = 0;

	if (lpn != INVALID_LPN && conv_ftl->global_valid_pg_cnt > 0)
		conv_ftl->global_valid_pg_cnt--;

	if (!in_slc && lpn != INVALID_LPN) {
		unsigned long flags;

		spin_lock_irqsave(&conv_ftl->qlc_zone_lock, flags);
		if (conv_ftl->qlc_resident_page_cnt > 0)
			conv_ftl->qlc_resident_page_cnt--;
		if (conv_ftl->qlc_resident_read_sum >= read_cnt)
			conv_ftl->qlc_resident_read_sum -= read_cnt;
		else
			conv_ftl->qlc_resident_read_sum = 0;
		spin_unlock_irqrestore(&conv_ftl->qlc_zone_lock, flags);
	}

    blk = get_blk(conv_ftl->ssd, ppa);
    if (!blk) {
        NVMEV_ERROR("[mark_page_invalid] Failed to get block for ppa ch=%d,lun=%d,blk=%d,pg=%d\n",
                   ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg);
        return;
    }

	{
		uint32_t max_pgs = blk->is_qlc ? conv_ftl->qlc_pgs_per_blk : spp->pgs_per_blk;

		NVMEV_ASSERT(blk->ipc >= 0 && blk->ipc < max_pgs);
		blk->ipc++;
		if (blk->vpc > 0) {
			blk->vpc--;
		} else {
			NVMEV_ERROR("blk->vpc already 0 before decrement, blk=%d\n", ppa->g.blk);
			/* Don't return here, continue with line management updates */
		}
	}
}

static void mark_page_valid(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp;
	struct heat_tracking *ht;
	uint64_t lpn;
	uint64_t read_cnt;
	struct nand_block *blk;
	struct nand_page *pg;
	bool in_slc;
	struct line *line;

	NVMEV_DEBUG("Entering mark_page_valid: ch=%d, lun=%d, blk=%d, pg=%d\n",
	            ppa ? ppa->g.ch : -1, ppa ? ppa->g.lun : -1,
	            ppa ? ppa->g.blk : -1, ppa ? ppa->g.pg : -1);
	/* 1. 增加全局参数验证 */
	if (!conv_ftl || !ppa || !conv_ftl->ssd) {
		NVMEV_ERROR("[mark_page_valid] Invalid parameters.\n");
		return;
	}

	spp = &conv_ftl->ssd->sp;
	ht = &conv_ftl->heat_track;
	/* 2. 验证PPA有效性 */
    if (!valid_ppa(conv_ftl, ppa)) {
        NVMEV_ERROR("[mark_page_valid] Invalid PPA: ch=%d, lun=%d, blk=%d, pg=%d\n",
                    ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg);
        return;
    }
	lpn = get_rmap_ent(conv_ftl, ppa);
	read_cnt = (ht && ht->access_count && lpn != INVALID_LPN) ?
		ht->access_count[lpn] : 0;

    /* 更新页和块的状态 (这部分不涉及共享数据结构，可以在锁外完成) */
    pg = get_pg(conv_ftl->ssd, ppa);
	if (!pg) {
        NVMEV_ERROR("[mark_page_valid] Failed to get page structure\n");
        return;
    }
    /* 4. 验证页面状态 */
    if (pg->status != PG_FREE) {
        NVMEV_WARN("[mark_page_valid] Page not FREE: status=%d at ch=%d,lun=%d,blk=%d,pg=%d\n",
                   pg->status, ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg);
        return;/* 根据实际需求决定是否继续 */
    }
    pg->status = PG_VALID;

    blk = get_blk(conv_ftl->ssd, ppa);
	if (!blk) {
        NVMEV_ERROR("[mark_page_valid] Failed to get block structure\n");
        return;
    }
    {
        uint32_t max_pgs = blk->is_qlc ? conv_ftl->qlc_pgs_per_blk : spp->pgs_per_blk;
        NVMEV_ASSERT(blk->vpc >= 0 && blk->vpc <= max_pgs);
    blk->vpc++;
        if (blk->vpc > max_pgs) {
            blk->vpc = max_pgs;
        }
    }

    /* 2. 根据介质类型，进入完全独立的原子操作块 */
    in_slc = is_slc_block(conv_ftl, ppa->g.blk);
    if (in_slc) {
        uint32_t die = encode_die(spp, ppa);
        struct line_mgmt *lm = get_slc_die_lm(conv_ftl, die);

        if (!lm || !lm->lines || ppa->g.blk >= lm->tt_lines) {
            NVMEV_ERROR("[mark_page_valid] SLC block index out of range: %u >= %u\n",
                        ppa->g.blk, lm ? lm->tt_lines : 0);
            return;
        }

    	spin_lock(&conv_ftl->slc_lock);
    	slc_apply_line_valid(lm, ppa->g.blk, spp);
		block_meta_note_valid(conv_ftl, ppa, lpn);
    	spin_unlock(&conv_ftl->slc_lock);

    } else { // QLC 路径
        uint32_t die = encode_die(spp, ppa);
        struct line_mgmt *lm = get_qlc_die_lm(conv_ftl, die);
        uint32_t start_blk = conv_ftl->slc_blks_per_pl;
        uint32_t idx;

        if (!lm || !lm->lines) {
            NVMEV_ERROR("[mark_page_valid] QLC line management not initialized\n");
            return;
        }

        if (ppa->g.blk < start_blk) {
            NVMEV_ERROR("[mark_page_valid] QLC block ID %u is less than start block %u\n",
                        ppa->g.blk, start_blk);
            return;
        }
        idx = ppa->g.blk - start_blk;
        if (idx >= lm->tt_lines) {
            NVMEV_ERROR("[mark_page_valid] QLC index out of range: %u >= %u\n",
                        idx, lm->tt_lines);
            return;
        }

        spin_lock(&conv_ftl->qlc_lock);
        line = &lm->lines[idx];
        if (line->vpc < conv_ftl->qlc_pgs_per_blk)
            line->vpc++;
        else
            line->vpc = conv_ftl->qlc_pgs_per_blk;
		block_meta_note_valid(conv_ftl, ppa, lpn);
        spin_unlock(&conv_ftl->qlc_lock);
    }

	if (lpn != INVALID_LPN) {
		conv_ftl->global_valid_pg_cnt++;
		if (ht && ht->access_count)
			conv_ftl->global_read_sum += read_cnt;
	}
}

// ... existing code ...

static void mark_block_free(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_block *blk = get_blk(conv_ftl->ssd, ppa);
	struct nand_page *pg = NULL;
	int i;

	if (!blk) {
		NVMEV_ERROR("mark_block_free: failed to locate block ch=%d,lun=%d,blk=%d\n",
			    ppa->g.ch, ppa->g.lun, ppa->g.blk);
		return;
	}

	for (i = 0; i < blk->npgs; i++) {
		/* reset page status */
		pg = &blk->pg[i];
		NVMEV_ASSERT(pg->nsecs == spp->secs_per_pg);
		pg->status = PG_FREE;
	}
	block_meta_reset(conv_ftl, ppa);

	/* reset block status */
	NVMEV_ASSERT(blk->npgs ==
		     (blk->is_qlc ? conv_ftl->qlc_pgs_per_blk : spp->pgs_per_blk));
	blk->ipc = 0;
	blk->vpc = 0;
	blk->erase_cnt++;
}

/* move valid page data (already in DRAM) from victim line to a new page */
/* Opportunistic QLC->SLC repromotion at QLC GC time.
 *
 * QLC GC has to read every valid page anyway. Right before re-writing back to
 * QLC we check if the page is hot enough to belong on SLC, and if SLC has
 * headroom and there's an active SB slot available, divert there instead. The
 * chain repacking part (writing same-chain pages near each other on QLC) is
 * already handled implicitly by `internal_place_die_for_lpn(... CHAIN_TIER_QLC)`
 * in the fall-through QLC->QLC path.
 *
 * Returns true if migrate_page_to_slc actually moved the page (caller should
 * skip the QLC->QLC rewrite). All bookkeeping (maptbl/rmap/page_in_slc/old
 * QLC mark_page_invalid) is done inside migrate_page_to_slc.
 */
static bool qlc_gc_try_repromote(struct conv_ftl *conv_ftl, uint64_t lpn,
				 struct ppa *qlc_ppa)
{
	uint64_t acc;
	struct line_pool_stats slc_st;

	if (!conv_ftl || lpn == INVALID_LPN || !qlc_ppa)
		return false;
	if (lpn >= conv_ftl->ssd->sp.tt_pgs)
		return false;
	if (!conv_ftl->heat_track.access_count)
		return false;

	acc = conv_ftl->heat_track.access_count[lpn];
	if (acc < REPROMOTE_HEAT_FLOOR)
		return false;

	/* SLC capacity guard: leave headroom so repromotion doesn't trigger
	 * the reverse SLC->QLC migration path. */
	collect_slc_stats(conv_ftl, &slc_st);
	if (slc_st.total && (slc_st.free * 100u <= slc_st.total * 20u))
		return false;

	migrate_page_to_slc(conv_ftl, lpn, qlc_ppa, NULL);
	/* migrate_page_to_slc returns void. Detect success via page_in_slc. */
	return (conv_ftl->page_in_slc && conv_ftl->page_in_slc[lpn]);
}

static int gc_write_page(struct conv_ftl *conv_ftl, struct ppa *old_ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	struct ppa new_ppa;
	uint64_t lpn = get_rmap_ent(conv_ftl, old_ppa);
	struct nand_page *old_pg = get_pg(conv_ftl->ssd, old_ppa);
	uint64_t stored_prev_lpn = old_pg ? old_pg->oob_prev_lpn : INVALID_LPN;
	uint32_t target_ch = old_ppa->g.ch;
	uint32_t target_lun = old_ppa->g.lun;
	uint32_t src_die = encode_die(spp, old_ppa);
	bool old_in_slc;
	bool slc_critical = false;
/* int prev_die_log = -1;
 	struct ppa prev_ppa = { .ppa = UNMAPPED_PPA };
*/
	NVMEV_ASSERT(valid_lpn(conv_ftl, lpn));

	if (stored_prev_lpn != INVALID_LPN && !valid_lpn(conv_ftl, stored_prev_lpn)) {
		NVMEV_WARN("gc_write_page: bad prev_lpn=%llu (ppa ch=%d lun=%d blk=%d pg=%d status=%d), drop prev link\n",
			   stored_prev_lpn, old_ppa->g.ch, old_ppa->g.lun, old_ppa->g.blk,
			   old_ppa->g.pg, old_pg ? old_pg->status : -1);
		stored_prev_lpn = INVALID_LPN;
	}

	if (!valid_ppa(conv_ftl, old_ppa)) {
		NVMEV_ERROR("gc_write_page: invalid source PPA ch=%d lun=%d blk=%d pg=%d, clearing lpn=%llu\n",
			    old_ppa->g.ch, old_ppa->g.lun, old_ppa->g.blk, old_ppa->g.pg, lpn);
		clear_lpn_mapping(conv_ftl, lpn);
		return -1;
	}

	/*
	 * Note: global_read_sum / global_valid_pg_cnt adjustments are handled
	 * by mark_page_invalid (subtract) and mark_page_valid (add).
	 * No explicit adjustment here to avoid double-counting.
	 */

/*	if (stored_prev_lpn != INVALID_LPN) {
		prev_ppa = get_maptbl_ent(conv_ftl, stored_prev_lpn);
		if (mapped_ppa(&prev_ppa) && valid_ppa(conv_ftl, &prev_ppa)) {
			uint32_t neighbor = next_adjacent_die(spp, encode_die(spp, &prev_ppa));
			prev_die_log = (int)encode_die(spp, &prev_ppa);
			decode_die(spp, neighbor, &target_ch, &target_lun);
		} else {
			stored_prev_lpn = INVALID_LPN;
		}
	}
	if (stored_prev_lpn == INVALID_LPN && dies) {
		uint32_t neighbor = next_adjacent_die(spp, encode_die(spp, old_ppa));
		decode_die(spp, neighbor, &target_ch, &target_lun);
	}
*/
	old_in_slc = is_slc_block(conv_ftl, old_ppa->g.blk);
	if (old_in_slc) {
		struct line_pool_stats slc_st;
		uint32_t actual_die;
		uint32_t die_index;
		bool chain_gc_to_qlc = false;

		collect_slc_stats(conv_ftl, &slc_st);
		slc_critical = (slc_st.free <= SLC_EMERGENCY_RESERVE);
#if NVMEV_ENABLE_CHAIN_AGGREGATION
		if (!slc_critical)
			chain_gc_to_qlc =
				slc_chain_gc_should_migrate_to_qlc(conv_ftl, old_ppa, lpn);
#endif

		if (slc_critical || chain_gc_to_qlc) {
			if (migrate_page_to_qlc(conv_ftl, lpn, old_ppa) < 0)
				return -1;
			if (chain_gc_to_qlc)
				conv_ftl->chain_gc_to_qlc_pages++;
			return 0;
		}

		die_index = internal_place_die_for_lpn(conv_ftl, lpn, old_ppa,
						       CHAIN_TIER_SLC, false);
		decode_die(spp, die_index, &target_ch, &target_lun);

		if (conv_ftl->gc_slc_lunwp) {
			struct write_pointer *gc_wp = &conv_ftl->gc_slc_lunwp[die_index];
			if (!gc_wp->curline || gc_wp->pg == 0) {
				gc_wp->ch = target_ch;
				gc_wp->lun = target_lun;
			}
		}

		new_ppa = get_new_gc_slc_page(conv_ftl, die_index);
		if (!mapped_ppa(&new_ppa)) {
			NVMEV_ERROR("gc_write_page: Failed to get new SLC page, flushing to QLC.\n");
			if (migrate_page_to_qlc(conv_ftl, lpn, old_ppa) < 0)
				return -1;
			return 0;
		}
			set_maptbl_ent_reason(conv_ftl, lpn, &new_ppa, NVMEV_DIE_CHANGE_GC);
			set_rmap_ent(conv_ftl, lpn, &new_ppa);
			mark_page_valid(conv_ftl, &new_ppa);
			/* Internal SLC->SLC GC keeps resident accounting in sync but does
			 * not alter the read/write heat inputs used by cold migration. */
			slc_resident_track_page(conv_ftl, lpn, encode_die(spp, &new_ppa));
			set_page_prev_link(conv_ftl, lpn, &new_ppa, stored_prev_lpn);
			actual_die = encode_die(spp, &new_ppa);
			advance_gc_slc_write_pointer(conv_ftl, actual_die);
			internal_place_note_lpn_write(conv_ftl, lpn, CHAIN_TIER_SLC,
						      false, actual_die);
			mark_page_invalid(conv_ftl, old_ppa);
		set_rmap_ent(conv_ftl, INVALID_LPN, old_ppa);
		NVMEV_DEBUG("[TASK2][GC-SLC] lpn=%llu prev_lpn=%lld src_die=%u dst_die=%u",
			lpn,
			stored_prev_lpn == INVALID_LPN ? -1LL : (long long)stored_prev_lpn,
/*			prev_die_log, */
			src_die,
			encode_die(spp, &new_ppa));
	} else {
		uint32_t target_die;
		uint32_t zone_hint = old_pg ? old_pg->qlc_latency_zone : 0;

		/* Opportunistic QLC->SLC repromotion at GC time: if this LPN is
		 * hot, divert to SLC instead of recopying back to QLC. The chain
		 * repacking on the QLC->QLC path below is handled by
		 * internal_place_die_for_lpn(CHAIN_TIER_QLC). */
		if (qlc_gc_try_repromote(conv_ftl, lpn, old_ppa))
			return 0;

		/*
		 * qlc_resident_page_cnt / qlc_resident_read_sum are handled by
		 * mark_page_invalid below.  Only adjust fast/slow zone counts
		 * here because mark_page_invalid does not track them.
		 */
		if (lpn != INVALID_LPN) {
			unsigned long stat_flags;
			spin_lock_irqsave(&conv_ftl->qlc_zone_lock, stat_flags);
			if (qlc_zone_is_fast(zone_hint)) {
				if (conv_ftl->qlc_fast_count > 0)
					conv_ftl->qlc_fast_count--;
			} else {
				if (conv_ftl->qlc_slow_count > 0)
					conv_ftl->qlc_slow_count--;
			}
			spin_unlock_irqrestore(&conv_ftl->qlc_zone_lock, stat_flags);
		}

		if (zone_hint >= QLC_ZONE_COUNT)
			zone_hint = QLC_ZONE_COUNT - 1;

		target_die = internal_place_die_for_lpn(conv_ftl, lpn, old_ppa,
							CHAIN_TIER_QLC, true);
		if (qlc_get_new_gc_page(conv_ftl, target_die, zone_hint, &new_ppa) != 0) {
			NVMEV_ERROR("gc_write_page: Failed to get new QLC GC page (zone_hint=%u).\n",
				    zone_hint);
			return -1;
		}
			set_maptbl_ent_reason(conv_ftl, lpn, &new_ppa, NVMEV_DIE_CHANGE_GC);
		set_rmap_ent(conv_ftl, lpn, &new_ppa);
		mark_page_valid(conv_ftl, &new_ppa);
		set_page_prev_link(conv_ftl, lpn, &new_ppa, stored_prev_lpn);
		update_qlc_latency_zone(conv_ftl, lpn, &new_ppa);
		mark_page_invalid(conv_ftl, old_ppa);
		set_rmap_ent(conv_ftl, INVALID_LPN, old_ppa);
		internal_place_note_lpn_write(conv_ftl, lpn, CHAIN_TIER_QLC,
					      true, encode_die(spp, &new_ppa));
		NVMEV_DEBUG("[TASK2][GC-QLC] lpn=%llu prev_lpn=%lld src_die=%u dst_die=%u zone_hint=%u",
			lpn,
			stored_prev_lpn == INVALID_LPN ? -1LL : (long long)stored_prev_lpn,
			src_die,
			encode_die(spp, &new_ppa),
			zone_hint);
	}

	if (cpp->enable_gc_delay) {
		struct nand_cmd gcw = {
			.type = GC_IO,
			.cmd = NAND_NOP,
			.stime = 0,
			.interleave_pci_dma = false,
			.ppa = &new_ppa,
		};
		if (last_pg_in_wordline(conv_ftl, &new_ppa)) {
			gcw.cmd = NAND_WRITE;
			gcw.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg;
		}

		ssd_advance_nand(conv_ftl->ssd, &gcw);
	}

	return 0;
}

/* 选择最佳的受害者line，支持定向池选择（SLC/QLC/ANY） */
static bool select_victim_line(struct conv_ftl *conv_ftl, bool force, int target_pool,
			       struct victim_candidate *victim)
{
	bool want_slc = (target_pool == 1 || target_pool == 0);
	bool want_qlc = (target_pool == 2 || target_pool == 0);

	while (true) {
		struct victim_candidate slc_cand = { .line = NULL };
		struct victim_candidate qlc_cand = { .line = NULL };
		bool slc_valid = want_slc && find_best_victim(conv_ftl, true, force, &slc_cand);
		bool qlc_valid = want_qlc && find_best_victim(conv_ftl, false, force, &qlc_cand);
		struct victim_candidate *pick = NULL;

		if (target_pool == 1) {
			if (!slc_valid)
				return false;
			pick = &slc_cand;
		} else if (target_pool == 2) {
			if (!qlc_valid)
				return false;
			pick = &qlc_cand;
		} else if (slc_valid && qlc_valid) {
			pick = (slc_cand.vpc <= qlc_cand.vpc) ? &slc_cand : &qlc_cand;
		} else if (slc_valid) {
			pick = &slc_cand;
		} else if (qlc_valid) {
			pick = &qlc_cand;
		} else {
			return false;
		}

		if (pop_victim_from_pool(conv_ftl, pick)) {
			*victim = *pick;
			return true;
		}
		/* 队列状态变化，重试 */
	}
}

static void mark_line_free(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm;
	struct line *line;
	bool in_slc;
	uint32_t die;
	uint32_t start_blk;
	uint32_t line_id;
	uint32_t idx;

	in_slc = is_slc_block(conv_ftl, ppa->g.blk);
	die = encode_die(spp, ppa);

	if (in_slc) {
		lm = get_slc_die_lm(conv_ftl, die);
		line_id = line_from_blk(ppa->g.blk);

		if (!lm || !lm->lines || line_id >= lm->tt_lines)
			return;

		spin_lock(&conv_ftl->slc_lock);
		line = &lm->lines[line_id];
		line->ipc = 0;
		line->vpc = 0;
		line->pos = 0;
		list_add_tail(&line->entry, &lm->free_line_list);
		lm->free_line_cnt++;
		/* SB state: clear this die's bit; if the SB was CLOSED and now no
		 * dies are still "filled", the SB is fully erased and goes FREE. */
		if (conv_ftl->slc_sb_die_full_mask &&
		    line_id < conv_ftl->ssd->sp.blks_per_pl) {
			uint32_t blk_id = line_id;
			uint32_t die_idx = encode_die(spp, ppa);
			conv_ftl->slc_sb_die_full_mask[blk_id] &= ~(uint16_t)(1u << die_idx);
			if (conv_ftl->slc_sb_state &&
			    conv_ftl->slc_sb_state[blk_id] == NVMEV_SB_CLOSED &&
			    conv_ftl->slc_sb_die_full_mask[blk_id] == 0)
				slc_sb_note_freed_locked(conv_ftl, blk_id);
		}
		spin_unlock(&conv_ftl->slc_lock);
	} else {
		start_blk = conv_ftl->slc_blks_per_pl;
		line_id = line_from_blk(ppa->g.blk);
		if (ppa->g.blk < start_blk)
			return;
		idx = line_id - start_blk;
		lm = get_qlc_die_lm(conv_ftl, die);

		if (!lm || !lm->lines || idx >= lm->tt_lines)
			return;

		spin_lock(&conv_ftl->qlc_lock);
		line = &lm->lines[idx];
		line->ipc = 0;
		line->vpc = 0;
		line->pos = 0;
		qlc_closed_repromote_note_open_locked(conv_ftl, ppa->g.blk);
		list_add_tail(&line->entry, &lm->free_line_list);
		lm->free_line_cnt++;
		spin_unlock(&conv_ftl->qlc_lock);
	}
}


/* SB-level SLC GC for the chain-aware FTL.
 *
 * Same shape as baseline's do_gc_superblock_slc: pick the SB whose summed
 * invalid-page count across dies is highest (skipping any SB whose line is
 * still being written by some chain), detach it from full/victim lists on
 * each die, rewrite valid pages via gc_write_page (chain-aware), erase, and
 * return all dies to the free pool together so the cross-die-aligned chain
 * allocator sees a fully free SB.
 *
 * Bonus: when the SB is fully erased, slc_sb_note_freed_locked transitions
 * the SB state machine NVMEV_SB_CLOSED -> NVMEV_SB_FREE. The hook is in mark_block_free.
 */
struct sb_gc_victim_no1 {
	uint32_t blk_id;
	uint32_t total_ipc;
	uint32_t total_vpc;
	uint16_t die_eligible_mask;
	uint16_t die_full_mask;
	uint8_t tier;
};

static bool select_sb_victim_slc_locked_no1(struct conv_ftl *conv_ftl,
					    struct sb_gc_victim_no1 *out, bool force)
{
	uint32_t line_cnt, i;
	struct slc_sb_summary_no1 best = { 0 };
	bool have_best = false;
	uint8_t best_tier = 3;
	uint64_t global_avg = 0;

	if (!conv_ftl || !conv_ftl->ssd || !conv_ftl->slc_lunlm ||
	    !conv_ftl->slc_sb_migrated_victim)
		return false;
	(void)force;
	line_cnt = conv_ftl->slc_blks_per_pl;
	if (!line_cnt)
		return false;
	(void)calc_global_avg_reads(conv_ftl, &global_avg);

	for (i = 0; i < line_cnt; i++) {
		struct slc_sb_summary_no1 cand;

		if (!conv_ftl->slc_sb_migrated_victim[i])
			continue;
		if (!slc_sb_collect_summary(conv_ftl, i, &cand))
			continue;
		if (cand.active || cand.open_writer ||
		    (cand.total_vpc == 0 && cand.total_ipc == 0)) {
			slc_migrated_victim_remove_locked(conv_ftl, i);
			conv_ftl->slc_sb_migration_victim_stale++;
			continue;
		}
		if (!have_best ||
		    cand.total_ipc > best.total_ipc ||
		    (cand.total_ipc == best.total_ipc &&
		     hweight16(cand.eligible_mask) > hweight16(best.eligible_mask)) ||
		    (cand.total_ipc == best.total_ipc &&
		     hweight16(cand.eligible_mask) == hweight16(best.eligible_mask) &&
		     cand.blk_id < best.blk_id)) {
			best = cand;
			have_best = true;
		}
	}

	if (!have_best)
		return false;

	slc_migrated_victim_remove_locked(conv_ftl, best.blk_id);
	conv_ftl->slc_sb_migration_victim_dequeues++;
	best_tier = slc_sb_summary_tier(&best, global_avg);
	out->blk_id = best.blk_id;
	out->total_ipc = best.total_ipc;
	out->total_vpc = best.total_vpc;
	out->die_eligible_mask = best.eligible_mask;
	out->die_full_mask = best.full_mask;
	out->tier = best_tier;
	return true;
}

static int do_gc_superblock_slc(struct conv_ftl *conv_ftl, bool force)
{
	struct ssdparams *spp;
	struct convparams *cpp;
	struct sb_gc_victim_no1 sv;
	uint32_t die_count, die;
	uint32_t valid_moved = 0;
	uint32_t erase_ops = 0;
	uint64_t erase_time_ns = 0;

	if (!conv_ftl || !conv_ftl->ssd)
		return -1;
	spp = &conv_ftl->ssd->sp;
	cpp = &conv_ftl->cp;
	die_count = conv_ftl->die_count ? conv_ftl->die_count : 1;

	spin_lock(&conv_ftl->slc_lock);
	if (!select_sb_victim_slc_locked_no1(conv_ftl, &sv, force)) {
		spin_unlock(&conv_ftl->slc_lock);
		return -1;
	}
	for (die = 0; die < die_count; die++) {
		struct line_mgmt *lm;
		struct line *line;

		if (!(sv.die_eligible_mask & (1u << die)))
			continue;
		lm = get_slc_die_lm(conv_ftl, die);
		if (!lm)
			continue;
		line = &lm->lines[sv.blk_id];

		if (sv.die_full_mask & (1u << die)) {
			list_del_init(&line->entry);
			if (lm->full_line_cnt)
				lm->full_line_cnt--;
		} else {
			if (line->pos && pqueue_remove(lm->victim_line_pq, line) == 0 &&
			    lm->victim_line_cnt)
				lm->victim_line_cnt--;
			line->pos = 0;
		}
	}
	spin_unlock(&conv_ftl->slc_lock);

	conv_ftl->wfc.credits_to_refill = sv.total_ipc;

	for (die = 0; die < die_count; die++) {
		struct ppa ppa = { .ppa = 0 };
		uint32_t ch, lun;
		struct nand_block *blk;
		int max_pgs;
		int pg;
		bool gc_failed = false;

		if (!(sv.die_eligible_mask & (1u << die)))
			continue;

		decode_die(spp, die, &ch, &lun);
		ppa.g.ch = ch;
		ppa.g.lun = lun;
		ppa.g.pl = 0;
		ppa.g.blk = sv.blk_id;

		blk = get_blk(conv_ftl->ssd, &ppa);
		max_pgs = blk ? blk->npgs : spp->pgs_per_blk;

		for (pg = 0; pg < max_pgs; pg++) {
			struct nand_page *pg_iter;

			ppa.g.pg = pg;
			pg_iter = get_pg(conv_ftl->ssd, &ppa);
			if (pg_iter && pg_iter->status == PG_VALID) {
				if (gc_write_page(conv_ftl, &ppa) < 0) {
					gc_failed = true;
					NVMEV_ERROR("do_gc_superblock_slc: write_page failed die=%u blk=%u pg=%d\n",
						    die, sv.blk_id, pg);
					break;
				}
				valid_moved++;
			}
		}

		if (gc_failed) {
			struct line_mgmt *lm = get_slc_die_lm(conv_ftl, die);
			spin_lock(&conv_ftl->slc_lock);
			if (lm) {
				struct line *line = &lm->lines[sv.blk_id];
				if (sv.die_full_mask & (1u << die)) {
					list_add_tail(&line->entry, &lm->full_line_list);
					lm->full_line_cnt++;
				} else {
					line->pos = 0;
				}
			}
			slc_migrated_victim_enqueue_locked(conv_ftl, sv.blk_id);
			spin_unlock(&conv_ftl->slc_lock);
			return -1;
		}

		ppa.g.pg = 0;
		mark_block_free(conv_ftl, &ppa);
		erase_ops++;
		if (cpp->enable_gc_delay) {
			uint64_t erase_start_ns = __get_ioclock(conv_ftl->ssd);
			uint64_t erase_completed_ns;
			struct nand_cmd gce = {
				.type = GC_IO,
				.cmd = NAND_ERASE,
				.stime = erase_start_ns,
				.interleave_pci_dma = false,
				.ppa = &ppa,
			};
			erase_completed_ns = ssd_advance_nand(conv_ftl->ssd, &gce);
			if (erase_completed_ns > erase_start_ns)
				erase_time_ns += erase_completed_ns - erase_start_ns;
		}
		mark_line_free(conv_ftl, &ppa);
	}
	conv_ftl->slc_sb_gc_count++;
	conv_ftl->slc_sb_gc_valid_pages += valid_moved;
	conv_ftl->slc_sb_gc_invalid_pages += sv.total_ipc;
	conv_ftl->slc_sb_gc_erase_ops += erase_ops;
	conv_ftl->slc_sb_gc_erase_time_ns += erase_time_ns;
	switch (sv.tier) {
	case 0:
		conv_ftl->slc_sb_gc_tier_empty++;
		break;
	case 1:
		conv_ftl->slc_sb_gc_tier_pure_cold++;
		break;
	case 2:
		conv_ftl->slc_sb_gc_tier_mixed++;
		break;
	default:
		conv_ftl->slc_sb_gc_tier_fallback++;
		break;
	}
	return 0;
}

static int do_gc(struct conv_ftl *conv_ftl, bool force, int target_pool)
{
	static uint64_t gc_last_print_ns = 0;
	static uint32_t gc_count_slc = 0;
	static uint32_t gc_count_qlc = 0;
	static uint32_t gc_pages_migrated = 0;
	static uint32_t gc_no_victim = 0;

	struct victim_candidate victim;
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct ppa ppa;
	int pg;
	int max_pgs = spp->pgs_per_blk;
	bool in_slc;
	struct convparams *cpp;
	uint32_t ch = 0, lun = 0;

	/* SLC pool: use SB-level GC only. ACTIVE SBs (still being written by
	 * a chain, fallback writer, or GC writer) are skipped by the selector. */
	if (target_pool == 1 || target_pool == 0) {
		if (do_gc_superblock_slc(conv_ftl, force) == 0)
			return 0;
		if (target_pool == 1)
			return -1;
		target_pool = 2; /* ANY fallback must not run legacy per-die SLC GC. */
	}

	if (!select_victim_line(conv_ftl, force, target_pool, &victim)) {
		gc_no_victim++;
		NVMEV_DEBUG("do_gc: No suitable victim line found.\n");
		return -1;
	}

	/* line ID 已为全局编号，可直接换算物理 block ID */
	ppa.g.blk = blk_from_line(victim.line->id);
	ppa.g.pl = 0;
	decode_die(spp, victim.die, &ch, &lun);
	ppa.g.ch = ch;
	ppa.g.lun = lun;
	
	/* 确定是SLC还是QLC并显示相应的统计信息 */
	in_slc = victim.is_slc;
	NVMEV_DEBUG("GC-ing %s line:%d (die=%u) ipc=%d vpc=%d\n",
		    in_slc ? "SLC" : "QLC", ppa.g.blk, victim.die,
		    victim.line->ipc, victim.line->vpc);
	{
		uint32_t owner_chain = INVALID_CHAIN_ID;
		bool high_purity = false;
		uint64_t idx = block_meta_index(conv_ftl, &ppa);

		if (idx != U64_MAX && conv_ftl->blk_owner_chain &&
		    idx < conv_ftl->ssd->sp.tt_blks)
			owner_chain = conv_ftl->blk_owner_chain[idx];
		if (in_slc)
			high_purity = block_meta_high_purity(conv_ftl, &ppa, NULL);
		note_gc_victim_event(conv_ftl, &ppa, in_slc, victim.line->vpc,
				     victim.line->ipc, owner_chain, high_purity);
	}

	conv_ftl->wfc.credits_to_refill = victim.line->ipc;
	{
		struct nand_block *victim_blk = get_blk(conv_ftl->ssd, &ppa);
		if (victim_blk)
			max_pgs = victim_blk->npgs;
	}

	{
		uint32_t pages_moved = 0;
		bool gc_failed = false;

		for (pg = 0; pg < max_pgs; pg++) {
			struct nand_page *pg_iter = NULL;

			ppa.g.pg = pg;
			pg_iter = get_pg(conv_ftl->ssd, &ppa);

			if (pg_iter && pg_iter->status == PG_VALID) {
				if (gc_write_page(conv_ftl, &ppa) < 0) {
					gc_failed = true;
					NVMEV_ERROR("do_gc: gc_write_page failed at pg=%d, aborting victim blk=%d die=%u\n",
						    pg, ppa.g.blk, victim.die);
					break;
				}
				pages_moved++;
			}
		}
		gc_pages_migrated += pages_moved;

		if (gc_failed && victim.line->vpc > 0) {
			/*
			 * Some valid pages could not be moved — keep the
			 * victim alive so data is not lost.  Re-insert it
			 * into the victim pqueue for a future retry.
			 */
			if (in_slc) {
				struct line_mgmt *lm = get_slc_die_lm(conv_ftl, victim.die);
				if (lm) {
					spin_lock(&conv_ftl->slc_lock);
					victim.line->pos = 0;
					spin_unlock(&conv_ftl->slc_lock);
				}
			} else {
				struct line_mgmt *lm = get_qlc_die_lm(conv_ftl, victim.die);
				if (lm) {
					spin_lock(&conv_ftl->qlc_lock);
					pqueue_insert(lm->victim_line_pq, victim.line);
					lm->victim_line_cnt++;
					spin_unlock(&conv_ftl->qlc_lock);
				}
			}

			if (in_slc)
				gc_count_slc++;
			else
				gc_count_qlc++;
			return -1;
		}
	}

	if (in_slc)
		gc_count_slc++;
	else
		gc_count_qlc++;

	/* 每 5 秒打印一次 GC 汇总，不刷屏 */
	{
		uint64_t now_ns = ktime_get_ns();
		if (gc_last_print_ns == 0)
			gc_last_print_ns = now_ns;
		if (now_ns - gc_last_print_ns >= 5000000000ULL) {
			struct line_pool_stats slc_st, qlc_st;
			collect_slc_stats(conv_ftl, &slc_st);
			collect_qlc_stats(conv_ftl, &qlc_st);
			NVMEV_ERROR("[GC-MONITOR] %us: slc_gc=%u qlc_gc=%u pages_migrated=%u no_victim=%u | SLC free/victim/full/total=%u/%u/%u/%u QLC free/victim/full/total=%u/%u/%u/%u\n",
				    (unsigned)(now_ns - gc_last_print_ns) / 1000000000U,
				    gc_count_slc, gc_count_qlc, gc_pages_migrated, gc_no_victim,
				    slc_st.free, slc_st.victim, slc_st.full, slc_st.total,
				    qlc_st.free, qlc_st.victim, qlc_st.full, qlc_st.total);
			gc_count_slc = 0;
			gc_count_qlc = 0;
			gc_pages_migrated = 0;
			gc_no_victim = 0;
			gc_last_print_ns = now_ns;
		}
	}

	ppa.g.pg = 0;
	mark_block_free(conv_ftl, &ppa);
	
	cpp = &conv_ftl->cp;
	if (cpp->enable_gc_delay) {
		struct nand_cmd gce = {
			.type = GC_IO,
			.cmd = NAND_ERASE,
			.stime = 0,
			.interleave_pci_dma = false,
			.ppa = &ppa,
		};
		ssd_advance_nand(conv_ftl->ssd, &gce);
	}

	mark_line_free(conv_ftl, &ppa);

	return 0;
}

static int do_gc_for_die(struct conv_ftl *conv_ftl, uint32_t target_die, bool is_slc)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm;
	struct line *victim_line;
	struct ppa ppa;
	int pg, max_pgs;
	uint32_t ch = 0, lun = 0;
	struct convparams *cpp;

	/* SLC pool: SB-level GC ignores target_die — we pick the SB with the
	 * most invalid pages globally and erase all dies of it together. */
	if (is_slc)
		return do_gc_superblock_slc(conv_ftl, true);

	if (is_slc)
		lm = get_slc_die_lm(conv_ftl, target_die);
	else
		lm = get_qlc_die_lm(conv_ftl, target_die);

	if (!lm)
		return -1;

	spin_lock(is_slc ? &conv_ftl->slc_lock : &conv_ftl->qlc_lock);
	victim_line = pqueue_peek(lm->victim_line_pq);
	if (!victim_line) {
		spin_unlock(is_slc ? &conv_ftl->slc_lock : &conv_ftl->qlc_lock);
		return -1;
	}
	pqueue_pop(lm->victim_line_pq);
	victim_line->pos = 0;
	lm->victim_line_cnt--;
	spin_unlock(is_slc ? &conv_ftl->slc_lock : &conv_ftl->qlc_lock);

	ppa.g.blk = blk_from_line(victim_line->id);
	ppa.g.pl = 0;
	decode_die(spp, target_die, &ch, &lun);
	ppa.g.ch = ch;
	ppa.g.lun = lun;

	max_pgs = spp->pgs_per_blk;
	{
		struct nand_block *victim_blk = get_blk(conv_ftl->ssd, &ppa);
		if (victim_blk)
			max_pgs = victim_blk->npgs;
	}

	{
		uint32_t pages_moved = 0;
		bool gc_failed = false;

		for (pg = 0; pg < max_pgs; pg++) {
			struct nand_page *pg_iter;

			ppa.g.pg = pg;
			pg_iter = get_pg(conv_ftl->ssd, &ppa);

			if (pg_iter && pg_iter->status == PG_VALID) {
				if (gc_write_page(conv_ftl, &ppa) < 0) {
					gc_failed = true;
					break;
				}
				pages_moved++;
			}
		}

		if (gc_failed && victim_line->vpc > 0) {
			spinlock_t *lock = is_slc ? &conv_ftl->slc_lock : &conv_ftl->qlc_lock;
			spin_lock(lock);
			if (is_slc) {
				victim_line->pos = 0;
			} else {
				pqueue_insert(lm->victim_line_pq, victim_line);
				lm->victim_line_cnt++;
			}
			spin_unlock(lock);
			return -1;
		}
	}

	ppa.g.pg = 0;
	mark_block_free(conv_ftl, &ppa);

	cpp = &conv_ftl->cp;
	if (cpp->enable_gc_delay) {
		struct nand_cmd gce = {
			.type = GC_IO,
			.cmd = NAND_ERASE,
			.stime = 0,
			.interleave_pci_dma = false,
			.ppa = &ppa,
		};
		ssd_advance_nand(conv_ftl->ssd, &gce);
	}

	mark_line_free(conv_ftl, &ppa);
	return 0;
}

static void forground_gc(struct conv_ftl *conv_ftl, enum foreground_gc_mode mode)
{
	static uint64_t fgc_last_print_ns = 0;
	static uint32_t fgc_calls = 0;
	static uint32_t fgc_triggered = 0;
	bool slc_victim_ready = false;
	bool qlc_victim_ready = false;
	bool force = (mode == FGC_MODE_HARD);

	fgc_calls++;

	/* Soft GC 只挑便宜 victim；Hard GC 才允许强制推进。 */
	if ((force && should_gc_slc_hard(conv_ftl)) ||
	    (!force && should_gc_slc_soft(conv_ftl))) {
		slc_victim_ready = slc_has_any_victim(conv_ftl);
		if (slc_victim_ready) {
			fgc_triggered++;
			do_gc(conv_ftl, force, 1);
			return;
		}
	}
	/* 其次保障 QLC：当 QLC free 行数过低时，仅清 QLC 受害者 */
	if (should_gc_qlc_high(conv_ftl)) {
		qlc_victim_ready = qlc_has_any_victim(conv_ftl);
		if (qlc_victim_ready) {
			fgc_triggered++;
			do_gc(conv_ftl, force, 2);
			return;
		}
	}
	/* 全局兜底只用于 hard GC，soft GC 不为了少量总空间波动打断前台布局。 */
	if (force && should_gc_high(conv_ftl)) {
		if (!slc_victim_ready)
			slc_victim_ready = slc_has_any_victim(conv_ftl);
		if (!qlc_victim_ready)
			qlc_victim_ready = qlc_has_any_victim(conv_ftl);
		if (slc_victim_ready || qlc_victim_ready) {
			fgc_triggered++;
			do_gc(conv_ftl, true, 0);
			return;
		}
	}

	/* per-die 保护只在 hard GC 下启用。 */
	if (force) {
		uint32_t starved_die;
		if (should_gc_slc_any_die_critical(conv_ftl, &starved_die)) {
			fgc_triggered++;
			do_gc_for_die(conv_ftl, starved_die, true);
			return;
		}
	}

	/* 没触发 GC 时，也每 5 秒打印一次状态（确保总能看到输出） */
	{
		uint64_t now_ns = ktime_get_ns();
		if (fgc_last_print_ns == 0)
			fgc_last_print_ns = now_ns;
		if (now_ns - fgc_last_print_ns >= 5000000000ULL) {
			struct line_pool_stats slc_st, qlc_st;
			collect_slc_stats(conv_ftl, &slc_st);
			collect_qlc_stats(conv_ftl, &qlc_st);
			NVMEV_ERROR("[FGC-MONITOR] mode=%s calls=%u triggered=%u | SLC free=%u QLC free=%u (no GC needed)\n",
				    force ? "hard" : "soft",
				    fgc_calls, fgc_triggered, slc_st.free, qlc_st.free);
			fgc_calls = 0;
			fgc_triggered = 0;
			fgc_last_print_ns = now_ns;
		}
	}
}

static bool is_same_flash_page(struct conv_ftl *conv_ftl, struct ppa ppa1, struct ppa ppa2)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint32_t ppa1_page = ppa1.g.pg / spp->pgs_per_flashpg;
	uint32_t ppa2_page = ppa2.g.pg / spp->pgs_per_flashpg;

	return (ppa1.h.blk_in_ssd == ppa2.h.blk_in_ssd) && (ppa1_page == ppa2_page);
}

/* 检查块是否为 SLC - 带安全检查 */
static bool is_slc_block(struct conv_ftl *conv_ftl, uint32_t blk_id)
{
	/* 检查初始化状态 */
	if (!conv_ftl || !conv_ftl->slc_initialized || !conv_ftl->is_slc_block) {
		NVMEV_ERROR("SLC blocks not properly initialized\n");
		return false;  /* 默认返回false，避免误判 */
	}
	
	/* 在多通道配置下，每个通道/LUN都有独立的block ID空间 (0-8191)
	 * SLC占用每个plane的前slc_blks_per_pl个block
	 */
	uint32_t max_blks = conv_ftl->slc_blks_per_pl + conv_ftl->qlc_blks_per_pl;
	if (!max_blks && conv_ftl->ssd)
		max_blks = conv_ftl->ssd->sp.blks_per_pl;

	if (blk_id >= max_blks) {
		NVMEV_ERROR("Block ID out of range: %u >= %u (max per plane)\n",
			    blk_id, max_blks);
		return false;
	}
	
	/* SLC块是每个plane中ID < slc_blks_per_pl的块 */
	return (blk_id < conv_ftl->slc_blks_per_pl);
}

/* 获取 SLC 的新页面 - 使用 Die Affinity, 支持 die fallback */
static void advance_chain_slc_write_pointer(struct conv_ftl *conv_ftl, uint32_t chain_id,
					    uint32_t die)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct write_pointer *wp;
	struct line_mgmt *lm;
	uint32_t blk_filled = 0;
	bool die_filled = false;

	lm = get_slc_die_lm(conv_ftl, die);
	if (!lm)
		return;

	spin_lock(&conv_ftl->slc_lock);
	wp = resolve_chain_active_wp_locked(conv_ftl, chain_id, die);
	if (!wp || !wp->curline) {
		spin_unlock(&conv_ftl->slc_lock);
		return;
	}
	wp->pg++;
	if (wp->pg >= spp->pgs_per_blk) {
		blk_filled = wp->blk;
		die_filled = true;
		slc_wp_close_line_locked(conv_ftl, lm, wp, die);
	}
	if (die_filled)
		slc_sb_note_die_full_locked(conv_ftl, blk_filled, die);
	spin_unlock(&conv_ftl->slc_lock);
}

/* Resolve the actual write_pointer to use for chain_id writing on `die`.
 * Honors the SB-owner redirect for parasitic shares: if chain_id's
 * chain_cur_active_sb points to an SB whose owner is some other chain Y,
 * we return Y's wps[die] so all writers advance the same per-die page counter.
 *
 * Caller must hold slc_lock. We deliberately bypass get_chain_host_slc_wp's
 * lazy-alloc path because that helper acquires slc_lock itself (would deadlock).
 * The owner's wps array is guaranteed allocated because slc_open_chain_active_sb_locked
 * requires it before transitioning the SB to ACTIVE.
 */
static struct write_pointer *resolve_chain_active_wp_locked(struct conv_ftl *conv_ftl,
							    uint32_t chain_id,
							    uint32_t die)
{
	uint32_t cur_blk;
	uint32_t owner;
	struct write_pointer *wps;

	if (!conv_ftl || !chain_id_valid(conv_ftl, chain_id) ||
	    !conv_ftl->chain_cur_active_sb || !conv_ftl->slc_sb_owner_chain ||
	    !conv_ftl->chain_host_slc_wps || !conv_ftl->die_count)
		return NULL;

	cur_blk = conv_ftl->chain_cur_active_sb[chain_id];
	if (cur_blk == U32_MAX)
		return NULL;
	owner = conv_ftl->slc_sb_owner_chain[cur_blk];
	if (!chain_id_valid(conv_ftl, owner))
		return NULL;
	wps = conv_ftl->chain_host_slc_wps[owner];
	if (!wps)
		return NULL;
	return &wps[die % conv_ftl->die_count];
}

static struct ppa get_new_chain_slc_page(struct conv_ftl *conv_ftl, uint32_t chain_id,
					 uint32_t die)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct ppa ppa;
	struct nand_page *pg;
	struct write_pointer *wp;
	struct line_mgmt *lm;
	uint32_t cur_blk;

	/* Lazy-alloc the chain's own wps array (used as the owner's write state
	 * if/when this chain becomes an SB owner). */
	(void)get_chain_host_slc_wp(conv_ftl, chain_id, die, true);
	lm = get_slc_die_lm(conv_ftl, die);
	if (!lm || !lm->lines)
		return (struct ppa){ .ppa = UNMAPPED_PPA };

retry_get_page:
	wp = NULL;
	spin_lock(&conv_ftl->slc_lock);
	if (!chain_ensure_slc_superblock_slot_locked(conv_ftl, chain_id)) {
		spin_unlock(&conv_ftl->slc_lock);
		return (struct ppa){ .ppa = UNMAPPED_PPA };
	}
	cur_blk = conv_ftl->chain_cur_active_sb ?
		conv_ftl->chain_cur_active_sb[chain_id] : U32_MAX;
	wp = resolve_chain_active_wp_locked(conv_ftl, chain_id, die);
	if (!wp || !wp->curline || wp->blk != cur_blk ||
	    wp->pg >= (uint32_t)spp->pgs_per_blk) {
		uint32_t alt;
		bool found_alt = false;

		for (alt = 0; alt < conv_ftl->die_count; alt++) {
			struct write_pointer *alt_wp;

			if (alt == die)
				continue;
			alt_wp = resolve_chain_active_wp_locked(conv_ftl, chain_id, alt);
			if (!alt_wp || !alt_wp->curline || alt_wp->blk != cur_blk)
				continue;
			if (alt_wp->pg >= (uint32_t)spp->pgs_per_blk)
				continue;
			die = alt;
			wp = alt_wp;
			found_alt = true;
			break;
		}

		if (!found_alt) {
			spin_unlock(&conv_ftl->slc_lock);
			return (struct ppa){ .ppa = UNMAPPED_PPA };
		}
		conv_ftl->chain_slc_die_reroute_count++;
	}

	ppa.ppa = 0;
	ppa.g.ch = wp->ch;
	ppa.g.lun = wp->lun;
	ppa.g.pg = wp->pg;
	ppa.g.blk = wp->blk;
	ppa.g.pl = wp->pl;
	spin_unlock(&conv_ftl->slc_lock);

	pg = get_pg(conv_ftl->ssd, &ppa);
	if (!pg)
		return (struct ppa){ .ppa = UNMAPPED_PPA };

	if (pg->status != PG_FREE) {
		advance_chain_slc_write_pointer(conv_ftl, chain_id, die);
		goto retry_get_page;
	}

	return ppa;
}

static struct ppa get_new_slc_page(struct conv_ftl *conv_ftl)
{
	struct ppa ppa;
	struct nand_page *pg;
	uint32_t die, tried;
	uint32_t die_count;
	struct write_pointer *wp;
	struct line_mgmt *lm;

	if (!conv_ftl || !conv_ftl->slc_lunwp) {
		NVMEV_ERROR("SLC lines or write pointers not initialized\n");
		return (struct ppa){ .ppa = UNMAPPED_PPA };
	}

	die_count = conv_ftl->die_count ? conv_ftl->die_count : 1;

	for (tried = 0; tried < die_count; tried++) {
		die = (conv_ftl->lunpointer + tried) % die_count;
		wp = &conv_ftl->slc_lunwp[die];
		lm = get_slc_die_lm(conv_ftl, die);
		if (!lm || !lm->lines)
			continue;

		if (!wp->curline) {
			spin_lock(&conv_ftl->slc_lock);
			(void)slc_open_aligned_wps_locked(conv_ftl,
							  conv_ftl->slc_lunwp,
							  true);
			spin_unlock(&conv_ftl->slc_lock);
			if (!wp->curline)
				continue;
		}

		if (tried > 0)
			conv_ftl->lunpointer = die;

retry_get_page:
		ppa.ppa = 0;
		ppa.g.ch = wp->ch;
		ppa.g.lun = wp->lun;
		ppa.g.pg = wp->pg;
		ppa.g.blk = wp->blk;
		ppa.g.pl = wp->pl;

		pg = get_pg(conv_ftl->ssd, &ppa);
		if (!pg)
			continue;

		if (pg->status != PG_FREE) {
			advance_slc_write_pointer(conv_ftl, die);
			if (!wp->curline)
				continue;
			goto retry_get_page;
		}

		return ppa;
	}

	NVMEV_ERROR("No free SLC line on any die!\n");
	return (struct ppa){ .ppa = UNMAPPED_PPA };
}

/* 推进 SLC 写指针 - 使用 Die Affinity */
static void advance_slc_write_pointer(struct conv_ftl *conv_ftl, uint32_t die)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct write_pointer *wp;
	struct line_mgmt *lm;
	uint32_t blk_filled = 0;
	bool die_filled = false;

	if (!conv_ftl->slc_lunwp) {
		NVMEV_ERROR("advance_slc_write_pointer called before SLC write pointers were initialized\n");
		return;
	}

	if (conv_ftl->die_count == 0)
		return;

	die %= conv_ftl->die_count;
	wp = &conv_ftl->slc_lunwp[die];
	lm = get_slc_die_lm(conv_ftl, die);
	if (!wp->curline) {
		NVMEV_ERROR("advance_slc_write_pointer: SLC WP for die %u not initialized\n", die);
		return;
	}
	if (!lm || !lm->lines) {
		NVMEV_ERROR("advance_slc_write_pointer: missing line manager for die %u\n", die);
		return;
	}

	wp->pg++;

	if (wp->pg >= spp->pgs_per_blk) {
		blk_filled = wp->blk;
		die_filled = true;
		spin_lock(&conv_ftl->slc_lock);
		if (wp->curline->vpc == spp->pgs_per_lun_line) {
			list_add_tail(&wp->curline->entry, &lm->full_line_list);
			lm->full_line_cnt++;
		} else {
			wp->curline->pos = 0;
		}

		wp->curline = NULL;
		wp->blk = 0;
		wp->pg = 0;
		wp->pl = 0;
		if (die_filled)
			slc_sb_note_die_full_locked(conv_ftl, blk_filled, die);
		spin_unlock(&conv_ftl->slc_lock);
	}

	if ((wp->pg % spp->pgs_per_oneshotpg) == 0)
		conv_ftl->lunpointer = (die + 1) % conv_ftl->die_count;
}

/* GC 专用 SLC 页面获取, 支持 die fallback */
static struct ppa get_new_gc_slc_page(struct conv_ftl *conv_ftl, uint32_t die)
{
	struct ppa ppa;
	struct write_pointer *wp;
	struct line_mgmt *lm;
	struct nand_page *pg;
	uint32_t tried;
	uint32_t die_count;

	if (!conv_ftl->gc_slc_lunwp || conv_ftl->die_count == 0)
		return (struct ppa){ .ppa = UNMAPPED_PPA };

	die_count = conv_ftl->die_count;

	for (tried = 0; tried < die_count; tried++) {
		uint32_t candidate = (die + tried) % die_count;

		wp = &conv_ftl->gc_slc_lunwp[candidate];
		lm = get_slc_die_lm(conv_ftl, candidate);
		if (!lm || !lm->lines)
			continue;

		if (!wp->curline) {
			spin_lock(&conv_ftl->slc_lock);
			(void)slc_open_aligned_wps_locked(conv_ftl,
							  conv_ftl->gc_slc_lunwp,
							  false);
			spin_unlock(&conv_ftl->slc_lock);
			if (!wp->curline)
				continue;
		}

retry_gc_get_page:
		ppa.ppa = 0;
		ppa.g.ch = wp->ch;
		ppa.g.lun = wp->lun;
		ppa.g.pg = wp->pg;
		ppa.g.blk = wp->blk;
		ppa.g.pl = wp->pl;

		pg = get_pg(conv_ftl->ssd, &ppa);
		if (!pg)
			continue;

		if (pg->status != PG_FREE) {
			advance_gc_slc_write_pointer(conv_ftl, candidate);
			if (!wp->curline)
				continue;
			goto retry_gc_get_page;
		}

		return ppa;
	}

	return (struct ppa){ .ppa = UNMAPPED_PPA };
}

/* 新增：GC 专用 SLC 写指针推进（使用 per-die GC 写指针） */
static void advance_gc_slc_write_pointer(struct conv_ftl *conv_ftl, uint32_t die)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct write_pointer *wp;
	struct line_mgmt *lm;
	uint32_t blk_filled = 0;
	bool die_filled = false;

	if (!conv_ftl->gc_slc_lunwp)
		return;
	if (conv_ftl->die_count == 0)
		return;

	die %= conv_ftl->die_count;
	wp = &conv_ftl->gc_slc_lunwp[die];
	lm = get_slc_die_lm(conv_ftl, die);
	if (!wp->curline) {
		NVMEV_ERROR("advance_gc_slc_write_pointer: GC WP for die %u not initialized\n", die);
		return;
	}
	if (!lm || !lm->lines) {
		NVMEV_ERROR("advance_gc_slc_write_pointer: missing line manager for die %u\n", die);
		return;
	}

	wp->pg++;
	if (wp->pg >= spp->pgs_per_blk) {
		blk_filled = wp->blk;
		die_filled = true;
		spin_lock(&conv_ftl->slc_lock);
		if (wp->curline->vpc == spp->pgs_per_lun_line) {
			list_add_tail(&wp->curline->entry, &lm->full_line_list);
			lm->full_line_cnt++;
		} else {
			wp->curline->pos = 0;
		}

		wp->curline = NULL;
		wp->blk = 0;
		wp->pg = 0;
		wp->pl = 0;
		if (die_filled)
			slc_sb_note_die_full_locked(conv_ftl, blk_filled, die);
		spin_unlock(&conv_ftl->slc_lock);
	}

	if ((wp->pg % spp->pgs_per_oneshotpg) == 0)
		conv_ftl->lunpointer = (die + 1) % conv_ftl->die_count;
}

static void qlc_reset_die_progress(struct write_pointer *wp)
{
	if (!wp)
		return;

	wp->pg = 0;
	wp->pl = 0;
}

static void qlc_reset_line_accounting(struct line *line)
{
	memset(line->zone_written, 0, sizeof(line->zone_written));
	line->ipc = 0;
	line->vpc = 0;
}

static uint32_t qlc_line_written_pages(const struct line *line)
{
	uint32_t zone;
	uint32_t written = 0;

	if (!line)
		return 0;
	for (zone = 0; zone < QLC_ZONE_COUNT; zone++)
		written += line->zone_written[zone];
	return written;
}

static void qlc_closed_repromote_note_open_locked(struct conv_ftl *conv_ftl,
						  uint32_t blk)
{
	uint32_t idx;

	if (!conv_ftl || !conv_ftl->qlc_closed_sb_die_mask ||
	    !conv_ftl->qlc_closed_repromote_queued ||
	    blk < conv_ftl->slc_blks_per_pl)
		return;

	idx = blk - conv_ftl->slc_blks_per_pl;
	if (idx >= conv_ftl->qlc_blks_per_pl)
		return;

	conv_ftl->qlc_closed_sb_die_mask[idx] = 0;
	conv_ftl->qlc_closed_repromote_queued[idx] = 0;
}

static bool qlc_closed_repromote_note_die_closed_locked(struct conv_ftl *conv_ftl,
							uint32_t die, uint32_t blk)
{
	uint32_t slot;
	uint32_t idx;
	uint32_t die_bit;
	uint32_t full_mask;

	if (!conv_ftl || !conv_ftl->qlc_closed_repromote_blk ||
	    !conv_ftl->qlc_closed_sb_die_mask ||
	    !conv_ftl->qlc_closed_repromote_queued ||
	    !conv_ftl->qlc_closed_repromote_size ||
	    !conv_ftl->die_count || die >= conv_ftl->die_count ||
	    blk < conv_ftl->slc_blks_per_pl)
		return false;

	idx = blk - conv_ftl->slc_blks_per_pl;
	if (idx >= conv_ftl->qlc_blks_per_pl)
		return false;
	if (die >= 32)
		return false;

	die_bit = 1U << die;
	conv_ftl->qlc_closed_sb_die_mask[idx] |= die_bit;
	full_mask = (conv_ftl->die_count >= 32) ? U32_MAX :
		((1U << conv_ftl->die_count) - 1U);
	if ((conv_ftl->qlc_closed_sb_die_mask[idx] & full_mask) != full_mask ||
	    conv_ftl->qlc_closed_repromote_queued[idx])
		return false;

	if (conv_ftl->qlc_closed_repromote_count >=
	    conv_ftl->qlc_closed_repromote_size) {
		uint32_t old_blk = conv_ftl->qlc_closed_repromote_blk[
			conv_ftl->qlc_closed_repromote_head];

		if (old_blk >= conv_ftl->slc_blks_per_pl) {
			uint32_t old_idx = old_blk - conv_ftl->slc_blks_per_pl;

			if (old_idx < conv_ftl->qlc_blks_per_pl)
				conv_ftl->qlc_closed_repromote_queued[old_idx] = 0;
		}
		conv_ftl->qlc_closed_repromote_head =
			(conv_ftl->qlc_closed_repromote_head + 1) %
			conv_ftl->qlc_closed_repromote_size;
		conv_ftl->qlc_closed_repromote_count--;
		conv_ftl->qlc_closed_repromote_drops++;
	}

	slot = conv_ftl->qlc_closed_repromote_tail;
	conv_ftl->qlc_closed_repromote_blk[slot] = blk;
	conv_ftl->qlc_closed_repromote_queued[idx] = 1;
	conv_ftl->qlc_closed_repromote_tail =
		(slot + 1) % conv_ftl->qlc_closed_repromote_size;
	conv_ftl->qlc_closed_repromote_count++;
	conv_ftl->qlc_closed_repromote_since_scan++;
	conv_ftl->qlc_closed_repromote_enqueues++;

	if (conv_ftl->qlc_closed_repromote_since_scan >=
	    QLC_CLOSED_REPROMOTE_TRIGGER)
		return true;
	return false;
}

static bool qlc_blk_to_idx(struct conv_ftl *conv_ftl, uint32_t blk, uint32_t *idx)
{
	if (!conv_ftl || !idx || blk < conv_ftl->slc_blks_per_pl)
		return false;
	*idx = blk - conv_ftl->slc_blks_per_pl;
	return *idx < conv_ftl->qlc_blks_per_pl;
}

static int qlc_find_free_sb_blk_locked(struct conv_ftl *conv_ftl)
{
	struct line_mgmt *lm0;
	struct line *line0;
	uint32_t die_count = conv_ftl ? conv_ftl->die_count : 0;
	uint32_t die;

	if (!conv_ftl || !die_count || !conv_ftl->qlc_lunlm)
		return -1;

	lm0 = get_qlc_die_lm(conv_ftl, 0);
	if (!lm0)
		return -1;

	list_for_each_entry(line0, &lm0->free_line_list, entry) {
		uint32_t blk = line0->id;
		uint32_t idx;
		bool all_free = true;

		if (!qlc_blk_to_idx(conv_ftl, blk, &idx))
			continue;
		if (conv_ftl->qlc_sb_state &&
		    conv_ftl->qlc_sb_state[idx] == NVMEV_SB_ACTIVE)
			continue;

		for (die = 1; die < die_count && all_free; die++) {
			struct line_mgmt *lm = get_qlc_die_lm(conv_ftl, die);
			struct line *line;
			bool found = false;

			if (!lm) {
				all_free = false;
				break;
			}
			list_for_each_entry(line, &lm->free_line_list, entry) {
				if ((uint32_t)line->id == blk) {
					found = true;
					break;
				}
			}
			if (!found)
				all_free = false;
		}

		if (all_free)
			return (int)blk;
	}
	return -1;
}

static bool qlc_open_chain_active_sb_locked(struct conv_ftl *conv_ftl,
					    uint32_t chain_id, uint32_t blk)
{
	struct write_pointer *wps;
	struct ssdparams *spp;
	uint32_t die;
	uint32_t die_count;
	uint32_t idx;

	if (!conv_ftl || !chain_id_valid(conv_ftl, chain_id) ||
	    !conv_ftl->chain_host_qlc_wps || !conv_ftl->ssd ||
	    !qlc_blk_to_idx(conv_ftl, blk, &idx))
		return false;

	wps = conv_ftl->chain_host_qlc_wps[chain_id];
	if (!wps)
		return false;

	for (die = 0; die < conv_ftl->die_count; die++) {
		if (wps[die].curline)
			return false;
	}
	if (conv_ftl->qlc_active_sb_count >= NVMEV_SUPERBLOCK_ACTIVE_LIMIT)
		return false;

	spp = &conv_ftl->ssd->sp;
	die_count = conv_ftl->die_count;
	if (!die_count)
		return false;

	for (die = 0; die < die_count; die++) {
		struct line_mgmt *lm = get_qlc_die_lm(conv_ftl, die);
		struct line *line;
		bool found = false;

		if (!lm)
			goto rollback;
		list_for_each_entry(line, &lm->free_line_list, entry) {
			if ((uint32_t)line->id == blk) {
				found = true;
				break;
			}
		}
		if (!found)
			goto rollback;

		list_del_init(&line->entry);
		lm->free_line_cnt--;
		qlc_reset_line_accounting(line);
		decode_die(spp, die, &wps[die].ch, &wps[die].lun);
		wps[die].curline = line;
		wps[die].blk = blk;
		wps[die].pg = 0;
		wps[die].pl = 0;
	}

	qlc_closed_repromote_note_open_locked(conv_ftl, blk);
	if (conv_ftl->qlc_sb_state)
		conv_ftl->qlc_sb_state[idx] = NVMEV_SB_ACTIVE;
	if (conv_ftl->qlc_sb_owner_chain)
		conv_ftl->qlc_sb_owner_chain[idx] = chain_id;
	if (conv_ftl->qlc_sb_die_closed_mask)
		conv_ftl->qlc_sb_die_closed_mask[idx] = 0;
	if (conv_ftl->qlc_sb_active_counted &&
	    !conv_ftl->qlc_sb_active_counted[idx]) {
		conv_ftl->qlc_sb_active_counted[idx] = 1;
		conv_ftl->qlc_active_sb_count++;
	}
	if (conv_ftl->chain_cur_active_qlc_sb)
		conv_ftl->chain_cur_active_qlc_sb[chain_id] = blk;
	return true;

rollback:
	while (die--) {
		struct line_mgmt *lm = get_qlc_die_lm(conv_ftl, die);

		if (!lm || !wps[die].curline)
			continue;
		list_add_tail(&wps[die].curline->entry, &lm->free_line_list);
		lm->free_line_cnt++;
		wps[die].curline = NULL;
		wps[die].blk = 0;
		wps[die].pg = 0;
		wps[die].pl = 0;
	}
	return false;
}

static struct write_pointer *resolve_chain_active_qlc_wp_locked(struct conv_ftl *conv_ftl,
								uint32_t chain_id,
								uint32_t die)
{
	uint32_t cur_blk;
	uint32_t idx;
	uint32_t owner;
	struct write_pointer *wps;

	if (!conv_ftl || !chain_id_valid(conv_ftl, chain_id) ||
	    !conv_ftl->chain_cur_active_qlc_sb || !conv_ftl->qlc_sb_owner_chain ||
	    !conv_ftl->chain_host_qlc_wps || !conv_ftl->die_count)
		return NULL;

	cur_blk = conv_ftl->chain_cur_active_qlc_sb[chain_id];
	if (cur_blk == U32_MAX || !qlc_blk_to_idx(conv_ftl, cur_blk, &idx))
		return NULL;
	if (conv_ftl->qlc_sb_state &&
	    conv_ftl->qlc_sb_state[idx] != NVMEV_SB_ACTIVE)
		return NULL;
	owner = conv_ftl->qlc_sb_owner_chain[idx];
	if (!chain_id_valid(conv_ftl, owner))
		return NULL;
	wps = conv_ftl->chain_host_qlc_wps[owner];
	if (!wps)
		return NULL;
	return &wps[die % conv_ftl->die_count];
}

static uint32_t qlc_pick_share_target_locked(struct conv_ftl *conv_ftl,
					     uint32_t self_chain_id)
{
	uint32_t idx;
	uint32_t best_blk = U32_MAX;
	uint64_t best_fill = 0;

	if (!conv_ftl || !conv_ftl->qlc_sb_state ||
	    !conv_ftl->qlc_sb_owner_chain || !conv_ftl->chain_host_qlc_wps)
		return U32_MAX;

	for (idx = 0; idx < conv_ftl->qlc_blks_per_pl; idx++) {
		uint32_t owner;
		struct write_pointer *wps;
		uint64_t fill = 0;
		uint32_t die;

		if (conv_ftl->qlc_sb_state[idx] != NVMEV_SB_ACTIVE)
			continue;
		owner = conv_ftl->qlc_sb_owner_chain[idx];
		if (owner == self_chain_id || !chain_id_valid(conv_ftl, owner))
			continue;
		wps = conv_ftl->chain_host_qlc_wps[owner];
		if (!wps)
			continue;
		for (die = 0; die < conv_ftl->die_count; die++) {
			if (wps[die].curline)
				fill += (uint32_t)wps[die].curline->vpc +
					(uint32_t)wps[die].curline->ipc;
			else
				fill += conv_ftl->qlc_pgs_per_blk;
		}
		if (fill > best_fill) {
			best_fill = fill;
			best_blk = conv_ftl->slc_blks_per_pl + idx;
		}
	}
	return best_blk;
}

static bool chain_ensure_qlc_superblock_slot_locked(struct conv_ftl *conv_ftl,
						    uint32_t chain_id)
{
	uint32_t cur_blk;
	uint32_t idx;
	int free_blk;
	uint32_t share_blk;

	if (!conv_ftl || !chain_id_valid(conv_ftl, chain_id))
		return false;

	if (conv_ftl->chain_cur_active_qlc_sb) {
		cur_blk = conv_ftl->chain_cur_active_qlc_sb[chain_id];
		if (cur_blk != U32_MAX && qlc_blk_to_idx(conv_ftl, cur_blk, &idx) &&
		    conv_ftl->qlc_sb_state &&
		    conv_ftl->qlc_sb_state[idx] == NVMEV_SB_ACTIVE)
			return true;
		if (cur_blk != U32_MAX)
			conv_ftl->chain_cur_active_qlc_sb[chain_id] = U32_MAX;
	}

	if (conv_ftl->qlc_active_sb_count < NVMEV_SUPERBLOCK_ACTIVE_LIMIT) {
		free_blk = qlc_find_free_sb_blk_locked(conv_ftl);
		if (free_blk >= 0 &&
		    qlc_open_chain_active_sb_locked(conv_ftl, chain_id,
						    (uint32_t)free_blk))
			return true;
	}

	share_blk = qlc_pick_share_target_locked(conv_ftl, chain_id);
	if (share_blk != U32_MAX && conv_ftl->chain_cur_active_qlc_sb) {
		conv_ftl->chain_cur_active_qlc_sb[chain_id] = share_blk;
		return true;
	}
	return false;
}

static bool qlc_sb_note_die_closed_locked(struct conv_ftl *conv_ftl,
					  uint32_t blk, uint32_t die,
					  bool nonempty)
{
	uint32_t idx;
	uint16_t full_mask;
	bool kick_repromote = false;

	if (!conv_ftl || !conv_ftl->die_count || die >= conv_ftl->die_count ||
	    !qlc_blk_to_idx(conv_ftl, blk, &idx))
		return false;
	(void)nonempty;

	kick_repromote =
		qlc_closed_repromote_note_die_closed_locked(conv_ftl, die, blk);
	if (!conv_ftl->qlc_sb_state || !conv_ftl->qlc_sb_die_closed_mask)
		return kick_repromote;

	conv_ftl->qlc_sb_die_closed_mask[idx] |= (uint16_t)(1u << die);
	full_mask = (conv_ftl->die_count >= 16) ? 0xFFFFu :
		(uint16_t)((1u << conv_ftl->die_count) - 1u);
	if ((conv_ftl->qlc_sb_die_closed_mask[idx] & full_mask) != full_mask)
		return kick_repromote;

	if (conv_ftl->qlc_sb_state[idx] == NVMEV_SB_ACTIVE) {
		uint32_t chain_id;

		conv_ftl->qlc_sb_state[idx] = NVMEV_SB_CLOSED;
		if (conv_ftl->qlc_sb_active_counted &&
		    conv_ftl->qlc_sb_active_counted[idx]) {
			conv_ftl->qlc_sb_active_counted[idx] = 0;
			if (conv_ftl->qlc_active_sb_count)
				conv_ftl->qlc_active_sb_count--;
		}
		if (conv_ftl->chain_cur_active_qlc_sb) {
			for (chain_id = 0; chain_id < conv_ftl->next_chain_id; chain_id++) {
				if (conv_ftl->chain_cur_active_qlc_sb[chain_id] == blk)
					conv_ftl->chain_cur_active_qlc_sb[chain_id] =
						U32_MAX;
			}
		}
	}
	return kick_repromote;
}

static bool __maybe_unused qlc_closed_repromote_pop(struct conv_ftl *conv_ftl,
						    uint32_t *blk)
{
	uint32_t slot;

	if (!conv_ftl || !blk || !conv_ftl->qlc_closed_repromote_blk ||
	    !conv_ftl->qlc_closed_repromote_queued ||
	    !conv_ftl->qlc_closed_repromote_size)
		return false;

	spin_lock(&conv_ftl->qlc_lock);
	while (conv_ftl->qlc_closed_repromote_count) {
		uint32_t idx;

		slot = conv_ftl->qlc_closed_repromote_head;
		*blk = conv_ftl->qlc_closed_repromote_blk[slot];
		conv_ftl->qlc_closed_repromote_head =
			(slot + 1) % conv_ftl->qlc_closed_repromote_size;
		conv_ftl->qlc_closed_repromote_count--;
		if (*blk < conv_ftl->slc_blks_per_pl)
			continue;
		idx = *blk - conv_ftl->slc_blks_per_pl;
		if (idx >= conv_ftl->qlc_blks_per_pl ||
		    !conv_ftl->qlc_closed_repromote_queued[idx])
			continue;
		conv_ftl->qlc_closed_repromote_queued[idx] = 0;
		conv_ftl->qlc_closed_repromote_dequeues++;
		spin_unlock(&conv_ftl->qlc_lock);
		return true;
	}
	spin_unlock(&conv_ftl->qlc_lock);
	return false;
}

static bool qlc_closed_repromote_take_trigger(struct conv_ftl *conv_ftl)
{
	bool ready = false;

	if (!conv_ftl || !conv_ftl->qlc_closed_repromote_queued ||
	    !conv_ftl->qlc_blks_per_pl)
		return false;

	spin_lock(&conv_ftl->qlc_lock);
	if (conv_ftl->qlc_closed_repromote_since_scan >=
	    QLC_CLOSED_REPROMOTE_TRIGGER) {
		ready = true;
		conv_ftl->qlc_closed_repromote_dequeues +=
			conv_ftl->qlc_closed_repromote_count;
		conv_ftl->qlc_closed_repromote_head = 0;
		conv_ftl->qlc_closed_repromote_tail = 0;
		conv_ftl->qlc_closed_repromote_count = 0;
		conv_ftl->qlc_closed_repromote_since_scan = 0;
		memset(conv_ftl->qlc_closed_repromote_queued, 0,
		       sizeof(*conv_ftl->qlc_closed_repromote_queued) *
		       conv_ftl->qlc_blks_per_pl);
	}
	spin_unlock(&conv_ftl->qlc_lock);
	return ready;
}

static void __maybe_unused qlc_advance_die_cursor(struct conv_ftl *conv_ftl,
						  struct write_pointer *wp)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	if (!wp)
		return;

	wp->ch++;
	if (wp->ch >= (uint32_t)spp->nchs) {
		wp->ch = 0;
		wp->lun++;
		if (wp->lun >= (uint32_t)spp->luns_per_ch)
			wp->lun = 0;
	}
}

static struct line *qlc_ensure_active_line(struct conv_ftl *conv_ftl,
				   struct write_pointer *wp,
				   struct line_mgmt *lm,
				   uint32_t die)
{
	struct line *line;
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	if (!lm)
		return NULL;

	if (wp->curline) {
		if (wp->curline < lm->lines || wp->curline >= (lm->lines + lm->tt_lines))
			wp->curline = NULL;
		else
			return wp->curline;
	}

	spin_lock(&conv_ftl->qlc_lock);
	line = list_first_entry_or_null(&lm->free_line_list, struct line, entry);
	if (!line) {
		spin_unlock(&conv_ftl->qlc_lock);
		return NULL;
	}

	list_del_init(&line->entry);
	lm->free_line_cnt--;
	qlc_reset_line_accounting(line);
	qlc_closed_repromote_note_open_locked(conv_ftl, line->id);

	wp->curline = line;
	wp->blk = blk_from_line(line->id);
	qlc_reset_die_progress(wp);
	decode_die(spp, die, &wp->ch, &wp->lun);
	spin_unlock(&conv_ftl->qlc_lock);

	return line;
}

static void qlc_close_active_line(struct conv_ftl *conv_ftl, struct write_pointer *wp,
				  struct line_mgmt *lm, uint32_t die)
{
	struct line *line;
	uint32_t written_pages;
	uint64_t full_threshold;
	bool kick_repromote = false;

	if (!wp || !wp->curline)
		return;

	line = wp->curline;
	full_threshold = conv_ftl->qlc_pgs_per_blk;
	written_pages = qlc_line_written_pages(line);

	spin_lock(&conv_ftl->qlc_lock);
	if (written_pages >= full_threshold && line->vpc >= full_threshold) {
		list_add_tail(&line->entry, &lm->full_line_list);
		lm->full_line_cnt++;
	} else {
		pqueue_insert(lm->victim_line_pq, line);
		lm->victim_line_cnt++;
	}
	if (written_pages || line->vpc || line->ipc)
		kick_repromote = qlc_sb_note_die_closed_locked(conv_ftl, wp->blk,
							       die, true);
	wp->curline = NULL;
	qlc_reset_die_progress(wp);
	spin_unlock(&conv_ftl->qlc_lock);

	if (kick_repromote && conv_ftl->bg_migration_wq)
		queue_work(conv_ftl->bg_migration_wq, &conv_ftl->repromotion_work);
}

static void qlc_record_page_write(struct conv_ftl *conv_ftl, struct write_pointer *wp,
				  struct line_mgmt *lm, uint32_t die)
{
	if (!conv_ftl || !wp)
		return;

	wp->pg++;
	if (wp->pg >= (uint32_t)conv_ftl->qlc_pgs_per_blk ||
	    (wp->curline &&
	     qlc_line_written_pages(wp->curline) >= conv_ftl->qlc_pgs_per_blk)) {
		wp->pg = 0;
		qlc_close_active_line(conv_ftl, wp, lm, die);
	}
}

static void qlc_build_type_priority(uint32_t preferred, uint32_t *order)
{
	static const uint32_t hot_order[QLC_PAGE_PATTERN] = {
		QLC_PAGE_TYPE_L,
		QLC_PAGE_TYPE_CL,
		QLC_PAGE_TYPE_CU,
		QLC_PAGE_TYPE_U,
	};
	static const uint32_t cold_order[QLC_PAGE_PATTERN] = {
		QLC_PAGE_TYPE_CU,
		QLC_PAGE_TYPE_U,
		QLC_PAGE_TYPE_L,
		QLC_PAGE_TYPE_CL,
	};
	const uint32_t *src = (preferred % QLC_PAGE_PATTERN) < 2 ? hot_order : cold_order;

	memcpy(order, src, sizeof(uint32_t) * QLC_PAGE_PATTERN);
}

static int qlc_try_allocate_zone(struct conv_ftl *conv_ftl, struct write_pointer *wp,
				 struct line *line, uint32_t zone, struct ppa *ppa_out)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint32_t pg_idx;
	uint32_t step = QLC_PAGE_PATTERN;
	uint32_t type = zone % QLC_PAGE_PATTERN;

	for (pg_idx = type; pg_idx < conv_ftl->qlc_pgs_per_blk; pg_idx += step) {
		struct ppa candidate;
		struct nand_page *page;

		candidate.ppa = 0;
		candidate.g.ch = wp->ch;
		candidate.g.lun = wp->lun;
		candidate.g.blk = wp->blk;
		candidate.g.pg = pg_idx;
		candidate.g.pl = 0;

		page = get_pg(conv_ftl->ssd, &candidate);
		if (!page)
			continue;
		if (page->status == PG_FREE) {
			*ppa_out = candidate;
			if (zone < QLC_ZONE_COUNT)
				line->zone_written[zone]++;
			wp->pg = candidate.g.pg;
			return 0;
		}
	}

	return -ENOSPC;
}

static int qlc_get_new_chain_page(struct conv_ftl *conv_ftl, uint32_t chain_id,
				  uint32_t die, uint32_t zone_hint,
				  struct ppa *ppa_out)
{
	uint32_t tried;
	uint32_t die_count;

	if (!conv_ftl || !ppa_out || !chain_id_valid(conv_ftl, chain_id) ||
	    !conv_ftl->die_count)
		return -ENOSPC;

	die_count = conv_ftl->die_count;
	die %= die_count;

	for (tried = 0; tried < die_count; tried++) {
		uint32_t candidate = (die + tried) % die_count;
		uint32_t type_order[QLC_PAGE_PATTERN];
		struct write_pointer *wp;
		struct line_mgmt *lm;
		uint32_t type_idx;
		bool ensured;

		lm = get_qlc_die_lm(conv_ftl, candidate);
		if (!lm || !lm->lines)
			continue;
		(void)get_chain_host_qlc_wp(conv_ftl, chain_id, candidate, true);

		spin_lock(&conv_ftl->qlc_lock);
		ensured = chain_ensure_qlc_superblock_slot_locked(conv_ftl, chain_id);
		wp = ensured ? resolve_chain_active_qlc_wp_locked(conv_ftl, chain_id,
								  candidate) : NULL;
		spin_unlock(&conv_ftl->qlc_lock);
		if (!ensured)
			return -ENOSPC;
		if (!wp || !wp->curline)
			continue;
		if (qlc_line_written_pages(wp->curline) >= conv_ftl->qlc_pgs_per_blk) {
			qlc_close_active_line(conv_ftl, wp, lm, candidate);
			continue;
		}

		qlc_build_type_priority(zone_hint, type_order);
		for (type_idx = 0; type_idx < QLC_PAGE_PATTERN; type_idx++) {
			if (qlc_try_allocate_zone(conv_ftl, wp, wp->curline,
						  type_order[type_idx], ppa_out) == 0) {
				qlc_record_page_write(conv_ftl, wp, lm, candidate);
				return 0;
			}
		}

		qlc_close_active_line(conv_ftl, wp, lm, candidate);
	}

	return -ENOSPC;
}

static int qlc_do_allocate(struct conv_ftl *conv_ftl, struct write_pointer *wp,
			   struct line_mgmt *lm, uint32_t die, uint32_t zone_hint,
			   struct ppa *ppa_out)
{
	uint32_t type_order[QLC_PAGE_PATTERN];
	uint32_t type_idx;
	uint32_t base_attempts = 0;

	if (!ppa_out || !lm)
		return -EINVAL;

	base_attempts = lm->free_line_cnt + (wp->curline ? 1 : 0);

	qlc_build_type_priority(zone_hint, type_order);

	for (type_idx = 0; type_idx < QLC_PAGE_PATTERN; type_idx++) {
		uint32_t type = type_order[type_idx];
		uint32_t attempts = base_attempts ? base_attempts : 1;

		while (attempts--) {
			struct line *line = qlc_ensure_active_line(conv_ftl, wp, lm, die);

			if (!line)
				break;

			if (qlc_try_allocate_zone(conv_ftl, wp, line, type, ppa_out) == 0) {
				qlc_record_page_write(conv_ftl, wp, lm, die);
				return 0;
			}
			qlc_close_active_line(conv_ftl, wp, lm, die);
		}
	}

	return -ENOSPC;
}

static int qlc_get_new_page(struct conv_ftl *conv_ftl, uint32_t chain_id,
			    uint32_t die, uint32_t zone_hint, struct ppa *ppa_out)
{
	struct line_mgmt *lm;
	struct write_pointer *wp;

	if (!conv_ftl || !conv_ftl->die_count)
		return -ENOSPC;
	if (chain_id_valid(conv_ftl, chain_id))
		return qlc_get_new_chain_page(conv_ftl, chain_id, die, zone_hint,
					      ppa_out);
	die %= conv_ftl->die_count;

	lm = get_qlc_die_lm(conv_ftl, die);
	wp = get_qlc_die_wp(conv_ftl, die, false);
	if (!lm || !lm->lines || !wp)
		return -ENOSPC;

	qlc_prepare_die_wp(conv_ftl, wp, lm, die);
	return qlc_do_allocate(conv_ftl, wp, lm, die, zone_hint, ppa_out);
}

static int qlc_get_new_gc_page(struct conv_ftl *conv_ftl, uint32_t die, uint32_t zone_hint,
			       struct ppa *ppa_out)
{
	struct line_mgmt *lm;
	struct write_pointer *wp;

	if (!conv_ftl || !conv_ftl->die_count)
		return -ENOSPC;

	die %= conv_ftl->die_count;
	lm = get_qlc_die_lm(conv_ftl, die);
	wp = get_qlc_die_wp(conv_ftl, die, true);
	if (!lm || !lm->lines || !wp)
		return -ENOSPC;

	qlc_prepare_die_wp(conv_ftl, wp, lm, die);
	return qlc_do_allocate(conv_ftl, wp, lm, die, zone_hint, ppa_out);
}

static uint64_t qlc_calc_promote_threshold(uint64_t avg_reads)
{
	uint64_t th;

	if (!avg_reads)
		return 0;
	if (avg_reads > div64_u64(~0ULL, QLC_PROMOTE_RATIO_NUM))
		return ~0ULL;

	th = div64_u64(avg_reads * QLC_PROMOTE_RATIO_NUM, QLC_PROMOTE_RATIO_DEN);
	return th;
}

static void qlc_count_resident_distribution(struct conv_ftl *conv_ftl,
					    uint64_t *qlc_total_pages,
					    uint64_t *qlc_fast_pages)
{
	struct ssdparams *spp;
	uint64_t lpn;
	uint64_t total = 0;
	uint64_t fast = 0;

	if (!qlc_total_pages || !qlc_fast_pages)
		return;

	if (!conv_ftl || !conv_ftl->ssd) {
		*qlc_total_pages = 0;
		*qlc_fast_pages = 0;
		return;
	}

	spp = &conv_ftl->ssd->sp;
	for (lpn = 0; lpn < spp->tt_pgs; lpn++) {
		struct ppa ppa = get_maptbl_ent(conv_ftl, lpn);
		uint8_t zone;

		if (!mapped_ppa(&ppa) || !valid_ppa(conv_ftl, &ppa))
			continue;
		if (is_slc_block(conv_ftl, ppa.g.blk))
			continue;

		zone = get_qlc_zone_for_read(conv_ftl, &ppa);
		total++;
		if (qlc_zone_is_fast(zone))
			fast++;
	}

	*qlc_total_pages = total;
	*qlc_fast_pages = fast;
}

static bool qlc_find_candidate(struct conv_ftl *conv_ftl, bool want_fast,
			       uint64_t threshold, bool strictly_greater,
			       uint64_t *cursor, uint64_t *lpn_out,
			       struct ppa *ppa_out)
{
	struct heat_tracking *ht;
	struct ssdparams *spp;
	uint64_t idx, scanned, scan_limit;

	if (!conv_ftl || !conv_ftl->ssd || !cursor || !lpn_out || !ppa_out)
		return false;

	ht = &conv_ftl->heat_track;
	if (!ht || !ht->access_count)
		return false;

	spp = &conv_ftl->ssd->sp;
	if (!spp->tt_pgs)
		return false;

	idx = *cursor % spp->tt_pgs;
	scanned = 0;
	scan_limit = min_t(uint64_t, spp->tt_pgs, (uint64_t)QLC_REBALANCE_SCAN_LIMIT);

	while (scanned < scan_limit) {
		struct ppa ppa = get_maptbl_ent(conv_ftl, idx);
		uint8_t zone;
		bool is_fast;
		uint64_t reads;

		if (mapped_ppa(&ppa) && valid_ppa(conv_ftl, &ppa) &&
		    !is_slc_block(conv_ftl, ppa.g.blk)) {
			zone = get_qlc_zone_for_read(conv_ftl, &ppa);
			is_fast = qlc_zone_is_fast(zone);
			reads = ht->access_count[idx];

			if (is_fast == want_fast) {
				bool hit = strictly_greater ? (reads > threshold) :
							    (reads <= threshold);

				if (hit) {
					*lpn_out = idx;
					*ppa_out = ppa;
					*cursor = (idx + 1) % spp->tt_pgs;
					return true;
				}
			}
		}

		scanned++;
		idx = (idx + 1) % spp->tt_pgs;
	}

	*cursor = idx;
	return false;
}

static int migrate_page_within_qlc(struct conv_ftl *conv_ftl, uint64_t lpn,
				   struct ppa *src_ppa, bool promote,
				   uint8_t *new_zone_out)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_page *src_pg, *dst_pg;
	struct ppa new_ppa;
	uint64_t prev_lpn;
	uint32_t src_die;
	uint32_t target_die;
	uint32_t zone_hint;
	uint8_t old_zone, actual_new_zone;
	unsigned long flags;
	bool test_phase_bg_tracked = false;

	if (!conv_ftl || !src_ppa || !mapped_ppa(src_ppa) || !valid_ppa(conv_ftl, src_ppa))
		return -EINVAL;
	if (is_slc_block(conv_ftl, src_ppa->g.blk))
		return -EINVAL;

	test_phase_note_bg_begin(conv_ftl, &conv_ftl->test_phase_bg_qlc_rebalance_ops,
				 &test_phase_bg_tracked);

	src_pg = get_pg(conv_ftl->ssd, src_ppa);
	if (!src_pg || src_pg->status != PG_VALID) {
		test_phase_note_bg_end(conv_ftl, test_phase_bg_tracked);
		return -EINVAL;
	}

	old_zone = src_pg->qlc_latency_zone;
	prev_lpn = src_pg->oob_prev_lpn;
	if (prev_lpn >= conv_ftl->ssd->sp.tt_pgs)
		prev_lpn = INVALID_LPN;

	src_die = encode_die(spp, src_ppa);
	target_die = internal_place_die_for_lpn(conv_ftl, lpn, src_ppa,
						 CHAIN_TIER_QLC, true);

	spin_lock_irqsave(&conv_ftl->qlc_zone_lock, flags);
	zone_hint = pick_locked_qlc_page_type(conv_ftl, promote);
	spin_unlock_irqrestore(&conv_ftl->qlc_zone_lock, flags);

	if (qlc_get_new_gc_page(conv_ftl, target_die, zone_hint, &new_ppa) != 0) {
		test_phase_note_bg_end(conv_ftl, test_phase_bg_tracked);
		return -ENOSPC;
	}

		set_maptbl_ent_reason(conv_ftl, lpn, &new_ppa, NVMEV_DIE_CHANGE_QLC_REBALANCE);
	set_rmap_ent(conv_ftl, lpn, &new_ppa);
	mark_page_invalid(conv_ftl, src_ppa);
	set_rmap_ent(conv_ftl, INVALID_LPN, src_ppa);
	mark_page_valid(conv_ftl, &new_ppa);
	set_page_prev_link(conv_ftl, lpn, &new_ppa, prev_lpn);

	dst_pg = get_pg(conv_ftl->ssd, &new_ppa);
	if (dst_pg)
		dst_pg->qlc_latency_zone = new_ppa.g.pg % QLC_PAGE_PATTERN;
	internal_place_note_lpn_write(conv_ftl, lpn, CHAIN_TIER_QLC,
				      true, encode_die(spp, &new_ppa));

	actual_new_zone = dst_pg ? dst_pg->qlc_latency_zone :
				   (new_ppa.g.pg % QLC_PAGE_PATTERN);
	if (qlc_zone_is_fast(old_zone)) {
		if (conv_ftl->qlc_fast_count > 0)
			conv_ftl->qlc_fast_count--;
	} else {
		if (conv_ftl->qlc_slow_count > 0)
			conv_ftl->qlc_slow_count--;
	}
	if (qlc_zone_is_fast(actual_new_zone))
		conv_ftl->qlc_fast_count++;
	else
		conv_ftl->qlc_slow_count++;

	if (new_zone_out)
		*new_zone_out = actual_new_zone;

	NVMEV_DEBUG("[QLC-REBAL] %s lpn=%llu src(ch=%u,lun=%u,blk=%u,pg=%u) dst(ch=%u,lun=%u,blk=%u,pg=%u) zone_hint=%u zone_new=%u",
		    promote ? "promote" : "demote",
		    lpn,
		    src_ppa->g.ch, src_ppa->g.lun, src_ppa->g.blk, src_ppa->g.pg,
		    new_ppa.g.ch, new_ppa.g.lun, new_ppa.g.blk, new_ppa.g.pg,
		    zone_hint,
		    new_zone_out ? *new_zone_out : (new_ppa.g.pg % QLC_PAGE_PATTERN));

	test_phase_note_bg_end(conv_ftl, test_phase_bg_tracked);
	return 0;
}

static void qlc_maybe_rebalance_internal(struct conv_ftl *conv_ftl)
{
	struct heat_tracking *ht;
	uint32_t promote_budget, demote_budget;
	uint64_t avg_reads, promote_th;
	uint64_t qlc_total_pages, qlc_fast_pages;
	uint64_t fast_pct;
	uint32_t promoted = 0, demoted = 0;
	uint64_t rd_seq;

	if (!conv_ftl || !conv_ftl->ssd)
		return;

	ht = &conv_ftl->heat_track;
	if (!ht || !ht->access_count)
		return;

	if (!calc_global_avg_reads(conv_ftl, &avg_reads))
		return;
	promote_th = qlc_calc_promote_threshold(avg_reads);

	rd_seq = atomic64_read(&conv_ftl->total_host_reads);
	if ((rd_seq % 1000) == 0) {
		uint64_t cal_total, cal_fast;
		qlc_count_resident_distribution(conv_ftl, &cal_total, &cal_fast);
		conv_ftl->qlc_fast_count = cal_fast;
		conv_ftl->qlc_slow_count = cal_total - cal_fast;
	}

	qlc_fast_pages = conv_ftl->qlc_fast_count;
	qlc_total_pages = conv_ftl->qlc_fast_count + conv_ftl->qlc_slow_count;
	if (!qlc_total_pages)
		return;

	promote_budget = conv_ftl->qlc_rebalance_promote_budget;
	demote_budget = conv_ftl->qlc_rebalance_demote_budget;
	fast_pct = div64_u64(qlc_fast_pages * 100, qlc_total_pages);
	if (fast_pct >= QLC_FAST_HIGH_WM_PCT)
		conv_ftl->qlc_fast_drain_active = true;

	while (conv_ftl->qlc_fast_drain_active &&
	       fast_pct > QLC_FAST_TARGET_WM_PCT &&
	       demoted < demote_budget) {
		struct ppa cand_ppa;
		uint64_t cand_lpn;
		uint8_t new_zone = QLC_PAGE_TYPE_U;

		if (!qlc_find_candidate(conv_ftl, true, avg_reads, false,
					&conv_ftl->qlc_demote_cursor,
					&cand_lpn, &cand_ppa))
			break;

		if (migrate_page_within_qlc(conv_ftl, cand_lpn, &cand_ppa, false, &new_zone) == 0) {
			demoted++;
			if (!qlc_zone_is_fast(new_zone) && qlc_fast_pages > 0)
				qlc_fast_pages--;
		}

		fast_pct = div64_u64(qlc_fast_pages * 100, qlc_total_pages);
		if (fast_pct <= QLC_FAST_TARGET_WM_PCT)
			break;
	}
	if (fast_pct <= QLC_FAST_TARGET_WM_PCT)
		conv_ftl->qlc_fast_drain_active = false;

	while (fast_pct < QLC_FAST_TARGET_WM_PCT && promoted < promote_budget) {
		struct ppa cand_ppa;
		uint64_t cand_lpn;
		uint8_t new_zone = QLC_PAGE_TYPE_CU;

		if (!qlc_find_candidate(conv_ftl, false, promote_th, true,
					&conv_ftl->qlc_promote_cursor,
					&cand_lpn, &cand_ppa))
			break;

		if (migrate_page_within_qlc(conv_ftl, cand_lpn, &cand_ppa, true, &new_zone) == 0) {
			promoted++;
			if (qlc_zone_is_fast(new_zone))
				qlc_fast_pages++;
		}

		fast_pct = div64_u64(qlc_fast_pages * 100, qlc_total_pages);
	}

	if (promoted || demoted) {
		NVMEV_DEBUG("[QLC-REBAL] avg=%llu promote_th=%llu promoted=%u demoted=%u fast=%llu/%llu(%llu%%)",
			    avg_reads, promote_th, promoted, demoted,
			    qlc_fast_pages, qlc_total_pages, fast_pct);
	}
}

/* 更新热数据信息 */
static void update_heat_info(struct conv_ftl *conv_ftl, uint64_t lpn, bool is_read)
{
	struct heat_tracking *ht;

	if (!conv_ftl)
		return;

	ht = &conv_ftl->heat_track;

	if (is_read && ht && ht->access_count && ht->last_access_time) {
		ht->access_count[lpn]++;
		ht->last_access_time[lpn] = __get_ioclock(conv_ftl->ssd);
		conv_ftl->global_read_sum++;
	}
}

/* 单页迁移函数 - 从 SLC 迁移一页到 QLC */
static int migrate_page_to_qlc(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *slc_ppa)
{
    struct ssdparams *spp = &conv_ftl->ssd->sp;
    struct ppa new_ppa;
    struct nand_cmd srd, swr;
    uint64_t nsecs_completed;
    
    if (!conv_ftl || !slc_ppa || !mapped_ppa(slc_ppa)) {
        NVMEV_ERROR("Invalid parameters for page migration\n");
        return -1;
    }

    if (!valid_ppa(conv_ftl, slc_ppa)) {
        NVMEV_ERROR("migrate_page_to_qlc: invalid SLC PPA ch=%d lun=%d blk=%d pg=%d for lpn=%llu, drop mapping\n",
                    slc_ppa->g.ch, slc_ppa->g.lun, slc_ppa->g.blk, slc_ppa->g.pg, lpn);
        clear_lpn_mapping(conv_ftl, lpn);
        return -1;
    }
    
    if (!is_slc_block(conv_ftl, slc_ppa->g.blk)) {
        NVMEV_ERROR("Page not in SLC, cannot migrate\n");
        return -1;
    }
    
    struct nand_page *pg = get_pg(conv_ftl->ssd, slc_ppa);
    if (!pg || pg->status != PG_VALID) {
        NVMEV_DEBUG("Page not valid for migration: status=%d at ch=%d,lun=%d,blk=%d,pg=%d\n",
                   pg ? pg->status : -1, slc_ppa->g.ch, slc_ppa->g.lun, slc_ppa->g.blk, slc_ppa->g.pg);
        return -1;
    }
    
	uint64_t stored_prev_lpn = pg->oob_prev_lpn;
	uint32_t src_die = encode_die(spp, slc_ppa);
	uint32_t target_die;
	struct ppa prev_ppa = { .ppa = UNMAPPED_PPA };

	struct heat_tracking *ht = &conv_ftl->heat_track;
	uint64_t read_cnt = 0;
	uint64_t migration_avg = 0;
	bool have_mig_avg;
	uint32_t zone_hint;
	uint32_t chain_id = chain_id_for_lpn(conv_ftl, lpn);
	unsigned long mig_flags;
	bool was_in_slc = false;

	if (conv_ftl->page_in_slc && lpn < conv_ftl->ssd->sp.tt_pgs)
		was_in_slc = conv_ftl->page_in_slc[lpn];

	if (ht && ht->access_count)
		read_cnt = ht->access_count[lpn];

	have_mig_avg = calc_migration_avg_reads(conv_ftl, &migration_avg);
	if (!have_mig_avg)
		migration_avg = read_cnt;

	spin_lock_irqsave(&conv_ftl->qlc_zone_lock, mig_flags);
	zone_hint = pick_locked_qlc_page_type(conv_ftl, read_cnt >= migration_avg);
	spin_unlock_irqrestore(&conv_ftl->qlc_zone_lock, mig_flags);

	if (stored_prev_lpn != INVALID_LPN && !valid_lpn(conv_ftl, stored_prev_lpn)) {
		NVMEV_WARN("migrate_page_to_qlc: bad prev_lpn=%llu (ppa ch=%d lun=%d blk=%d pg=%d status=%d), drop prev link\n",
			   stored_prev_lpn, slc_ppa->g.ch, slc_ppa->g.lun, slc_ppa->g.blk,
			   slc_ppa->g.pg, pg ? pg->status : -1);
		stored_prev_lpn = INVALID_LPN;
	}

	if (stored_prev_lpn != INVALID_LPN) {
		prev_ppa = get_maptbl_ent(conv_ftl, stored_prev_lpn);
		if (!(mapped_ppa(&prev_ppa) && valid_ppa(conv_ftl, &prev_ppa))) {
			stored_prev_lpn = INVALID_LPN;
		}
	}

	target_die = internal_place_die_for_lpn(conv_ftl, lpn, slc_ppa,
						 CHAIN_TIER_QLC, true);

	if (qlc_get_new_page(conv_ftl, chain_id, target_die, zone_hint, &new_ppa) != 0) {
		NVMEV_DEBUG("[MIGRATION_DEBUG] Failed to allocate QLC page (zone_hint=%u)\n",
			    zone_hint);
		return -1;
	}
    
    /* 读取 SLC 页面（内部迁移，跳过通道模型） */
    srd.type = GC_IO;
    srd.cmd = NAND_READ;
    srd.stime = __get_ioclock(conv_ftl->ssd);
    srd.interleave_pci_dma = false;
    srd.xfer_size = spp->pgsz;
    srd.ppa = slc_ppa;
    
    nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd);
    
    /* 写入 QLC 页面（内部迁移，跳过通道模型） */
    swr.type = GC_IO;
    swr.cmd = NAND_WRITE;
    swr.stime = nsecs_completed;
    swr.interleave_pci_dma = false;
    swr.xfer_size = spp->pgsz;
    swr.ppa = &new_ppa;
    
    ssd_advance_nand(conv_ftl->ssd, &swr);
    
    /* 更新映射表 */
    set_maptbl_ent_reason(conv_ftl, lpn, &new_ppa, NVMEV_DIE_CHANGE_SLC_TO_QLC);
	set_rmap_ent(conv_ftl, lpn, &new_ppa);
    
    /* 标记旧页面无效 */
    mark_page_invalid(conv_ftl, slc_ppa);
    set_rmap_ent(conv_ftl, INVALID_LPN, slc_ppa);
    
	/* 更新元数据 */
	slc_resident_untrack_page(conv_ftl, lpn);
	conv_ftl->page_in_slc[lpn] = false;
	chain_slc_note_state_change(conv_ftl, chain_id, was_in_slc,
				       chain_id, false, false);
	mark_page_valid(conv_ftl, &new_ppa);
	set_page_prev_link(conv_ftl, lpn, &new_ppa, stored_prev_lpn);
	update_qlc_latency_zone(conv_ftl, lpn, &new_ppa);
	internal_place_note_lpn_write(conv_ftl, lpn, CHAIN_TIER_QLC,
				      true, encode_die(spp, &new_ppa));
	spin_lock_irqsave(&conv_ftl->qlc_zone_lock, mig_flags);
	conv_ftl->qlc_migration_read_sum += read_cnt;
	conv_ftl->qlc_migration_page_cnt++;
	spin_unlock_irqrestore(&conv_ftl->qlc_zone_lock, mig_flags);

	NVMEV_DEBUG("[TASK2][MIGRATE] lpn=%llu prev_lpn=%lld src_die=%u dst_die=%u zone_hint=%u",
		  lpn,
		  stored_prev_lpn == INVALID_LPN ? -1LL : (long long)stored_prev_lpn,
		  src_die,
		  encode_die(spp, &new_ppa),
		  zone_hint);

    conv_ftl->migration_cnt++;
    
    NVMEV_DEBUG("Migrated LPN %llu from SLC to QLC (zone_hint=%u)\n", lpn, zone_hint);
    return 0;
}

/* Chain-batched repromotion: scan a slice of chain_id's LPN space starting
 * from chain_repromote_cursor[chain_id]; for each LPN currently mapped to
 * QLC with sufficiently high heat, rewrite it back to SLC. Bounded by
 * REPROMOTE_BATCH_PAGES per scan. Returns the number of pages repromoted.
 *
 * SLC capacity guard: if SLC is too tight, skip entirely so we don't trigger
 * a back-and-forth (repromote → SLC fills → SLC->QLC migrate → repromote …).
 */
static uint32_t __maybe_unused chain_repromote_scan_one(struct conv_ftl *conv_ftl,
							uint32_t chain_id)
{
	struct ssdparams *spp;
	uint64_t start, lpn, end_excl;
	uint32_t scanned = 0;
	uint32_t moved = 0;
	uint32_t batch_cap = REPROMOTE_BATCH_PAGES;
	uint32_t scan_cap = REPROMOTE_SCAN_WINDOW;
	struct line_pool_stats slc_stats;

	if (!conv_ftl || !conv_ftl->ssd || !chain_id_valid(conv_ftl, chain_id))
		return 0;
	if (!conv_ftl->lpn_chain_id || !conv_ftl->page_in_slc ||
	    !conv_ftl->chain_repromote_cursor || !conv_ftl->slc_lunlm)
		return 0;

	spp = &conv_ftl->ssd->sp;

	collect_slc_stats(conv_ftl, &slc_stats);
	if (conv_ftl->slc_repromote_guard_lines &&
	    slc_stats.free <= conv_ftl->slc_repromote_guard_lines)
		return 0;

	start = conv_ftl->chain_repromote_cursor[chain_id];
	if (start >= spp->tt_pgs)
		start = 0;
	end_excl = min_t(uint64_t, start + scan_cap, spp->tt_pgs);

	for (lpn = start; lpn < end_excl && moved < batch_cap; lpn++) {
		struct ppa cur;
		uint64_t acc;

		scanned++;
		if (conv_ftl->lpn_chain_id[lpn] != chain_id)
			continue;
		if (conv_ftl->page_in_slc[lpn])
			continue;
		cur = get_maptbl_ent(conv_ftl, lpn);
		if (!mapped_ppa(&cur) || !valid_ppa(conv_ftl, &cur))
			continue;
		if (is_slc_block(conv_ftl, cur.g.blk))
			continue;

		acc = (conv_ftl->heat_track.access_count) ?
			conv_ftl->heat_track.access_count[lpn] : 0;
		if (acc < REPROMOTE_HEAT_FLOOR)
			continue;
		if (recent_write_guard(conv_ftl, lpn))
			continue;

		migrate_page_to_slc(conv_ftl, lpn, &cur, NULL);
		moved++;
	}

	conv_ftl->chain_repromote_cursor[chain_id] =
		(lpn >= spp->tt_pgs) ? 0 : lpn;
	(void)scanned;
	return moved;
}

static void bg_repromotion_worker(struct work_struct *work)
{
	struct conv_ftl *conv_ftl = container_of(work, struct conv_ftl, repromotion_work);
#if !NVMEV_ENABLE_READ_REPROMOTION
	(void)conv_ftl;
#elif NVMEV_ENABLE_CHAIN_BLOCK_REPROMOTION
	migrate_hot_from_closed_qlc(conv_ftl);
#elif !NVMEV_ENABLE_DIE_BATCHED_REPROMOTION
	uint64_t lpn;
	struct ppa ppa, cur;
	uint64_t migration_done = 0;
	uint32_t budget = conv_ftl->repromote_budget_per_run;
	uint32_t processed = 0;

	while (processed < budget) {
		spin_lock(&conv_ftl->repromote_queue_lock);
		if (conv_ftl->repromote_head == conv_ftl->repromote_tail) {
			spin_unlock(&conv_ftl->repromote_queue_lock);
			break;
		}
		lpn = conv_ftl->repromote_lpns[conv_ftl->repromote_head];
		ppa = conv_ftl->repromote_ppas[conv_ftl->repromote_head];
		conv_ftl->repromote_head =
			(conv_ftl->repromote_head + 1) % REPROMOTE_QUEUE_SIZE;
		spin_unlock(&conv_ftl->repromote_queue_lock);

		cur = get_maptbl_ent(conv_ftl, lpn);
		if (cur.ppa == ppa.ppa && !is_slc_block(conv_ftl, cur.g.blk))
			migrate_page_to_slc(conv_ftl, lpn, &cur, &migration_done);
		processed++;
	}
#else
	uint64_t lpns[REPROMOTE_QUEUE_SIZE];
	struct ppa ppas[REPROMOTE_QUEUE_SIZE];
	bool consumed[REPROMOTE_QUEUE_SIZE] = { false };
	uint64_t migration_done = 0;
	uint32_t budget = min_t(uint32_t, conv_ftl->repromote_budget_per_run,
				 REPROMOTE_QUEUE_SIZE);
	uint32_t pulled = 0;
	uint32_t processed = 0;
	uint32_t start_slot = 0;
	uint32_t step;
	uint32_t i;

	while (pulled < budget) {
		spin_lock(&conv_ftl->repromote_queue_lock);
		if (conv_ftl->repromote_head == conv_ftl->repromote_tail) {
			spin_unlock(&conv_ftl->repromote_queue_lock);
			break;
		}
		lpns[pulled] = conv_ftl->repromote_lpns[conv_ftl->repromote_head];
		ppas[pulled] = conv_ftl->repromote_ppas[conv_ftl->repromote_head];
		conv_ftl->repromote_head =
			(conv_ftl->repromote_head + 1) % REPROMOTE_QUEUE_SIZE;
		spin_unlock(&conv_ftl->repromote_queue_lock);
		pulled++;
	}

	if (pulled)
		start_slot = conv_ftl->repromote_die_cursor % pulled;

	for (step = 0; step < pulled && processed < pulled; step++) {
		uint32_t anchor = (start_slot + step) % pulled;
		uint32_t anchor_chain;

		if (consumed[anchor])
			continue;
		anchor_chain = chain_id_for_lpn(conv_ftl, lpns[anchor]);

		for (i = 0; i < pulled; i++) {
			struct ppa cur;
			uint32_t idx = (anchor + i) % pulled;
			uint32_t cur_chain;

			if (consumed[idx])
				continue;
			cur_chain = chain_id_for_lpn(conv_ftl, lpns[idx]);
			if (idx != anchor) {
				if (!chain_id_valid(conv_ftl, anchor_chain) ||
				    cur_chain != anchor_chain)
					continue;
			}

			consumed[idx] = true;
			processed++;
			cur = get_maptbl_ent(conv_ftl, lpns[idx]);
			if (cur.ppa == ppas[idx].ppa && !is_slc_block(conv_ftl, cur.g.blk))
				migrate_page_to_slc(conv_ftl, lpns[idx], &cur, &migration_done);
		}
	}

	conv_ftl->repromote_die_cursor = pulled ? ((start_slot + 1) % pulled) : 0;
#endif
}

static void bg_qlc_rebalance_worker(struct work_struct *work)
{
#if NVMEV_ENABLE_QLC_REBALANCE
	struct conv_ftl *conv_ftl = container_of(work, struct conv_ftl, qlc_rebalance_work);

	qlc_maybe_rebalance_internal(conv_ftl);
#else
	(void)work;
#endif
}

static bool conv_read(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];
	struct conv_ftl *stats_ftl = conv_ftl;
	/* spp are shared by all instances*/
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;
	uint64_t lpn;
	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_completed, nsecs_latest = nsecs_start;
	uint32_t xfer_size, i;
	uint32_t nr_parts = ns->nr_parts;

	uint64_t rd_seq;
	struct ppa prev_ppa;
	uint64_t prev_lpn = INVALID_LPN;
	bool test_phase_read_tracked = false;
	struct nand_cmd srd = {
		.type = USER_IO,
		.cmd = NAND_READ,
		.stime = nsecs_start,
		.interleave_pci_dma = true,
	};

	NVMEV_ASSERT(conv_ftls);
	NVMEV_DEBUG("conv_read: start_lpn=%lld, len=%lld, end_lpn=%lld", start_lpn, nr_lba, end_lpn);
	if (unlikely(nr_parts == 0)) {
		NVMEV_ERROR("conv_read: nr_parts=0\n");
		ret->status = NVME_SC_INTERNAL;
		ret->nsecs_target = nsecs_start;
		return true;
	}
	if (unlikely(nr_lba == 0 || lba > U64_MAX - (nr_lba - 1))) {
		NVMEV_ERROR("conv_read: LBA overflow lba=%llu nr_lba=%llu\n", lba, nr_lba);
		ret->status = NVME_SC_LBA_RANGE;
		ret->nsecs_target = nsecs_start;
		return true;
	}
    if ((end_lpn / nr_parts) >= spp->tt_pgs) {
        NVMEV_ERROR("conv_read: lpn passed FTL range(start_lpn=%lld,tt_pgs=%ld)\n",
                    start_lpn, spp->tt_pgs);
        ret->status = NVME_SC_LBA_RANGE;
        ret->nsecs_target = nsecs_start;
        return true; /* Return completion with error to avoid host timeout */
    }

	rd_seq = atomic64_inc_return(&conv_ftl->total_host_reads);
	test_phase_note_read_begin(stats_ftl, &test_phase_read_tracked);
	if (test_phase_read_tracked && stats_ftl) {
		srd.tracked_read_die_conflicts = &stats_ftl->test_phase_read_die_conflicts;
		srd.tracked_read_die_wait_ns = &stats_ftl->test_phase_read_die_wait_ns;
	}

	if (LBA_TO_BYTE(nr_lba) <= (KB(4) * nr_parts)) {
		srd.stime += spp->fw_4kb_rd_lat;
	} else {
		srd.stime += spp->fw_rd_lat;
	}

	for (i = 0; (i < nr_parts) && (start_lpn <= end_lpn); i++, start_lpn++) {
		uint64_t avg_reads = 0;
		bool has_avg;
		struct heat_tracking *ht;

		conv_ftl = &conv_ftls[start_lpn % nr_parts];
		xfer_size = 0;
		nvmev_set_maptbl_site("conv_read.prev_ppa_init", start_lpn / nr_parts);
		prev_ppa = get_maptbl_ent(conv_ftl, start_lpn / nr_parts);
		prev_lpn = start_lpn / nr_parts;
		has_avg = calc_global_avg_reads(conv_ftl, &avg_reads);
		ht = &conv_ftl->heat_track;

		/* normal IO read path */
		for (lpn = start_lpn; lpn <= end_lpn; lpn += nr_parts) {
			uint64_t local_lpn;
			struct ppa cur_ppa;

			local_lpn = lpn / nr_parts;
			if (unlikely(local_lpn >= conv_ftl->ssd->sp.tt_pgs)) {
				NVMEV_ERROR("conv_read: BAD local_lpn=%llu lpn=%llu start_lpn=%llu end_lpn=%llu nr_parts=%u\n",
					    local_lpn, lpn, start_lpn, end_lpn, nr_parts);
				dump_stack();
				continue;
			}
			nvmev_set_maptbl_site("conv_read.cur_ppa", local_lpn);
			cur_ppa = get_maptbl_ent(conv_ftl, local_lpn);
			if (!mapped_ppa(&cur_ppa) || !valid_ppa(conv_ftl, &cur_ppa)) {
				NVMEV_DEBUG("lpn 0x%llx not mapped to valid ppa\n", local_lpn);
				NVMEV_DEBUG("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d\n",
					    cur_ppa.g.ch, cur_ppa.g.lun, cur_ppa.g.blk,
					    cur_ppa.g.pl, cur_ppa.g.pg);
				continue;
			}

			/* 更新热数据跟踪信息 */
			update_heat_info(conv_ftl, local_lpn, true);

			/* chain-triggered repromotion: count host reads per chain
			 * (only QLC reads; SLC pages are already on the fast tier).
			 * When the per-chain counter crosses REPROMOTE_READ_TRIGGER
			 * we kick the bg worker so it can scan this chain's QLC
			 * pages and batch-rewrite the hot ones to SLC. */
			if (!is_slc_block(conv_ftl, cur_ppa.g.blk) &&
			    conv_ftl->chain_host_read_count && conv_ftl->lpn_chain_id) {
				uint32_t rch = conv_ftl->lpn_chain_id[local_lpn];
				if (chain_id_valid(conv_ftl, rch)) {
					uint64_t v = ++conv_ftl->chain_host_read_count[rch];
					if (v >= REPROMOTE_READ_TRIGGER) {
						conv_ftl->chain_host_read_count[rch] = 0;
						if (conv_ftl->bg_migration_wq)
							queue_work(conv_ftl->bg_migration_wq,
								   &conv_ftl->repromotion_work);
					}
				}
			}

			if (NVMEV_ENABLE_READ_REPROMOTION &&
			    conv_ftl->enable_read_repromotion &&
			    !is_slc_block(conv_ftl, cur_ppa.g.blk) && has_avg &&
			    ht && ht->access_count) {
				uint64_t access_cnt = ht->access_count[local_lpn];

				if (access_cnt > avg_reads) {
#if NVMEV_ENABLE_CHAIN_BLOCK_REPROMOTION
					conv_ftl->migration_read_path_count++;
#else
					uint32_t next_tail;

					spin_lock(&conv_ftl->repromote_queue_lock);
					next_tail = (conv_ftl->repromote_tail + 1) %
						    REPROMOTE_QUEUE_SIZE;
					if (next_tail != conv_ftl->repromote_head) {
						conv_ftl->repromote_lpns[conv_ftl->repromote_tail] = local_lpn;
						conv_ftl->repromote_ppas[conv_ftl->repromote_tail] = cur_ppa;
						conv_ftl->repromote_tail = next_tail;
					}
					spin_unlock(&conv_ftl->repromote_queue_lock);
					conv_ftl->migration_read_path_count++;
#endif
				}
			}

				{
					struct nand_page *pg_chk;

					/* [CRITICAL FIX] 防止 GC 在我们读之前的瞬间释放了该页 */
					if (mapped_ppa(&cur_ppa)) {
						pg_chk = get_pg(conv_ftl->ssd, &cur_ppa);
						if (!pg_chk || pg_chk->status == PG_FREE ||
						    pg_chk->status == PG_INVALID) {
							NVMEV_WARN("Race detected! LPN %llu PPA ch%d-blk%d was freed by GC while reading. Retrying.\n",
								   local_lpn, cur_ppa.g.ch, cur_ppa.g.blk);
							cur_ppa.ppa = UNMAPPED_PPA;
							prev_ppa.ppa = UNMAPPED_PPA;
							prev_lpn = INVALID_LPN;
							continue;
						}
					}
				}

				// aggregate read io in same flash page
				if (mapped_ppa(&prev_ppa) &&
				    is_same_flash_page(conv_ftl, cur_ppa, prev_ppa)) {
				xfer_size += spp->pgsz;
				continue;
			}

			if (xfer_size > 0) {
				bool issue_prev = true;
				struct ppa refreshed;

				if (prev_lpn == INVALID_LPN) {
					issue_prev = false;
				} else {
					refreshed = get_maptbl_ent(conv_ftl, prev_lpn);
					if (!mapped_ppa(&refreshed) || !valid_ppa(conv_ftl, &refreshed)) {
						issue_prev = false;
					} else {
						prev_ppa = refreshed;
					}
				}

				/* 根据页面位置调整读延迟 */
				uint64_t original_stime = srd.stime;
				
				if (issue_prev) {
					/* 检查页面是否在 SLC 或 QLC 中 */
					if (is_slc_block(conv_ftl, prev_ppa.g.blk)) {
						/* SLC 读延迟 - 使用原有的延迟参数 */
						if (xfer_size == 4096) {
							srd.stime += spp->pg_4kb_rd_lat[get_cell(conv_ftl->ssd, &prev_ppa)];
						} else {
							srd.stime += spp->pg_rd_lat[get_cell(conv_ftl->ssd, &prev_ppa)];
						}
					} else {
						/* QLC 读延迟 - 使用动态区域 */
						uint8_t zone = get_qlc_zone_for_read(conv_ftl, &prev_ppa);
						if (xfer_size == 4096) {
							srd.stime += spp->qlc_pg_4kb_rd_lat[zone];
						} else {
							srd.stime += spp->qlc_pg_rd_lat[zone];
						}
					}
					
					srd.xfer_size = xfer_size;
					srd.ppa = &prev_ppa;
					nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd);
					nsecs_latest = max(nsecs_completed, nsecs_latest);
				}
				
				srd.stime = original_stime;  /* 恢复原始时间 */
			}

			xfer_size = spp->pgsz;
			prev_ppa = cur_ppa;
			prev_lpn = local_lpn;
		}

		// issue remaining io
		if (xfer_size > 0) {
			bool issue_prev = true;
			struct ppa refreshed;

			if (prev_lpn == INVALID_LPN) {
				issue_prev = false;
			} else {
				refreshed = get_maptbl_ent(conv_ftl, prev_lpn);
				if (!mapped_ppa(&refreshed) || !valid_ppa(conv_ftl, &refreshed)) {
					issue_prev = false;
				} else {
					prev_ppa = refreshed;
				}
			}

			/* 根据页面位置调整读延迟 */
			if (issue_prev) {
				if (is_slc_block(conv_ftl, prev_ppa.g.blk)) {
					/* SLC 读延迟 */
					if (xfer_size == 4096) {
						srd.stime += spp->pg_4kb_rd_lat[get_cell(conv_ftl->ssd, &prev_ppa)];
					} else {
						srd.stime += spp->pg_rd_lat[get_cell(conv_ftl->ssd, &prev_ppa)];
					}
				} else {
					/* QLC 读延迟 */
					uint8_t zone = get_qlc_zone_for_read(conv_ftl, &prev_ppa);
					if (xfer_size == 4096) {
						srd.stime += spp->qlc_pg_4kb_rd_lat[zone];
					} else {
						srd.stime += spp->qlc_pg_rd_lat[zone];
					}
				}
				
				srd.xfer_size = xfer_size;
				srd.ppa = &prev_ppa;
				nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd);
				nsecs_latest = max(nsecs_completed, nsecs_latest);
			}
		}
	}

	if (NVMEV_ENABLE_QLC_REBALANCE &&
	    (rd_seq % 10) == 0 && conv_ftl->bg_migration_wq)
		queue_work(conv_ftl->bg_migration_wq, &conv_ftl->qlc_rebalance_work);
	if (NVMEV_ENABLE_READ_REPROMOTION &&
	    (rd_seq % conv_ftl->repromote_period_reads) == 0 && conv_ftl->bg_migration_wq)
		queue_work(conv_ftl->bg_migration_wq, &conv_ftl->repromotion_work);

ret->nsecs_target = nsecs_latest;
	ret->status = NVME_SC_SUCCESS;
	    NVMEV_DEBUG("[READ_VERIFY] LBA Range: %llu + %d. Total Latency: %llu ns\n", 
			                   cmd->rw.slba, cmd->rw.length, nsecs_latest - nsecs_start);

	printk_ratelimited(KERN_INFO
		"[COLD_RD_PROBE] lba=%llu nr_lba=%llu sim_lat_ns=%llu total_mig_in_read=%llu\n",
		lba, nr_lba, nsecs_latest - nsecs_start,
		conv_ftl->migration_read_path_count);

	test_phase_note_read_end(stats_ftl, test_phase_read_tracked);
	return true;
}

/* 反向迁移函数：从 QLC 迁移热数据回 SLC */
static void migrate_page_to_slc(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *qlc_ppa,
				uint64_t *migration_done)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct ppa new_ppa;
	struct nand_cmd srd, swr;
	uint64_t nsecs_completed;
	uint32_t target_ch, target_lun;
	uint32_t die_index;
	uint32_t chain_id = chain_id_for_lpn(conv_ftl, lpn);
	uint64_t stored_prev_lpn = INVALID_LPN;
	unsigned long flags;
	struct line_pool_stats slc_stats;
	bool test_phase_bg_tracked = false;
	bool was_in_slc = false;

	if (!conv_ftl || !qlc_ppa)
		return;
	if (!mapped_ppa(qlc_ppa) || !valid_ppa(conv_ftl, qlc_ppa))
		return;
	if (conv_ftl->page_in_slc && lpn < conv_ftl->ssd->sp.tt_pgs)
		was_in_slc = conv_ftl->page_in_slc[lpn];

	test_phase_note_bg_begin(conv_ftl, &conv_ftl->test_phase_bg_repromote_ops,
				 &test_phase_bg_tracked);

	if (conv_ftl->slc_repromote_guard_lines) {
		collect_slc_stats(conv_ftl, &slc_stats);
		if (slc_stats.free <= conv_ftl->slc_repromote_guard_lines) {
			NVMEV_DEBUG("migrate_page_to_slc: skip due to low SLC free lines (%u <= %u)\n",
				    slc_stats.free, conv_ftl->slc_repromote_guard_lines);
			test_phase_note_bg_end(conv_ftl, test_phase_bg_tracked);
			return;
		}
	}

	/* Read QLC page（内部迁移，跳过通道模型） */
	srd.type = GC_IO;
	srd.cmd = NAND_READ;
	srd.stime = __get_ioclock(conv_ftl->ssd);
	srd.interleave_pci_dma = false;
	srd.xfer_size = spp->pgsz;
	srd.ppa = qlc_ppa;

	nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd);

	die_index = internal_place_die_for_lpn(conv_ftl, lpn, qlc_ppa,
					       CHAIN_TIER_SLC, false);
	decode_die(spp, die_index, &target_ch, &target_lun);

	/* Repromotion is an internal hot-page migration, not host append.
	 * Put it into the internal SLC GC/repromotion SB so it does not consume
	 * the 14 host-active-SB budget. */
	if (conv_ftl->gc_slc_lunwp) {
		struct write_pointer *gc_wp = &conv_ftl->gc_slc_lunwp[die_index];
		if (!gc_wp->curline || gc_wp->pg == 0) {
			gc_wp->ch = target_ch;
			gc_wp->lun = target_lun;
		}
	}
	new_ppa = get_new_gc_slc_page(conv_ftl, die_index);
	if (!mapped_ppa(&new_ppa)) {
		NVMEV_ERROR("migrate_page_to_slc: Failed to allocate SLC page for back-migration\n");
		test_phase_note_bg_end(conv_ftl, test_phase_bg_tracked);
		return;
	}

	/* Write SLC page（内部迁移，跳过通道模型） */
	swr.type = GC_IO;
	swr.cmd = NAND_WRITE;
	swr.stime = nsecs_completed;
	swr.interleave_pci_dma = false;
	swr.xfer_size = spp->pgsz;
	swr.ppa = &new_ppa;

	nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &swr);

	/*
	 * qlc_resident_page_cnt / qlc_resident_read_sum are handled by
	 * mark_page_invalid(qlc_ppa) below.  Only adjust fast/slow zone
	 * counts here because mark_page_invalid does not track them.
	 */
	{
		struct nand_page *old_pg = get_pg(conv_ftl->ssd, qlc_ppa);
		uint8_t old_zone = old_pg ? old_pg->qlc_latency_zone : 0;

		if (old_pg) {
			stored_prev_lpn = old_pg->oob_prev_lpn;
			if (stored_prev_lpn >= conv_ftl->ssd->sp.tt_pgs)
				stored_prev_lpn = INVALID_LPN;
		}

		spin_lock_irqsave(&conv_ftl->qlc_zone_lock, flags);
		if (qlc_zone_is_fast(old_zone)) {
			if (conv_ftl->qlc_fast_count > 0)
				conv_ftl->qlc_fast_count--;
		} else {
			if (conv_ftl->qlc_slow_count > 0)
				conv_ftl->qlc_slow_count--;
		}
		spin_unlock_irqrestore(&conv_ftl->qlc_zone_lock, flags);
	}

	/* Update mappings */
		set_maptbl_ent_reason(conv_ftl, lpn, &new_ppa, NVMEV_DIE_CHANGE_REPROMOTE);
	set_rmap_ent(conv_ftl, lpn, &new_ppa);

	/* Mark old QLC invalid */
	mark_page_invalid(conv_ftl, qlc_ppa);
	set_rmap_ent(conv_ftl, INVALID_LPN, qlc_ppa);

		/* Update new SLC valid */
		//slc_mark_page_resident(conv_ftl, lpn);
			/* Update new SLC valid */
			if (conv_ftl->page_in_slc && lpn < conv_ftl->ssd->sp.tt_pgs &&
						    !conv_ftl->page_in_slc[lpn]) {
						conv_ftl->page_in_slc[lpn] = true;
								atomic64_inc(&conv_ftl->slc_resident_page_cnt);
									}
		chain_slc_note_state_change(conv_ftl, chain_id, was_in_slc,
				       chain_id, true, true);
		mark_page_valid(conv_ftl, &new_ppa);
		slc_resident_track_page(conv_ftl, lpn, encode_die(spp, &new_ppa));

	/* Advance the allocator that produced the destination page. */
	advance_gc_slc_write_pointer(conv_ftl, encode_die(spp, &new_ppa));
	conv_ftl->repromote_gc_pool_pages++;
	internal_place_note_lpn_write(conv_ftl, lpn, CHAIN_TIER_SLC,
				      false, encode_die(spp, &new_ppa));

	/* Update prev link */
	set_page_prev_link(conv_ftl, lpn, &new_ppa, stored_prev_lpn);

	if (migration_done)
		*migration_done = nsecs_completed;

	NVMEV_DEBUG("Migrated LPN %llu from QLC to SLC (req_die=%u actual_die=%u)\n",
		    lpn, die_index, encode_die(spp, &new_ppa));
	test_phase_note_bg_end(conv_ftl, test_phase_bg_tracked);
}

static uint64_t chain_hot_repromote_threshold(uint64_t avg_reads)
{
	uint64_t th = avg_reads ? avg_reads : 1;

	if (CHAIN_HOT_REPROMOTE_RATIO_NUM > 1 &&
	    th > div64_u64(U64_MAX, CHAIN_HOT_REPROMOTE_RATIO_NUM))
		return U64_MAX;
	th = div64_u64(th * CHAIN_HOT_REPROMOTE_RATIO_NUM +
		       CHAIN_HOT_REPROMOTE_RATIO_DEN - 1,
		       CHAIN_HOT_REPROMOTE_RATIO_DEN);
	return th ? th : 1;
}

static bool qlc_closed_sb_candidate(struct conv_ftl *conv_ftl, uint32_t blk)
{
	uint32_t idx;
	bool candidate = false;

	if (!qlc_blk_to_idx(conv_ftl, blk, &idx))
		return false;

	spin_lock(&conv_ftl->qlc_lock);
	candidate = !conv_ftl->qlc_sb_state ||
		conv_ftl->qlc_sb_state[idx] == NVMEV_SB_CLOSED;
	spin_unlock(&conv_ftl->qlc_lock);
	return candidate;
}

static bool qlc_closed_line_candidate(struct conv_ftl *conv_ftl,
				      uint32_t die, uint32_t blk)
{
	struct line_mgmt *lm;
	struct line *line;
	struct write_pointer *host_wp;
	struct write_pointer *gc_wp;
	uint32_t idx;
	bool candidate = false;

	if (!conv_ftl || !conv_ftl->die_count || die >= conv_ftl->die_count ||
	    blk < conv_ftl->slc_blks_per_pl)
		return false;

	lm = get_qlc_die_lm(conv_ftl, die);
	if (!lm || !lm->lines)
		return false;
	idx = blk - conv_ftl->slc_blks_per_pl;
	if (idx >= lm->tt_lines)
		return false;

	host_wp = get_qlc_die_wp(conv_ftl, die, false);
	gc_wp = get_qlc_die_wp(conv_ftl, die, true);

	spin_lock(&conv_ftl->qlc_lock);
	line = &lm->lines[idx];
	if (line->vpc > 0 &&
	    !(host_wp && host_wp->curline == line) &&
	    !(gc_wp && gc_wp->curline == line)) {
		candidate = true;
		if (conv_ftl->qlc_sb_state &&
		    conv_ftl->qlc_sb_state[idx] != NVMEV_SB_CLOSED)
			candidate = false;
	}
	spin_unlock(&conv_ftl->qlc_lock);
	return candidate;
}

static uint32_t migrate_hot_closed_qlc_line_to_slc(struct conv_ftl *conv_ftl,
						   uint32_t die, uint32_t blk,
						   uint64_t hot_th,
						   uint32_t budget)
{
	struct heat_tracking *ht = &conv_ftl->heat_track;
	struct ssdparams *spp;
	uint32_t ch = 0, lun = 0;
	uint32_t moved = 0;
	uint32_t pg_idx;

	if (!conv_ftl || !conv_ftl->ssd || !ht || !ht->access_count || !budget)
		return 0;
	if (!qlc_closed_line_candidate(conv_ftl, die, blk))
		return 0;

	spp = &conv_ftl->ssd->sp;
	decode_die(spp, die, &ch, &lun);

	for (pg_idx = 0; pg_idx < conv_ftl->qlc_pgs_per_blk && moved < budget; pg_idx++) {
		struct ppa ppa = { .ppa = 0 };
		struct nand_page *pg;
		struct ppa cur;
		uint64_t lpn;
		uint64_t acc;

		ppa.g.ch = ch;
		ppa.g.lun = lun;
		ppa.g.blk = blk;
		ppa.g.pl = 0;
		ppa.g.pg = pg_idx;

		pg = get_pg(conv_ftl->ssd, &ppa);
		if (!pg || pg->status != PG_VALID)
			continue;

		lpn = get_rmap_ent(conv_ftl, &ppa);
		if (lpn == INVALID_LPN || lpn >= spp->tt_pgs)
			continue;

		acc = ht->access_count[lpn];
		if (acc < hot_th || acc < REPROMOTE_HEAT_FLOOR)
			continue;
		if (recent_write_guard(conv_ftl, lpn))
			continue;

		cur = get_maptbl_ent(conv_ftl, lpn);
		if (cur.ppa != ppa.ppa || is_slc_block(conv_ftl, cur.g.blk))
			continue;

		migrate_page_to_slc(conv_ftl, lpn, &cur, NULL);
		cur = get_maptbl_ent(conv_ftl, lpn);
		if (mapped_ppa(&cur) && is_slc_block(conv_ftl, cur.g.blk))
			moved++;
	}

	return moved;
}

static uint32_t migrate_hot_closed_qlc_sb_to_slc(struct conv_ftl *conv_ftl,
						 uint32_t blk,
						 uint64_t hot_th,
						 uint32_t budget)
{
	uint32_t die;
	uint32_t moved = 0;
	uint32_t die_count;

	if (!conv_ftl || !budget)
		return 0;

	die_count = conv_ftl->die_count ? conv_ftl->die_count : 1;
	for (die = 0; die < die_count && moved < budget; die++) {
		moved += migrate_hot_closed_qlc_line_to_slc(conv_ftl, die, blk,
							    hot_th,
							    budget - moved);
	}
	return moved;
}

static uint32_t migrate_hot_from_closed_qlc(struct conv_ftl *conv_ftl)
{
	struct line_pool_stats slc_stats;
	uint64_t avg_reads = 0;
	uint64_t hot_th;
	uint32_t budget;
	uint32_t scanned = 0;
	uint32_t moved = 0;

	if (!conv_ftl || !conv_ftl->ssd || !conv_ftl->qlc_closed_repromote_size)
		return 0;

	if (!qlc_closed_repromote_take_trigger(conv_ftl))
		return 0;

	if (conv_ftl->slc_repromote_guard_lines) {
		collect_slc_stats(conv_ftl, &slc_stats);
		if (slc_stats.free <= conv_ftl->slc_repromote_guard_lines)
			return 0;
	}

	if (!calc_global_avg_reads(conv_ftl, &avg_reads))
		avg_reads = REPROMOTE_HEAT_FLOOR;
	hot_th = chain_hot_repromote_threshold(avg_reads);
	if (hot_th < REPROMOTE_HEAT_FLOOR)
		hot_th = REPROMOTE_HEAT_FLOOR;

	budget = qlc_pages_per_superblock(conv_ftl);
	if (!budget)
		budget = conv_ftl->repromote_budget_per_run ?
			conv_ftl->repromote_budget_per_run : 1;

	while (scanned < conv_ftl->qlc_blks_per_pl && moved < budget) {
		uint32_t blk = conv_ftl->slc_blks_per_pl + scanned;
		uint32_t one_moved;

		scanned++;
		if (!qlc_closed_sb_candidate(conv_ftl, blk))
			continue;
		one_moved = migrate_hot_closed_qlc_sb_to_slc(conv_ftl, blk,
							     hot_th,
							     budget - moved);
		moved += one_moved;
	}

	if (scanned) {
		conv_ftl->qlc_closed_repromote_scans++;
		conv_ftl->qlc_closed_repromote_pages += moved;
	}
	return moved;
}

static bool qlc_block_chain_owner_strict(struct conv_ftl *conv_ftl, const struct ppa *ppa,
					 uint32_t *owner_chain_out)
{
	uint64_t idx = block_meta_index(conv_ftl, ppa);
	uint32_t owner_chain;
	uint32_t owner_pages;
	uint32_t valid_pages;

	if (!conv_ftl || !ppa || idx == U64_MAX || idx >= conv_ftl->ssd->sp.tt_blks ||
	    !conv_ftl->blk_owner_chain || !conv_ftl->blk_owner_pages ||
	    !conv_ftl->blk_valid_pages || is_slc_block(conv_ftl, ppa->g.blk))
		return false;

	owner_chain = conv_ftl->blk_owner_chain[idx];
	owner_pages = conv_ftl->blk_owner_pages[idx];
	valid_pages = conv_ftl->blk_valid_pages[idx];
	if (!chain_id_valid(conv_ftl, owner_chain) ||
	    valid_pages < CHAIN_HOT_REPROMOTE_MIN_PAGES)
		return false;
	if (owner_pages * 100U < valid_pages * CHAIN_HOT_REPROMOTE_OWNER_PCT)
		return false;

	if (owner_chain_out)
		*owner_chain_out = owner_chain;
	return true;
}

static uint32_t migrate_hot_qlc_range_to_slc(struct conv_ftl *conv_ftl,
					     const struct ppa *block_ppa,
					     uint32_t begin_pg, uint32_t end_pg,
					     uint32_t owner_chain,
					     uint64_t hot_th, uint32_t budget)
{
	struct heat_tracking *ht = &conv_ftl->heat_track;
	uint32_t moved = 0;
	uint32_t pg_idx;

	if (!conv_ftl || !block_ppa || !ht || !ht->access_count || budget == 0)
		return 0;

	for (pg_idx = begin_pg; pg_idx < end_pg && moved < budget; pg_idx++) {
		struct ppa ppa = *block_ppa;
		struct nand_page *pg;
		struct ppa cur;
		uint64_t lpn;
		uint64_t acc;

		ppa.g.pg = pg_idx;
		pg = get_pg(conv_ftl->ssd, &ppa);
		if (!pg || pg->status != PG_VALID)
			continue;

		lpn = get_rmap_ent(conv_ftl, &ppa);
		if (lpn == INVALID_LPN || lpn >= conv_ftl->ssd->sp.tt_pgs)
			continue;
		if (!conv_ftl->lpn_chain_id || conv_ftl->lpn_chain_id[lpn] != owner_chain)
			continue;

		acc = ht->access_count[lpn];
		if (acc < hot_th)
			continue;

		cur = get_maptbl_ent(conv_ftl, lpn);
		if (cur.ppa != ppa.ppa || is_slc_block(conv_ftl, cur.g.blk))
			continue;

		migrate_page_to_slc(conv_ftl, lpn, &cur, NULL);
		moved++;
	}

	return moved;
}

static uint32_t migrate_hot_chain_block_from_qlc(struct conv_ftl *conv_ftl,
						 const struct ppa *block_ppa,
						 uint64_t hot_th, uint32_t budget)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_block *blk;
	struct heat_tracking *ht = &conv_ftl->heat_track;
	uint32_t owner_chain = INVALID_CHAIN_ID;
	uint32_t heat_pages = 0;
	uint32_t hot_pages = 0;
	uint32_t best_base = 0;
	uint32_t best_heat = 0;
	uint32_t best_hot = 0;
	uint32_t chunk_pages;
	uint32_t pg_idx;

	if (!conv_ftl || !block_ppa || !ht || !ht->access_count || budget == 0)
		return 0;
	if (!qlc_block_chain_owner_strict(conv_ftl, block_ppa, &owner_chain))
		return 0;

	blk = get_blk(conv_ftl->ssd, block_ppa);
	if (!blk || !blk->npgs)
		return 0;

	for (pg_idx = 0; pg_idx < blk->npgs; pg_idx++) {
		struct ppa ppa = *block_ppa;
		struct nand_page *pg;
		uint64_t lpn;
		uint64_t acc;

		ppa.g.pg = pg_idx;
		pg = get_pg(conv_ftl->ssd, &ppa);
		if (!pg || pg->status != PG_VALID)
			continue;

		lpn = get_rmap_ent(conv_ftl, &ppa);
		if (lpn == INVALID_LPN || lpn >= conv_ftl->ssd->sp.tt_pgs)
			continue;
		if (!conv_ftl->lpn_chain_id || conv_ftl->lpn_chain_id[lpn] != owner_chain)
			continue;

		acc = ht->access_count[lpn];
		heat_pages++;
		if (acc >= hot_th)
			hot_pages++;
	}

	if (heat_pages < CHAIN_HOT_REPROMOTE_MIN_PAGES || hot_pages == 0)
		return 0;

	if (hot_pages * 100U >= heat_pages * CHAIN_HOT_REPROMOTE_BLOCK_HOT_PCT)
		return migrate_hot_qlc_range_to_slc(conv_ftl, block_ppa, 0, blk->npgs,
						    owner_chain, hot_th, budget);

	chunk_pages = spp->pgs_per_oneshotpg ? spp->pgs_per_oneshotpg : 1;
	for (pg_idx = 0; pg_idx < blk->npgs; pg_idx += chunk_pages) {
		uint32_t end_pg = min_t(uint32_t, pg_idx + chunk_pages, blk->npgs);
		uint32_t chunk_heat = 0;
		uint32_t chunk_hot = 0;
		uint32_t cur_pg;

		for (cur_pg = pg_idx; cur_pg < end_pg; cur_pg++) {
			struct ppa ppa = *block_ppa;
			struct nand_page *pg;
			uint64_t lpn;
			uint64_t acc;

			ppa.g.pg = cur_pg;
			pg = get_pg(conv_ftl->ssd, &ppa);
			if (!pg || pg->status != PG_VALID)
				continue;

			lpn = get_rmap_ent(conv_ftl, &ppa);
			if (lpn == INVALID_LPN || lpn >= conv_ftl->ssd->sp.tt_pgs)
				continue;
			if (!conv_ftl->lpn_chain_id || conv_ftl->lpn_chain_id[lpn] != owner_chain)
				continue;

			acc = ht->access_count[lpn];
			chunk_heat++;
			if (acc >= hot_th)
				chunk_hot++;
		}

		if (chunk_heat < CHAIN_HOT_REPROMOTE_MIN_PAGES)
			continue;
		if (chunk_hot * 100U < chunk_heat * CHAIN_HOT_REPROMOTE_CHUNK_HOT_PCT)
			continue;
		if (chunk_hot > best_hot ||
		    (chunk_hot == best_hot && chunk_heat > best_heat)) {
			best_base = pg_idx;
			best_heat = chunk_heat;
			best_hot = chunk_hot;
		}
	}

	if (!best_hot)
		return 0;

	return migrate_hot_qlc_range_to_slc(conv_ftl, block_ppa, best_base,
					    min_t(uint32_t, best_base + chunk_pages, blk->npgs),
					    owner_chain, hot_th, budget);
}

/* 扫描 QLC 热 chain/block，并以较长周期迁回 SLC。 */
static void __maybe_unused migrate_hot_from_qlc(struct conv_ftl *conv_ftl)
{
	struct ssdparams *spp;
	struct heat_tracking *ht;
	struct line_pool_stats slc_stats;
	uint64_t avg_reads = 0;
	uint64_t hot_th;
	uint32_t scanned = 0;
	uint32_t migrated = 0;
	uint32_t budget;
	uint32_t die_count;
	uint64_t total_blocks;
	uint64_t cursor;

	if (!conv_ftl || !conv_ftl->ssd || !conv_ftl->lpn_chain_id)
		return;

	spp = &conv_ftl->ssd->sp;
	ht = &conv_ftl->heat_track;
	if (!ht || !ht->access_count)
		return;
	if (!calc_global_avg_reads(conv_ftl, &avg_reads))
		return;

	if (conv_ftl->slc_repromote_guard_lines) {
		collect_slc_stats(conv_ftl, &slc_stats);
		if (slc_stats.free <= conv_ftl->slc_repromote_guard_lines)
			return;
	}

	die_count = conv_ftl->die_count ? conv_ftl->die_count : 1;
	if (!conv_ftl->qlc_blks_per_pl || !spp->pls_per_lun)
		return;

	total_blocks = (uint64_t)die_count * spp->pls_per_lun * conv_ftl->qlc_blks_per_pl;
	if (!total_blocks)
		return;

	budget = conv_ftl->repromote_budget_per_run ? conv_ftl->repromote_budget_per_run : 1;
	hot_th = chain_hot_repromote_threshold(avg_reads);
	cursor = conv_ftl->qlc_promote_cursor % total_blocks;

	while (scanned < CHAIN_HOT_REPROMOTE_SCAN_BLOCKS &&
	       scanned < total_blocks && migrated < budget) {
		uint64_t logical = (cursor + scanned) % total_blocks;
		uint64_t rem;
		uint32_t die;
		uint32_t pl;
		uint32_t qlc_blk_idx;
		uint32_t ch = 0, lun = 0;
		struct ppa block_ppa = { .ppa = 0 };

		die = (uint32_t)(logical % die_count);
		rem = div64_u64(logical, die_count);
		pl = (uint32_t)(rem % spp->pls_per_lun);
		qlc_blk_idx = (uint32_t)div64_u64(rem, spp->pls_per_lun);

		decode_die(spp, die, &ch, &lun);
		block_ppa.g.ch = ch;
		block_ppa.g.lun = lun;
		block_ppa.g.pl = pl;
		block_ppa.g.blk = conv_ftl->slc_blks_per_pl + qlc_blk_idx;
		block_ppa.g.pg = 0;

		migrated += migrate_hot_chain_block_from_qlc(conv_ftl, &block_ppa,
							     hot_th, budget - migrated);
		scanned++;
	}

	conv_ftl->qlc_promote_cursor = (cursor + scanned) % total_blocks;
	if (migrated) {
		NVMEV_DEBUG("[REPROMOTE-CHAIN] avg=%llu hot_th=%llu scanned_blocks=%u migrated=%u budget=%u period_reads=%u\n",
			    avg_reads, hot_th, scanned, migrated, budget,
			    conv_ftl->repromote_period_reads);
	}
}

static bool conv_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	NVMEV_DEBUG("[DEBUG] conv_write: Function entry\n");
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];
	struct conv_ftl *stats_ftl = conv_ftl;

	/* wbuf and spp are shared by all instances */
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct buffer *wbuf = conv_ftl->ssd->write_buffer;

	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;

	uint64_t lpn;
	uint64_t local_lpn;
	uint32_t nr_parts = ns->nr_parts;
	uint64_t max_lba = ns->size >> 9;

	uint64_t nsecs_latest;
	uint64_t nsecs_xfer_completed;
	uint64_t nsecs_completed = 0;
	uint32_t allocated_buf_size;
	uint32_t xfer_size = 0;  /* 声明缺失的变量 */
//66f1
	uint16_t bOverwrite = (cmd->rw.control & NVME_RW_OVERWRITE) ? 1 : 0;
	uint16_t bAppend = (cmd->rw.control & NVME_RW_APPEND) ? 1 : 0;
	bool is_append_opcode = (cmd->rw.opcode == nvme_cmd_zone_append);

	uint64_t plba = 0;
	uint64_t plpn = 0;
	uint64_t stripe_bytes = 0;
	uint64_t wbuf_needed = 0;
	uint64_t prev_link_lpn = INVALID_LPN;
	bool test_phase_overwrite_tracked = false;
//66f1

	struct ppa ppa;
	struct line_pool_stats slc_stats;
	uint32_t slc_free_lines;
	uint32_t slc_used_lines;

	struct nand_cmd swr = {
		.type = USER_IO,
		.cmd = NAND_WRITE,
		.interleave_pci_dma = false,
		.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg,
	};
//66f1
	if (!bAppend && is_append_opcode)
		bAppend = 1;

	if (bAppend) {
		plba = cmd->rw.pslba;
		plpn = plba / spp->secs_per_pg;
		if (unlikely(plba >= max_lba)) {
			if (printk_ratelimit()) {
				NVMEV_ERROR("conv_write: BAD pslba=%llu (slba=%llu len=%u sqid=%d), disable append\n",
					    plba, cmd->rw.slba, cmd->rw.length + 1, req->sq_id);
			}
			bAppend = 0;
			plba = 0;
			plpn = 0;
		}
	}
	if (bOverwrite)
	{
		//NVMEV_ERROR("[NVMEVIRT]_OW\n");
	}
//66f1

	NVMEV_DEBUG("[DEBUG] conv_write: start_lpn=%lld, len=%lld, end_lpn=%lld, nr_parts=%u, tt_pgs=%ld\n", 
		           start_lpn, nr_lba, end_lpn, nr_parts, spp->tt_pgs);
	if (unlikely(nr_parts == 0)) {
		NVMEV_ERROR("conv_write: nr_parts=0\n");
		ret->status = NVME_SC_INTERNAL;
		ret->nsecs_target = req->nsecs_start;
		return true;
	}
	if (unlikely(nr_lba == 0 || lba > U64_MAX - (nr_lba - 1))) {
		NVMEV_ERROR("conv_write: LBA overflow lba=%llu nr_lba=%llu\n", lba, nr_lba);
		ret->status = NVME_SC_LBA_RANGE;
		ret->nsecs_target = req->nsecs_start;
		return true;
	}
    if ((end_lpn / nr_parts) >= spp->tt_pgs) {
        NVMEV_DEBUG("[DEBUG] conv_write: LPN RANGE CHECK FAILED - lpn passed FTL range(start_lpn=%lld,tt_pgs=%ld)\n",
                    start_lpn, spp->tt_pgs);
        ret->status = NVME_SC_LBA_RANGE;
        ret->nsecs_target = req->nsecs_start;
        return true; /* Return completion with error to avoid host timeout */
    }
/*
    allocated_buf_size = buffer_allocate(wbuf, LBA_TO_BYTE(nr_lba));
	NVMEV_DEBUG("[DEBUG] conv_write: buffer alloc size = %u, needed = %llu\n", allocated_buf_size, LBA_TO_BYTE(nr_lba));
    if (allocated_buf_size < LBA_TO_BYTE(nr_lba)) {
        NVMEV_DEBUG("[DEBUG] conv_write: BUFFER ALLOCATION FAILED - insufficient write buffer (%u < %llu)\n",
                    allocated_buf_size, LBA_TO_BYTE(nr_lba));
        ret->status = NVME_SC_WRITE_FAULT;
        ret->nsecs_target = req->nsecs_start;
        return true;  Complete with error */
    
          
    {		            /* 写缓冲不足时短暂重试，避免瞬间满导致失败 */
        uint64_t needed = LBA_TO_BYTE(nr_lba);
        int wb_retry = 0;
        const int WB_MAX_RETRIES = 100;    /* 最多重试 100 次 */
        const int WB_RETRY_US = 1000000;      /* 每次等待 1ms */

	if (spp->pgsz) {
		uint64_t remainder = needed % spp->pgsz;
		if (remainder)
			needed += spp->pgsz - remainder;
	}

	wbuf_needed = needed;
	if (unlikely(needed > wbuf->size)) {
		NVMEV_ERROR("write buffer too small (need=%llu, size=%zu)\n",
			    needed, wbuf->size);
		ret->status = NVME_SC_WRITE_FAULT;
		ret->nsecs_target = req->nsecs_start;
		return true; /* Complete with error */
	}

retry_wb_alloc:
	allocated_buf_size = buffer_allocate(wbuf, needed);
	NVMEV_DEBUG("[DEBUG] conv_write: buffer alloc size = %u, needed = %llu\n",
		    allocated_buf_size, needed);
	if (allocated_buf_size < needed) {
		if (wb_retry < WB_MAX_RETRIES) {
			wb_retry++;
			usleep_range(WB_RETRY_US, WB_RETRY_US + 100);
			cond_resched();
			goto retry_wb_alloc;
		}
		NVMEV_ERROR("write buffer allocation failed after %d retries (need=%llu)\n",
			    WB_MAX_RETRIES, needed);
		ret->status = NVME_SC_WRITE_FAULT;
		ret->nsecs_target = req->nsecs_start;
		return true; /* Complete with error */
	}
   }
	nsecs_latest = ssd_advance_write_buffer(conv_ftl->ssd, req->nsecs_start, wbuf_needed);
	nsecs_xfer_completed = nsecs_latest;

	swr.stime = nsecs_latest;
	if (bOverwrite)
		test_phase_note_overwrite_begin(stats_ftl, &test_phase_overwrite_tracked);
    
	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		bool aff_append_req = false;
		bool aff_overwrite_req = false;
		bool aff_target_valid = false;
		uint32_t aff_target_die = 0;
		uint32_t chain_id = INVALID_CHAIN_ID;
		uint32_t old_chain_id = INVALID_CHAIN_ID;
		uint64_t old_prev_lpn = INVALID_LPN;
		uint16_t seed_die = 0;
		bool was_in_slc = false;
		        /* 调试：检查是否进入了写入循环 */
		if (lpn == start_lpn) {
			NVMEV_DEBUG("[DEBUG] conv_write: Starting write loop, lpn=%llu to %llu\n", start_lpn, end_lpn);
		}

		conv_ftl = &conv_ftls[lpn % nr_parts];
		local_lpn = lpn / nr_parts;
		if (unlikely(local_lpn >= conv_ftl->ssd->sp.tt_pgs)) {
			NVMEV_ERROR("BAD local_lpn=%llu lpn=%llu start_lpn=%llu end_lpn=%llu nr_parts=%u slba=%llu len=%u\n",
				    local_lpn, lpn, start_lpn, end_lpn, nr_parts,
				    cmd->rw.slba, cmd->rw.length + 1);
			dump_stack();
			return true;
		}
		prev_link_lpn = INVALID_LPN;
		if (local_lpn > 0) {
			nvmev_set_maptbl_site("conv_write.prev_tmp", local_lpn - 1);
			struct ppa prev_tmp = get_maptbl_ent(conv_ftl, local_lpn - 1);
			if (mapped_ppa(&prev_tmp) && valid_ppa(conv_ftl, &prev_tmp))
				prev_link_lpn = local_lpn - 1;
		}
		nvmev_set_maptbl_site("conv_write.ppa", local_lpn);
		ppa = get_maptbl_ent(conv_ftl, local_lpn); // Check whether the given LPN has been written before
		if (mapped_ppa(&ppa)) {
			struct nand_page *old_pg = get_pg(conv_ftl->ssd, &ppa);

			if (old_pg && old_pg->status == PG_VALID) {
				old_prev_lpn = old_pg->oob_prev_lpn;
				if (old_prev_lpn >= conv_ftl->ssd->sp.tt_pgs)
					old_prev_lpn = INVALID_LPN;
			}
			if (conv_ftl->lpn_chain_id && local_lpn < conv_ftl->ssd->sp.tt_pgs)
				old_chain_id = conv_ftl->lpn_chain_id[local_lpn];
			if (conv_ftl->page_in_slc && local_lpn < conv_ftl->ssd->sp.tt_pgs)
				was_in_slc = conv_ftl->page_in_slc[local_lpn];

			/* update old page information first */
			mark_page_invalid(conv_ftl, &ppa);
			set_rmap_ent(conv_ftl, INVALID_LPN, &ppa);
			NVMEV_DEBUG("conv_write: %lld is invalid, ", ppa2pgidx(conv_ftl, &ppa));
			
		}

//66f1
//#define DIEAFFINITY (0)

		/* new write */
		//need branch
		if (lpn == start_lpn)
		{
			if (bAppend)
			{
				uint64_t p_local_lpn = plpn / nr_parts;
				struct conv_ftl* p_conv_ftl = &conv_ftls[plpn % nr_parts];
				if (unlikely(p_local_lpn >= p_conv_ftl->ssd->sp.tt_pgs)) {
					NVMEV_ERROR("BAD p_local_lpn=%llu plpn=%llu nr_parts=%u slba=%llu pslba=%llu\n",
						    p_local_lpn, plpn, nr_parts, cmd->rw.slba, cmd->rw.pslba);
					dump_stack();
				} else {
					nvmev_set_maptbl_site("conv_write.append_ppa", p_local_lpn);
					ppa = get_maptbl_ent(p_conv_ftl,p_local_lpn); 
					if (mapped_ppa(&ppa)) {		
#if NVMEV_ENABLE_HOST_DIE_HINT
						uint32_t originlun = conv_ftl->lunpointer;
						uint32_t hinted_die = get_glun(conv_ftl, &ppa);
						uint32_t total_die = conv_ftl->ssd->sp.nchs *
								      conv_ftl->ssd->sp.luns_per_ch;

						if (total_die) {
							hinted_die = (hinted_die + 1) % total_die;
							conv_ftl->lunpointer = hinted_die;
							conv_ftl->die_aff_append_requests++;
							aff_append_req = true;
							aff_target_valid = true;
							aff_target_die = hinted_die;
						}
#endif
						if (p_conv_ftl == conv_ftl)
							prev_link_lpn = p_local_lpn;
#if NVMEV_ENABLE_HOST_DIE_HINT
						(void)originlun;
#endif
						//NVMEV_ERROR("target lun: %d -> %d\n", originlun, conv_ftl->lunpointer);
					}
				}
			}
			else if (bOverwrite)
			{				
				nvmev_set_maptbl_site("conv_write.overwrite_ppa", local_lpn);
				ppa = get_maptbl_ent(conv_ftl,local_lpn); 
				if (mapped_ppa(&ppa)) {
#if NVMEV_ENABLE_HOST_DIE_HINT
					uint32_t originlun = conv_ftl->lunpointer;
					uint32_t hinted_die = get_glun(conv_ftl, &ppa);

					conv_ftl->lunpointer = hinted_die;
					conv_ftl->die_aff_overwrite_requests++;
					aff_overwrite_req = true;
					aff_target_valid = true;
					aff_target_die = hinted_die;
					(void)originlun;
#endif
					//NVMEV_ERROR("target lun: %d -> %d\n", originlun, conv_ftl->lunpointer);
				}
			}
		}

			if (bOverwrite && old_prev_lpn != INVALID_LPN)
				prev_link_lpn = old_prev_lpn;

			seed_die = conv_ftl->die_count ?
				   (uint16_t)(conv_ftl->lunpointer % conv_ftl->die_count) : 0;
			if (bOverwrite && chain_id_valid(conv_ftl, old_chain_id))
				chain_id = old_chain_id;
			else
				chain_id = chain_for_write(conv_ftl, local_lpn, prev_link_lpn,
							   seed_die, bAppend, bOverwrite);
			if (aff_target_valid && conv_ftl->die_count)
				conv_ftl->lunpointer = aff_target_die % conv_ftl->die_count;
			else if (chain_id_valid(conv_ftl, chain_id) && conv_ftl->die_count)
				conv_ftl->lunpointer =
					chain_place_die(conv_ftl, chain_id, CHAIN_TIER_SLC,
							conv_ftl->lunpointer % conv_ftl->die_count);

			{
				int slc_retry = 0;
				const int SLC_MAX_RETRIES = 8;

				if (chain_id_valid(conv_ftl, chain_id) && conv_ftl->die_count)
					ppa = get_new_chain_slc_page(conv_ftl, chain_id,
									 conv_ftl->lunpointer % conv_ftl->die_count);
				else
					ppa = get_new_slc_page(conv_ftl);
				while (!mapped_ppa(&ppa) && slc_retry < SLC_MAX_RETRIES) {
					uint32_t starved_die;
					int32_t target_die = conv_ftl->die_count ?
						(int32_t)(conv_ftl->lunpointer % conv_ftl->die_count) : -1;

						slc_retry++;
						migrate_some_cold_from_slc(conv_ftl, slc_pages_per_superblock(conv_ftl),
									   target_die);

						if (should_gc_slc_any_die_critical(conv_ftl, &starved_die))
							do_gc_for_die(conv_ftl, starved_die, true);

					forground_gc(conv_ftl, FGC_MODE_HARD);
					if (chain_id_valid(conv_ftl, chain_id) && conv_ftl->die_count)
						ppa = get_new_chain_slc_page(conv_ftl, chain_id,
										 conv_ftl->lunpointer % conv_ftl->die_count);
					else
						ppa = get_new_slc_page(conv_ftl);
				}
			}

			if (!mapped_ppa(&ppa)) {
				NVMEV_ERROR("SLC exhausted, write failed for LPN %lld (after retries)\n", local_lpn);
				ret->status = NVME_SC_WRITE_FAULT;
				ret->nsecs_target = nsecs_latest;
				goto slc_fail_release;
			}

		if (aff_target_valid) {
			uint32_t actual_die = encode_die(spp, &ppa);

			if (aff_append_req && actual_die == aff_target_die)
				conv_ftl->die_aff_append_effective++;
			if (aff_overwrite_req && actual_die == aff_target_die)
				conv_ftl->die_aff_overwrite_effective++;
		}

			/* 记录页面在 SLC 中 */
			conv_ftl->page_in_slc[local_lpn] = true;
			conv_ftl->slc_write_cnt++;
			conv_ftl->total_host_writes++;
			if (conv_ftl->heat_track.write_epoch)
				conv_ftl->heat_track.write_epoch[local_lpn] = conv_ftl->total_host_writes;

		//NVMEV_ERROR("PPA: ch:%d, lun:%d, blk:%d, pg:%d \n", ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pg );

//66f1

		/* update maptbl */
			if (conv_ftl->lpn_chain_id && local_lpn < conv_ftl->ssd->sp.tt_pgs)
				conv_ftl->lpn_chain_id[local_lpn] = chain_id;
			chain_slc_note_state_change(conv_ftl, old_chain_id, was_in_slc,
					       chain_id, true, true);
			set_maptbl_ent_reason(conv_ftl, local_lpn, &ppa,
					      bOverwrite ? NVMEV_DIE_CHANGE_HOST_OVERWRITE :
					      NVMEV_DIE_CHANGE_HOST_APPEND);
		NVMEV_DEBUG("conv_write: got new ppa %lld, ", ppa2pgidx(conv_ftl, &ppa));
		/* update rmap */
		set_rmap_ent(conv_ftl, local_lpn, &ppa);

			mark_page_valid(conv_ftl, &ppa);
			slc_resident_track_page(conv_ftl, local_lpn, encode_die(spp, &ppa));
			set_page_prev_link(conv_ftl, local_lpn, &ppa, prev_link_lpn);

		/* need to advance the write pointer here */
		//need branch
//66f1
			/* 使用 chain-private SLC block（若可用）推进写指针 */
			if (chain_id_valid(conv_ftl, chain_id))
				advance_chain_slc_write_pointer(conv_ftl, chain_id, encode_die(spp, &ppa));
			else
				advance_slc_write_pointer(conv_ftl, encode_die(spp, &ppa));
			chain_place_note_write(conv_ftl, chain_id, CHAIN_TIER_SLC,
					       encode_die(spp, &ppa));

	//	nsecs_completed = ssd_advance_write_buffer(conv_ftl->ssd, nsecs_latest, conv_ftl->ssd->sp.pgsz);

        /* Check whether we need to do a write in this stripe
         * Use current page offset within oneshot page (flash page)
         */
        stripe_bytes += spp->pgsz;
        {
            uint32_t pg_off = ppa.g.pg % spp->pgs_per_oneshotpg;
            bool control_tick = (pg_off == (spp->pgs_per_oneshotpg - 1) || lpn == end_lpn);

            if (control_tick) {
                uint64_t transfer_bytes = stripe_bytes;
                if (transfer_bytes == 0) {
                    transfer_bytes = (uint64_t)(pg_off + 1) * spp->pgsz;
                }
                xfer_size = (uint32_t)transfer_bytes;
			swr.xfer_size = xfer_size;
			
			swr.ppa = &ppa;
			nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &swr);
			/* 异步释放写缓冲，交给 IO worker 归还 */
			enqueue_writeback_io_req(req->sq_id, nsecs_completed, wbuf,
				       (unsigned int)transfer_bytes);
			stripe_bytes = 0;
			swr.stime = nsecs_completed;
            }

			if (control_tick) {
				uint32_t target_lines;
				uint32_t over_lines;
				uint32_t max_pages;
				uint32_t cap_pages;
				int32_t target_die = conv_ftl->die_count ?
					(int32_t)(conv_ftl->lunpointer % conv_ftl->die_count) : -1;

				collect_slc_stats(conv_ftl, &slc_stats);
				slc_free_lines = slc_stats.free;
				slc_used_lines = slc_stats.total - slc_free_lines;

				NVMEV_DEBUG("[DEBUG] SLC control tick: free_lines=%u, used_lines=%u, migrate=%u, soft_gc=%u, hard_gc=%u, total=%u\n",
					   slc_free_lines, slc_used_lines,
					   conv_ftl->slc_high_watermark,
					   conv_ftl->slc_gc_free_thres_high,
					   conv_ftl->slc_gc_free_thres_low,
					   slc_stats.total);

					if (slc_free_lines < conv_ftl->slc_high_watermark) {
						target_lines = conv_ftl->slc_target_watermark;
						if (!target_lines || target_lines >= slc_stats.total)
							target_lines = (slc_stats.total > 1) ?
								(slc_stats.total - 1) : slc_stats.total;

					over_lines = (slc_free_lines < target_lines) ?
						(target_lines - slc_free_lines) : 1;
					max_pages = over_lines * slc_pages_per_superblock(conv_ftl);
					cap_pages = slc_pages_per_superblock(conv_ftl);
					if (max_pages < 8)
						max_pages = 8;
					if (cap_pages && max_pages > cap_pages)
						max_pages = cap_pages;

						NVMEV_DEBUG("[DEBUG] SLC free low (%u < %u), migrating %u cold pages (target_free=%u, cap=%u)\n",
							    slc_free_lines, conv_ftl->slc_high_watermark,
							    max_pages, target_lines, cap_pages);
							migrate_some_cold_from_slc(conv_ftl, max_pages, target_die);
						}

				if (slc_free_lines < conv_ftl->slc_gc_free_thres_low) {
					forground_gc(conv_ftl, FGC_MODE_HARD);
				} else if (slc_free_lines <= conv_ftl->slc_gc_free_thres_high) {
					forground_gc(conv_ftl, FGC_MODE_SOFT);
				}
			}
        }

		nsecs_latest = max(nsecs_completed, nsecs_latest);

		/* 更新热数据信息 */
		update_heat_info(conv_ftl, local_lpn, false);
		conv_ftl->heat_track.last_access_time[local_lpn] = __get_ioclock(conv_ftl->ssd);
		/* [DISABLED] Mechanism 1: qlc_maybe_rebalance_internal disabled */
		

		consume_write_credit(conv_ftl);
		check_and_refill_write_credit(conv_ftl);
	}
	if (stripe_bytes > 0) {
		uint64_t flush_time = nsecs_latest;
		enqueue_writeback_io_req(req->sq_id, flush_time, wbuf,
			       (unsigned int)stripe_bytes);
		stripe_bytes = 0;
	}

	if ((cmd->rw.control & NVME_RW_FUA) || (conv_ftl->ssd->sp.write_early_completion == 0)) {
		    ret->nsecs_target = nsecs_latest;
	} else {
		    ret->nsecs_target = nsecs_xfer_completed;
	}
	
	ret->status = NVME_SC_SUCCESS;
	
	if ((conv_ftl->slc_write_cnt + conv_ftl->qlc_write_cnt) % 10000 == 0) {
		NVMEV_INFO("Write Stats: SLC writes=%llu, QLC writes=%llu, Migrations=%llu\n",
			   conv_ftl->slc_write_cnt, conv_ftl->qlc_write_cnt, conv_ftl->migration_cnt);
	}

	test_phase_note_overwrite_end(stats_ftl, test_phase_overwrite_tracked);
	return true;

slc_fail_release:
	if (stripe_bytes > 0) {
		enqueue_writeback_io_req(req->sq_id, nsecs_latest, wbuf,
				(unsigned int)stripe_bytes);
		stripe_bytes = 0;
	}
	test_phase_note_overwrite_end(stats_ftl, test_phase_overwrite_tracked);
	return true;
}

static void conv_flush(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	uint64_t start, latest;
	uint32_t i;
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;

	start = local_clock();
	latest = start;
	for (i = 0; i < ns->nr_parts; i++) {
		latest = max(latest, ssd_next_idle_time(conv_ftls[i].ssd));
	}

	NVMEV_DEBUG("%s latency=%llu\n", __FUNCTION__, latest - start);

	ret->status = NVME_SC_SUCCESS;
	ret->nsecs_target = latest;
	return;
}

bool conv_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
    /* C90: declarations must precede statements */
    struct nvme_command *cmd;
    struct nvmev_cmd_debug *dbg;
	struct conv_ftl *conv_ftls;
	struct ssdparams *spp;
	unsigned long long ftl_start;
	unsigned long long ftl_dur;
	u64 start_lpn = 0;
	u64 end_lpn = 0;
    
    NVMEV_DEBUG("[DEBUG] conv_proc_nvme_io_cmd: Function entry\n");
    
    if (!ns || !ns->ftls || !req || !ret || !req->cmd) {
        NVMEV_DEBUG("[DEBUG] conv_proc_nvme_io_cmd: NULL parameter check failed\n");
        if (ret) {
            ret->status = NVME_SC_INTERNAL;
            ret->nsecs_target = req ? req->nsecs_start : local_clock();
        }
        /* 必须完成请求，避免超时导致控制器复位 */
        return true;
    }
    
    cmd = req->cmd;
	ftl_start = local_clock();
	conv_ftls = (struct conv_ftl *)ns->ftls;
	spp = conv_ftls ? &conv_ftls[0].ssd->sp : NULL;

	dbg = this_cpu_ptr(&nvmev_last_cmd);
	if (dbg) {
		dbg->valid = true;
		dbg->opcode = cmd->common.opcode;
		dbg->nsid = cmd->common.nsid;
		dbg->slba = cmd->rw.slba;
		dbg->len = cmd->rw.length + 1;
		dbg->sqid = req->sq_id;
		dbg->ts = local_clock();
	}
	NVMEV_ASSERT(ns->csi == NVME_CSI_NVM);

	NVMEV_DEBUG("[DEBUG] conv_proc_nvme_io_cmd: Processing opcode %d (%s)\n", 
	           cmd->common.opcode, nvme_opcode_string(cmd->common.opcode));
	
	switch (cmd->common.opcode) {
	case nvme_cmd_write:
		NVMEV_DEBUG("[DEBUG] conv_proc_nvme_io_cmd: Calling conv_write\n");
		if (spp) {
			start_lpn = cmd->rw.slba / spp->secs_per_pg;
			end_lpn = (cmd->rw.slba + cmd->rw.length) / spp->secs_per_pg;
			nvmev_lpn_mark_range(start_lpn, end_lpn, req->sq_id, cmd->common.command_id);
		}
        if (!conv_write(ns, req, ret))
            goto out; /* 出错也返回完成，状态在 ret 内 */
		if (ret->status != NVME_SC_SUCCESS && printk_ratelimit())
			NVMEV_ERROR("conv_write status=0x%x sqid=%d opcode=0x%x slba=%llu len=%u\n",
				    ret->status, req->sq_id, cmd->rw.opcode,
				    (unsigned long long)cmd->rw.slba, cmd->rw.length + 1);
		break;
	case nvme_cmd_read:
		NVMEV_DEBUG("[DEBUG] conv_proc_nvme_io_cmd: Calling conv_read\n");
        if (!conv_read(ns, req, ret))
            goto out; /* 出错也返回完成，状态在 ret 内 */
		break;
	case nvme_cmd_flush:
		NVMEV_DEBUG("[DEBUG] conv_proc_nvme_io_cmd: Calling conv_flush\n");
		conv_flush(ns, req, ret);
		break;
	default:
		NVMEV_DEBUG("[DEBUG] conv_proc_nvme_io_cmd: Unimplemented command: %s(%d)\n", 
			   nvme_opcode_string(cmd->common.opcode), cmd->common.opcode);
		break;
	}

out:
	if (spp && cmd->common.opcode == nvme_cmd_write)
		nvmev_lpn_unmark_range(start_lpn, end_lpn);

	if (nvmev_ftl_slow_ns) {
		ftl_dur = local_clock() - ftl_start;
		if (ftl_dur > nvmev_ftl_slow_ns && printk_ratelimit()) {
			NVMEV_ERROR("ftl slow: sqid=%d cid=%u opcode=0x%x nsid=%u slba=%llu len=%u dur_ns=%llu\n",
				    req->sq_id, cmd->common.command_id, cmd->common.opcode,
				    cmd->common.nsid, (unsigned long long)cmd->rw.slba,
				    cmd->rw.length + 1, ftl_dur);
		}
	}

	return true;
}

 /* background threads removed */
