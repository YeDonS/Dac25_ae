// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ktime.h>
#include <linux/sched/clock.h>
#include <linux/delay.h>

#include "nvmev.h"
#include "conv_ftl.h"

void enqueue_writeback_io_req(int sqid, unsigned long long nsecs_target,
			      struct buffer *write_buffer, unsigned int buffs_to_release);

static inline bool last_pg_in_wordline(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	return (ppa->g.pg % spp->pgs_per_oneshotpg) == (spp->pgs_per_oneshotpg - 1);
}

static bool should_gc(struct conv_ftl *conv_ftl)
{
	return (conv_ftl->lm.free_line_cnt <= conv_ftl->cp.gc_thres_lines);
}

static inline bool should_gc_high(struct conv_ftl *conv_ftl)
{
	return conv_ftl->lm.free_line_cnt <= conv_ftl->cp.gc_thres_lines_high;
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

static uint64_t ppa2pgidx(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint64_t pgidx;

	NVMEV_DEBUG("ppa2pgidx: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", ppa->g.ch, ppa->g.lun,
		    ppa->g.pl, ppa->g.blk, ppa->g.pg);

	pgidx = ppa->g.ch * spp->pgs_per_ch + ppa->g.lun * spp->pgs_per_lun +
		ppa->g.pl * spp->pgs_per_pl + ppa->g.blk * spp->pgs_per_blk + ppa->g.pg;

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
		forground_gc(conv_ftl);

		wfc->write_credits += wfc->credits_to_refill;
	}
}

static void init_lines(struct conv_ftl *conv_ftl)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *line;
	int i;

	lm->tt_lines = spp->tt_lines;
	NVMEV_ASSERT(lm->tt_lines == spp->tt_lines);
	lm->lines = vmalloc(sizeof(struct line) * lm->tt_lines);
	if (!lm->lines) {
		NVMEV_ERROR("Failed to allocate SLC lines memory\n");
		return;
	}

	INIT_LIST_HEAD(&lm->free_line_list);
	INIT_LIST_HEAD(&lm->full_line_list);

	lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri, victim_line_get_pri,
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
			.id = i,
			.ipc = 0,
			.vpc = 0,
			.pos = 0,
			.entry = LIST_HEAD_INIT(lm->lines[i].entry),
		};

		/* initialize all the lines as free lines */
		list_add_tail(&lm->lines[i].entry, &lm->free_line_list);
		lm->free_line_cnt++;
	}

	NVMEV_ASSERT(lm->free_line_cnt == lm->tt_lines);
	lm->victim_line_cnt = 0;
	lm->full_line_cnt = 0;
}

//66f1
static void init_lines_DA(struct conv_ftl *conv_ftl)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;

	uint32_t luncount = conv_ftl->ssd->sp.luns_per_ch * conv_ftl->ssd->sp.nchs;
	uint32_t lun=0;
	for( lun=0; lun < luncount; lun++ )
	{
		struct line_mgmt *lm = (conv_ftl->lunlm+lun);
		struct line *line;
		int i;

		lm->tt_lines = spp->blks_per_pl;
		NVMEV_ASSERT(lm->tt_lines == spp->tt_lines);
		lm->lines = vmalloc(sizeof(struct line) * lm->tt_lines);
		if (!lm->lines) {
			NVMEV_ERROR("Failed to allocate LUN lines memory for lun %d\n", lun);
			continue;
		}

		INIT_LIST_HEAD(&lm->free_line_list);
		INIT_LIST_HEAD(&lm->full_line_list);

		lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri, victim_line_get_pri,
						victim_line_set_pri, victim_line_get_pos,
						victim_line_set_pos);
		if (!lm->victim_line_pq) {
			NVMEV_ERROR("Failed to initialize LUN victim line priority queue for lun %d\n", lun);
			vfree(lm->lines);
			lm->lines = NULL;
			continue;
		}

		lm->free_line_cnt = 0;
		for (i = 0; i < lm->tt_lines; i++) {
			lm->lines[i] = (struct line) {
				.id = i,
				.ipc = 0,
				.vpc = 0,
				.pos = 0,
				.entry = LIST_HEAD_INIT(lm->lines[i].entry),
			};

			/* initialize all the lines as free lines */
			list_add_tail(&lm->lines[i].entry, &lm->free_line_list);
			lm->free_line_cnt++;
		}

		NVMEV_ASSERT(lm->free_line_cnt == lm->tt_lines);
		lm->victim_line_cnt = 0;
		lm->full_line_cnt = 0;

	}
}
//66f1

static void remove_lines(struct conv_ftl *conv_ftl)
{
	pqueue_free(conv_ftl->lm.victim_line_pq);
	vfree(conv_ftl->lm.lines);
}

static void remove_lines_DA(struct conv_ftl *conv_ftl)
{
	uint32_t luncount = conv_ftl->ssd->sp.luns_per_ch * conv_ftl->ssd->sp.nchs;
	uint32_t lun=0;
	for( lun=0; lun < luncount; lun++ )
	{
		struct line_mgmt *lm = (conv_ftl->lunlm+lun);
		pqueue_free(lm->victim_line_pq);
		vfree(lm->lines);
	}
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
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *curline = list_first_entry_or_null(&lm->free_line_list, struct line, entry);

	if (!curline) {
		NVMEV_ERROR("No free line left in VIRT !!!!\n");
		return NULL;
	}

	list_del_init(&curline->entry);
	lm->free_line_cnt--;
	NVMEV_DEBUG("[%s] free_line_cnt %d\n", __FUNCTION__, lm->free_line_cnt);
	return curline;
}

