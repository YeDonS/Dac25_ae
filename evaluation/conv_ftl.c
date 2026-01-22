// SPDX-License-Identifier: GPL-2.0-only


#include <linux/sched/clock.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/math64.h>
#include <linux/ctype.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
/* kthread/waitqueue no longer needed (threads removed) */

#include "nvmev.h"
#include "conv_ftl.h"
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#ifndef NVMEV_WARN
#define NVMEV_WARN(fmt, ...) pr_warn("[nvmev] " fmt, ##__VA_ARGS__)
#endif
void enqueue_writeback_io_req(int sqid, unsigned long long nsecs_target,
			      struct buffer *write_buffer, unsigned int buffs_to_release);

#define RECENT_WRITE_GUARD_PCT 10U

static bool recent_write_guard(struct conv_ftl *conv_ftl, uint64_t lpn);

static bool recent_write_guard(struct conv_ftl *conv_ftl, uint64_t lpn);

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
	memset(stats, 0, sizeof(*stats));
	if (slc) {
		uint32_t die_count = conv_ftl->die_count ? conv_ftl->die_count : 1;
		struct line_mgmt *array = conv_ftl->slc_lunlm;
		uint32_t die;

		if (!array)
			return;

		spin_lock(&conv_ftl->slc_lock);
		for (die = 0; die < die_count; die++) {
			struct line_mgmt *lm = &array[die];
			stats->total += lm->tt_lines;
			stats->free += lm->free_line_cnt;
			stats->victim += lm->victim_line_cnt;
			stats->full += lm->full_line_cnt;
	}
	spin_unlock(&conv_ftl->slc_lock);
	return;
	}

	/* QLC 采用 per-die line 池 */
	{
		uint32_t die_count = conv_ftl->die_count ? conv_ftl->die_count : 1;
		struct line_mgmt *array = conv_ftl->qlc_lunlm;
		uint32_t die;

		if (!array)
			return;

		spin_lock(&conv_ftl->qlc_lock);
		for (die = 0; die < die_count; die++) {
			struct line_mgmt *lm = &array[die];

			if (!lm->lines)
				continue;

			stats->total += lm->tt_lines;
			stats->free += lm->free_line_cnt;
			stats->victim += lm->victim_line_cnt;
			stats->full += lm->full_line_cnt;
		}
		spin_unlock(&conv_ftl->qlc_lock);
	}
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

	if (warm)
		type = (conv_ftl->qlc_zone_rr_cursor++ & 0x1) ?
		       QLC_PAGE_TYPE_CL : QLC_PAGE_TYPE_L;
	else
		type = (conv_ftl->qlc_zone_rr_cursor++ & 0x1) ?
		       QLC_PAGE_TYPE_U : QLC_PAGE_TYPE_CU;

	return type;
}

/* 前向声明以避免隐式声明错误 */
static inline struct ppa get_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn);
static inline bool mapped_ppa(struct ppa *ppa);
static bool is_slc_block(struct conv_ftl *conv_ftl, uint32_t blk_id);
static void migrate_page_to_qlc(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *slc_ppa);
static void migrate_page_to_slc(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *qlc_ppa,
				uint64_t *migration_done);
static int qlc_get_new_page(struct conv_ftl *conv_ftl, uint32_t die, uint32_t zone_hint,
			    struct ppa *ppa_out);
static int qlc_get_new_gc_page(struct conv_ftl *conv_ftl, uint32_t die, uint32_t zone_hint,
			       struct ppa *ppa_out);
static void advance_slc_write_pointer(struct conv_ftl *conv_ftl, uint32_t die);
static struct ppa get_new_slc_page(struct conv_ftl *conv_ftl);
/* 新增：GC 专用 SLC 写指针函数声明（仅在本文件使用） */
static void advance_gc_slc_write_pointer(struct conv_ftl *conv_ftl, uint32_t die);
static struct ppa get_new_gc_slc_page(struct conv_ftl *conv_ftl, uint32_t die);
static uint64_t get_dynamic_cold_threshold(struct conv_ftl *conv_ftl);
/* 无阈值：总是尝试从 SLC 迁移少量更冷页面到 QLC */
static void migrate_some_cold_from_slc(struct conv_ftl *conv_ftl, uint32_t max_pages)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct heat_tracking *ht = &conv_ftl->heat_track;
	uint32_t migrated = 0;
	uint32_t scanned = 0;
	static uint64_t cursor2 = 0;
	uint64_t idx;
	uint64_t dyn_thresh;

    NVMEV_DEBUG("[MIGRATION_DEBUG] migrate_some_cold_from_slc called: max_pages=%u\n", max_pages);
    
	if (!conv_ftl->page_in_slc || !ht || !ht->access_count || max_pages == 0) {
		NVMEV_DEBUG("[MIGRATION_DEBUG] Early return: page_in_slc=%p, ht=%p, last_access_time=%p, max_pages=%u\n",
			   conv_ftl->page_in_slc, ht, ht ? ht->last_access_time : NULL, max_pages);
		return;
	}

	dyn_thresh = get_dynamic_cold_threshold(conv_ftl);
	if (dyn_thresh == 0)
		dyn_thresh = 1;
	idx = cursor2 % spp->tt_pgs;
	
	while (scanned < 4096 && migrated < max_pages) {
		if (conv_ftl->page_in_slc[idx]) {
			uint64_t acc = ht->access_count[idx];
			
			if (acc <= dyn_thresh) {
				if (recent_write_guard(conv_ftl, idx))
					goto next_idx2;
				struct ppa old_ppa = get_maptbl_ent(conv_ftl, idx);
				if (mapped_ppa(&old_ppa) && is_slc_block(conv_ftl, old_ppa.g.blk)) {
					NVMEV_DEBUG("[MIGRATION_DEBUG] Found cold page to migrate: lpn=%llu, ppa=ch%d,lun%d,blk%d,pg%d\n",
					       idx, old_ppa.g.ch, old_ppa.g.lun, old_ppa.g.blk, old_ppa.g.pg);
                    /* 执行单页迁移 */
                    migrate_page_to_qlc(conv_ftl, idx, &old_ppa);
                    migrated++;
                } else {
                    NVMEV_DEBUG("[MIGRATION_DEBUG] Cold page not suitable: lpn=%llu, mapped=%d, is_slc=%d\n",
                               idx, mapped_ppa(&old_ppa), mapped_ppa(&old_ppa) ? is_slc_block(conv_ftl, old_ppa.g.blk) : 0);
                }
            }
        }
        
next_idx2:
        scanned++;
        idx = (idx + 1) % spp->tt_pgs;
    }
    
    cursor2 = idx;
    
    NVMEV_DEBUG("[MIGRATION_DEBUG] Migration attempt complete: scanned=%u, migrated=%u\n", scanned, migrated);
    
    if (migrated > 0) {
        NVMEV_DEBUG("Migrated %d pages from SLC to QLC\n", migrated);
    }
}
static inline bool last_pg_in_wordline(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	return (ppa->g.pg % spp->pgs_per_oneshotpg) == (spp->pgs_per_oneshotpg - 1);
}

static inline bool should_gc_high(struct conv_ftl *conv_ftl)
{
	struct line_pool_stats slc_stats, qlc_stats;
	collect_slc_stats(conv_ftl, &slc_stats);
	collect_qlc_stats(conv_ftl, &qlc_stats);
	uint32_t total_free_lines = slc_stats.free + qlc_stats.free;
	return total_free_lines <= conv_ftl->cp.gc_thres_lines_high;
}
static inline bool should_gc_slc_high(struct conv_ftl *conv_ftl)
{
	struct line_pool_stats slc_stats;
	collect_slc_stats(conv_ftl, &slc_stats);
	return slc_stats.free <= conv_ftl->slc_gc_free_thres_high;
}

static inline bool should_gc_qlc_high(struct conv_ftl *conv_ftl)
{
	struct line_pool_stats qlc_stats;
	collect_qlc_stats(conv_ftl, &qlc_stats);
	return qlc_stats.free <= conv_ftl->qlc_gc_free_thres_high;
}
static inline struct ppa get_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn)
{
	return conv_ftl->maptbl[lpn];
}

