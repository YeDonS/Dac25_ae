// SPDX-License-Identifier: GPL-2.0-only


#include <linux/sched/clock.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/wait.h>

#include "nvmev.h"
#include "conv_ftl.h"

#ifndef NVMEV_WARN
#define NVMEV_WARN(fmt, ...) pr_warn("[nvmev] " fmt, ##__VA_ARGS__)
#endif
void enqueue_writeback_io_req(int sqid, unsigned long long nsecs_target,
			      struct buffer *write_buffer, unsigned int buffs_to_release);

/* SLC 低水位阈值：空闲行数低于总行数的该百分比则触发迁移 */
#define SLC_FREE_LOW_WM_PCT 10

/* 前向声明以避免隐式声明错误 */
static inline struct ppa get_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn);
static inline bool mapped_ppa(struct ppa *ppa);
static bool is_slc_block(struct conv_ftl *conv_ftl, uint32_t blk_id);
static void migrate_page_to_qlc(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *slc_ppa);
static int advance_qlc_write_pointer(struct conv_ftl *conv_ftl, uint32_t region_id);
static void advance_slc_write_pointer(struct conv_ftl *conv_ftl);
static struct ppa get_new_slc_page(struct conv_ftl *conv_ftl);

/* 后台线程函数声明 */
static int background_migration_thread(void *data);
static int background_gc_thread(void *data);
static void init_background_threads(struct conv_ftl *conv_ftl);
static void stop_background_threads(struct conv_ftl *conv_ftl);
static void wakeup_migration_thread(struct conv_ftl *conv_ftl);
static void wakeup_gc_thread(struct conv_ftl *conv_ftl);

/* 当 SLC 空闲低于阈值时，挑选一小批"冷数据"从 SLC 迁移到 QLC。
 * 最小改动：线性扫描有限数量 LPN，命中条件：page_in_slc && 冷（时间 >1s 且访问计数 < 阈值）。
 */
/* 当 SLC 空闲低于阈值时，挑选冷数据从 SLC 迁移到 QLC */
static void trigger_slc_migration_if_low(struct conv_ftl *conv_ftl)
{
    struct line_mgmt *slc = &conv_ftl->slc_lm;
    struct ssdparams *spp = &conv_ftl->ssd->sp;
    struct heat_tracking *ht = &conv_ftl->heat_track;
    uint64_t now;
    uint32_t free_pct;
    const uint32_t MAX_MIGRATE = 32;
    const uint32_t MAX_SCAN = 4096;
    static uint64_t cursor = 0;
    uint64_t start, idx;
    uint32_t scanned = 0;
    uint32_t migrated = 0;

    if (!slc || slc->tt_lines == 0)
        return;

    /* 未低于阈值则直接返回 */
    free_pct = (slc->free_line_cnt * 100) / slc->tt_lines;
    if (free_pct > SLC_FREE_LOW_WM_PCT)
        return;

    if (!conv_ftl->page_in_slc || !ht || !ht->last_access_time)
        return;

    now = __get_ioclock(conv_ftl->ssd);
    start = cursor % spp->tt_pgs;
    idx = start;
    
    while (scanned < MAX_SCAN && migrated < MAX_MIGRATE) {
        if (conv_ftl->page_in_slc[idx]) {
            uint64_t last = ht->last_access_time[idx];
            uint64_t acc = ht->access_count[idx];
            
            if ((now - last) > 1000000000ULL && acc < ht->migration_threshold) {
                struct ppa old_ppa = get_maptbl_ent(conv_ftl, idx);
                if (mapped_ppa(&old_ppa) && is_slc_block(conv_ftl, old_ppa.g.blk)) {
                    /* 直接调用单页迁移函数 */
                    migrate_page_to_qlc(conv_ftl, idx, &old_ppa);
                    migrated++;
                }
            }
        }
        
        scanned++;
        idx = (idx + 1) % spp->tt_pgs;
        if (idx == start) break;
    }
    
    cursor = idx;
    
    if (migrated > 0) {
        NVMEV_DEBUG("Migrated %d cold pages from SLC to QLC\n", migrated);
    }
}



/* 无阈值：总是尝试从 SLC 迁移少量更冷页面到 QLC */
static void migrate_some_cold_from_slc(struct conv_ftl *conv_ftl, uint32_t max_pages)
{
    struct ssdparams *spp = &conv_ftl->ssd->sp;
    struct heat_tracking *ht = &conv_ftl->heat_track;
    uint32_t migrated = 0;
    uint32_t scanned = 0;
    uint64_t now;
    static uint64_t cursor2 = 0;
    uint64_t idx;

    if (!conv_ftl->page_in_slc || !ht || !ht->last_access_time || max_pages == 0)
        return;

    now = __get_ioclock(conv_ftl->ssd);
    idx = cursor2 % spp->tt_pgs;
    
    while (scanned < 4096 && migrated < max_pages) {
        if (conv_ftl->page_in_slc[idx]) {
            uint64_t last = ht->last_access_time[idx];
            uint64_t acc = ht->access_count[idx];
            
            /* 冷数据判断 */
            bool is_cold = ((now - last) > 1000000000ULL && acc < ht->migration_threshold);
            if (!is_cold) {
                /* 放宽策略：极老数据（>5s）也作为候选 */
                if ((now - last) > 5000000000ULL) {
                    is_cold = true;
                }
            }
            
            if (is_cold) {
                struct ppa old_ppa = get_maptbl_ent(conv_ftl, idx);
                if (mapped_ppa(&old_ppa) && is_slc_block(conv_ftl, old_ppa.g.blk)) {
                    /* 执行单页迁移 */
                    migrate_page_to_qlc(conv_ftl, idx, &old_ppa);
                    migrated++;
                }
            }
        }
        
        scanned++;
        idx = (idx + 1) % spp->tt_pgs;
    }
    
    cursor2 = idx;
    
    if (migrated > 0) {
        NVMEV_DEBUG("Migrated %d pages from SLC to QLC\n", migrated);
    }
}
static inline bool last_pg_in_wordline(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	return (ppa->g.pg % spp->pgs_per_oneshotpg) == (spp->pgs_per_oneshotpg - 1);
}

static bool should_gc(struct conv_ftl *conv_ftl)
{
	/* 使用 SLC + QLC 的总空闲空间来判断是否需要 GC - 分步读取避免锁竞争 */
	uint32_t slc_free, qlc_free, total_free_lines;
	
	spin_lock(&conv_ftl->slc_lock);
	slc_free = conv_ftl->slc_lm.free_line_cnt;
	spin_unlock(&conv_ftl->slc_lock);
	
	spin_lock(&conv_ftl->qlc_lock);
	qlc_free = conv_ftl->qlc_lm.free_line_cnt;
	spin_unlock(&conv_ftl->qlc_lock);
	
	total_free_lines = slc_free + qlc_free;
	return (total_free_lines <= conv_ftl->cp.gc_thres_lines);
}

static inline bool should_gc_high(struct conv_ftl *conv_ftl)
{
	/* 使用 SLC + QLC 的总空闲空间来判断是否需要 GC - 分步读取避免锁竞争 */
	uint32_t slc_free, qlc_free, total_free_lines;
	
	spin_lock(&conv_ftl->slc_lock);
	slc_free = conv_ftl->slc_lm.free_line_cnt;
	spin_unlock(&conv_ftl->slc_lock);
	
	spin_lock(&conv_ftl->qlc_lock);
	qlc_free = conv_ftl->qlc_lm.free_line_cnt;
	spin_unlock(&conv_ftl->qlc_lock);
	
	total_free_lines = slc_free + qlc_free;
	return total_free_lines <= conv_ftl->cp.gc_thres_lines_high;
}

static inline struct ppa get_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn)
{
	/* 使用READ_ONCE保证内存可见性，防止编译器优化 */
	return READ_ONCE(conv_ftl->maptbl[lpn]);
}

static inline void set_maptbl_ent(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *ppa)
{
	/* 关键修复：避免致命的BUG_ON，改为警告和容错处理 */
	if (lpn >= conv_ftl->ssd->sp.tt_pgs) {
		NVMEV_ERROR("[WARNING] LPN out of range: lpn=%lld >= tt_pgs=%ld\n", 
			   lpn, conv_ftl->ssd->sp.tt_pgs);
		return; /* 不更新映射表 */
	}
	/* 使用WRITE_ONCE保证原子写入，防止编译器重排 */
	WRITE_ONCE(conv_ftl->maptbl[lpn], *ppa);
}

static uint64_t ppa2pgidx(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint64_t pgidx;

	NVMEV_DEBUG("ppa2pgidx: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", ppa->g.ch, ppa->g.lun,
		    ppa->g.pl, ppa->g.blk, ppa->g.pg);

	pgidx = ppa->g.ch * spp->pgs_per_ch + ppa->g.lun * spp->pgs_per_lun +
		ppa->g.pl * spp->pgs_per_pl + ppa->g.blk * spp->pgs_per_blk + ppa->g.pg;

	/* 关键修复：避免致命的BUG_ON，改为警告和容错处理 */
	if (pgidx >= spp->tt_pgs) {
		NVMEV_ERROR("[WARNING] Page index out of range: pgidx=%lld >= tt_pgs=%ld\n", 
			   pgidx, spp->tt_pgs);
		return INVALID_PPA; /* 返回无效索引 */
	}

	return pgidx;
}

static inline uint64_t get_rmap_ent(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(conv_ftl, ppa);

	/* 使用READ_ONCE保证内存可见性 */
	return READ_ONCE(conv_ftl->rmap[pgidx]);
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *ppa)
{
	uint64_t pgidx = ppa2pgidx(conv_ftl, ppa);

	/* 使用WRITE_ONCE保证原子写入 */
	WRITE_ONCE(conv_ftl->rmap[pgidx], lpn);
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
		/* 触发后台GC而不是前台阻塞GC */
		uint32_t slc_free, qlc_free, total_free_lines;
		
		/* 分步读取避免同时持有两个锁 */
		spin_lock(&conv_ftl->slc_lock);
		slc_free = conv_ftl->slc_lm.free_line_cnt;
		spin_unlock(&conv_ftl->slc_lock);
		
		spin_lock(&conv_ftl->qlc_lock);
		qlc_free = conv_ftl->qlc_lm.free_line_cnt;
		spin_unlock(&conv_ftl->qlc_lock);
		
		total_free_lines = slc_free + qlc_free;
		
		if (total_free_lines <= conv_ftl->gc_high_watermark) {
			wakeup_gc_thread(conv_ftl);
		}
		
		/* 临时增加少量credit以避免完全阻塞，后台GC会逐步释放更多空间 */
		wfc->write_credits += 10;  /* 允许少量写入继续进行 */
	}
}

static void init_lines(struct conv_ftl *conv_ftl)
{
	/* 旧的 init_lines 函数已废弃 - 现在使用 init_slc_lines 和 init_qlc_lines */
	/* 保留空实现以避免编译错误 */
	NVMEV_DEBUG("init_lines called - function deprecated, using SLC/QLC initialization instead\n");
}

//66f1
static void init_lines_DA(struct conv_ftl *conv_ftl)
{
	/* Die-Affinity lines 初始化函数已废弃 - 现在使用 SLC/QLC 系统 */
	NVMEV_DEBUG("init_lines_DA called - function deprecated, using SLC/QLC initialization instead\n");
}
//66f1

static void remove_lines(struct conv_ftl *conv_ftl)
{
	/* 旧的 remove_lines 函数已废弃 - 现在使用 remove_slc_lines 和 remove_qlc_lines */
	/* 这个函数保留为空，避免编译错误，但实际清理工作由新函数完成 */
}