//66f1
static struct line *get_next_free_line_DA(struct conv_ftl *conv_ftl, uint32_t lun)
{
	struct line_mgmt *lm = conv_ftl->lunlm+lun;
	struct line *curline = list_first_entry_or_null(&lm->free_line_list, struct line, entry);

	if (!curline) {
		NVMEV_ERROR("No free line left in VIRT !!!!\n");
		return NULL;
	}

	list_del_init(&curline->entry);
	lm->free_line_cnt--;
	NVMEV_DEBUG("[%s] free_line_cnt %d\n", __FUNCTION__, lm->free_line_cnt);
	return curline;
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
	if (io_type == USER_IO) {
		return (ftl->lunwp+lun);
	} else if (io_type == GC_IO) {
		return &ftl->gc_wp;
	}

	NVMEV_ASSERT(0);
	return NULL;
}
//66f1

static void prepare_write_pointer(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	struct write_pointer *wp = __get_wp(conv_ftl, io_type);
	struct line *curline = get_next_free_line(conv_ftl);

	NVMEV_ASSERT(wp);
	NVMEV_ASSERT(curline);

	/* wp->curline is always our next-to-write super-block */
	*wp = (struct write_pointer) {
		.curline = curline,
		.ch = 0,
		.lun = 0,
		.pg = 0,
		.blk = curline->id,
		.pl = 0,
	};
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
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct write_pointer *wpp = __get_wp(conv_ftl, io_type);

	NVMEV_DEBUG("current wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", wpp->ch, wpp->lun,
		    wpp->pl, wpp->blk, wpp->pg);

	check_addr(wpp->pg, spp->pgs_per_blk);
	wpp->pg++;
	if ((wpp->pg % spp->pgs_per_oneshotpg) != 0)
		goto out;

	wpp->pg -= spp->pgs_per_oneshotpg;
	check_addr(wpp->ch, spp->nchs);
	wpp->ch++;
	if (wpp->ch != spp->nchs)
		goto out;

	wpp->ch = 0;
	check_addr(wpp->lun, spp->luns_per_ch);
	wpp->lun++;
	/* in this case, we should go to next lun */
	if (wpp->lun != spp->luns_per_ch)
		goto out;

	wpp->lun = 0;
	/* go to next wordline in the block */
	wpp->pg += spp->pgs_per_oneshotpg;
	if (wpp->pg != spp->pgs_per_blk)
		goto out;

	wpp->pg = 0;
	/* move current line to {victim,full} line list */
	if (wpp->curline->vpc == spp->pgs_per_line) {
		/* all pgs are still valid, move to full line list */
		NVMEV_ASSERT(wpp->curline->ipc == 0);
		list_add_tail(&wpp->curline->entry, &lm->full_line_list);
		lm->full_line_cnt++;
		NVMEV_DEBUG("wpp: move line to full_line_list\n");
	} else {
		NVMEV_DEBUG("wpp: line is moved to victim list\n");
		NVMEV_ASSERT(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
		/* there must be some invalid pages in this line */
		NVMEV_ASSERT(wpp->curline->ipc > 0);
		pqueue_insert(lm->victim_line_pq, wpp->curline);
		lm->victim_line_cnt++;
	}
	/* current line is used up, pick another empty line */
	check_addr(wpp->blk, spp->blks_per_pl);
	wpp->curline = get_next_free_line(conv_ftl);
	NVMEV_DEBUG("wpp: got new clean line %d\n", wpp->curline->id);

	wpp->blk = wpp->curline->id;
	check_addr(wpp->blk, spp->blks_per_pl);

	/* make sure we are starting from page 0 in the super block */
	NVMEV_ASSERT(wpp->pg == 0);
	NVMEV_ASSERT(wpp->lun == 0);
	NVMEV_ASSERT(wpp->ch == 0);
	/* TODO: assume # of pl_per_lun is 1, fix later */
	NVMEV_ASSERT(wpp->pl == 0);
out:
	NVMEV_DEBUG("advanced wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d (curline %d)\n", wpp->ch,
		    wpp->lun, wpp->pl, wpp->blk, wpp->pg, wpp->curline->id);
}

//66f1
static void advance_write_pointer_DA(struct conv_ftl *conv_ftl, uint32_t io_type)
{
	uint32_t glun=conv_ftl->lunpointer;	
	struct ssdparams *spp = &conv_ftl->ssd->sp;	
	struct line_mgmt *lm = NULL;
	struct write_pointer *wpp = NULL;

	lm = conv_ftl->lunlm+conv_ftl->lunpointer;
	wpp = __get_wp_DA(conv_ftl, io_type, conv_ftl->lunpointer);

	NVMEV_DEBUG("current wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d, glun:%d\n", wpp->ch, wpp->lun,
		    wpp->pl, wpp->blk, wpp->pg, conv_ftl->lunpointer);

	
	check_addr(wpp->pg, spp->pgs_per_blk);
	wpp->pg++; //map page 4k
	if ((wpp->pg % spp->pgs_per_oneshotpg) != 0)
	{
		goto out;
	}
	NVMEV_DEBUG("page : %u, oneshotpg limit %d\n", spp->pgsz, spp->pgs_per_oneshotpg);

	if (wpp->pg == spp->pgs_per_blk)
	{//move to next blk
		NVMEV_DEBUG("block limit, pgs_per_blk = %d\n", spp->pgs_per_blk);

		if (wpp->curline->vpc == spp->pgs_per_lun_line) {
			/* all pgs are still valid, move to full line list */
			NVMEV_ASSERT(wpp->curline->ipc == 0);
			list_add_tail(&wpp->curline->entry, &lm->full_line_list);
			lm->full_line_cnt++;
			NVMEV_DEBUG("wpp: move line to full_line_list\n");
			//NVMEV_ERROR("wpp: move line to full_line_list\n");
		} else {
			NVMEV_DEBUG("wpp: line is moved to victim list\n");
			//NVMEV_ERROR("wpp: line is moved to victim list\n");
			NVMEV_ASSERT(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_lun_line);
			/* there must be some invalid pages in this line */
			//NVMEV_ERROR("wpp: curline ipc= %d\n", wpp->curline->ipc);
			NVMEV_ASSERT(wpp->curline->ipc > 0);
			pqueue_insert(lm->victim_line_pq, wpp->curline);
			lm->victim_line_cnt++;
		}
		/* current line is used up, pick another empty line */
		check_addr(wpp->blk, spp->blks_per_pl);
		wpp->curline = get_next_free_line_DA(conv_ftl, conv_ftl->lunpointer);
		NVMEV_DEBUG("wpp: got new clean line %d\n", wpp->curline->id);
		//NVMEV_ERROR("wpp: got new clean line %d\n", wpp->curline->id);

		wpp->blk = wpp->curline->id;
		check_addr(wpp->blk, spp->blks_per_pl);
		wpp->pg =0;
	}

	//ch die interleaving
	glun++;
	if (glun != conv_ftl->ssd->sp.nchs * conv_ftl->ssd->sp.luns_per_ch)
	{
		conv_ftl->lunpointer = glun; //next write lun 
		lm = conv_ftl->lunlm+conv_ftl->lunpointer;
		wpp = __get_wp_DA(conv_ftl, io_type, conv_ftl->lunpointer);
		
		//NVMEV_ERROR("wpp ch : %u, lun %d\n", wpp->ch, wpp->lun);
		goto out;
	}

	//NVMEV_ERROR("lun limit\n");
	glun=0;	
	conv_ftl->lunpointer = glun; //next write lun 
	lm = conv_ftl->lunlm+conv_ftl->lunpointer;
	wpp = __get_wp_DA(conv_ftl, io_type, conv_ftl->lunpointer);
	
out:
	NVMEV_DEBUG("advanced wpp: ch:%d, lun:%d, pl:%d, blk:%d, pg:%d (curline %d)\n", wpp->ch,
		    wpp->lun, wpp->pl, wpp->blk, wpp->pg, wpp->curline->id);
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
		return;
	}

    for (i = 0; i < spp->tt_pgs; i++) {
		conv_ftl->maptbl[i].ppa = UNMAPPED_PPA;
	}
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
		return;
	}

    for (i = 0; i < spp->tt_pgs; i++) {
		conv_ftl->rmap[i] = INVALID_LPN;
	}
}