static inline void set_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *ppa)
{
	NVMEV_ASSERT(lpn < conv_ftl->ssd->sp.tt_pgs);
	conv_ftl->maptbl[lpn] = *ppa;
}

static inline void clear_lpn_mapping(struct conv_ftl *conv_ftl, uint64_t lpn)
{
	struct ppa invalid = { .ppa = UNMAPPED_PPA };

	set_maptbl_ent(conv_ftl, lpn, &invalid);
	if (conv_ftl->page_in_slc)
		conv_ftl->page_in_slc[lpn] = false;
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

static inline uint64_t get_rmap_ent(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(conv_ftl, ppa);

	return conv_ftl->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(conv_ftl, ppa);

	conv_ftl->rmap[pgidx] = lpn;
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
	/* User requested to disable the 10% recent write guard */
	return false;
}

static void update_qlc_latency_zone(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *ppa)
{
	struct heat_tracking *ht = &conv_ftl->heat_track;
	struct nand_page *pg;
	uint64_t old_cnt, new_cnt;
	uint64_t read_cnt = 0;
	uint64_t avg_reads;
	uint8_t zone;

	if (!conv_ftl->qlc_page_wcnt)
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
	uint32_t dies = conv_ftl->die_count ? conv_ftl->die_count : 1;
	return conv_ftl->slc_blks_per_pl * dies;
}

static inline uint32_t total_qlc_lines(const struct conv_ftl *conv_ftl)
{
	uint32_t dies = conv_ftl->die_count ? conv_ftl->die_count : 1;

	return conv_ftl->qlc_blks_per_pl * dies;
}

static inline uint64_t total_slc_pages(const struct conv_ftl *conv_ftl)
{
	return (uint64_t)total_slc_lines(conv_ftl) * conv_ftl->slc_pgs_per_blk;
}

static inline uint64_t total_qlc_pages(const struct conv_ftl *conv_ftl)
{
	uint32_t dies = conv_ftl->die_count ? conv_ftl->die_count : 1;
	return (uint64_t)conv_ftl->qlc_blks_per_pl * conv_ftl->qlc_pgs_per_blk * dies;
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
		uint32_t die;

		if (!array)
			return false;

		spin_lock(lock);
		for (die = 0; die < die_count; die++) {
			struct line_mgmt *lm = &array[die];
			struct line *candidate = pqueue_peek(lm->victim_line_pq);

			if (!candidate)
				continue;
			if (!force && candidate->vpc > (spp->pgs_per_line / 8))
				continue;
			if (!best || candidate->vpc < best->vpc) {
				best = candidate;
				best_die = die;
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

		if (!array || die_count == 0)
			return false;

		uint32_t die = cand->die % die_count;
		struct line_mgmt *lm = &array[die];

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
	if (line->pos) {
		pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
	} else {
		if (line->vpc > 0)
			line->vpc--;
	}

	if (was_full_line) {
		list_del_init(&line->entry);
		lm->full_line_cnt--;
		pqueue_insert(lm->victim_line_pq, line);
		lm->victim_line_cnt++;
	}
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

static void forground_gc(struct conv_ftl *conv_ftl);

static inline void check_and_refill_write_credit(struct conv_ftl *conv_ftl)
{
	struct write_flow_control *wfc = &(conv_ftl->wfc);
	if (wfc->write_credits <= 0) {
		/* 前台小步 GC：避免后台线程，快速释放少量行 */
		forground_gc(conv_ftl);
		/* 小额补充信用，允许写入继续推进 */
		wfc->write_credits += 10;
	}
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

	conv_ftl->maptbl = vmalloc(sizeof(struct ppa) * spp->tt_pgs);
	if (!conv_ftl->maptbl) {
		NVMEV_ERROR("Failed to allocate mapping table memory\n");
        conv_ftl->maptbl_initialized = false;
		return;
	}

    for (i = 0; i < spp->tt_pgs; i++) {
		conv_ftl->maptbl[i].ppa = UNMAPPED_PPA;
	}
    conv_ftl->maptbl_initialized = true;
}

static void remove_maptbl(struct conv_ftl *conv_ftl)
{
	vfree(conv_ftl->maptbl);
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
	
	/* 初始化所有数组 */
	for (i = 0; i < spp->tt_pgs; i++) {
		ht->access_count[i] = 0;
		ht->last_access_time[i] = 0;
		ht->write_epoch[i] = 0;
		conv_ftl->page_in_slc[i] = false;
	}
	
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
	conv_ftl->heat_track.access_count = NULL;
	conv_ftl->heat_track.last_access_time = NULL;
	conv_ftl->heat_track.write_epoch = NULL;
	conv_ftl->page_in_slc = NULL;
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
	conv_ftl->slc_pgs_per_blk = ssd->sp.pgs_per_blk;
	conv_ftl->qlc_pgs_per_blk = conv_ftl->slc_pgs_per_blk * QLC_BLOCK_CAPACITY_FACTOR;
	conv_ftl->debug_access_count = NULL;

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

	/* initialize maptbl */
	NVMEV_INFO("initialize maptbl\n");
	init_maptbl(conv_ftl); // mapping table

	/* initialize rmap */
	NVMEV_INFO("initialize rmap\n");
	init_rmap(conv_ftl); // reverse mapping table (?)

	/* 删除旧的 init_lines 调用 - 使用新的 SLC/QLC 系统 */

	/* 初始化 SLC/QLC 混合存储 */
	NVMEV_INFO("initialize SLC/QLC blocks\n");
	init_slc_qlc_blocks(conv_ftl);
	
	if (init_per_die_line_mgmt(conv_ftl, true) != 0) {
		NVMEV_ERROR("Failed to initialize per-die SLC line managers\n");
		return;
	}
	
	if (init_qlc_lines(conv_ftl) != 0) {
		NVMEV_ERROR("Failed to initialize per-die QLC line managers\n");
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
	spin_lock_init(&conv_ftl->qlc_zone_lock);

	/* 直接初始化水位线（无后台线程） */
	{
		uint64_t slc_total_pages = total_slc_pages(conv_ftl);
		uint64_t qlc_total_pages = total_qlc_pages(conv_ftl);
		uint64_t tmp;

		tmp = div_u64(slc_total_pages * 80, 100);
		conv_ftl->slc_high_watermark = pages_to_lines(tmp, conv_ftl->slc_pgs_per_blk);

		tmp = div_u64(slc_total_pages * 75, 100);
		conv_ftl->slc_target_watermark = pages_to_lines(tmp, conv_ftl->slc_pgs_per_blk);

		tmp = div_u64(slc_total_pages * 25, 100);
		conv_ftl->slc_gc_free_thres_high = pages_to_lines(tmp, conv_ftl->slc_pgs_per_blk);

		tmp = div_u64(qlc_total_pages * 15, 100);
		conv_ftl->qlc_gc_free_thres_high = pages_to_lines(tmp, conv_ftl->qlc_pgs_per_blk);

		tmp = div_u64(slc_total_pages * 10, 100);
		conv_ftl->slc_repromote_guard_lines = pages_to_lines(tmp, conv_ftl->slc_pgs_per_blk);
	}

	NVMEV_INFO("Init FTL Instance with %d channels(%ld pages)\n", conv_ftl->ssd->sp.nchs,
		   conv_ftl->ssd->sp.tt_pgs);
	NVMEV_INFO("SLC/QLC Hybrid Mode: SLC %d blks, QLC %d blks, QLC zones per line=%u\n", 
		   conv_ftl->slc_blks_per_pl, conv_ftl->qlc_blks_per_pl, QLC_ZONE_COUNT);
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
	}

	return;
}

static void conv_remove_ftl(struct conv_ftl *conv_ftl)
{
	/* 无后台线程可停止（同步模式） */

	if (conv_ftl->debug_dir) {
		debugfs_remove_recursive(conv_ftl->debug_dir);
		conv_ftl->debug_dir = NULL;
		conv_ftl->debug_access_count = NULL;
		conv_ftl->debug_access_inject = NULL;
	} else {
		if (conv_ftl->debug_access_count) {
			debugfs_remove(conv_ftl->debug_access_count);
			conv_ftl->debug_access_count = NULL;
		}
		if (conv_ftl->debug_access_inject) {
			debugfs_remove(conv_ftl->debug_access_inject);
			conv_ftl->debug_access_inject = NULL;
		}
	}
	
	/* 清理 SLC/QLC 相关资源 */
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

	conv_ftls = kmalloc(sizeof(struct conv_ftl) * nr_parts, GFP_KERNEL);

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

	if (is_slc_block(conv_ftl, ppa->g.blk)) {
		struct line_mgmt *lm = get_slc_die_lm(conv_ftl, die);
		if (!lm || !lm->lines || ppa->g.blk >= lm->tt_lines)
			return NULL;
		return &lm->lines[ppa->g.blk];
	}

	if (ppa->g.blk < conv_ftl->slc_blks_per_pl)
		return NULL;

	struct line_mgmt *lm = get_qlc_die_lm(conv_ftl, die);
	if (!lm || !lm->lines)
		return NULL;

	uint32_t idx = ppa->g.blk - conv_ftl->slc_blks_per_pl;
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
    /* 1. 增加全局参数验证 */
    if (!conv_ftl || !ppa || !conv_ftl->ssd) {
        NVMEV_ERROR("[mark_page_invalid] Invalid parameters.\n");
        return;
    }
    
    struct ssdparams *spp = &conv_ftl->ssd->sp;
    struct nand_block *blk;
    struct nand_page *pg;
	struct heat_tracking *ht = &conv_ftl->heat_track;
	uint64_t lpn = INVALID_LPN;
	uint64_t read_cnt = 0;

    /* 更新页和块的状态 (这部分不涉及共享数据结构，可以在锁外完成) */
    pg = get_pg(conv_ftl->ssd, ppa);
    
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
    
	pg->status = PG_INVALID;
	pg->oob_prev_lpn = INVALID_LPN;

	lpn = get_rmap_ent(conv_ftl, ppa);
	if (lpn != INVALID_LPN && ht && ht->access_count)
		read_cnt = ht->access_count[lpn];

	if (conv_ftl->global_read_sum >= read_cnt)
		conv_ftl->global_read_sum -= read_cnt;
	else
		conv_ftl->global_read_sum = 0;

	if (lpn != INVALID_LPN && conv_ftl->global_valid_pg_cnt > 0)
		conv_ftl->global_valid_pg_cnt--;

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

    /* 2. 根据介质类型，进入完全独立的原子操作块 */
    bool in_slc = is_slc_block(conv_ftl, ppa->g.blk);
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
	slc_apply_line_invalid(lm, ppa->g.blk, spp);
	spin_unlock(&conv_ftl->slc_lock); // --- SLC 解锁 ---

	} else { // QLC 路径
		uint32_t die = encode_die(spp, ppa);
		struct line_mgmt *lm = get_qlc_die_lm(conv_ftl, die);
		struct line *line;
		uint32_t start_blk = conv_ftl->slc_blks_per_pl;
		uint32_t idx;
		bool was_full_line = false;
		if (lpn != INVALID_LPN) {
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

        spin_unlock(&conv_ftl->qlc_lock); // --- QLC 解锁 ---
    }
}

static void mark_page_valid(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
    NVMEV_DEBUG("Entering mark_page_valid: ch=%d, lun=%d, blk=%d, pg=%d\n",
                ppa ? ppa->g.ch : -1, ppa ? ppa->g.lun : -1, 
                ppa ? ppa->g.blk : -1, ppa ? ppa->g.pg : -1);
	/* 1. 增加全局参数验证 */
    if (!conv_ftl || !ppa || !conv_ftl->ssd) {
        NVMEV_ERROR("[mark_page_valid] Invalid parameters.\n");
        return;
    }
    
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct heat_tracking *ht = &conv_ftl->heat_track;
	uint64_t lpn;
	uint64_t read_cnt;
	struct nand_block *blk;
	struct nand_page *pg;
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
    bool in_slc = is_slc_block(conv_ftl, ppa->g.blk);
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
        struct line *line = &lm->lines[idx];
        if (line->vpc < conv_ftl->qlc_pgs_per_blk)
            line->vpc++;
        else
            line->vpc = conv_ftl->qlc_pgs_per_blk;
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

	/* reset block status */
	NVMEV_ASSERT(blk->npgs ==
		     (blk->is_qlc ? conv_ftl->qlc_pgs_per_blk : spp->pgs_per_blk));
	blk->ipc = 0;
	blk->vpc = 0;
	blk->erase_cnt++;
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct conv_ftl *conv_ftl, struct ppa *old_ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	struct ppa new_ppa;
	uint64_t lpn = get_rmap_ent(conv_ftl, old_ppa);
	struct nand_page *old_pg = get_pg(conv_ftl->ssd, old_ppa);
	uint64_t stored_prev_lpn = old_pg ? old_pg->oob_prev_lpn : INVALID_LPN;
	uint32_t target_ch = old_ppa->g.ch;
	uint32_t target_lun = old_ppa->g.lun;
	uint32_t dies = total_dies(spp);
	uint32_t src_die = encode_die(spp, old_ppa);
	struct heat_tracking *ht = &conv_ftl->heat_track;
	uint64_t old_read_cnt = (ht && ht->access_count && lpn != INVALID_LPN) ?
		ht->access_count[lpn] : 0;
	bool old_in_slc;
/* int prev_die_log = -1;
 	struct ppa prev_ppa = { .ppa = UNMAPPED_PPA };
*/
	NVMEV_ASSERT(valid_lpn(conv_ftl, lpn));

	if (!valid_ppa(conv_ftl, old_ppa)) {
		NVMEV_ERROR("gc_write_page: invalid source PPA ch=%d lun=%d blk=%d pg=%d, clearing lpn=%llu\n",
			    old_ppa->g.ch, old_ppa->g.lun, old_ppa->g.blk, old_ppa->g.pg, lpn);
		clear_lpn_mapping(conv_ftl, lpn);
		return 0;
	}

	if (lpn != INVALID_LPN) {
		if (conv_ftl->global_read_sum >= old_read_cnt)
			conv_ftl->global_read_sum -= old_read_cnt;
		else
			conv_ftl->global_read_sum = 0;

		if (conv_ftl->global_valid_pg_cnt > 0)
			conv_ftl->global_valid_pg_cnt--;
	}

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
		uint32_t die_index = 0;

		if (dies)
			die_index = target_lun * spp->nchs + target_ch;
		if (conv_ftl->die_count)
			die_index %= conv_ftl->die_count;
		else
			die_index = 0;

		conv_ftl->lunpointer = die_index;

		if (conv_ftl->gc_slc_lunwp) {
			struct write_pointer *gc_wp = &conv_ftl->gc_slc_lunwp[die_index];
			if (!gc_wp->curline || gc_wp->pg == 0) {
				gc_wp->ch = target_ch;
				gc_wp->lun = target_lun;
			}
		}

		new_ppa = get_new_gc_slc_page(conv_ftl, die_index);
		if (!mapped_ppa(&new_ppa)) {
			NVMEV_ERROR("gc_write_page: Failed to get new SLC page (gc_wp).\n");
			return 0;
		}
		set_maptbl_ent(conv_ftl, lpn, &new_ppa);
		set_rmap_ent(conv_ftl, lpn, &new_ppa);
		mark_page_valid(conv_ftl, &new_ppa);
		set_page_prev_link(conv_ftl, lpn, &new_ppa, stored_prev_lpn);
		advance_gc_slc_write_pointer(conv_ftl, die_index);
		NVMEV_DEBUG("[TASK2][GC-SLC] lpn=%llu prev_lpn=%lld src_die=%u dst_die=%u",
			lpn,
			stored_prev_lpn == INVALID_LPN ? -1LL : (long long)stored_prev_lpn,
/*			prev_die_log, */
			src_die,
			encode_die(spp, &new_ppa));
	} else {
		uint32_t zone_hint = old_pg ? old_pg->qlc_latency_zone : 0;
		if (lpn != INVALID_LPN) {
			unsigned long stat_flags;
			spin_lock_irqsave(&conv_ftl->qlc_zone_lock, stat_flags);
			if (conv_ftl->qlc_resident_page_cnt > 0)
				conv_ftl->qlc_resident_page_cnt--;
			if (conv_ftl->qlc_resident_read_sum >= old_read_cnt)
				conv_ftl->qlc_resident_read_sum -= old_read_cnt;
			else
				conv_ftl->qlc_resident_read_sum = 0;
			spin_unlock_irqrestore(&conv_ftl->qlc_zone_lock, stat_flags);
		}

		if (zone_hint >= QLC_ZONE_COUNT)
			zone_hint = QLC_ZONE_COUNT - 1;

		/* QLC GC Die Affinity: prefer to stay on the source die */
		if (conv_ftl->die_count)
			src_die %= conv_ftl->die_count;

		if (qlc_get_new_gc_page(conv_ftl, src_die, zone_hint, &new_ppa) != 0) {
			NVMEV_ERROR("gc_write_page: Failed to get new QLC GC page (zone_hint=%u).\n",
				    zone_hint);
			return 0;
		}
		set_maptbl_ent(conv_ftl, lpn, &new_ppa);
		set_rmap_ent(conv_ftl, lpn, &new_ppa);
		mark_page_valid(conv_ftl, &new_ppa);
		set_page_prev_link(conv_ftl, lpn, &new_ppa, stored_prev_lpn);
		update_qlc_latency_zone(conv_ftl, lpn, &new_ppa);
		NVMEV_DEBUG("[TASK2][GC-QLC] lpn=%llu prev_lpn=%lld src_die=%u dst_die=%u zone_hint=%u",
			lpn,
			stored_prev_lpn == INVALID_LPN ? -1LL : (long long)stored_prev_lpn,
			//prev_die_log,
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
	bool in_slc = is_slc_block(conv_ftl, ppa->g.blk);
	uint32_t die = encode_die(spp, ppa);

	if (in_slc) {
		struct line_mgmt *lm = get_slc_die_lm(conv_ftl, die);
		uint32_t line_id = line_from_blk(ppa->g.blk);

		if (!lm || !lm->lines || line_id >= lm->tt_lines)
			return;

		spin_lock(&conv_ftl->slc_lock);
		struct line *line = &lm->lines[line_id];
		line->ipc = 0;
		line->vpc = 0;
		list_add_tail(&line->entry, &lm->free_line_list);
		lm->free_line_cnt++;
		spin_unlock(&conv_ftl->slc_lock);
	} else {
		uint32_t start_blk = conv_ftl->slc_blks_per_pl;
		uint32_t line_id = line_from_blk(ppa->g.blk);
		if (ppa->g.blk < start_blk)
			return;
		uint32_t idx = line_id - start_blk;
		struct line_mgmt *lm = get_qlc_die_lm(conv_ftl, die);

		if (!lm || !lm->lines || idx >= lm->tt_lines)
			return;

		spin_lock(&conv_ftl->qlc_lock);
		struct line *line = &lm->lines[idx];
		line->ipc = 0;
		line->vpc = 0;
		list_add_tail(&line->entry, &lm->free_line_list);
		lm->free_line_cnt++;
		spin_unlock(&conv_ftl->qlc_lock);
	}
}


static int do_gc(struct conv_ftl *conv_ftl, bool force, int target_pool)
{
	struct victim_candidate victim;
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct ppa ppa;
	int pg;
	int max_pgs = spp->pgs_per_blk;
	bool in_slc;
	struct convparams *cpp;
	uint32_t ch = 0, lun = 0;

	if (!select_victim_line(conv_ftl, force, target_pool, &victim)) {
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

	conv_ftl->wfc.credits_to_refill = victim.line->ipc;
	{
		struct nand_block *victim_blk = get_blk(conv_ftl->ssd, &ppa);
		if (victim_blk)
			max_pgs = victim_blk->npgs;
	}

	/*
	 * === 核心修复：简化循环，只处理受害块内的页面 ===
	 * 遍历受害块中的每一个页，搬走有效数据
	 */
	for (pg = 0; pg < max_pgs; pg++) {
		struct nand_page *pg_iter = NULL;

		ppa.g.pg = pg;
		pg_iter = get_pg(conv_ftl->ssd, &ppa);

		/* 如果页面有效，就把它搬走 */
		if (pg_iter && pg_iter->status == PG_VALID) {
			gc_write_page(conv_ftl, &ppa);
		}
	}

	/*
	 * 所有有效页都已搬走，现在擦除整个块并更新状态
	 */
	ppa.g.pg = 0; /* 重置页号 */
	mark_block_free(conv_ftl, &ppa);
	
	/* 如果启用了GC延迟，执行实际的擦除操作 */
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

	/* 更新line状态，将其放回空闲列表 */
	mark_line_free(conv_ftl, &ppa);

	return 0;
}

static void forground_gc(struct conv_ftl *conv_ftl)
{
	/* 优先保障 SLC：当 SLC free 行数过低时，仅清 SLC 受害者 */
	if (should_gc_slc_high(conv_ftl)) {
		do_gc(conv_ftl, true, 1);
		return;
	}
	/* 其次保障 QLC：当 QLC free 行数过低时，仅清 QLC 受害者 */
	if (should_gc_qlc_high(conv_ftl)) {
		do_gc(conv_ftl, true, 2);
		return;
	}
	/* 兜底：总剩余过低时，任意清理 */
	if (should_gc_high(conv_ftl)) {
		do_gc(conv_ftl, true, 0);
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

/* 获取 SLC 的新页面 - 使用 Die Affinity */
static struct ppa get_new_slc_page(struct conv_ftl *conv_ftl)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct ppa ppa;
	struct nand_page *pg;
	uint32_t die;
	struct write_pointer *wp;
	struct line_mgmt *lm;

	if (!conv_ftl || !conv_ftl->slc_lunwp) {
		NVMEV_ERROR("SLC lines or write pointers not initialized\n");
		return (struct ppa){ .ppa = UNMAPPED_PPA };
	}

	die = conv_ftl->lunpointer % conv_ftl->die_count;
	wp = &conv_ftl->slc_lunwp[die];
	lm = get_slc_die_lm(conv_ftl, die);
	if (!lm || !lm->lines) {
		NVMEV_ERROR("SLC line manager missing for die %u\n", die);
		return (struct ppa){ .ppa = UNMAPPED_PPA };
	}

	if (!wp->curline) {
		spin_lock(&conv_ftl->slc_lock);
		struct line *curline = list_first_entry_or_null(&lm->free_line_list, struct line, entry);

		if (!curline) {
			NVMEV_ERROR("No free SLC line available!\n");
			spin_unlock(&conv_ftl->slc_lock);
			return (struct ppa){ .ppa = UNMAPPED_PPA };
		}

		NVMEV_DEBUG("[DEBUG] get_new_slc_page: Allocated SLC line with ID %u (range: [0, %u), total_lines=%u)\n",
			    curline->id, conv_ftl->slc_blks_per_pl, lm->tt_lines);
		if (curline->id >= conv_ftl->slc_blks_per_pl) {
			NVMEV_ERROR("[CRITICAL] get_new_slc_page: SLC line ID %u exceeds range [0, %u)\n",
				    curline->id, conv_ftl->slc_blks_per_pl);
		}

		list_del_init(&curline->entry);
		lm->free_line_cnt--;

		decode_die(spp, die, &wp->ch, &wp->lun);
		wp->curline = curline;
		wp->blk = curline->id;
		wp->pg = 0;
		wp->pl = 0;
		spin_unlock(&conv_ftl->slc_lock);
	}

retry_get_page:
	ppa.ppa = 0;
	ppa.g.ch = wp->ch;
	ppa.g.lun = wp->lun;
	ppa.g.pg = wp->pg;
	ppa.g.blk = wp->blk;
	ppa.g.pl = wp->pl;

	pg = get_pg(conv_ftl->ssd, &ppa);
	if (!pg)
		return (struct ppa){ .ppa = UNMAPPED_PPA };

	if (pg->status != PG_FREE) {
		advance_slc_write_pointer(conv_ftl, die);
		goto retry_get_page;
	}

	if (ppa.g.blk >= conv_ftl->slc_blks_per_pl) {
		NVMEV_ERROR("get_new_slc_page: Generated invalid SLC block %u >= %u\n",
			    ppa.g.blk, conv_ftl->slc_blks_per_pl);
	}

	return ppa;
}

/* 推进 SLC 写指针 - 使用 Die Affinity */
static void advance_slc_write_pointer(struct conv_ftl *conv_ftl, uint32_t die)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct write_pointer *wp;
	struct line_mgmt *lm;

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
		spin_lock(&conv_ftl->slc_lock);
		if (wp->curline->vpc == spp->pgs_per_lun_line) {
			list_add_tail(&wp->curline->entry, &lm->full_line_list);
			lm->full_line_cnt++;
		} else {
			pqueue_insert(lm->victim_line_pq, wp->curline);
			lm->victim_line_cnt++;
		}

		wp->curline = list_first_entry_or_null(&lm->free_line_list, struct line, entry);
		if (!wp->curline) {
			NVMEV_ERROR("No free SLC line available when advancing WP (die=%u)!\n", die);
			wp->curline = NULL;
			spin_unlock(&conv_ftl->slc_lock);
			return;
		}

		NVMEV_DEBUG("advance_slc_write_pointer: Allocated new SLC line with ID %u for die %u\n",
			    wp->curline->id, die);
		list_del_init(&wp->curline->entry);
		lm->free_line_cnt--;

		wp->blk = wp->curline->id;
		wp->pg = 0;
		wp->pl = 0;
		spin_unlock(&conv_ftl->slc_lock);
	}

	if ((wp->pg % spp->pgs_per_oneshotpg) == 0)
		conv_ftl->lunpointer = (die + 1) % conv_ftl->die_count;
}

/* 新增：GC 专用 SLC 页面获取（使用 per-die GC 写指针） */
static struct ppa get_new_gc_slc_page(struct conv_ftl *conv_ftl, uint32_t die)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct ppa ppa;
	struct write_pointer *wp;
	struct line_mgmt *lm = NULL;
	struct nand_page *pg;

	if (!conv_ftl->gc_slc_lunwp)
		return (struct ppa){ .ppa = UNMAPPED_PPA };

	if (conv_ftl->die_count == 0)
		return (struct ppa){ .ppa = UNMAPPED_PPA };

	die %= conv_ftl->die_count;
	wp = &conv_ftl->gc_slc_lunwp[die];
	lm = get_slc_die_lm(conv_ftl, die);
	if (!lm || !lm->lines)
		return (struct ppa){ .ppa = UNMAPPED_PPA };

	if (!wp->curline) {
		spin_lock(&conv_ftl->slc_lock);
		struct line *curline = list_first_entry_or_null(&lm->free_line_list, struct line, entry);

		if (!curline) {
			NVMEV_ERROR("No free SLC line available for GC (die=%u)!\n", die);
			spin_unlock(&conv_ftl->slc_lock);
			return (struct ppa){ .ppa = UNMAPPED_PPA };
		}

		list_del_init(&curline->entry);
		lm->free_line_cnt--;

		decode_die(spp, die, &wp->ch, &wp->lun);
		wp->curline = curline;
		wp->blk = curline->id;
		wp->pg = 0;
		wp->pl = 0;
		spin_unlock(&conv_ftl->slc_lock);
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
		return (struct ppa){ .ppa = UNMAPPED_PPA };

	if (pg->status != PG_FREE) {
		advance_gc_slc_write_pointer(conv_ftl, die);
		goto retry_gc_get_page;
	}

	return ppa;
}

/* 新增：GC 专用 SLC 写指针推进（使用 per-die GC 写指针） */
static void advance_gc_slc_write_pointer(struct conv_ftl *conv_ftl, uint32_t die)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct write_pointer *wp;
	struct line_mgmt *lm;

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
		spin_lock(&conv_ftl->slc_lock);
		if (wp->curline->vpc == spp->pgs_per_lun_line) {
			list_add_tail(&wp->curline->entry, &lm->full_line_list);
			lm->full_line_cnt++;
		} else {
			pqueue_insert(lm->victim_line_pq, wp->curline);
			lm->victim_line_cnt++;
		}

		wp->curline = list_first_entry_or_null(&lm->free_line_list, struct line, entry);
		if (!wp->curline) {
			NVMEV_ERROR("No free SLC line available for GC during advance (die=%u)!\n", die);
			wp->curline = NULL;
			spin_unlock(&conv_ftl->slc_lock);
			return;
		}

		list_del_init(&wp->curline->entry);
		lm->free_line_cnt--;
		wp->blk = wp->curline->id;
		wp->pg = 0;
		wp->pl = 0;
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

	wp->curline = line;
	wp->blk = blk_from_line(line->id);
	qlc_reset_die_progress(wp);
	decode_die(spp, die, &wp->ch, &wp->lun);
	spin_unlock(&conv_ftl->qlc_lock);

	return line;
}

static void qlc_close_active_line(struct conv_ftl *conv_ftl, struct write_pointer *wp,
				       struct line_mgmt *lm)
{
	struct line *line;
	uint64_t full_threshold;

	if (!wp || !wp->curline)
		return;

	line = wp->curline;
	full_threshold = conv_ftl->qlc_pgs_per_blk;

	spin_lock(&conv_ftl->qlc_lock);
	if (line->vpc >= full_threshold) {
		list_add_tail(&line->entry, &lm->full_line_list);
		lm->full_line_cnt++;
	} else {
		pqueue_insert(lm->victim_line_pq, line);
		lm->victim_line_cnt++;
	}
	wp->curline = NULL;
	qlc_reset_die_progress(wp);
	spin_unlock(&conv_ftl->qlc_lock);
}

static void qlc_record_page_write(struct conv_ftl *conv_ftl, struct write_pointer *wp,
				  struct line_mgmt *lm)
{
	struct ssdparams *spp;

	if (!conv_ftl || !wp)
		return;

	spp = &conv_ftl->ssd->sp;
	wp->pg++;
	if (wp->pg >= (uint32_t)conv_ftl->qlc_pgs_per_blk) {
		wp->pg = 0;
		qlc_close_active_line(conv_ftl, wp, lm);
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
				qlc_record_page_write(conv_ftl, wp, lm);
				return 0;
			}
			qlc_close_active_line(conv_ftl, wp, lm);
		}
	}

	return -ENOSPC;
}

static int qlc_get_new_page(struct conv_ftl *conv_ftl, uint32_t die, uint32_t zone_hint,
			    struct ppa *ppa_out)
{
	struct line_mgmt *lm;
	struct write_pointer *wp;

	if (!conv_ftl || !conv_ftl->die_count)
		return -ENOSPC;
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
static void migrate_page_to_qlc(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *slc_ppa)
{
    struct ssdparams *spp = &conv_ftl->ssd->sp;
    struct ppa new_ppa;
    struct nand_cmd srd, swr;
    uint64_t nsecs_completed;
    
    /* 参数验证 */
    if (!conv_ftl || !slc_ppa || !mapped_ppa(slc_ppa)) {
        NVMEV_ERROR("Invalid parameters for page migration\n");
        return;
    }

    if (!valid_ppa(conv_ftl, slc_ppa)) {
        NVMEV_ERROR("migrate_page_to_qlc: invalid SLC PPA ch=%d lun=%d blk=%d pg=%d for lpn=%llu, drop mapping\n",
                    slc_ppa->g.ch, slc_ppa->g.lun, slc_ppa->g.blk, slc_ppa->g.pg, lpn);
        clear_lpn_mapping(conv_ftl, lpn);
        return;
    }
    
    /* 验证页面确实在 SLC 中 */
    if (!is_slc_block(conv_ftl, slc_ppa->g.blk)) {
        NVMEV_ERROR("Page not in SLC, cannot migrate\n");
        return;
    }
    
    /* 验证页面状态是有效的 */
    struct nand_page *pg = get_pg(conv_ftl->ssd, slc_ppa);
    if (!pg || pg->status != PG_VALID) {
        NVMEV_DEBUG("Page not valid for migration: status=%d at ch=%d,lun=%d,blk=%d,pg=%d\n",
                   pg ? pg->status : -1, slc_ppa->g.ch, slc_ppa->g.lun, slc_ppa->g.blk, slc_ppa->g.pg);
        return;
    }
    
	/* 选择目标 die ，尽量让迁移后的页与前驱相邻 */
	uint64_t stored_prev_lpn = pg->oob_prev_lpn;
	uint32_t src_die = encode_die(spp, slc_ppa);
	uint32_t target_die = src_die;
	uint32_t target_ch = slc_ppa->g.ch;
	uint32_t target_lun = slc_ppa->g.lun;
	int prev_die_log = -1;
	struct ppa prev_ppa = { .ppa = UNMAPPED_PPA };
	uint32_t dies = total_dies(spp);

	struct heat_tracking *ht = &conv_ftl->heat_track;
	uint64_t read_cnt = 0;
	uint64_t migration_avg = 0;
	bool have_mig_avg;
	uint32_t zone_hint;
	unsigned long mig_flags;

	if (ht && ht->access_count)
		read_cnt = ht->access_count[lpn];

	have_mig_avg = calc_migration_avg_reads(conv_ftl, &migration_avg);
	if (!have_mig_avg)
		migration_avg = read_cnt;

	spin_lock_irqsave(&conv_ftl->qlc_zone_lock, mig_flags);
	zone_hint = pick_locked_qlc_page_type(conv_ftl, read_cnt >= migration_avg);
	spin_unlock_irqrestore(&conv_ftl->qlc_zone_lock, mig_flags);

	if (stored_prev_lpn != INVALID_LPN) {
		prev_ppa = get_maptbl_ent(conv_ftl, stored_prev_lpn);
		if (mapped_ppa(&prev_ppa) && valid_ppa(conv_ftl, &prev_ppa)) {
			uint32_t prev_die = encode_die(spp, &prev_ppa);
			prev_die_log = (int)prev_die;
			target_die = next_adjacent_die(spp, prev_die);
			decode_die(spp, target_die, &target_ch, &target_lun);
		} else {
			stored_prev_lpn = INVALID_LPN;
		}
	}
	if (stored_prev_lpn == INVALID_LPN && dies) {
		target_die = next_adjacent_die(spp, src_die);
		decode_die(spp, target_die, &target_ch, &target_lun);
	}

	target_die = conv_ftl->die_count ? (target_die % conv_ftl->die_count) : 0;

	if (qlc_get_new_page(conv_ftl, target_die, zone_hint, &new_ppa) != 0) {
		NVMEV_DEBUG("[MIGRATION_DEBUG] Failed to allocate QLC page (zone_hint=%u)\n",
			    zone_hint);
		return;
	}
    
    /* 读取 SLC 页面 */
    srd.type = USER_IO;
    srd.cmd = NAND_READ;
    srd.stime = __get_ioclock(conv_ftl->ssd);
    srd.interleave_pci_dma = false;
    srd.xfer_size = spp->pgsz;
    srd.ppa = slc_ppa;
    
    nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd);
    
    /* 写入 QLC 页面 */
    swr.type = USER_IO;
    swr.cmd = NAND_WRITE;
    swr.stime = nsecs_completed;
    swr.interleave_pci_dma = false;
    swr.xfer_size = spp->pgsz;
    swr.ppa = &new_ppa;
    
    ssd_advance_nand(conv_ftl->ssd, &swr);
    
    /* 更新映射表 */
    set_maptbl_ent(conv_ftl, lpn, &new_ppa);
	set_rmap_ent(conv_ftl, lpn, &new_ppa);
    
    /* 标记旧页面无效 */
    mark_page_invalid(conv_ftl, slc_ppa);
    set_rmap_ent(conv_ftl, INVALID_LPN, slc_ppa);
    
	/* 更新元数据 */
	conv_ftl->page_in_slc[lpn] = false;
	mark_page_valid(conv_ftl, &new_ppa);
	set_page_prev_link(conv_ftl, lpn, &new_ppa, stored_prev_lpn);
	update_qlc_latency_zone(conv_ftl, lpn, &new_ppa);
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

    /* 更新统计 */
    conv_ftl->migration_cnt++;
    
    NVMEV_DEBUG("Migrated LPN %llu from SLC to QLC (zone_hint=%u)\n", lpn, zone_hint);
}
static bool conv_read(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];
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

	struct ppa prev_ppa;
	struct nand_cmd srd = {
		.type = USER_IO,
		.cmd = NAND_READ,
		.stime = nsecs_start,
		.interleave_pci_dma = true,
	};

	NVMEV_ASSERT(conv_ftls);
	NVMEV_DEBUG("conv_read: start_lpn=%lld, len=%lld, end_lpn=%lld", start_lpn, nr_lba, end_lpn);
    if ((end_lpn / nr_parts) >= spp->tt_pgs) {
        NVMEV_ERROR("conv_read: lpn passed FTL range(start_lpn=%lld,tt_pgs=%ld)\n",
                    start_lpn, spp->tt_pgs);
        ret->status = NVME_SC_LBA_RANGE;
        ret->nsecs_target = nsecs_start;
        return true; /* Return completion with error to avoid host timeout */
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
		prev_ppa = get_maptbl_ent(conv_ftl, start_lpn / nr_parts);
		has_avg = calc_global_avg_reads(conv_ftl, &avg_reads);
		ht = &conv_ftl->heat_track;

		/* normal IO read path */
		for (lpn = start_lpn; lpn <= end_lpn; lpn += nr_parts) {
			uint64_t local_lpn;
			struct ppa cur_ppa;

			local_lpn = lpn / nr_parts;
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

			if (!is_slc_block(conv_ftl, cur_ppa.g.blk) && has_avg &&
			    ht && ht->access_count) {
				uint64_t access_cnt = ht->access_count[local_lpn];

				if (access_cnt > avg_reads) {
					uint64_t migration_done = 0;
					uint64_t mig_start = ktime_get_ns();
					NVMEV_DEBUG("[REPROMOTION_VERIFY] Hot LPN %llu (Acc:%llu > Avg:%llu) triggering QLC->SLC migration\n", 
							                               local_lpn, access_cnt, avg_reads);

					migrate_page_to_slc(conv_ftl, local_lpn, &cur_ppa,
							    &migration_done);
					cur_ppa = get_maptbl_ent(conv_ftl, local_lpn);
					conv_ftl->migration_read_path_time_ns +=
						ktime_get_ns() - mig_start;
					NVMEV_DEBUG("[REPROMOTION_VERIFY] LPN %llu Promoted. Extra Latency: %llu ns\n", 
							                               local_lpn,ktime_get_ns() - mig_start);
					conv_ftl->migration_read_path_count++;
					if (migration_done)
						nsecs_latest = max(nsecs_latest, migration_done);
				}
			}

			// aggregate read io in same flash page
			if (mapped_ppa(&prev_ppa) &&
			    is_same_flash_page(conv_ftl, cur_ppa, prev_ppa)) {
				xfer_size += spp->pgsz;
				continue;
			}

			if (xfer_size > 0) {
				/* 根据页面位置调整读延迟 */
				uint64_t original_stime = srd.stime;
				
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
				
				srd.stime = original_stime;  /* 恢复原始时间 */
			}

			xfer_size = spp->pgsz;
			prev_ppa = cur_ppa;
		}

		// issue remaining io
		if (xfer_size > 0) {
			/* 根据页面位置调整读延迟 */
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

ret->nsecs_target = nsecs_latest;
	ret->status = NVME_SC_SUCCESS;
	    NVMEV_DEBUG("[READ_VERIFY] LBA Range: %llu + %d. Total Latency: %llu ns\n", 
			                   cmd->rw.slba, cmd->rw.length, nsecs_latest - nsecs_start);
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
	struct nand_page *pg;
	unsigned long flags;
	struct line_pool_stats slc_stats;

	if (!conv_ftl || !qlc_ppa)
		return;
	if (!mapped_ppa(qlc_ppa) || !valid_ppa(conv_ftl, qlc_ppa))
		return;

	if (conv_ftl->slc_repromote_guard_lines) {
		collect_slc_stats(conv_ftl, &slc_stats);
		if (slc_stats.free <= conv_ftl->slc_repromote_guard_lines) {
			NVMEV_DEBUG("migrate_page_to_slc: skip due to low SLC free lines (%u <= %u)\n",
				    slc_stats.free, conv_ftl->slc_repromote_guard_lines);
			return;
		}
	}

	/* Read QLC page */
	srd.type = USER_IO;
	srd.cmd = NAND_READ;
	srd.stime = __get_ioclock(conv_ftl->ssd);
	srd.interleave_pci_dma = false;
	srd.xfer_size = spp->pgsz;
	srd.ppa = qlc_ppa;

	nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srd);

	/* Allocate SLC page on same die */
	target_ch = qlc_ppa->g.ch;
	target_lun = qlc_ppa->g.lun;
	die_index = target_lun * spp->nchs + target_ch;
	if (conv_ftl->die_count)
		die_index %= conv_ftl->die_count;
	else
		die_index = 0;

	/* Initialize GC WP if needed */
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
		return;
	}

	/* Write SLC page */
	swr.type = USER_IO;
	swr.cmd = NAND_WRITE;
	swr.stime = nsecs_completed;
	swr.interleave_pci_dma = false;
	swr.xfer_size = spp->pgsz;
	swr.ppa = &new_ppa;

	nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &swr);

	/* Update stats for QLC resident */
	spin_lock_irqsave(&conv_ftl->qlc_zone_lock, flags);
	if (conv_ftl->qlc_resident_page_cnt > 0)
		conv_ftl->qlc_resident_page_cnt--;
	spin_unlock_irqrestore(&conv_ftl->qlc_zone_lock, flags);

	/* Update mappings */
	set_maptbl_ent(conv_ftl, lpn, &new_ppa);
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
	mark_page_valid(conv_ftl, &new_ppa);

	/* Advance GC WP */
	advance_gc_slc_write_pointer(conv_ftl, die_index);

	/* Update prev link */
	pg = get_pg(conv_ftl->ssd, qlc_ppa);
	if (pg)
		set_page_prev_link(conv_ftl, lpn, &new_ppa, pg->oob_prev_lpn);

	if (migration_done)
		*migration_done = nsecs_completed;

	NVMEV_DEBUG("Migrated LPN %llu from QLC to SLC (die=%u)\n", lpn, die_index);
}