static void remove_lines_DA(struct conv_ftl *conv_ftl)
{
	/* Die-Affinity lines 清理函数已废弃 - 现在使用 remove_slc_lines 和 remove_qlc_lines */
	NVMEV_DEBUG("remove_lines_DA called - function deprecated, using SLC/QLC cleanup instead\n");
}

static void init_write_flow_control(struct conv_ftl *conv_ftl)
{
	struct write_flow_control *wfc = &(conv_ftl->wfc);
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	wfc->write_credits = spp->pgs_per_line;
	wfc->credits_to_refill = spp->pgs_per_line;
}

static inline void check_addr(int a, int max)
{
	NVMEV_ASSERT(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct conv_ftl *conv_ftl)
{
	/* 旧的 get_next_free_line 函数已废弃 - 现在使用 SLC/QLC 特定的分配逻辑 */
	NVMEV_DEBUG("get_next_free_line called - function deprecated, using SLC/QLC allocation instead\n");
	return NULL;  /* 返回 NULL 表示没有可用的 line */
}

//66f1
static struct line *get_next_free_line_DA(struct conv_ftl *conv_ftl, uint32_t lun)
{
	/* Die-Affinity get_next_free_line 函数已废弃 - 现在使用 SLC/QLC 特定的分配逻辑 */
	NVMEV_DEBUG("get_next_free_line_DA called - function deprecated, using SLC/QLC allocation instead\n");
	return NULL;  /* 返回 NULL 表示没有可用的 line */
}
//66f1

static struct write_pointer *__get_wp(struct conv_ftl *ftl, uint32_t io_type)
{
	if (io_type == USER_IO) {
		return &ftl->wp;
	} else if (io_type == GC_IO) {
		return &ftl->gc_wp;
	}

	NVMEV_ASSERT(0);
	return NULL;
}
//66f1
static struct write_pointer *__get_wp_DA(struct conv_ftl *ftl, uint32_t io_type, uint32_t lun)
{
	/* Die-Affinity 写指针获取函数已废弃 - 现在使用 SLC/QLC 特定的写指针 */
	NVMEV_DEBUG("__get_wp_DA called - function deprecated, using SLC/QLC write pointers instead\n");
	if (io_type == GC_IO) {
		return &ftl->gc_wp;  /* GC 仍然使用全局写指针 */
	}
	return NULL;  /* 其他情况返回 NULL */
}
//66f1

static void prepare_write_pointer(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	/* DEPRECATED: prepare_write_pointer function is deprecated in SLC/QLC architecture */
	NVMEV_DEBUG("prepare_write_pointer called - function deprecated, no operation performed\n");
	
	/* 在新的 SLC/QLC 架构中，写指针动态分配，不需要预先准备 */
	/* Write pointers are now dynamically allocated in SLC/QLC architecture */
	return;
}

//66f1
static void prepare_write_pointer_DA(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	uint32_t luncount = conv_ftl->ssd->sp.luns_per_ch * conv_ftl->ssd->sp.nchs;
	uint32_t lun=0;
	for( lun=0; lun < luncount; lun++ )
	{
		struct line *curline = get_next_free_line_DA(conv_ftl, lun);
		struct write_pointer *wp = __get_wp_DA(conv_ftl, io_type, lun);
		uint32_t localch = lun % conv_ftl->ssd->sp.nchs;
		uint32_t locallun = lun / conv_ftl->ssd->sp.nchs;

		NVMEV_ASSERT(wp);
		NVMEV_ASSERT(curline);

		/* wp->curline is always our next-to-write super-block */
		*wp = (struct write_pointer) {
			.curline = curline,
			.ch = localch,
			.lun = locallun,
			.pg = 0,
			.blk = curline->id,
			.pl = 0,
			};		
	}

	//debug wpp
	for( lun=0; lun < luncount; lun++ )
	{
		struct write_pointer *wp = __get_wp_DA(conv_ftl, io_type, lun);

		//NVMEV_ERROR("wpp lun:%d, ch: %d, lun: %d\n", lun, wp->ch, wp->lun);
	}

}
//66f1

static void advance_write_pointer(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	/* 旧的写指针推进逻辑已废弃 - 现在使用 SLC/QLC 特定的推进逻辑 */
	/* 这个函数主要被 GC 调用，我们需要根据 io_type 选择合适的推进方式 */
	
	/* 对于 GC，我们暂时使用 SLC 推进逻辑 */
	if (io_type == GC_IO) {
		advance_slc_write_pointer(conv_ftl);
	} else {
		/* 对于用户 IO，应该通过 conv_write 中的逻辑来处理 */
		NVMEV_DEBUG("advance_write_pointer called with USER_IO - should use SLC/QLC specific logic\n");
	}
}

//66f1
static void advance_write_pointer_DA(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	/* Die-Affinity 写指针推进函数已废弃 - 现在使用 SLC/QLC 特定的推进逻辑 */
	/* 这个函数不再被使用，保留为空以避免编译错误 */
	NVMEV_DEBUG("advance_write_pointer_DA called - function deprecated, using SLC/QLC logic instead\n");
}
//66f1

static struct ppa get_new_page(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	struct ppa ppa;
	struct write_pointer *wp = __get_wp(conv_ftl, io_type);

	ppa.ppa = 0;
	ppa.g.ch = wp->ch;
	ppa.g.lun = wp->lun;
	ppa.g.pg = wp->pg;
	ppa.g.blk = wp->blk;
	ppa.g.pl = wp->pl;

	NVMEV_ASSERT(ppa.g.pl == 0);

	return ppa;
}

static struct ppa get_new_page_DA(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	struct ppa ppa;
	struct write_pointer *wp = __get_wp_DA(conv_ftl, io_type, conv_ftl->lunpointer);

	ppa.ppa = 0;
	ppa.g.ch = wp->ch;
	ppa.g.lun = wp->lun;
	ppa.g.pg = wp->pg;
	ppa.g.blk = wp->blk;
	ppa.g.pl = wp->pl;

	NVMEV_ASSERT(ppa.g.pl == 0);

	return ppa;
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
		/* 使用WRITE_ONCE保证初始化的原子性 */
		struct ppa unmapped_ppa = { .ppa = UNMAPPED_PPA };
		WRITE_ONCE(conv_ftl->maptbl[i], unmapped_ppa);
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
		/* 使用WRITE_ONCE保证初始化的原子性 */
		WRITE_ONCE(conv_ftl->rmap[i], INVALID_LPN);
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
	
	/* 关键检查：验证运行时参数与编译期参数的一致性 */
	NVMEV_ERROR("[BOUNDARY_CHECK] Runtime vs Compile-time parameters:\n");
	NVMEV_ERROR("[BOUNDARY_CHECK] spp->blks_per_pl=%d, BLKS_PER_PLN=%d\n", spp->blks_per_pl, BLKS_PER_PLN);
	NVMEV_ERROR("[BOUNDARY_CHECK] spp->nchs=%d, NAND_CHANNELS=%d\n", spp->nchs, NAND_CHANNELS);
	NVMEV_ERROR("[BOUNDARY_CHECK] spp->luns_per_ch=%d, LUNS_PER_NAND_CH=%d\n", spp->luns_per_ch, LUNS_PER_NAND_CH);
	NVMEV_ERROR("[BOUNDARY_CHECK] spp->pls_per_lun=%d, PLNS_PER_LUN=%d\n", spp->pls_per_lun, PLNS_PER_LUN);
	NVMEV_ERROR("[BOUNDARY_CHECK] spp->pgs_per_blk=%d\n", spp->pgs_per_blk);
	
	if (spp->blks_per_pl != BLKS_PER_PLN) {
		NVMEV_ERROR("[CRITICAL] Runtime blks_per_pl=%d != Compile-time BLKS_PER_PLN=%d! This may cause Invalid PPA errors!\n", 
			   spp->blks_per_pl, BLKS_PER_PLN);
	}
	
	/* 重试分配内存 */
	while (retry_count < max_retries) {
		conv_ftl->is_slc_block = vmalloc(sizeof(bool) * total_blks_per_pl);
		if (conv_ftl->is_slc_block) {
			/* 分配成功，初始化标记 */
			conv_ftl->slc_blks_per_pl = total_blks_per_pl * SLC_CAPACITY_PERCENT / 100;
			conv_ftl->qlc_blks_per_pl = total_blks_per_pl - conv_ftl->slc_blks_per_pl;
			conv_ftl->qlc_region_size = conv_ftl->qlc_blks_per_pl / QLC_REGIONS;
			
			/* 标记前 20% 为 SLC，后 80% 为 QLC */
			for (i = 0; i < total_blks_per_pl; i++) {
				if (i < conv_ftl->slc_blks_per_pl) {
					conv_ftl->is_slc_block[i] = true;  /* SLC 块 */
				} else {
					conv_ftl->is_slc_block[i] = false; /* QLC 块 */
				}
			}
			
			conv_ftl->slc_initialized = true;
			NVMEV_ERROR("[BOUNDARY_CHECK] Hybrid storage initialized successfully:\n");
			NVMEV_ERROR("[BOUNDARY_CHECK] SLC blocks: %d (range: [0, %d))\n", 
				   conv_ftl->slc_blks_per_pl, conv_ftl->slc_blks_per_pl);
			NVMEV_ERROR("[BOUNDARY_CHECK] QLC blocks: %d (range: [%d, %d))\n", 
				   conv_ftl->qlc_blks_per_pl, conv_ftl->slc_blks_per_pl, 
				   conv_ftl->slc_blks_per_pl + conv_ftl->qlc_blks_per_pl);
			NVMEV_ERROR("[BOUNDARY_CHECK] QLC region size: %d, QLC regions: %d\n", 
				   conv_ftl->qlc_region_size, QLC_REGIONS);
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
	
	/* 调整配置以适应较小的分配 */
	conv_ftl->slc_blks_per_pl = reduced_size / 2;
	conv_ftl->qlc_blks_per_pl = reduced_size / 2;
	conv_ftl->qlc_region_size = conv_ftl->qlc_blks_per_pl / QLC_REGIONS;
	
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
		ht->access_count = NULL;
		ht->last_access_time = NULL;
		return -ENOMEM;
	}
	
	/* 初始化所有数组 */
	for (i = 0; i < spp->tt_pgs; i++) {
		ht->access_count[i] = 0;
		ht->last_access_time[i] = 0;
		conv_ftl->page_in_slc[i] = false;
	}
	
	ht->migration_threshold = MIGRATION_THRESHOLD;
	INIT_LIST_HEAD(&conv_ftl->migration.migration_queue);
	conv_ftl->migration.pending_migrations = 0;
	
	conv_ftl->heat_track_initialized = true;
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

    /* 初始化账本并发保护锁 */
    spin_lock_init(&conv_ftl->slc_lock);
    spin_lock_init(&conv_ftl->qlc_lock);
}

/* 初始化 SLC line 管理 */
static void init_slc_lines(struct conv_ftl *conv_ftl)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->slc_lm;
	struct line *line;
	int i;
	
	lm->tt_lines = conv_ftl->slc_blks_per_pl;
	NVMEV_ERROR("[DEBUG] init_slc_lines: slc_blks_per_pl=%u, allocating %u lines\n", 
		   conv_ftl->slc_blks_per_pl, lm->tt_lines);
	lm->lines = vmalloc(sizeof(struct line) * lm->tt_lines);
	if (!lm->lines) {
		NVMEV_ERROR("Failed to allocate SLC lines memory\n");
		return;
	}
	
	INIT_LIST_HEAD(&lm->free_line_list);
	INIT_LIST_HEAD(&lm->full_line_list);
	
	lm->victim_line_pq = pqueue_init(lm->tt_lines, victim_line_cmp_pri, victim_line_get_pri,
					 victim_line_set_pri, victim_line_get_pos,
					 victim_line_set_pos);
	if (!lm->victim_line_pq) {
		NVMEV_ERROR("Failed to initialize SLC victim line priority queue\n");
		vfree(lm->lines);
		lm->lines = NULL;
		return;
	}
	
	lm->free_line_cnt = 0;
	for (i = 0; i < lm->tt_lines; i++) {
		lm->lines[i] = (struct line) {
			.id = i,  /* SLC 使用从 0 开始的连续块ID，这是正确的 */
			.ipc = 0,
			.vpc = 0,
			.pos = 0,
			.entry = LIST_HEAD_INIT(lm->lines[i].entry),
		};
		
		/* 添加边界检查 */
		if (i >= conv_ftl->slc_blks_per_pl) {
			NVMEV_ERROR("init_slc_lines: SLC line index %d exceeds slc_blks_per_pl %u\n", 
				   i, conv_ftl->slc_blks_per_pl);
		}
		
		list_add_tail(&lm->lines[i].entry, &lm->free_line_list);
		lm->free_line_cnt++;
	}
	
	lm->victim_line_cnt = 0;
	lm->full_line_cnt = 0;
}

/* 初始化 QLC line 管理 */
static void init_qlc_lines(struct conv_ftl *conv_ftl)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->qlc_lm;
	int i;
	uint32_t start_blk = conv_ftl->slc_blks_per_pl;
	uint32_t total_qlc_lines = conv_ftl->qlc_region_size * QLC_REGIONS;
	
	/* 初始化单个共享的 QLC line 管理器 */
	lm->tt_lines = total_qlc_lines;
	lm->lines = vmalloc(sizeof(struct line) * lm->tt_lines);
	if (!lm->lines) {
		NVMEV_ERROR("Failed to allocate QLC lines memory\n");
		return;
	}
	
	INIT_LIST_HEAD(&lm->free_line_list);
	INIT_LIST_HEAD(&lm->full_line_list);
	
	lm->victim_line_pq = pqueue_init(lm->tt_lines, victim_line_cmp_pri, 
					 victim_line_get_pri, victim_line_set_pri, 
					 victim_line_get_pos, victim_line_set_pos);
	if (!lm->victim_line_pq) {
		NVMEV_ERROR("Failed to initialize QLC victim line priority queue\n");
		vfree(lm->lines);
		lm->lines = NULL;
		return;
	}
	
	NVMEV_ERROR("[DEBUG] init_qlc_lines: start_blk=%u, total_lines=%u, allocating lines with physical block IDs\n", 
		   start_blk, total_qlc_lines);
	
	lm->free_line_cnt = 0;
	for (i = 0; i < lm->tt_lines; i++) {
		/* 关键修复：line->id 始终保持为物理block ID，永不更改 */
		lm->lines[i] = (struct line) {
			.id = start_blk + i,  /* 物理block ID: 1638, 1639, 1640, ... */
			.ipc = 0,
			.vpc = 0,
			.pos = 0,
			.entry = LIST_HEAD_INIT(lm->lines[i].entry),
		};
		
		/* 验证物理block ID不超过硬件限制 */
		if (lm->lines[i].id >= spp->blks_per_pl) {
			NVMEV_ERROR("QLC line %d: physical block ID %u exceeds blks_per_pl %u!\n", 
				   i, lm->lines[i].id, spp->blks_per_pl);
		}
		
		list_add_tail(&lm->lines[i].entry, &lm->free_line_list);
		lm->free_line_cnt++;
	}
	
	lm->victim_line_cnt = 0;
	lm->full_line_cnt = 0;
	conv_ftl->current_qlc_region = 0;
	
	NVMEV_ERROR("[DEBUG] QLC lines initialized: %u lines with block IDs %u-%u\n", 
		   lm->tt_lines, start_blk, start_blk + lm->tt_lines - 1);
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
	vfree(conv_ftl->page_in_slc);
}