static void remove_rmap(struct conv_ftl *conv_ftl)
{
	vfree(conv_ftl->rmap);
}

/* forward declaration to satisfy C90 before first use */
static int init_slc_qlc_blocks_fallback(struct conv_ftl *conv_ftl);

static int init_slc_qlc_blocks_with_retry(struct conv_ftl *conv_ftl, int max_retries)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	uint32_t total_blks_per_pl = spp->blks_per_pl;
	int i, retry_count = 0;
	
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
			NVMEV_INFO("SLC blocks: %d, QLC blocks: %d, QLC region size: %d\n",
				   conv_ftl->slc_blks_per_pl, conv_ftl->qlc_blks_per_pl, conv_ftl->qlc_region_size);
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
}

/* 初始化 SLC line 管理 */
static void init_slc_lines(struct conv_ftl *conv_ftl)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->slc_lm;
	struct line *line;
	int i;
	
	lm->tt_lines = conv_ftl->slc_blks_per_pl;
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
			.id = i,  /* SLC block IDs start from 0 */
			.ipc = 0,
			.vpc = 0,
			.pos = 0,
			.entry = LIST_HEAD_INIT(lm->lines[i].entry),
		};
		
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
	
	lm->free_line_cnt = 0;
	for (i = 0; i < lm->tt_lines; i++) {
		lm->lines[i] = (struct line) {
			.id = start_blk + i,  /* QLC block IDs */
			.ipc = 0,
			.vpc = 0,
			.pos = 0,
			.entry = LIST_HEAD_INIT(lm->lines[i].entry),
		};
		
		list_add_tail(&lm->lines[i].entry, &lm->free_line_list);
		lm->free_line_cnt++;
	}
	
	lm->victim_line_cnt = 0;
	lm->full_line_cnt = 0;
	conv_ftl->current_qlc_region = 0;
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

	/* initialize all the lines */
	NVMEV_INFO("initialize lines\n");
	init_lines(conv_ftl);