/* 扫描 QLC 热数据并迁移回 SLC */
static void migrate_hot_from_qlc(struct conv_ftl *conv_ftl)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct heat_tracking *ht = &conv_ftl->heat_track;
	static uint64_t cursor = 0;
	uint32_t scanned = 0;
	uint32_t migrated = 0;
	const uint32_t MAX_SCAN = 1024;
	const uint32_t MAX_MIGRATE = 4;
	uint64_t avg_reads = 0;
	bool has_avg = false;

	if (!ht || !ht->access_count)
		return;

	has_avg = calc_global_avg_reads(conv_ftl, &avg_reads);
	if (!has_avg)
		return;

	uint64_t idx = cursor % spp->tt_pgs;
	uint64_t start = idx;

	while (scanned < MAX_SCAN && migrated < MAX_MIGRATE) {
		struct ppa ppa = get_maptbl_ent(conv_ftl, idx);

		if (mapped_ppa(&ppa) && !is_slc_block(conv_ftl, ppa.g.blk)) {
			uint64_t reads = ht->access_count[idx];

			if (reads > avg_reads) {
				migrate_page_to_slc(conv_ftl, idx, &ppa, NULL);
				migrated++;
			}
		}

		scanned++;
		idx = (idx + 1) % spp->tt_pgs;
		if (idx == start) break;
	}
	cursor = idx;
}