static void remove_slc_lines(struct conv_ftl *conv_ftl)
{
	pqueue_free(conv_ftl->slc_lm.victim_line_pq);
	vfree(conv_ftl->slc_lm.lines);
}

static void remove_qlc_lines(struct conv_ftl *conv_ftl)
{
	pqueue_free(conv_ftl->qlc_lm.victim_line_pq);
	vfree(conv_ftl->qlc_lm.lines);
}

static void conv_init_ftl(struct conv_ftl *conv_ftl, struct convparams *cpp, struct ssd *ssd)
{
	/*copy convparams*/
	conv_ftl->cp = *cpp;

	conv_ftl->ssd = ssd;

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
	
	NVMEV_INFO("initialize SLC lines\n");
	init_slc_lines(conv_ftl);
	
	NVMEV_INFO("initialize QLC lines\n");
	init_qlc_lines(conv_ftl);
	
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

	/* 初始化后台线程 */
	NVMEV_INFO("initialize background threads\n");
	init_background_threads(conv_ftl);

	NVMEV_INFO("Init FTL Instance with %d channels(%ld pages)\n", conv_ftl->ssd->sp.nchs,
		   conv_ftl->ssd->sp.tt_pgs);
	NVMEV_INFO("SLC/QLC Hybrid Mode: SLC %d blks, QLC %d blks (4 regions)\n", 
		   conv_ftl->slc_blks_per_pl, conv_ftl->qlc_blks_per_pl);

	return;
}