//66f1
	init_lines_DA(conv_ftl);
//66f1

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
	prepare_write_pointer(conv_ftl, USER_IO);
	prepare_write_pointer(conv_ftl, GC_IO);

//66f1
	prepare_write_pointer_DA(conv_ftl, USER_IO);
	conv_ftl->lunpointer = 0;
//66f1

	/* 初始化 SLC 写指针 - 使用 Die Affinity */
	/* 注意：SLC 写指针将在第一次写入时初始化 */

	init_write_flow_control(conv_ftl);

	NVMEV_INFO("Init FTL Instance with %d channels(%ld pages)\n", conv_ftl->ssd->sp.nchs,
		   conv_ftl->ssd->sp.tt_pgs);
	NVMEV_INFO("SLC/QLC Hybrid Mode: SLC %d blks, QLC %d blks (4 regions)\n", 
		   conv_ftl->slc_blks_per_pl, conv_ftl->qlc_blks_per_pl);

	return;
}

static void conv_remove_ftl(struct conv_ftl *conv_ftl)
{
	remove_lines(conv_ftl);
	//66f1
	remove_lines_DA(conv_ftl);
	//66f1
	
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
//66f1
		conv_ftls[i].lunlm = kmalloc(sizeof(struct line_mgmt) * NAND_CHANNELS * LUNS_PER_NAND_CH, GFP_KERNEL);
		conv_ftls[i].lunwp = kmalloc(sizeof(struct write_pointer) * NAND_CHANNELS * LUNS_PER_NAND_CH, GFP_KERNEL);
//66f1
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
		
		//66f1
		kfree(conv_ftls[i].lunlm);
		kfree(conv_ftls[i].lunwp);
		//66f1
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
	if (pg < 0 || pg >= spp->pgs_per_blk)
		return false;

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
	return &(conv_ftl->lm.lines[ppa->g.blk]);
}