static bool conv_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	NVMEV_DEBUG("[DEBUG] conv_write: Function entry\n");
	struct conv_ftl *conv_ftls = (struct conv_ftl *)ns->ftls;
	struct conv_ftl *conv_ftl = &conv_ftls[0];

	/* wbuf and spp are shared by all instances */
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct buffer *wbuf = conv_ftl->ssd->write_buffer;

	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;

	uint64_t lpn;
	uint32_t nr_parts = ns->nr_parts;

	uint64_t nsecs_latest;
	uint64_t nsecs_xfer_completed;
	uint32_t allocated_buf_size;
	uint32_t xfer_size = 0;  /* 声明缺失的变量 */
//66f1
	uint16_t bOverwrite = (cmd->rw.control & NVME_RW_OVERWRITE) ? 1 : 0;
	uint16_t bAppend = (cmd->rw.control & NVME_RW_APPEND) ? 1 : 0;

	uint64_t plba = 0;
	uint64_t plpn = 0;
	uint64_t stripe_bytes = 0;
	uint64_t wbuf_needed = 0;
//66f1

	struct nand_cmd swr = {
		.type = USER_IO,
		.cmd = NAND_WRITE,
		.interleave_pci_dma = false,
		.xfer_size = spp->pgsz * spp->pgs_per_oneshotpg,
	};