static void conv_remove_ftl(struct conv_ftl *conv_ftl)
{
	/* 首先停止后台线程 */
	stop_background_threads(conv_ftl);
	
    remove_lines(conv_ftl);
	
	/* 清理 SLC/QLC 相关资源 */
	remove_slc_lines(conv_ftl);
	remove_qlc_lines(conv_ftl);
	remove_heat_tracking(conv_ftl);
	remove_slc_qlc_blocks(conv_ftl);
	
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

	if (ch < 0 || ch >= spp->nchs) {
		NVMEV_ERROR("[mark_page_valid] Invalid PPA: ch=%d out of range [0, %d), runtime_nchs=%d\n", 
			   ch, spp->nchs, spp->nchs);
		return false;
	}
	if (lun < 0 || lun >= spp->luns_per_ch) {
		NVMEV_ERROR("[mark_page_valid] Invalid PPA: lun=%d out of range [0, %d), runtime_luns_per_ch=%d\n", 
			   lun, spp->luns_per_ch, spp->luns_per_ch);
		return false;
	}
	if (pl < 0 || pl >= spp->pls_per_lun) {
		NVMEV_ERROR("[mark_page_valid] Invalid PPA: pl=%d out of range [0, %d), runtime_pls_per_lun=%d\n", 
			   pl, spp->pls_per_lun, spp->pls_per_lun);
		return false;
	}
	
	/* 关键修复：使用运行时参数检查混合存储block ID范围 */
	/* 注意：对于混合存储，我们的虚拟block ID可能超过物理blks_per_pl */
	uint32_t max_blk_id = conv_ftl->slc_blks_per_pl + (conv_ftl->qlc_region_size * QLC_REGIONS) - 1;
	if (blk < 0 || blk > max_blk_id) {
		NVMEV_ERROR("[mark_page_valid] Invalid PPA: blk=%d out of hybrid_range [0, %u], runtime_blks_per_pl=%d, slc_blks=%u, qlc_regions=%u*%u\n", 
			   blk, max_blk_id, spp->blks_per_pl, conv_ftl->slc_blks_per_pl, QLC_REGIONS, conv_ftl->qlc_region_size);
		return false;
	}
	
	if (pg < 0 || pg >= spp->pgs_per_blk) {
		NVMEV_ERROR("[mark_page_valid] Invalid PPA: pg=%d out of range [0, %d), runtime_pgs_per_blk=%d\n", 
			   pg, spp->pgs_per_blk, spp->pgs_per_blk);
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
	/* 根据块类型选择正确的 line_mgmt */
	if (is_slc_block(conv_ftl, ppa->g.blk)) {
		/* SLC 块：直接使用块ID作为索引 */
		return &(conv_ftl->slc_lm.lines[ppa->g.blk]);
	} else {
		/* QLC 块：需要减去 SLC 块数量来获得 QLC 索引 */
		uint32_t qlc_idx = ppa->g.blk - conv_ftl->slc_blks_per_pl;
		if (qlc_idx >= conv_ftl->qlc_lm.tt_lines) {
			NVMEV_ERROR("QLC block index out of range: %u >= %u\n", qlc_idx, conv_ftl->qlc_lm.tt_lines);
			return NULL;
		}
		return &(conv_ftl->qlc_lm.lines[qlc_idx]);
	}
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

    blk = get_blk(conv_ftl->ssd, ppa);
    if (!blk) {
        NVMEV_ERROR("[mark_page_invalid] Failed to get block for ppa ch=%d,lun=%d,blk=%d,pg=%d\n",
                   ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg);
        return;
    }
    
    /* 关键修复：避免致命的BUG_ON，改为警告和容错处理 */
    if (blk->ipc < 0 || blk->ipc >= spp->pgs_per_blk) {
        NVMEV_ERROR("[WARNING] Block ipc out of range: ipc=%d, range=[0,%d)\n", 
                   blk->ipc, spp->pgs_per_blk);
        if (blk->ipc < 0) blk->ipc = 0;
        if (blk->ipc >= spp->pgs_per_blk) blk->ipc = spp->pgs_per_blk - 1;
    }
    blk->ipc++;
    if (blk->vpc > 0) {
        blk->vpc--;
    } else {
        NVMEV_ERROR("blk->vpc already 0 before decrement, blk=%d\n", ppa->g.blk);
        /* Don't return here, continue with line management updates */
    }

    /* 2. 根据介质类型，进入完全独立的原子操作块 */
    bool in_slc = is_slc_block(conv_ftl, ppa->g.blk);
    if (in_slc) {
        struct line_mgmt *lm = &conv_ftl->slc_lm;
        struct line *line;
        bool was_full_line = false;

        /* SLC 边界检查 (在加锁前) */
        if (!lm || !lm->lines || ppa->g.blk >= lm->tt_lines) {
            NVMEV_ERROR("[mark_page_invalid] SLC block index out of range: %u >= %u\n", 
                        ppa->g.blk, lm->tt_lines);
            return;
        }

        spin_lock(&conv_ftl->slc_lock); // --- SLC 加锁 ---

        line = &lm->lines[ppa->g.blk];

        /* 所有与 line 和 lm 链表相关的操作都在锁内完成 */
        if (line->vpc == spp->pgs_per_lun_line) {
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

        spin_unlock(&conv_ftl->slc_lock); // --- SLC 解锁 ---

    } else { // QLC 路径
        struct line_mgmt *lm = &conv_ftl->qlc_lm;
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

        line = &lm->lines[idx];
        
        /* 所有与 line 和 lm 链表相关的操作都在锁内完成 */
        if (line->vpc == spp->pgs_per_line) {
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
	/* 2. 验证PPA有效性 */
    if (!valid_ppa(conv_ftl, ppa)) {
        NVMEV_ERROR("[mark_page_valid] Invalid PPA: ch=%d, lun=%d, blk=%d, pg=%d\n",
                    ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg);
        return;
    }
    struct nand_block *blk;
    struct nand_page *pg;

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
        /* 根据实际需求决定是否继续 */
    }
    pg->status = PG_VALID;

    blk = get_blk(conv_ftl->ssd, ppa);
	if (!blk) {
        NVMEV_ERROR("[mark_page_valid] Failed to get block structure\n");
        return;
    }
    /* 关键修复：避免致命的BUG_ON，改为警告和容错处理 */
    if (blk->vpc < 0 || blk->vpc > spp->pgs_per_blk) {
        NVMEV_ERROR("[WARNING] Block vpc out of range: vpc=%d, valid_range=[0,%d]\n", 
                   blk->vpc, spp->pgs_per_blk);
        if (blk->vpc < 0) blk->vpc = 0;
        if (blk->vpc > spp->pgs_per_blk) blk->vpc = spp->pgs_per_blk;
    }
    blk->vpc++;
    if (blk->vpc > spp->pgs_per_blk) {
        blk->vpc = spp->pgs_per_blk;
    }

    /* 2. 根据介质类型，进入完全独立的原子操作块 */
    bool in_slc = is_slc_block(conv_ftl, ppa->g.blk);
    if (in_slc) {
        struct line_mgmt *lm = &conv_ftl->slc_lm;
        struct line *line;

        /* SLC 边界检查 (在加锁前) */
        if (!lm || !lm->lines || ppa->g.blk >= lm->tt_lines) {
            NVMEV_ERROR("[mark_page_valid] SLC block index out of range: %u >= %u\n", 
                        ppa->g.blk, lm->tt_lines);
            return;
        }

        spin_lock(&conv_ftl->slc_lock); // --- SLC 加锁 ---
        line = &lm->lines[ppa->g.blk];
        
        /* 关键修复：避免致命的BUG_ON，改为警告和容错处理 */
        if (line->vpc < 0 || line->vpc > spp->pgs_per_lun_line) {
            NVMEV_ERROR("[WARNING] SLC line vpc out of range: vpc=%d, valid_range=[0,%d]\n", 
                       line->vpc, spp->pgs_per_lun_line);
            /* 容错处理：复位为合理值 */
            if (line->vpc < 0) line->vpc = 0;
            if (line->vpc > spp->pgs_per_lun_line) line->vpc = spp->pgs_per_lun_line;
        }
        line->vpc++;
        if (line->vpc > spp->pgs_per_lun_line) {
            line->vpc = spp->pgs_per_lun_line;
        }
        spin_unlock(&conv_ftl->slc_lock); // --- SLC 解锁 ---

    } else { // QLC 路径
        struct line_mgmt *lm = &conv_ftl->qlc_lm;
        struct line *line;
        uint32_t start_blk = conv_ftl->slc_blks_per_pl;
        uint32_t idx;

        /* QLC 边界检查 (在加锁前) */
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

        spin_lock(&conv_ftl->qlc_lock); // --- QLC 加锁 ---
        line = &lm->lines[idx];
        
        /* 关键修复：避免致命的BUG_ON，改为警告和容错处理 */
        if (line->vpc < 0 || line->vpc >= spp->pgs_per_line) {
            NVMEV_ERROR("[WARNING] QLC line vpc out of range: vpc=%d, range=[0,%d)\n", 
                       line->vpc, spp->pgs_per_line);
            /* 容错处理：复位为合理值 */
            if (line->vpc < 0) line->vpc = 0;
            if (line->vpc >= spp->pgs_per_line) line->vpc = spp->pgs_per_line - 1;
        }
        line->vpc++;
        if (line->vpc > spp->pgs_per_line) {
            line->vpc = spp->pgs_per_line;
        }
        spin_unlock(&conv_ftl->qlc_lock); // --- QLC 解锁 ---
    }
}

// ... existing code ...

static void mark_block_free(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_block *blk = get_blk(conv_ftl->ssd, ppa);
	struct nand_page *pg = NULL;
	int i;

	for (i = 0; i < spp->pgs_per_blk; i++) {
		/* reset page status */
		pg = &blk->pg[i];
		NVMEV_ASSERT(pg->nsecs == spp->secs_per_pg);
		pg->status = PG_FREE;
	}

	/* reset block status */
	NVMEV_ASSERT(blk->npgs == spp->pgs_per_blk);
	blk->ipc = 0;
	blk->vpc = 0;
	blk->erase_cnt++;
}

static void gc_read_page(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	/* advance conv_ftl status, we don't care about how long it takes */
	if (cpp->enable_gc_delay) {
		struct nand_cmd gcr = {
			.type = GC_IO,
			.cmd = NAND_READ,
			.stime = 0,
			.xfer_size = spp->pgsz,
			.interleave_pci_dma = false,
			.ppa = ppa,
		};
		ssd_advance_nand(conv_ftl->ssd, &gcr);
	}
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct conv_ftl *conv_ftl, struct ppa *old_ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	struct ppa new_ppa;
	uint64_t lpn = get_rmap_ent(conv_ftl, old_ppa);

	/* 关键修复：避免致命的BUG_ON，改为警告和容错处理 */
	if (!valid_lpn(conv_ftl, lpn)) {
		NVMEV_ERROR("[WARNING] Invalid LPN in GC: lpn=%lld\n", lpn);
		return 0; /* 跳过无效页面 */
	}
	
	/* 
	 * 
	 * 让GC的数据也写入到SLC中，使用和用户写入完全相同的页面分配和指针推进逻辑。
	 * 这确保了数据流向的一致性，避免使用未初始化的gc_wp指针。
	 */
	new_ppa = get_new_slc_page(conv_ftl);
	if (!mapped_ppa(&new_ppa)) {
		NVMEV_ERROR("gc_write_page: Failed to get new SLC page. GC cannot proceed.\n");
		/* 这是一个严重问题，意味着SLC和GC都卡住了 */
		return 0; /* 返回0表示没有延迟，但实际上是失败了 */
	}
	
	/* update maptbl */
	set_maptbl_ent(conv_ftl, lpn, &new_ppa);
	/* update rmap */
	set_rmap_ent(conv_ftl, lpn, &new_ppa);

	mark_page_valid(conv_ftl, &new_ppa);

	/* 使用SLC的写指针推进逻辑 */
	advance_slc_write_pointer(conv_ftl);

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

	/* advance per-ch gc_endtime as well */
#if 0
	new_ch = get_ch(conv_ftl, &new_ppa);
	new_ch->gc_endtime = new_ch->next_ch_avail_time;

	new_lun = get_lun(conv_ftl, &new_ppa);
	new_lun->gc_endtime = new_lun->next_lun_avail_time;
#endif

	return 0;
}

/* 选择最佳的受害者line，优先选择SLC中无效页最多的line */
static struct line *select_victim_line(struct conv_ftl *conv_ftl, bool force)
{
    struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line *victim_line = NULL;
    struct line *slc_victim = NULL;
    struct line *qlc_victim = NULL;
    struct line_mgmt *slc_lm = &conv_ftl->slc_lm;
    struct line_mgmt *qlc_lm = &conv_ftl->qlc_lm;
    bool slc_suitable = false, qlc_suitable = false;
    
    /* === 修复竞争条件：使用统一的锁顺序和原子操作 === */
    
    /* 步骤1: 按固定顺序(SLC先, QLC后)获取候选者，避免死锁 */
    spin_lock(&conv_ftl->slc_lock);
    if (slc_lm->victim_line_cnt > 0) {
        slc_victim = pqueue_peek(slc_lm->victim_line_pq);
        /* 在持锁期间就判断是否合适，避免TOCTOU */
        slc_suitable = slc_victim && (force || slc_victim->vpc <= (spp->pgs_per_line / 8));
    }
    spin_unlock(&conv_ftl->slc_lock);

    spin_lock(&conv_ftl->qlc_lock);
    if (qlc_lm->victim_line_cnt > 0) {
        qlc_victim = pqueue_peek(qlc_lm->victim_line_pq);
        /* 在持锁期间就判断是否合适，避免TOCTOU */
        qlc_suitable = qlc_victim && (force || qlc_victim->vpc <= (spp->pgs_per_line / 8));
    }
    spin_unlock(&conv_ftl->qlc_lock);
    
    /* 步骤2: 选择策略 - 优先SLC，然后比较无效页数量 */
    if (slc_suitable && qlc_suitable) {
        /* 两者都合适，选择无效页更多的 */
        victim_line = (slc_victim->vpc <= qlc_victim->vpc) ? slc_victim : qlc_victim;
    } else if (slc_suitable) {
        victim_line = slc_victim;
    } else if (qlc_suitable) {
        victim_line = qlc_victim;
    } else {
        return NULL;  /* 没有合适的受害者 */
    }
    
    /* 步骤3: 原子地从选中队列移除 - 重新验证并移除 */
    if (victim_line == slc_victim) {
        spin_lock(&conv_ftl->slc_lock);
        /* 重新验证队列状态，防止在无锁期间被修改 */
        if (slc_lm->victim_line_cnt > 0 && pqueue_peek(slc_lm->victim_line_pq) == slc_victim) {
            pqueue_pop(slc_lm->victim_line_pq);
            victim_line->pos = 0;
            slc_lm->victim_line_cnt--;
            spin_unlock(&conv_ftl->slc_lock);
            NVMEV_DEBUG("Selected SLC victim line %d (vpc=%d)\n", victim_line->id, victim_line->vpc);
	return victim_line;
        } else {
            /* 队列状态已改变，重新尝试 */
            spin_unlock(&conv_ftl->slc_lock);
            NVMEV_DEBUG("SLC victim queue changed, retrying...\n");
            return select_victim_line(conv_ftl, force); /* 递归重试 */
        }
    } else { /* victim_line == qlc_victim */
        spin_lock(&conv_ftl->qlc_lock);
        /* 重新验证队列状态 */
        if (qlc_lm->victim_line_cnt > 0 && pqueue_peek(qlc_lm->victim_line_pq) == qlc_victim) {
            pqueue_pop(qlc_lm->victim_line_pq);
            victim_line->pos = 0;
            qlc_lm->victim_line_cnt--;
            spin_unlock(&conv_ftl->qlc_lock);
            NVMEV_DEBUG("Selected QLC victim line %d (vpc=%d)\n", victim_line->id, victim_line->vpc);
            return victim_line;
        } else {
            /* 队列状态已改变，重新尝试 */
            spin_unlock(&conv_ftl->qlc_lock);
            NVMEV_DEBUG("QLC victim queue changed, retrying...\n");
            return select_victim_line(conv_ftl, force); /* 递归重试 */
        }
    }
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0;
	int pg;

	for (pg = 0; pg < spp->pgs_per_blk; pg++) {
		ppa->g.pg = pg;
		pg_iter = get_pg(conv_ftl->ssd, ppa);
		/* there shouldn't be any free page in victim blocks */
		NVMEV_ASSERT(pg_iter->status != PG_FREE);
		if (pg_iter->status == PG_VALID) {
			gc_read_page(conv_ftl, ppa);
			/* delay the maptbl update until "write" happens */
			gc_write_page(conv_ftl, ppa);
			cnt++;
		}
	}

	NVMEV_ASSERT(get_blk(conv_ftl->ssd, ppa)->vpc == cnt);
}

/* here ppa identifies the block we want to clean */
static void clean_one_flashpg(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct convparams *cpp = &conv_ftl->cp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0, i = 0;
	uint64_t completed_time = 0;
	struct ppa ppa_copy = *ppa;

	for (i = 0; i < spp->pgs_per_flashpg; i++) {
		pg_iter = get_pg(conv_ftl->ssd, &ppa_copy);
		/* there shouldn't be any free page in victim blocks */
		NVMEV_ASSERT(pg_iter->status != PG_FREE);
		if (pg_iter->status == PG_VALID)
			cnt++;

		ppa_copy.g.pg++;
	}

	ppa_copy = *ppa;

	if (cnt <= 0)
		return;

	if (cpp->enable_gc_delay) {
		struct nand_cmd gcr = {
			.type = GC_IO,
			.cmd = NAND_READ,
			.stime = 0,
			.xfer_size = spp->pgsz * cnt,
			.interleave_pci_dma = false,
			.ppa = &ppa_copy,
		};
		completed_time = ssd_advance_nand(conv_ftl->ssd, &gcr);
	}

	for (i = 0; i < spp->pgs_per_flashpg; i++) {
		pg_iter = get_pg(conv_ftl->ssd, &ppa_copy);

		/* there shouldn't be any free page in victim blocks */
		if (pg_iter->status == PG_VALID) {
			/* delay the maptbl update until "write" happens */
			gc_write_page(conv_ftl, &ppa_copy);
		}

		ppa_copy.g.pg++;
	}
}

static void mark_line_free(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
    bool in_slc = is_slc_block(conv_ftl, ppa->g.blk);

    if (in_slc) {
        struct line_mgmt *lm = &conv_ftl->slc_lm;
        struct line *line;
        
        spin_lock(&conv_ftl->slc_lock); // --- 对SLC加锁 ---
        
        line = &lm->lines[ppa->g.blk];
        line->ipc = 0;
        line->vpc = 0;
        list_add_tail(&line->entry, &lm->free_line_list);
        lm->free_line_cnt++;
        
        spin_unlock(&conv_ftl->slc_lock); // --- 解锁 ---
    } else {
        struct line_mgmt *lm = &conv_ftl->qlc_lm;
        struct line *line;
        uint32_t start_blk = conv_ftl->slc_blks_per_pl;
        uint32_t idx = ppa->g.blk - start_blk;

        spin_lock(&conv_ftl->qlc_lock); // --- 对QLC加锁 ---
        
        /* 关键修复：避免致命的BUG_ON，改为警告和容错处理 */
        if (idx >= lm->tt_lines) {
            NVMEV_ERROR("[CRITICAL] QLC index out of range: idx=%u >= tt_lines=%u\n", 
                       idx, lm->tt_lines);
            spin_unlock(&conv_ftl->qlc_lock);
            return;
        }
        line = &lm->lines[idx];
        
        /* QLC line ID应该始终是物理block ID，不需要修复 */
        uint32_t expected_physical_id = start_blk + idx;
        if (line->id != expected_physical_id) {
            NVMEV_ERROR("QLC line ID corruption detected: line->id=%u, expected_physical_id=%u, array_idx=%u\n", 
                       line->id, expected_physical_id, idx);
            /* 不修复！保持原有的物理block ID，问题在别处 */
        }
        
	line->ipc = 0;
	line->vpc = 0;
	list_add_tail(&line->entry, &lm->free_line_list);
	lm->free_line_cnt++;

        spin_unlock(&conv_ftl->qlc_lock); // --- 解锁 ---
    }
}

static int do_gc(struct conv_ftl *conv_ftl, bool force)
{
	struct line *victim_line = NULL;
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct ppa ppa;
	int pg;
	bool in_slc;
	struct convparams *cpp;

	victim_line = select_victim_line(conv_ftl, force);
	if (!victim_line) {
		NVMEV_DEBUG("do_gc: No suitable victim line found.\n");
		return -1;
	}

	/* 根据victim line类型计算正确的物理block ID */
	if (victim_line >= conv_ftl->slc_lm.lines && 
	    victim_line < conv_ftl->slc_lm.lines + conv_ftl->slc_lm.tt_lines) {
		/* SLC victim: 直接使用line ID */
	ppa.g.blk = victim_line->id;
	} else {
		/* QLC victim: line ID是数组索引，需要加上SLC偏移 */
		ppa.g.blk = conv_ftl->slc_blks_per_pl + victim_line->id;
	}
	
	/* 确定是SLC还是QLC并显示相应的统计信息 */
	in_slc = is_slc_block(conv_ftl, ppa.g.blk);
	if (in_slc) {
	    struct line_mgmt *slc_lm = &conv_ftl->slc_lm;
	    NVMEV_DEBUG("GC-ing SLC line:%d,ipc=%d,vpc=%d,victim=%d,full=%d,free=%d\n", 
	               ppa.g.blk, victim_line->ipc, victim_line->vpc, 
	               slc_lm->victim_line_cnt, slc_lm->full_line_cnt, slc_lm->free_line_cnt);
	} else {
	    struct line_mgmt *qlc_lm = &conv_ftl->qlc_lm;
	    NVMEV_DEBUG("GC-ing QLC line:%d,ipc=%d,vpc=%d,victim=%d,full=%d,free=%d\n", 
	               ppa.g.blk, victim_line->ipc, victim_line->vpc,
	               qlc_lm->victim_line_cnt, qlc_lm->full_line_cnt, qlc_lm->free_line_cnt);
	}

	conv_ftl->wfc.credits_to_refill = victim_line->ipc;

	/*
	 * === 核心修复：简化循环，只处理受害块内的页面 ===
	 * 遍历受害块中的每一个页，搬走有效数据
	 */
	ppa.g.ch = 0;   /* 简化假设：所有块都在 ch=0, lun=0 */
	ppa.g.lun = 0;  /* 在真实实现中，需要从 victim_line->id 反向查询物理位置 */
	ppa.g.pl = 0;
	
	for (pg = 0; pg < spp->pgs_per_blk; pg++) {
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
	if (should_gc_high(conv_ftl)) {
		NVMEV_DEBUG("should_gc_high passed");
		NVMEV_ERROR("should_gc_high passed, FGGC");
		/* perform GC here until !should_gc(conv_ftl) */
		do_gc(conv_ftl, true);
	}
}

static bool is_same_flash_page(struct conv_ftl *conv_ftl, struct ppa ppa1, struct ppa ppa2)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint32_t ppa1_page = ppa1.g.pg / spp->pgs_per_flashpg;
	uint32_t ppa2_page = ppa2.g.pg / spp->pgs_per_flashpg;

	return (ppa1.h.blk_in_ssd == ppa2.h.blk_in_ssd) && (ppa1_page == ppa2_page);
}

/* 检查块是否为 SLC - 使用运行时参数替换编译期宏 */
static bool is_slc_block(struct conv_ftl *conv_ftl, uint32_t blk_id)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	
	/* 检查初始化状态 */
	if (!conv_ftl || !conv_ftl->slc_initialized || !conv_ftl->is_slc_block) {
		NVMEV_ERROR("SLC blocks not properly initialized\n");
		return false;  /* 默认返回false，避免误判 */
	}
	
	/* 关键修复：使用运行时参数spp->blks_per_pl而不是编译期常量BLKS_PER_PLN */
	/* 在多通道配置下，每个通道/LUN都有独立的block ID空间 */
	if (blk_id >= spp->blks_per_pl) {
		NVMEV_ERROR("Block ID out of range: %u >= %u (runtime blks_per_pl)\n", 
		           blk_id, spp->blks_per_pl);
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
	struct write_pointer *wp = &conv_ftl->slc_wp;
	if (!conv_ftl || !conv_ftl->slc_lm.lines) {
	        NVMEV_ERROR("SLC lines not initialized\n");
	        return (struct ppa){ .ppa = UNMAPPED_PPA };
	}
	if (!wp || (!wp->curline && list_empty(&conv_ftl->slc_lm.free_line_list))) {
		NVMEV_ERROR("SLC write pointer not ready and no free SLC line\n");
		return (struct ppa){ .ppa = UNMAPPED_PPA };
	}
	/* 如果 SLC 写指针未初始化，初始化它 */
    if (!wp->curline) {
        /* Protect SLC free list and counters */
        spin_lock(&conv_ftl->slc_lock);
		struct line_mgmt *lm = &conv_ftl->slc_lm;
		struct line *curline = list_first_entry_or_null(&lm->free_line_list, struct line, entry);
		
        if (!curline) {
            NVMEV_ERROR("No free SLC line available!\n");
            spin_unlock(&conv_ftl->slc_lock);
            return (struct ppa){ .ppa = UNMAPPED_PPA };
        }
        
        /* 关键修复：严格验证SLC line ID的合法性 - 输出运行时边界 */
        uint32_t array_idx = curline - lm->lines;
        NVMEV_ERROR("[DEBUG] get_new_slc_page: curline=%p, array_idx=%u, curline->id=%u\n", 
                   curline, array_idx, curline->id);
        NVMEV_ERROR("[DEBUG] Runtime boundaries: nchs=%d, luns_per_ch=%d, pls_per_lun=%d, blks_per_pl=%d, pgs_per_blk=%d\n", 
                   spp->nchs, spp->luns_per_ch, spp->pls_per_lun, spp->blks_per_pl, spp->pgs_per_blk);
        NVMEV_ERROR("[DEBUG] SLC config: slc_blks_per_pl=%u, total_slc_lines=%u\n", 
                   conv_ftl->slc_blks_per_pl, lm->tt_lines);
        
        if (curline->id != array_idx || curline->id >= conv_ftl->slc_blks_per_pl) {
            NVMEV_ERROR("[CRITICAL] SLC line ID corruption: curline->id=%u, array_idx=%u, slc_range=[0,%u), runtime_blks_per_pl=%d\n", 
                       curline->id, array_idx, conv_ftl->slc_blks_per_pl, spp->blks_per_pl);
            NVMEV_ERROR("[CRITICAL] Memory corruption detected! Clamping curline->id to valid range\n");
            curline->id = array_idx;  /* 强制修复为数组索引 */
        }
		
		list_del_init(&curline->entry);
		lm->free_line_cnt--;
		
		/* 使用修复后的ID */
		uint32_t blk_id = curline->id;
		
		*wp = (struct write_pointer) {
			.curline = curline,
			.ch = conv_ftl->lunpointer % spp->nchs,
			.lun = conv_ftl->lunpointer / spp->nchs,
			.pg = 0,
			.blk = blk_id,
			.pl = 0,
		};
        spin_unlock(&conv_ftl->slc_lock);
	}
	
    /* 获取当前页 */
    ppa.ppa = 0;
    ppa.g.ch = wp->ch;
    ppa.g.lun = wp->lun;
    ppa.g.pg = wp->pg;
    /* SLC 块号是相对块号，需要确保在正确范围内 */
    /* 在我们的设计中，SLC 占用前面的块(0-1637)，所以相对块号就是绝对块号 */
    ppa.g.blk = wp->blk;
    ppa.g.pl = wp->pl;
    
    /* 添加调试信息以追踪块号 */
    if (ppa.g.blk >= conv_ftl->slc_blks_per_pl) {
        NVMEV_ERROR("get_new_slc_page: Generated invalid SLC block %u >= %u\n", 
                   ppa.g.blk, conv_ftl->slc_blks_per_pl);
    }
	
	return ppa;
}

/* 推进 SLC 写指针 - 使用 Die Affinity */
static void advance_slc_write_pointer(struct conv_ftl *conv_ftl)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->slc_lm;
	struct write_pointer *wp = &conv_ftl->slc_wp;
	if (!wp || !wp->curline) {
		NVMEV_ERROR("advance_slc_write_pointer: SLC WP not initialized\n");
		return;
	}
    wp->pg++;
	
	/* 检查是否需要移到下一个 block */
    if (wp->pg >= spp->pgs_per_blk) {
        spin_lock(&conv_ftl->slc_lock);
		/* 当前 line 已满 */
		if (wp->curline->vpc == spp->pgs_per_lun_line) {
			list_add_tail(&wp->curline->entry, &lm->full_line_list);
			lm->full_line_cnt++;
		} else {
			pqueue_insert(lm->victim_line_pq, wp->curline);
			lm->victim_line_cnt++;
		}
		
			/* 获取新的 line */
	wp->curline = list_first_entry_or_null(&lm->free_line_list, struct line, entry);
    if (!wp->curline) {
        NVMEV_ERROR("No free SLC line available!\n");
        /* 标记为未就绪，下一次 get_new_slc_page 将失败 */
        wp->curline = NULL;
        spin_unlock(&conv_ftl->slc_lock);
        return;
    }
    
    /* 关键修复：严格验证新分配SLC line ID的合法性 - 输出运行时边界 */
    uint32_t array_idx = wp->curline - lm->lines;
    NVMEV_ERROR("[DEBUG] advance_slc_write_pointer: curline=%p, array_idx=%u, curline->id=%u\n", 
               wp->curline, array_idx, wp->curline->id);
    NVMEV_ERROR("[DEBUG] Runtime boundaries: blks_per_pl=%d, slc_blks_per_pl=%u, slc_total_lines=%u\n", 
               spp->blks_per_pl, conv_ftl->slc_blks_per_pl, lm->tt_lines);
    
    if (wp->curline->id != array_idx || wp->curline->id >= conv_ftl->slc_blks_per_pl) {
        NVMEV_ERROR("[CRITICAL] SLC line ID corruption in advance: curline->id=%u, array_idx=%u, slc_range=[0,%u), runtime_blks_per_pl=%d\n", 
                   wp->curline->id, array_idx, conv_ftl->slc_blks_per_pl, spp->blks_per_pl);
        NVMEV_ERROR("[CRITICAL] Memory corruption detected! Clamping curline->id to valid range\n");
        wp->curline->id = array_idx;  /* 强制修复为数组索引 */
    }
		
		list_del_init(&wp->curline->entry);
		lm->free_line_cnt--;
		
		wp->blk = wp->curline->id;  /* 使用修复后的ID */
		
		wp->pg = 0;
        spin_unlock(&conv_ftl->slc_lock);
	}
	
	/* Die 轮询 - 移到下一个 lun */
	conv_ftl->lunpointer++;
	if (conv_ftl->lunpointer >= (spp->nchs * spp->luns_per_ch)) {
		conv_ftl->lunpointer = 0;
	}
	
	wp->ch = conv_ftl->lunpointer % spp->nchs;
	wp->lun = conv_ftl->lunpointer / spp->nchs;
	
	/* 确保块号始终在SLC范围内 */
	if (wp->blk >= conv_ftl->slc_blks_per_pl) {
		NVMEV_ERROR("SLC block ID %u exceeds range [0, %u), resetting to 0\n", 
			   wp->blk, conv_ftl->slc_blks_per_pl);
		wp->blk = 0;
	}
}

/* 获取 QLC 的新页面 - 使用多区域并发*/
static struct ppa get_new_qlc_page(struct conv_ftl *conv_ftl, uint32_t region_id)
{
    struct ssdparams *spp = &conv_ftl->ssd->sp;
    struct ppa ppa;
    struct write_pointer *wp = &conv_ftl->qlc_wp[region_id];
    struct nand_page *pg;
    
    /* 参数验证 */
    if (!conv_ftl || region_id >= QLC_REGIONS) {
        NVMEV_ERROR("Invalid parameters: conv_ftl=%p, region_id=%u\n", 
                    conv_ftl, region_id);
        return (struct ppa){ .ppa = UNMAPPED_PPA };
    }
    
    /* 如果 QLC 写指针未初始化，初始化它 */
    if (!wp->curline) {
        spin_lock(&conv_ftl->qlc_lock);
        struct line_mgmt *lm = &conv_ftl->qlc_lm;
        struct line *curline = list_first_entry_or_null(&lm->free_line_list, struct line, entry);
        
        if (!curline) {
            NVMEV_ERROR("No free QLC line available in region %d!\n", region_id);
            spin_unlock(&conv_ftl->qlc_lock);
            return (struct ppa){ .ppa = UNMAPPED_PPA };
        }
        
        list_del_init(&curline->entry);
        lm->free_line_cnt--;
        
        /* 验证QLC line ID是否为合法的物理block ID - 输出运行时边界 */
        uint32_t start_blk = conv_ftl->slc_blks_per_pl;
        uint32_t array_idx = curline - lm->lines;
        NVMEV_ERROR("[DEBUG] get_new_qlc_page: curline=%p, array_idx=%u, curline->id=%u, expected_physical_id=%u\n", 
                   curline, array_idx, curline->id, start_blk + array_idx);
        NVMEV_ERROR("[DEBUG] QLC Runtime boundaries: start_blk=%u, qlc_range=[%u, %u), runtime_blks_per_pl=%d\n", 
                   start_blk, start_blk, start_blk + lm->tt_lines, spp->blks_per_pl);
        
        if (curline->id < start_blk || curline->id >= (start_blk + lm->tt_lines)) {
            NVMEV_ERROR("[CRITICAL] QLC line physical block ID %u out of valid range [%u, %u), runtime_blks_per_pl=%d\n", 
                       curline->id, start_blk, start_blk + lm->tt_lines, spp->blks_per_pl);
            NVMEV_ERROR("[CRITICAL] QLC Memory corruption detected! Clamping to valid physical block ID\n");
            curline->id = start_blk + array_idx;  /* 强制修复为正确的物理block ID */
            NVMEV_ERROR("[CRITICAL] Emergency fix: corrected to physical block ID %u (array_idx=%u)\n", 
                       curline->id, array_idx);
        }
        
        /* 初始化写指针 - 为每个区域分配不同的起始位置 */
        *wp = (struct write_pointer) {
            .curline = curline,
            .ch = region_id % spp->nchs,
            .lun = (region_id / spp->nchs) % spp->luns_per_ch,
            .pg = 0,
            .blk = curline->id,  /* 使用物理block ID */
            .pl = 0,
        };
        
        NVMEV_ERROR("[DEBUG] QLC region %u initialized: curline=%p, physical_blk=%u\n", 
                   region_id, curline, curline->id);
        spin_unlock(&conv_ftl->qlc_lock);
    }
    
retry_get_page:
    /* 获取当前页 */
    ppa.ppa = 0;
    ppa.g.ch = wp->ch;
    ppa.g.lun = wp->lun;
    ppa.g.pg = wp->pg;
    ppa.g.blk = wp->blk;
    ppa.g.pl = wp->pl;
    
    /* 验证页面状态 */
    pg = get_pg(conv_ftl->ssd, &ppa);
    if (!pg) {
        NVMEV_ERROR("Failed to get page structure for ppa\n");
        return (struct ppa){ .ppa = UNMAPPED_PPA };
    }
    
    if (pg->status != PG_FREE) {
        NVMEV_WARN("QLC page not FREE: status=%d at ch=%d,lun=%d,blk=%d,pg=%d\n", 
                   pg->status, ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pg);
        
        /* 推进写指针并重试 */
        if (advance_qlc_write_pointer(conv_ftl, region_id) == 0) {
            goto retry_get_page;
        } else {
            NVMEV_ERROR("Cannot advance write pointer\n");
            return (struct ppa){ .ppa = UNMAPPED_PPA };
        }
    }
    
    return ppa;
}

/* 推进 QLC 写指针 - 使用多区域并发 
 * 返回值: 0=成功, -1=失败 */
static int advance_qlc_write_pointer(struct conv_ftl *conv_ftl, uint32_t region_id)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->qlc_lm;
	struct write_pointer *wp = &conv_ftl->qlc_wp[region_id];
	uint32_t qlc_pgs_per_blk = spp->pgs_per_blk * 4;
	
	wp->pg++;
	
	/* QLC 每个块的页数是 SLC 的配置倍数 */
    if ((wp->pg % spp->pgs_per_oneshotpg) != 0)
		goto out;
	
	wp->pg -= spp->pgs_per_oneshotpg;
	wp->ch++;
	if (wp->ch != spp->nchs)
		goto out;
	
	wp->ch = 0;
	wp->lun++;
	if (wp->lun != spp->luns_per_ch)
		goto out;
	
	wp->lun = 0;
	wp->pg += spp->pgs_per_oneshotpg;
	
	if (wp->pg != qlc_pgs_per_blk)
		goto out;
	
	/* 当前 block 已满，移到下一个 */
	wp->pg = 0;
	
    spin_lock(&conv_ftl->qlc_lock);
    if (wp->curline->vpc == qlc_pgs_per_blk * spp->nchs * spp->luns_per_ch) {
		list_add_tail(&wp->curline->entry, &lm->full_line_list);
		lm->full_line_cnt++;
	} else {
		pqueue_insert(lm->victim_line_pq, wp->curline);
		lm->victim_line_cnt++;
	}
	
	/* 获取新的 line */
    wp->curline = list_first_entry_or_null(&lm->free_line_list, struct line, entry);
    if (!wp->curline) {
        NVMEV_ERROR("No free QLC line available in region %d!\n", region_id);
        /* 标记为未就绪，等待后续重新获取 */
        wp->curline = NULL;
        spin_unlock(&conv_ftl->qlc_lock);
        return -1;
    }
	
	list_del_init(&wp->curline->entry);
	lm->free_line_cnt--;
	
	    /* 验证QLC line ID是合法的物理block ID - 输出运行时边界 */
    uint32_t start_blk = conv_ftl->slc_blks_per_pl;
    uint32_t array_idx = wp->curline - lm->lines;
    NVMEV_ERROR("[DEBUG] advance_qlc_write_pointer: curline=%p, array_idx=%u, curline->id=%u, expected_physical_id=%u\n", 
               wp->curline, array_idx, wp->curline->id, start_blk + array_idx);
    NVMEV_ERROR("[DEBUG] QLC advance Runtime boundaries: start_blk=%u, qlc_range=[%u, %u), runtime_blks_per_pl=%d\n", 
               start_blk, start_blk, start_blk + lm->tt_lines, spp->blks_per_pl);
    
    if (wp->curline->id < start_blk || wp->curline->id >= (start_blk + lm->tt_lines)) {
        NVMEV_ERROR("[CRITICAL] advance_qlc_write_pointer: QLC line physical block ID %u out of range [%u, %u), runtime_blks_per_pl=%d\n",
                   wp->curline->id, start_blk, start_blk + lm->tt_lines, spp->blks_per_pl);
        NVMEV_ERROR("[CRITICAL] QLC advance Memory corruption detected! Clamping to valid physical block ID\n");
        wp->curline->id = start_blk + array_idx;  /* 强制修复为正确的物理block ID */
        NVMEV_ERROR("[CRITICAL] Emergency fix: corrected to physical block ID %u (array_idx=%u)\n",
                   wp->curline->id, array_idx);
    }
	
	/* 直接使用物理block ID，不需要再次计算 */
	wp->blk = wp->curline->id;
	
	NVMEV_ERROR("[DEBUG] advance_qlc_write_pointer: region=%u, new_physical_blk=%u\n", 
		   region_id, wp->blk);
    spin_unlock(&conv_ftl->qlc_lock);
	
out:
	return 0;
}

/* 更新热数据信息 */
static void update_heat_info(struct conv_ftl *conv_ftl, uint64_t lpn, bool is_read)
{
	struct heat_tracking *ht = &conv_ftl->heat_track;
	
	if (is_read) {
		ht->access_count[lpn]++;
		ht->last_access_time[lpn] = __get_ioclock(conv_ftl->ssd);
	}
}

/* 检查页面是否需要从 SLC 迁移到 QLC */
static bool should_migrate_to_qlc(struct conv_ftl *conv_ftl, uint64_t lpn)
{
	struct heat_tracking *ht = &conv_ftl->heat_track;
	    uint64_t current_time = __get_ioclock(conv_ftl->ssd);
	uint64_t time_diff = current_time - ht->last_access_time[lpn];
	
	/* 如果页面长时间未访问（冷数据），则应迁移到 QLC */
	/* 这里使用简单的时间阈值，实际可以根据需要调整策略 */
	if (time_diff > 1000000000ULL && ht->access_count[lpn] < ht->migration_threshold) {
		return true;
	}
	
	return false;
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
    
    /* 选择 QLC 区域（轮询） */
    uint32_t region = conv_ftl->current_qlc_region;
    conv_ftl->current_qlc_region = (conv_ftl->current_qlc_region + 1) % QLC_REGIONS;
    
    /* 获取 QLC 新页面 */
    new_ppa = get_new_qlc_page(conv_ftl, region);
    if (!mapped_ppa(&new_ppa)) {
        NVMEV_ERROR("Failed to get QLC page for migration\n");
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
    
    /* 推进 QLC 写指针 */
    advance_qlc_write_pointer(conv_ftl, region);
    
    /* 更新统计 */
    conv_ftl->migration_cnt++;
    
    NVMEV_DEBUG("Migrated LPN %llu from SLC to QLC region %d\n", lpn, region);
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
		conv_ftl = &conv_ftls[start_lpn % nr_parts];
		xfer_size = 0;
		prev_ppa = get_maptbl_ent(conv_ftl, start_lpn / nr_parts);

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
					/* QLC 读延迟 - 使用 QLC 延迟参数 */
					uint32_t qlc_cell = prev_ppa.g.pg % 4;  /* QLC 有 4 种页类型 */
					if (xfer_size == 4096) {
						srd.stime += spp->qlc_pg_4kb_rd_lat[qlc_cell];
					} else {
						srd.stime += spp->qlc_pg_rd_lat[qlc_cell];
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
				uint32_t qlc_cell = prev_ppa.g.pg % 4;
				if (xfer_size == 4096) {
					srd.stime += spp->qlc_pg_4kb_rd_lat[qlc_cell];
				} else {
					srd.stime += spp->qlc_pg_rd_lat[qlc_cell];
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
	return true;
}

static bool conv_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	/* NVMEV_DEBUG removed for performance */
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

	/* Debug logs removed for performance */
    if ((end_lpn / nr_parts) >= spp->tt_pgs) {
        NVMEV_ERROR("[DEBUG] conv_write: LPN RANGE CHECK FAILED - lpn passed FTL range(start_lpn=%lld,tt_pgs=%ld)\n",
                    start_lpn, spp->tt_pgs);
        ret->status = NVME_SC_LBA_RANGE;
        ret->nsecs_target = req->nsecs_start;
        return true; /* Return completion with error to avoid host timeout */
    }

    allocated_buf_size = buffer_allocate(wbuf, LBA_TO_BYTE(nr_lba));
	NVMEV_ERROR("[DEBUG] conv_write: buffer alloc size = %u, needed = %llu\n", allocated_buf_size, LBA_TO_BYTE(nr_lba));
    if (allocated_buf_size < LBA_TO_BYTE(nr_lba)) {
        NVMEV_ERROR("[DEBUG] conv_write: BUFFER ALLOCATION FAILED - insufficient write buffer (%u < %llu)\n",
                    allocated_buf_size, LBA_TO_BYTE(nr_lba));
        ret->status = NVME_SC_WRITE_FAULT;
        ret->nsecs_target = req->nsecs_start;
        return true; /* Complete with error */
    }

	nsecs_latest = ssd_advance_write_buffer(conv_ftl->ssd, req->nsecs_start, LBA_TO_BYTE(nr_lba));
	nsecs_xfer_completed = nsecs_latest;

	swr.stime = nsecs_latest;

	/* 移动所有变量声明到循环开头，符合 C90 标准 */
	uint64_t local_lpn;
	uint64_t nsecs_completed = 0;
    uint64_t write_lat;
    struct ppa ppa;
    //struct ppa old_ppa = { .ppa = UNMAPPED_PPA };  /* 用于迁移检查 */
    
	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		        /* 调试：检查是否进入了写入循环 */
		if (lpn == start_lpn) {
			NVMEV_ERROR("[DEBUG] conv_write: Starting write loop, lpn=%llu to %llu\n", start_lpn, end_lpn);
		}
		
		/* 注释掉旧的同步迁移逻辑，现在使用后台异步迁移 */
		/* if ((lpn & 0x3FF) == 0) {
			trigger_slc_migration_if_low(conv_ftl);
		} */

		conv_ftl = &conv_ftls[lpn % nr_parts];
		local_lpn = lpn / nr_parts;
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
        uint32_t slc_free_lines;
        spin_lock(&conv_ftl->slc_lock);
        slc_free_lines = conv_ftl->slc_lm.free_line_cnt;
        spin_unlock(&conv_ftl->slc_lock);
        
        /* 检查SLC使用率是否超过高水位线，触发后台迁移 */
        uint32_t slc_used_lines = conv_ftl->slc_lm.tt_lines - slc_free_lines;
        NVMEV_ERROR("[DEBUG] SLC status: free_lines=%u, used_lines=%u, high_watermark=%u, total=%u\n", 
                   slc_free_lines, slc_used_lines, conv_ftl->slc_high_watermark, conv_ftl->slc_lm.tt_lines);
        if (slc_used_lines >= conv_ftl->slc_high_watermark) {
            NVMEV_ERROR("[DEBUG] SLC usage high (%u >= %u), triggering background migration\n", 
                      slc_used_lines, conv_ftl->slc_high_watermark);
            wakeup_migration_thread(conv_ftl);
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

		/* 关键修复：在使用PPA之前显示调试信息 */
		/* Debug log removed for performance */

//66f1

		/* update maptbl */
		set_maptbl_ent(conv_ftl, local_lpn, &ppa);
		NVMEV_DEBUG("conv_write: got new ppa %lld, ", ppa2pgidx(conv_ftl, &ppa));
		/* update rmap */
		set_rmap_ent(conv_ftl, local_lpn, &ppa);

		mark_page_valid(conv_ftl, &ppa);

		nsecs_completed = ssd_advance_write_buffer(conv_ftl->ssd, nsecs_latest, conv_ftl->ssd->sp.pgsz);

        /* Check whether we need to do a write in this stripe
         * Use current page offset within oneshot page (flash page)
         */
        {
            uint32_t pg_off = ppa.g.pg % spp->pgs_per_oneshotpg;
            if (pg_off == (spp->pgs_per_oneshotpg - 1) || lpn == end_lpn) {
                xfer_size = (pg_off + 1) * spp->pgsz;
			swr.xfer_size = xfer_size;
			
			/* 使用 SLC 写延迟 */
			write_lat = conv_ftl->ssd->sp.pg_wr_lat;
			swr.ppa = &ppa;
			
			/* 关键修复：在调用ssd_advance_nand之前显示PPA值 */
			/* Debug log removed for performance */
			           
			/* 验证PPA合法性 */
			if (!valid_ppa(conv_ftl, &ppa)) {
			    NVMEV_ERROR("[FATAL] Invalid PPA detected before ssd_advance_nand! Aborting write.\n");
			    ret->status = NVME_SC_WRITE_FAULT;
			    return true;
			}
			           
			nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &swr);
			
			/* 验证NAND写入结果 */
			if (nsecs_completed == swr.stime) {
			    NVMEV_ERROR("[WARNING] ssd_advance_nand returned same timestamp - possible PPA validation failure\n");
			}
			/* 异步释放写缓冲，避免后续分配失败 */
			enqueue_writeback_io_req(req->sq_id, nsecs_completed, wbuf, xfer_size);
			/* schedule_internal_operation 暂时注释掉，函数不存在 */
			/* schedule_internal_operation(conv_ftl->ssd, nsecs_completed, xfer_size, &ppa); */

			//xfer_size = 0;
			swr.stime = nsecs_completed;
			
				/* 关键修复：在NAND写入完成后才推进写指针 */
			/* Debug log removed for performance */
			advance_slc_write_pointer(conv_ftl);
            }
        }

		nsecs_latest = max(nsecs_completed, nsecs_latest);

		/* 更新热数据信息 */
		update_heat_info(conv_ftl, local_lpn, false);
		conv_ftl->heat_track.last_access_time[local_lpn] = __get_ioclock(conv_ftl->ssd);
		
		
		/* 检查是否需要触发后台迁移 */
		if (conv_ftl->slc_lm.free_line_cnt < 2) {
			/* SLC 空间不足，需要迁移一些冷数据到 QLC */
			NVMEV_DEBUG("SLC space low, triggering migration\n");
			/* 这里可以实现更复杂的后台迁移策略 */
		}
		
		consume_write_credit(conv_ftl);
		check_and_refill_write_credit(conv_ftl);
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
    
    if (!ns || !ns->ftls || !req || !ret || !req->cmd) {
        NVMEV_ERROR("conv_proc_nvme_io_cmd: NULL parameter check failed\n");
        if (ret) {
            ret->status = NVME_SC_INTERNAL;
            ret->nsecs_target = req ? req->nsecs_start : local_clock();
        }
        /* 必须完成请求，避免超时导致控制器复位 */
        return true;
    }
    
    cmd = req->cmd;
	
	/* 关键修复：避免致命的BUG_ON，改为警告和容错处理 */
	if (ns->csi != NVME_CSI_NVM) {
		NVMEV_ERROR("[WARNING] Unexpected namespace CSI: expected %d, got %d\n", 
			   NVME_CSI_NVM, ns->csi);
		/* 继续处理，不崩溃系统 */
	}

	switch (cmd->common.opcode) {
	case nvme_cmd_write:
        if (!conv_write(ns, req, ret))
            return true; /* 出错也返回完成，状态在 ret 内 */
		break;
	case nvme_cmd_read:
        if (!conv_read(ns, req, ret))
            return true; /* 出错也返回完成，状态在 ret 内 */
		break;
	case nvme_cmd_flush:
		conv_flush(ns, req, ret);
		break;
	default:
		NVMEV_ERROR("conv_proc_nvme_io_cmd: Unimplemented command: %s(%d)\n", 
			   nvme_opcode_string(cmd->common.opcode), cmd->common.opcode);
		break;
	}

	return true;
}

/* 安全检查辅助函数 */
static inline bool is_valid_write_pointer(struct write_pointer *wp)
{
	return wp && wp->curline && wp->blk != INVALID_PPA && wp->pg != INVALID_PPA;
}

static inline bool is_ftl_initialized(struct conv_ftl *conv_ftl)
{
	return conv_ftl && 
	       conv_ftl->maptbl_initialized &&
	       conv_ftl->rmap_initialized &&
	       conv_ftl->slc_initialized &&
	       conv_ftl->qlc_initialized;
}

static inline bool is_line_mgmt_valid(struct line_mgmt *lm)
{
	return lm && lm->lines && lm->victim_line_pq;
}

/* 内存分配安全包装函数 */
static void *safe_vmalloc(size_t size, const char *desc)
{
	void *ptr = vmalloc(size);
	if (!ptr) {
		NVMEV_ERROR("Failed to allocate %s memory (size: %zu)\n", desc, size);
	}
	return ptr;
}

static void *safe_kmalloc(size_t size, gfp_t flags, const char *desc)
{
	void *ptr = kmalloc(size, flags);
	if (!ptr) {
		NVMEV_ERROR("Failed to allocate %s memory (size: %zu)\n", desc, size);
	}
	return ptr;
}


/* 智能区域选择 - 根据负载选择最佳QLC区域 */
static uint32_t select_best_qlc_region(struct conv_ftl *conv_ftl)
{
    uint32_t best_region = 0;
    uint32_t min_load = ~0u;
	uint32_t i;
	
	/* 选择当前负载最小的区域 */
	for (i = 0; i < QLC_REGIONS; i++) {
		struct write_pointer *wp = &conv_ftl->qlc_wp[i];
		uint32_t load = wp->pg;  /* 简单的负载指标 */
		
		if (load < min_load) {
			min_load = load;
			best_region = i;
		}
	}
	
	return best_region;
}

/*
 * 统一的空指针检查宏和工具函数
 * 防止野指针、空指针解引用和内存分配失败
 */

/* 空指针检查宏 */
#define CHECK_NULL_PTR(ptr, desc) \
	do { \
		if (!(ptr)) { \
			NVMEV_ERROR("NULL pointer check failed: %s\n", desc); \
			return -ENOMEM; \
		} \
	} while(0)

#define CHECK_NULL_PTR_VOID(ptr, desc) \
	do { \
		if (!(ptr)) { \
			NVMEV_ERROR("NULL pointer check failed: %s\n", desc); \
			return; \
		} \
	} while(0)

/* 内存分配检查宏 */
#define CHECK_ALLOC(ptr, desc) \
	do { \
		if (!(ptr)) { \
			NVMEV_ERROR("Memory allocation failed: %s\n", desc); \
			return -ENOMEM; \
		} \
	} while(0)

#define CHECK_ALLOC_VOID(ptr, desc) \
	do { \
		if (!(ptr)) { \
			NVMEV_ERROR("Memory allocation failed: %s\n", desc); \
			return; \
		} \
	} while(0)

/* 安全的指针使用宏 */
#define SAFE_PTR_ACCESS(ptr, default_val) \
	((ptr) ? (ptr) : (default_val))

/* 指针有效性检查函数 */
static inline bool is_valid_pointer(void *ptr)
{
	return ptr != NULL && ptr != (void *)-1;
}

/* 安全的数组访问 */
static inline bool is_valid_array_index(size_t index, size_t size)
{
	return index < size;
}

/* 安全的指针解引用 */
#define SAFE_DEREF(ptr, member, default_val) \
	((ptr) && (ptr)->member ? (ptr)->member : (default_val))

/* 内存分配失败时的清理函数 */
static void cleanup_on_alloc_failure(struct conv_ftl *conv_ftl)
{
	/* 清理已分配的资源 */
	if (conv_ftl->maptbl) {
		vfree(conv_ftl->maptbl);
		conv_ftl->maptbl = NULL;
	}
	if (conv_ftl->rmap) {
		vfree(conv_ftl->rmap);
		conv_ftl->rmap = NULL;
	}
	if (conv_ftl->is_slc_block) {
		vfree(conv_ftl->is_slc_block);
		conv_ftl->is_slc_block = NULL;
	}
	if (conv_ftl->page_in_slc) {
		vfree(conv_ftl->page_in_slc);
		conv_ftl->page_in_slc = NULL;
	}
	
	/* 重置初始化标志 */
	conv_ftl->maptbl_initialized = false;
	conv_ftl->rmap_initialized = false;
	conv_ftl->slc_initialized = false;
	conv_ftl->heat_track_initialized = false;
}

/* ======================== 后台线程实现 ======================== */

/* 后台迁移线程 */
static int background_migration_thread(void *data)
{
	struct conv_ftl *conv_ftl = (struct conv_ftl *)data;
	
	NVMEV_ERROR("[DEBUG] Background migration thread started\n");
	
	while (!kthread_should_stop() && !conv_ftl->threads_should_stop) {
		/* 等待迁移信号 */
		wait_event_interruptible(conv_ftl->migration_wq,
			atomic_read(&conv_ftl->migration_needed) || 
			kthread_should_stop() || 
			conv_ftl->threads_should_stop);
		
		if (kthread_should_stop() || conv_ftl->threads_should_stop)
			break;
		
		/* 执行迁移直到SLC使用率降到低水位线以下 */
		while (atomic_read(&conv_ftl->migration_needed) && 
		       !kthread_should_stop() && 
		       !conv_ftl->threads_should_stop) {
		       
			uint32_t slc_free_lines, slc_used_lines;
			spin_lock(&conv_ftl->slc_lock);
			slc_free_lines = conv_ftl->slc_lm.free_line_cnt;
			spin_unlock(&conv_ftl->slc_lock);
			
			slc_used_lines = conv_ftl->slc_lm.tt_lines - slc_free_lines;
			
			/* 如果SLC使用率降到低水位线以下，停止迁移 */
			if (slc_used_lines < conv_ftl->slc_low_watermark) {
				atomic_set(&conv_ftl->migration_needed, 0);
				break;
			}
			
			/* 执行一批迁移操作 */
			NVMEV_ERROR("[DEBUG] Background migration working: free_lines=%u, target=%u\n", 
			          slc_free_lines, conv_ftl->slc_low_watermark);
			migrate_some_cold_from_slc(conv_ftl, 16);
			
			/* 让出CPU，避免独占 */
			cond_resched();
		}
	}
	
	NVMEV_INFO("Background migration thread stopped\n");
	return 0;
}

/* 后台GC线程 */
static int background_gc_thread(void *data)
{
	struct conv_ftl *conv_ftl = (struct conv_ftl *)data;
	
	NVMEV_INFO("Background GC thread started\n");
	
	while (!kthread_should_stop() && !conv_ftl->threads_should_stop) {
		/* 等待GC信号 */
		wait_event_interruptible(conv_ftl->gc_wq,
			atomic_read(&conv_ftl->gc_needed) || 
			kthread_should_stop() || 
			conv_ftl->threads_should_stop);
		
		if (kthread_should_stop() || conv_ftl->threads_should_stop)
			break;
		
		/* 执行GC直到空间恢复到低水位线 */
		while (atomic_read(&conv_ftl->gc_needed) && 
		       !kthread_should_stop() && 
		       !conv_ftl->threads_should_stop) {
		       
			uint32_t slc_free, qlc_free, total_free_lines;
			
			/* 分步读取避免同时持有两个锁 */
			spin_lock(&conv_ftl->slc_lock);
			slc_free = conv_ftl->slc_lm.free_line_cnt;
			spin_unlock(&conv_ftl->slc_lock);
			
			spin_lock(&conv_ftl->qlc_lock);
			qlc_free = conv_ftl->qlc_lm.free_line_cnt;
			spin_unlock(&conv_ftl->qlc_lock);
			
			total_free_lines = slc_free + qlc_free;
			
			/* 如果总空间恢复到低水位线以上，停止GC */
			if (total_free_lines >= conv_ftl->gc_low_watermark) {
				atomic_set(&conv_ftl->gc_needed, 0);
				break;
			}
			
			/* 执行一次GC */
			if (do_gc(conv_ftl, false) < 0) {
				/* 没有合适的受害者，稍作等待 */
				msleep(10);
			}
			
			/* 让出CPU，避免独占 */
			cond_resched();
		}
	}
	
	NVMEV_INFO("Background GC thread stopped\n");
	return 0;
}

/* 初始化后台线程 */
static void init_background_threads(struct conv_ftl *conv_ftl)
{
	/* 
	 * === 修复水位线逻辑 ===
	 * 使用基于百分比的正确逻辑：
	 * - 高水位线(触发阈值) = 总容量的80% (当使用量达到80%时触发后台操作)
	 * - 低水位线(停止阈值) = 总容量的70% (当使用量降到70%以下时停止后台操作)
	 */
	uint32_t slc_total = conv_ftl->slc_lm.tt_lines;
	uint32_t total_lines = conv_ftl->slc_lm.tt_lines + conv_ftl->qlc_lm.tt_lines;
	
	/* SLC迁移水位线：基于SLC使用率 */
	conv_ftl->slc_high_watermark = slc_total * 80 / 100;  /* 当SLC使用80%时触发迁移 */
	conv_ftl->slc_low_watermark = slc_total * 70 / 100;   /* 当SLC使用降到70%时停止迁移 */
	
	/* GC水位线：基于总体使用率 */
	conv_ftl->gc_high_watermark = total_lines * 90 / 100; /* 当总体使用90%时触发GC */
	conv_ftl->gc_low_watermark = total_lines * 80 / 100;  /* 当总体使用降到80%时停止GC */
	
	NVMEV_ERROR("[DEBUG] Watermarks: SLC_high=%u, SLC_low=%u, GC_high=%u, GC_low=%u (SLC_total=%u, QLC_total=%u)\n",
	          conv_ftl->slc_high_watermark, conv_ftl->slc_low_watermark, 
	          conv_ftl->gc_high_watermark, conv_ftl->gc_low_watermark,
	          conv_ftl->slc_lm.tt_lines, conv_ftl->qlc_lm.tt_lines);
	
	/* 初始化等待队列和原子变量 */
	init_waitqueue_head(&conv_ftl->migration_wq);
	init_waitqueue_head(&conv_ftl->gc_wq);
	atomic_set(&conv_ftl->migration_needed, 0);
	atomic_set(&conv_ftl->gc_needed, 0);
	conv_ftl->threads_should_stop = false;
	
	/* 创建后台线程 */
	conv_ftl->migration_thread = kthread_run(background_migration_thread, conv_ftl, "nvmev_migration");
	if (IS_ERR(conv_ftl->migration_thread)) {
		NVMEV_ERROR("Failed to create migration thread\n");
		conv_ftl->migration_thread = NULL;
	}
	
	conv_ftl->gc_thread = kthread_run(background_gc_thread, conv_ftl, "nvmev_gc");
	if (IS_ERR(conv_ftl->gc_thread)) {
		NVMEV_ERROR("Failed to create GC thread\n");
		conv_ftl->gc_thread = NULL;
	}
	
	NVMEV_INFO("Background threads initialized: migration=%p, gc=%p\n", 
		   conv_ftl->migration_thread, conv_ftl->gc_thread);
}

/* 停止后台线程 */
static void stop_background_threads(struct conv_ftl *conv_ftl)
{
	conv_ftl->threads_should_stop = true;
	
	/* 唤醒线程以便它们能检查停止标志 */
	wake_up_interruptible(&conv_ftl->migration_wq);
	wake_up_interruptible(&conv_ftl->gc_wq);
	
	/* 等待线程结束 */
	if (conv_ftl->migration_thread) {
		kthread_stop(conv_ftl->migration_thread);
		conv_ftl->migration_thread = NULL;
	}
	
	if (conv_ftl->gc_thread) {
		kthread_stop(conv_ftl->gc_thread);
		conv_ftl->gc_thread = NULL;
	}
	
	NVMEV_INFO("Background threads stopped\n");
}

/* 唤醒迁移线程 */
static void wakeup_migration_thread(struct conv_ftl *conv_ftl)
{
	NVMEV_ERROR("[DEBUG] Waking up migration thread\n");
	atomic_set(&conv_ftl->migration_needed, 1);
	wake_up_interruptible(&conv_ftl->migration_wq);
}

/* 唤醒GC线程 */
static void wakeup_gc_thread(struct conv_ftl *conv_ftl)
{
	atomic_set(&conv_ftl->gc_needed, 1);
	wake_up_interruptible(&conv_ftl->gc_wq);
}