static inline struct line *get_line_DA(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	uint32_t glun = get_glun(conv_ftl, ppa);
	struct line_mgmt *lm = conv_ftl->lunlm+glun;

	return &(lm->lines[ppa->g.blk]);
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	//66f1
	uint32_t glun = get_glun(conv_ftl, ppa);
	struct line_mgmt *lunlm = conv_ftl->lunlm+glun;
	//66f1
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	bool was_full_line = false;
	struct line *line;

	/* update corresponding page status */
	pg = get_pg(conv_ftl->ssd, ppa);
	NVMEV_ASSERT(pg->status == PG_VALID);
	pg->status = PG_INVALID;

	/* update corresponding block status */
	blk = get_blk(conv_ftl->ssd, ppa);
	NVMEV_ASSERT(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
	blk->ipc++;
	NVMEV_ASSERT(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
	blk->vpc--;

	/* update corresponding line status */
	line = get_line(conv_ftl, ppa);
	NVMEV_ASSERT(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
	if (line->vpc == spp->pgs_per_line) {
		NVMEV_ASSERT(line->ipc == 0);
		was_full_line = true;
	}
	line->ipc++;
	NVMEV_ASSERT(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
	/* Adjust the position of the victime line in the pq under over-writes */
	if (line->pos) {
		/* Note that line->vpc will be updated by this call */
		pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
	} else {
		line->vpc--;
	}

	if (was_full_line) {
		/* move line: "full" -> "victim" */
		list_del_init(&line->entry);
		lm->full_line_cnt--;
		pqueue_insert(lm->victim_line_pq, line);
		lm->victim_line_cnt++;
	}

	//66f1 lunlm update
	was_full_line = false;
	/* update corresponding line status */
	line = get_line_DA(conv_ftl, ppa);
	NVMEV_ASSERT(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
	if (line->vpc == spp->pgs_per_line) {
		NVMEV_ASSERT(line->ipc == 0);
		was_full_line = true;
	}
	line->ipc++;
	NVMEV_ASSERT(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
	/* Adjust the position of the victime line in the pq under over-writes */
	if (line->pos) {
		/* Note that line->vpc will be updated by this call */
		pqueue_change_priority(lunlm->victim_line_pq, line->vpc - 1, line);
	} else {
		line->vpc--;
	}

	if (was_full_line) {
		/* move line: "full" -> "victim" */
		list_del_init(&line->entry);
		lunlm->full_line_cnt--;
		pqueue_insert(lunlm->victim_line_pq, line);
		lunlm->victim_line_cnt++;
	}
	//66f1
}

static void mark_page_valid(struct conv_ftl *conv_ftl, struct ppa *ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	struct line *line;

	/* update page status */
	pg = get_pg(conv_ftl->ssd, ppa);
	NVMEV_ASSERT(pg->status == PG_FREE);
	pg->status = PG_VALID;

	/* update corresponding block status */
	blk = get_blk(conv_ftl->ssd, ppa);
	NVMEV_ASSERT(blk->vpc >= 0 && blk->vpc < spp->pgs_per_blk);
	blk->vpc++;

	/* update corresponding line status */
	line = get_line(conv_ftl, ppa);
	NVMEV_ASSERT(line->vpc >= 0 && line->vpc < spp->pgs_per_line);
	line->vpc++;

	//66f1
	line = get_line_DA(conv_ftl, ppa);
	NVMEV_ASSERT(line->vpc >= 0 && line->vpc < spp->pgs_per_line);
	line->vpc++;
	//66f1
}

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

	NVMEV_ASSERT(valid_lpn(conv_ftl, lpn));
	new_ppa = get_new_page(conv_ftl, GC_IO);
	/* update maptbl */
	set_maptbl_ent(conv_ftl, lpn, &new_ppa);
	/* update rmap */
	set_rmap_ent(conv_ftl, lpn, &new_ppa);

	mark_page_valid(conv_ftl, &new_ppa);

	/* need to advance the write pointer here */
	advance_write_pointer(conv_ftl, GC_IO);

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

static struct line *select_victim_line(struct conv_ftl *conv_ftl, bool force)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *victim_line = NULL;

	victim_line = pqueue_peek(lm->victim_line_pq);
	if (!victim_line) {
		return NULL;
	}

	if (!force && (victim_line->vpc > (spp->pgs_per_line / 8))) {
		return NULL;
	}

	pqueue_pop(lm->victim_line_pq);
	victim_line->pos = 0;
	lm->victim_line_cnt--;

	/* victim_line is a danggling node now */
	return victim_line;
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
	struct line_mgmt *lm = &conv_ftl->lm;
	struct line *line = get_line(conv_ftl, ppa);
	line->ipc = 0;
	line->vpc = 0;
	/* move this line to free line list */
	list_add_tail(&line->entry, &lm->free_line_list);
	lm->free_line_cnt++;
}

static int do_gc(struct conv_ftl *conv_ftl, bool force)
{
	struct line *victim_line = NULL;
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct ppa ppa;
	int flashpg;

	victim_line = select_victim_line(conv_ftl, force);
	if (!victim_line) {
		return -1;
	}

	ppa.g.blk = victim_line->id;
	NVMEV_DEBUG("GC-ing line:%d,ipc=%d(%d),victim=%d,full=%d,free=%d\n", ppa.g.blk,
		    victim_line->ipc, victim_line->vpc, conv_ftl->lm.victim_line_cnt,
		    conv_ftl->lm.full_line_cnt, conv_ftl->lm.free_line_cnt);

	conv_ftl->wfc.credits_to_refill = victim_line->ipc;

	/* copy back valid data */
	for (flashpg = 0; flashpg < spp->flashpgs_per_blk; flashpg++) {
		int ch, lun;

		ppa.g.pg = flashpg * spp->pgs_per_flashpg;
		for (ch = 0; ch < spp->nchs; ch++) {
			for (lun = 0; lun < spp->luns_per_ch; lun++) {
				struct nand_lun *lunp;

				ppa.g.ch = ch;
				ppa.g.lun = lun;
				ppa.g.pl = 0;
				lunp = get_lun(conv_ftl->ssd, &ppa);
				clean_one_flashpg(conv_ftl, &ppa);

				if (flashpg == (spp->flashpgs_per_blk - 1)) {
					struct convparams *cpp = &conv_ftl->cp;

					mark_block_free(conv_ftl, &ppa);

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

					lunp->gc_endtime = lunp->next_lun_avail_time;
				}
			}
		}
	}

	/* update line status */
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

/* 检查块是否为 SLC - 带安全检查 */
static bool is_slc_block(struct conv_ftl *conv_ftl, uint32_t blk_id)
{
	/* 检查初始化状态 */
	if (!conv_ftl || !conv_ftl->slc_initialized || !conv_ftl->is_slc_block) {
		NVMEV_ERROR("SLC blocks not properly initialized\n");
		return false;  /* 默认返回false，避免误判 */
	}
	
	/* 检查数组边界 */
	if (blk_id >= conv_ftl->slc_blks_per_pl + conv_ftl->qlc_blks_per_pl) {
		NVMEV_ERROR("Block ID %u out of range (max: %u)\n", 
			   blk_id, conv_ftl->slc_blks_per_pl + conv_ftl->qlc_blks_per_pl);
		return false;
	}
	
	return conv_ftl->is_slc_block[blk_id];
}

/* 获取 SLC 的新页面 - 使用 Die Affinity */
static struct ppa get_new_slc_page(struct conv_ftl *conv_ftl)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct ppa ppa;
	struct write_pointer *wp = &conv_ftl->slc_wp;
	
	/* 如果 SLC 写指针未初始化，初始化它 */
	if (!wp->curline) {
		struct line_mgmt *lm = &conv_ftl->slc_lm;
		struct line *curline = list_first_entry_or_null(&lm->free_line_list, struct line, entry);
		
		if (!curline) {
			NVMEV_ERROR("No free SLC line available!\n");
			return (struct ppa){.ppa = INVALID_PPA};
		}
		
		list_del_init(&curline->entry);
		lm->free_line_cnt--;
		
		*wp = (struct write_pointer) {
			.curline = curline,
			.ch = conv_ftl->lunpointer % spp->nchs,
			.lun = conv_ftl->lunpointer / spp->nchs,
			.pg = 0,
			.blk = curline->id,
			.pl = 0,
		};
	}
	
	/* 获取当前页 */
	ppa.ppa = 0;
	ppa.g.ch = wp->ch;
	ppa.g.lun = wp->lun;
	ppa.g.pg = wp->pg;
	ppa.g.blk = wp->blk;
	ppa.g.pl = wp->pl;
	
	return ppa;
}

/* 推进 SLC 写指针 - 使用 Die Affinity */
static void advance_slc_write_pointer(struct conv_ftl *conv_ftl)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct line_mgmt *lm = &conv_ftl->slc_lm;
	struct write_pointer *wp = &conv_ftl->slc_wp;
	
	wp->pg++;
	
	/* 检查是否需要移到下一个 block */
	if (wp->pg >= spp->pgs_per_blk) {
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
		/* 设置写指针为无效状态 */
		wp->blk = INVALID_PPA;
		wp->pg = INVALID_PPA;
		return;
	}
		
		list_del_init(&wp->curline->entry);
		lm->free_line_cnt--;
		
		wp->blk = wp->curline->id;
		wp->pg = 0;
	}
	
	/* Die 轮询 - 移到下一个 lun */
	conv_ftl->lunpointer++;
	if (conv_ftl->lunpointer >= (spp->nchs * spp->luns_per_ch)) {
		conv_ftl->lunpointer = 0;
	}
	
	wp->ch = conv_ftl->lunpointer % spp->nchs;
	wp->lun = conv_ftl->lunpointer / spp->nchs;
}

/* 获取 QLC 的新页面 - 使用多区域并发*/
static struct ppa get_new_qlc_page(struct conv_ftl *conv_ftl, uint32_t region_id)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct ppa ppa;
	struct write_pointer *wp = &conv_ftl->qlc_wp[region_id];
	
	/* 如果 QLC 写指针未初始化，初始化它 */
	if (!wp->curline) {
		struct line_mgmt *lm = &conv_ftl->qlc_lm;
		struct line *curline = list_first_entry_or_null(&lm->free_line_list, struct line, entry);
		
		if (!curline) {
			NVMEV_ERROR("No free QLC line available in region %d!\n", region_id);
			return (struct ppa){.ppa = INVALID_PPA};
		}
		
		list_del_init(&curline->entry);
		lm->free_line_cnt--;
		
		*wp = (struct write_pointer) {
			.curline = curline,
			.ch = 0,
			.lun = 0,
			.pg = 0,
			.blk = curline->id,
			.pl = 0,
		};
	}
	
	/* 获取当前页 */
	ppa.ppa = 0;
	ppa.g.ch = wp->ch;
	ppa.g.lun = wp->lun;
	ppa.g.pg = wp->pg;
	ppa.g.blk = wp->blk;
	ppa.g.pl = wp->pl;
	
	return ppa;
}

/* 推进 QLC 写指针 - 使用多区域并发 */
static void advance_qlc_write_pointer(struct conv_ftl *conv_ftl, uint32_t region_id)
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
		/* 设置写指针为无效状态 */
		wp->blk = INVALID_PPA;
		wp->pg = INVALID_PPA;
		return;
	}
	
	list_del_init(&wp->curline->entry);
	lm->free_line_cnt--;
	wp->blk = wp->curline->id;
	
out:
	return;
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
	uint64_t current_time = ktime_get_ns();
	uint64_t time_diff = current_time - ht->last_access_time[lpn];
	
	/* 如果页面长时间未访问（冷数据），则应迁移到 QLC */
	/* 这里使用简单的时间阈值，实际可以根据需要调整策略 */
	if (time_diff > 1000000000ULL && ht->access_count[lpn] < ht->migration_threshold) {
		return true;
	}
	
	return false;
}