//66f1
	if (bAppend)
	{
		plba = cmd->rw.pslba;
		plpn = plba / spp->secs_per_pg;
		//NVMEV_ERROR("[NVMEVIRT]_AP, plba = %llu\n", plba);
	}	
	if (bOverwrite)
	{
		//NVMEV_ERROR("[NVMEVIRT]_OW\n");
	}
//66f1

	NVMEV_DEBUG("[DEBUG] conv_write: start_lpn=%lld, len=%lld, end_lpn=%lld, nr_parts=%u, tt_pgs=%ld\n", 
	           start_lpn, nr_lba, end_lpn, nr_parts, spp->tt_pgs);
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
    
          
    {		            /* 写缓冲不足时返回 BUSY，交给上层重试 */
        uint64_t needed = LBA_TO_BYTE(nr_lba);

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

	allocated_buf_size = buffer_allocate(wbuf, needed);
	NVMEV_DEBUG("[DEBUG] conv_write: buffer alloc size = %u, needed = %llu\n",
		    allocated_buf_size, needed);
	if (allocated_buf_size < needed) {
		ret->status = NVME_SC_BUSY;
		ret->nsecs_target = req->nsecs_start;
		return true;
	}
   }
	nsecs_latest = ssd_advance_write_buffer(conv_ftl->ssd, req->nsecs_start, wbuf_needed);
	nsecs_xfer_completed = nsecs_latest;

	swr.stime = nsecs_latest;

	/* 移动所有变量声明到循环开头，符合 C90 标准 */
	uint64_t local_lpn;
	uint64_t nsecs_completed = 0;
    struct ppa ppa;
	uint64_t prev_link_lpn = INVALID_LPN;
    //struct ppa old_ppa = { .ppa = UNMAPPED_PPA };  /* 用于迁移检查 */
    
	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		        /* 调试：检查是否进入了写入循环 */
		if (lpn == start_lpn) {
			NVMEV_DEBUG("[DEBUG] conv_write: Starting write loop, lpn=%llu to %llu\n", start_lpn, end_lpn);
		}

		conv_ftl = &conv_ftls[lpn % nr_parts];
		local_lpn = lpn / nr_parts;
		prev_link_lpn = INVALID_LPN;
		if (local_lpn > 0) {
			struct ppa prev_tmp = get_maptbl_ent(conv_ftl, local_lpn - 1);
			if (mapped_ppa(&prev_tmp) && valid_ppa(conv_ftl, &prev_tmp))
				prev_link_lpn = local_lpn - 1;
		}
		ppa = get_maptbl_ent(conv_ftl, local_lpn); // Check whether the given LPN has been written before
		if (mapped_ppa(&ppa)) {
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
				ppa = get_maptbl_ent(p_conv_ftl,p_local_lpn); 
				if (mapped_ppa(&ppa)) {		
					uint32_t originlun = conv_ftl->lunpointer;
					conv_ftl->lunpointer = get_glun(conv_ftl, &ppa); 
					//advance lun for append
					conv_ftl->lunpointer++;
					if (conv_ftl->lunpointer == (conv_ftl->ssd->sp.nchs * conv_ftl->ssd->sp.luns_per_ch))
						conv_ftl->lunpointer = 0;
					//					
					//NVMEV_ERROR("target lun: %d -> %d\n", originlun, conv_ftl->lunpointer);
				}
			}
			else if (bOverwrite)
			{				
				ppa = get_maptbl_ent(conv_ftl,local_lpn); 
				if (mapped_ppa(&ppa)) {
					uint32_t originlun = conv_ftl->lunpointer;
					conv_ftl->lunpointer = get_glun(conv_ftl, &ppa); 
					//NVMEV_ERROR("target lun: %d -> %d\n", originlun, conv_ftl->lunpointer);
				}
			}
		}

        /* 修改：所有新写入都先写到 SLC（不直接写 QLC） */
        /* 每次写入都检查SLC状态并触发迁移 */
        struct line_pool_stats slc_stats;
        collect_slc_stats(conv_ftl, &slc_stats);
        uint32_t slc_free_lines = slc_stats.free;
        
        /* 检查SLC使用率是否超过高水位线，触发后台迁移 */
        uint32_t slc_used_lines = slc_stats.total - slc_free_lines;
        NVMEV_DEBUG("[DEBUG] SLC status: free_lines=%u, used_lines=%u, high_watermark=%u, total=%u\n", 
                   slc_free_lines, slc_used_lines, conv_ftl->slc_high_watermark, slc_stats.total);
        if (slc_used_lines >= conv_ftl->slc_high_watermark) {
			uint32_t target_lines = conv_ftl->slc_target_watermark;
			uint32_t over_lines;
			uint32_t max_pages;
			uint32_t cap_pages;

			if (!target_lines || target_lines >= slc_stats.total)
				target_lines = (slc_stats.total > 1) ? (slc_stats.total - 1) : slc_stats.total;

			over_lines = (slc_used_lines > target_lines) ? (slc_used_lines - target_lines) : 1;
			max_pages = over_lines * conv_ftl->slc_pgs_per_blk;
			cap_pages = conv_ftl->slc_pgs_per_blk ? (conv_ftl->slc_pgs_per_blk * 4) : 0;
			if (max_pages < 8)
				max_pages = 8;
			if (cap_pages && max_pages > cap_pages)
				max_pages = cap_pages;

            NVMEV_DEBUG("[DEBUG] SLC usage high (%u >= %u), migrating %u cold pages (target=%u, cap=%u)\n", 
                      slc_used_lines, conv_ftl->slc_high_watermark, max_pages, target_lines, cap_pages);
            migrate_some_cold_from_slc(conv_ftl, max_pages);
        }
        /* SLC free 低于阈值时触发前台 GC（无节流） */
        if (slc_free_lines <= conv_ftl->slc_gc_free_thres_high) {
            NVMEV_DEBUG("FGGC: SLC free=%u <= thres_high=%u, running SLC GC now\n",
                       slc_free_lines, conv_ftl->slc_gc_free_thres_high);
            forground_gc(conv_ftl);
        }	   
	        /* 尝试获取SLC页面 */
	        ppa = get_new_slc_page(conv_ftl);
        if (!mapped_ppa(&ppa)) {
            /* SLC空间不足，立即返回写入失败 */
            NVMEV_ERROR("SLC exhausted, write failed for LPN %lld - background migration needed\n", local_lpn);
            ret->status = NVME_SC_WRITE_FAULT;
            ret->nsecs_target = nsecs_latest;
            return true;
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
		set_maptbl_ent(conv_ftl, local_lpn, &ppa);
		NVMEV_DEBUG("conv_write: got new ppa %lld, ", ppa2pgidx(conv_ftl, &ppa));
		/* update rmap */
		set_rmap_ent(conv_ftl, local_lpn, &ppa);

			mark_page_valid(conv_ftl, &ppa);
			set_page_prev_link(conv_ftl, local_lpn, &ppa, prev_link_lpn);

			/* need to advance the write pointer here */
		//need branch
//66f1
        /* 使用 SLC 的 Die Affinity 推进写指针 */
        advance_slc_write_pointer(conv_ftl, encode_die(spp, &ppa));

	//	nsecs_completed = ssd_advance_write_buffer(conv_ftl->ssd, nsecs_latest, conv_ftl->ssd->sp.pgsz);

        /* Check whether we need to do a write in this stripe
         * Use current page offset within oneshot page (flash page)
         */
        stripe_bytes += spp->pgsz;
        {
            uint32_t pg_off = ppa.g.pg % spp->pgs_per_oneshotpg;
            if (pg_off == (spp->pgs_per_oneshotpg - 1) || lpn == end_lpn) {
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
        }

		nsecs_latest = max(nsecs_completed, nsecs_latest);

		/* 更新热数据信息 */
		update_heat_info(conv_ftl, local_lpn, false);
		conv_ftl->heat_track.last_access_time[local_lpn] = __get_ioclock(conv_ftl->ssd);
		
		
		/* 检查是否需要触发后台迁移 */
		struct line_pool_stats slc_stats_tail;
		collect_slc_stats(conv_ftl, &slc_stats_tail);
		if (slc_stats_tail.free < 2) {
			/* SLC 空间不足，需要迁移一些冷数据到 QLC */
			NVMEV_DEBUG("SLC space low, triggering migration\n");
			/* 这里可以实现更复杂的后台迁移策略 */
		}
		
		consume_write_credit(conv_ftl);
		check_and_refill_write_credit(conv_ftl);
	}
/* 方案B：保险释放 - 确保所有分配的buffer都被释放 */
	if (stripe_bytes > 0) {
		uint64_t flush_time = nsecs_latest;
		enqueue_writeback_io_req(req->sq_id, flush_time, wbuf,
			       (unsigned int)stripe_bytes);
		stripe_bytes = 0;
	}

	if ((cmd->rw.control & NVME_RW_FUA) || (conv_ftl->ssd->sp.write_early_completion == 0)) {
		    /* Wait all flash operations */
		    ret->nsecs_target = nsecs_latest;
	} else {
		    /* Early completion */
		    ret->nsecs_target = nsecs_xfer_completed;
	}
	
	ret->status = NVME_SC_SUCCESS;
	
	/* 打印统计信息 */
	if ((conv_ftl->slc_write_cnt + conv_ftl->qlc_write_cnt) % 10000 == 0) {
		NVMEV_INFO("Write Stats: SLC writes=%llu, QLC writes=%llu, Migrations=%llu\n",
			   conv_ftl->slc_write_cnt, conv_ftl->qlc_write_cnt, conv_ftl->migration_cnt);
	}
	
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
	NVMEV_ASSERT(ns->csi == NVME_CSI_NVM);

	NVMEV_DEBUG("[DEBUG] conv_proc_nvme_io_cmd: Processing opcode %d (%s)\n", 
	           cmd->common.opcode, nvme_opcode_string(cmd->common.opcode));
	
	switch (cmd->common.opcode) {
	case nvme_cmd_write:
		NVMEV_DEBUG("[DEBUG] conv_proc_nvme_io_cmd: Calling conv_write\n");
        if (!conv_write(ns, req, ret))
            return true; /* 出错也返回完成，状态在 ret 内 */
		break;
	case nvme_cmd_read:
		NVMEV_DEBUG("[DEBUG] conv_proc_nvme_io_cmd: Calling conv_read\n");
        if (!conv_read(ns, req, ret))
            return true; /* 出错也返回完成，状态在 ret 内 */
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

	return true;
}

 /* background threads removed */