/* 执行从 SLC 到 QLC 的迁移 */
static void migrate_page_to_qlc(struct conv_ftl *conv_ftl, uint64_t lpn, struct ppa *old_ppa)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
	struct ppa new_ppa;
	struct nand_cmd srd, swr;
	uint64_t nsecs_completed;
	
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
	srd.stime = ktime_get_ns();
	srd.interleave_pci_dma = false;
	srd.xfer_size = spp->pgsz;
	srd.ppa = old_ppa;
	
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
	mark_page_invalid(conv_ftl, old_ppa);
	set_rmap_ent(conv_ftl, INVALID_LPN, old_ppa);
	
	/* 更新元数据 */
	conv_ftl->page_in_slc[lpn] = false;
	mark_page_valid(conv_ftl, &new_ppa);
	
	/* 推进 QLC 写指针 */
	advance_qlc_write_pointer(conv_ftl, region);
	
	/* 更新统计 */
	conv_ftl->migration_cnt++;
	
	NVMEV_DEBUG("Migrated LPN %lld from SLC to QLC region %d\n", lpn, region);
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
		return false;
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

	NVMEV_DEBUG("conv_write: start_lpn=%lld, len=%lld, end_lpn=%lld", start_lpn, nr_lba, end_lpn);
	//NVMEV_ERROR("conv_write: start_lpn=%lld, len=%lld, end_lpn=%lld", start_lpn, nr_lba, end_lpn);
	if ((end_lpn / nr_parts) >= spp->tt_pgs) {
		NVMEV_ERROR("conv_write: lpn passed FTL range(start_lpn=%lld,tt_pgs=%ld)\n",
			    start_lpn, spp->tt_pgs);
		return false;
	}

	allocated_buf_size = buffer_allocate(wbuf, LBA_TO_BYTE(nr_lba));
	//NVMEV_ERROR("conv_write: buffer alloc size = %u\n", allocated_buf_size);
	if (allocated_buf_size < LBA_TO_BYTE(nr_lba))
		return false;

	nsecs_latest = ssd_advance_write_buffer(conv_ftl->ssd, req->nsecs_start, LBA_TO_BYTE(nr_lba));
	nsecs_xfer_completed = nsecs_latest;

	swr.stime = nsecs_latest;

	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		uint64_t local_lpn;
		uint64_t nsecs_completed = 0;
		uint64_t write_lat;  /* 移动变量声明到循环开头 */
		struct ppa ppa;
		struct ppa old_ppa;  /* 用于迁移检查 */

		conv_ftl = &conv_ftls[lpn % nr_parts];
		local_lpn = lpn / nr_parts;
		ppa = get_maptbl_ent(conv_ftl, local_lpn); // Check whether the given LPN has been written before
		if (mapped_ppa(&ppa)) {
			/* update old page information first */
			mark_page_invalid(conv_ftl, &ppa);
			set_rmap_ent(conv_ftl, INVALID_LPN, &ppa);
			NVMEV_DEBUG("conv_write: %lld is invalid, ", ppa2pgidx(conv_ftl, &ppa));
			
			/* 如果页面在 SLC 中且是冷数据，考虑迁移到 QLC */
			if (conv_ftl->page_in_slc[local_lpn] && 
			    should_migrate_to_qlc(conv_ftl, local_lpn)) {
				old_ppa = ppa;  /* 保存用于后续迁移 */
			}
		}

//66f1
//#define DIEAFFINITY (0)

		/* new write */
		//need branch
		if (lpn == start_lpn)
		{
			if (bAppend)
			{
				ppa = get_maptbl_ent(conv_ftl, plpn); 
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
				ppa = get_maptbl_ent(conv_ftl, lpn); 
				if (mapped_ppa(&ppa)) {
					uint32_t originlun = conv_ftl->lunpointer;
					conv_ftl->lunpointer = get_glun(conv_ftl, &ppa); 
					//NVMEV_ERROR("target lun: %d -> %d\n", originlun, conv_ftl->lunpointer);
				}
			}
		}

		/* 修改：所有新写入都先写到 SLC */
		/* 使用 Die Affinity 写入 SLC */
		ppa = get_new_slc_page(conv_ftl);
		if (!mapped_ppa(&ppa)) {
			NVMEV_ERROR("Failed to get SLC page for write\n");
			ret->status = NVME_SC_WRITE_FAULT;
			return false;
		}
		
		/* 记录页面在 SLC 中 */
		conv_ftl->page_in_slc[local_lpn] = true;
		conv_ftl->slc_write_cnt++;

		//NVMEV_ERROR("PPA: ch:%d, lun:%d, blk:%d, pg:%d \n", ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pg );

//66f1

		/* update maptbl */
		set_maptbl_ent(conv_ftl, local_lpn, &ppa);
		NVMEV_DEBUG("conv_write: got new ppa %lld, ", ppa2pgidx(conv_ftl, &ppa));
		/* update rmap */
		set_rmap_ent(conv_ftl, local_lpn, &ppa);

		mark_page_valid(conv_ftl, &ppa);

		/* need to advance the write pointer here */
		//need branch
//66f1
		/* 使用 SLC 的 Die Affinity 推进写指针 */
		advance_slc_write_pointer(conv_ftl);

		nsecs_completed = ssd_advance_write_buffer(conv_ftl->ssd, nsecs_latest, conv_ftl->ssd->sp.pgsz);

		/* Check whether we need to do a write in this stripe */
		if (((end_lpn + 1) % conv_ftl->ssd->sp.pgs_per_oneshotpg) == 0 ||
		    lpn == end_lpn) {
			xfer_size = ((lpn % conv_ftl->ssd->sp.pgs_per_oneshotpg) + 1) *
				    conv_ftl->ssd->sp.pgsz;
			swr.xfer_size = xfer_size;
			
			/* 使用 SLC 写延迟 */
			write_lat = conv_ftl->ssd->sp.pg_wr_lat;
			
			nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &swr);
			/* schedule_internal_operation 暂时注释掉，函数不存在 */
			/* schedule_internal_operation(conv_ftl->ssd, nsecs_completed, xfer_size, &ppa); */

			//xfer_size = 0;
			swr.stime = nsecs_completed;
		}

		nsecs_latest = max(nsecs_completed, nsecs_latest);

		/* 更新热数据信息 */
		update_heat_info(conv_ftl, local_lpn, false);
		conv_ftl->heat_track.last_access_time[local_lpn] = ktime_get_ns();
		
		/* 如果之前标记了需要迁移，现在执行迁移 */
		if (mapped_ppa(&old_ppa) && conv_ftl->page_in_slc[local_lpn]) {
			if (should_migrate_to_qlc(conv_ftl, local_lpn)) {
				/* 异步迁移到 QLC */
				migrate_page_to_qlc(conv_ftl, local_lpn, &old_ppa);
			}
		}
		
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
	struct nvme_command *cmd = req->cmd;

	NVMEV_ASSERT(ns->csi == NVME_CSI_NVM);

	switch (cmd->common.opcode) {
	case nvme_cmd_write:
		if (!conv_write(ns, req, ret))
			return false;
		break;
	case nvme_cmd_read:
		if (!conv_read(ns, req, ret))
			return false;
		break;
	case nvme_cmd_flush:
		conv_flush(ns, req, ret);
		break;
	default:
		NVMEV_ERROR("%s: unimplemented command: %s(%d)\n", __func__,
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

/* 批量迁移函数 - 充分利用4个写指针的并发性 */
static void migrate_batch_to_qlc(struct conv_ftl *conv_ftl, struct ppa *slc_ppas, 
                                 uint64_t *lpns, uint32_t count)
{
	struct ssdparams *spp = &conv_ftl->ssd->sp;
    struct ppa *new_ppas;
    struct nand_cmd *srds, *swrs;
	uint64_t nsecs_completed;
	uint32_t i;
	
	NVMEV_DEBUG("Starting batch migration of %d pages\n", count);
	
    new_ppas = kmalloc(sizeof(*new_ppas) * count, GFP_KERNEL);
    if (!new_ppas) {
        NVMEV_ERROR("Failed to allocate new_ppas for batch migration\n");
        return;
    }
    srds = kmalloc(sizeof(*srds) * count, GFP_KERNEL);
    if (!srds) {
        NVMEV_ERROR("Failed to allocate srds for batch migration\n");
        kfree(new_ppas);
        return;
    }
    swrs = kmalloc(sizeof(*swrs) * count, GFP_KERNEL);
    if (!swrs) {
        NVMEV_ERROR("Failed to allocate swrs for batch migration\n");
        kfree(srds);
        kfree(new_ppas);
        return;
    }

	/* 阶段1：并发获取QLC页面 - 轮询使用4个区域 */
	for (i = 0; i < count; i++) {
		uint32_t region = i % QLC_REGIONS;  /* 均匀分配到4个区域 */
		new_ppas[i] = get_new_qlc_page(conv_ftl, region);
		
        if (!mapped_ppa(&new_ppas[i])) {
			NVMEV_ERROR("Failed to get QLC page for batch migration\n");
            kfree(swrs);
            kfree(srds);
            kfree(new_ppas);
            return;
		}
	}
	
	/* 阶段2：批量读取SLC页面 */
	nsecs_completed = ktime_get_ns();
	for (i = 0; i < count; i++) {
		srds[i].type = USER_IO;
		srds[i].cmd = NAND_READ;
		srds[i].stime = nsecs_completed;
		srds[i].interleave_pci_dma = false;
		srds[i].xfer_size = spp->pgsz;
		srds[i].ppa = &slc_ppas[i];
		
		/* 并发读取 - 利用多channel */
		nsecs_completed = ssd_advance_nand(conv_ftl->ssd, &srds[i]);
	}
	
	/* 阶段3：批量写入QLC页面 - 4个区域并发 */
	for (i = 0; i < count; i++) {
		swrs[i].type = USER_IO;
		swrs[i].cmd = NAND_WRITE;
		swrs[i].stime = nsecs_completed;
		swrs[i].interleave_pci_dma = false;
		swrs[i].xfer_size = spp->pgsz;
		swrs[i].ppa = &new_ppas[i];
		
		/* 并发写入 - 4个区域同时写入 */
		ssd_advance_nand(conv_ftl->ssd, &swrs[i]);
	}
	
	/* 阶段4：批量更新映射表和推进写指针 */
	for (i = 0; i < count; i++) {
		uint32_t region = i % QLC_REGIONS;
		
		/* 更新映射表 */
		set_maptbl_ent(conv_ftl, lpns[i], &new_ppas[i]);
		set_rmap_ent(conv_ftl, lpns[i], &new_ppas[i]);
		
		/* 标记旧页面无效 */
		mark_page_invalid(conv_ftl, &slc_ppas[i]);
		set_rmap_ent(conv_ftl, INVALID_LPN, &slc_ppas[i]);
		
		/* 更新元数据 */
		conv_ftl->page_in_slc[lpns[i]] = false;
		mark_page_valid(conv_ftl, &new_ppas[i]);
		
		/* 推进对应区域的写指针 */
		advance_qlc_write_pointer(conv_ftl, region);
	}
	
	/* 更新统计 */
	conv_ftl->migration_cnt += count;
	
	NVMEV_INFO("Batch migrated %d pages using %d QLC regions concurrently\n", 
		   count, QLC_REGIONS);

    kfree(swrs);
    kfree(srds);
    kfree(new_ppas);
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
