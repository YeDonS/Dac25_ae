// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ktime.h>
#include <linux/sched/clock.h>
#include <linux/math64.h>
#include <linux/moduleparam.h>

#include "nvmev.h"
#include "ssd.h"

static int gc_nand_timing = 0;
module_param(gc_nand_timing, int, 0664);

#define DIE_STAT_MAX 64
static atomic_long_t die_read_total[DIE_STAT_MAX];
static atomic_long_t die_read_conflict[DIE_STAT_MAX];
static atomic_long_t die_read_wait_ns[DIE_STAT_MAX];
static atomic64_t bg_nand_busy_time_ns;
static atomic64_t bg_nand_read_time_ns;
static atomic64_t bg_nand_write_time_ns;
static atomic64_t bg_nand_erase_time_ns;
static atomic64_t bg_nand_read_ops;
static atomic64_t bg_nand_write_ops;
static atomic64_t bg_nand_erase_ops;
static atomic64_t read_priority_bypass_ops;
static atomic64_t read_priority_bypass_ns;
static atomic64_t read_priority_ch_bypass_ops;
static atomic64_t read_priority_ch_bypass_ns;
static atomic64_t read_priority_pcie_bypass_ops;
static atomic64_t read_priority_pcie_bypass_ns;
static atomic64_t lp_host_write_ops;
static atomic64_t lp_host_write_time_ns;

static int die_stats_reset = 0;
static int die_stats_set_reset(const char *val, const struct kernel_param *kp) {
	int i;
	for (i = 0; i < DIE_STAT_MAX; i++) {
		atomic_long_set(&die_read_total[i], 0);
		atomic_long_set(&die_read_conflict[i], 0);
		atomic_long_set(&die_read_wait_ns[i], 0);
	}
	atomic64_set(&bg_nand_busy_time_ns, 0);
	atomic64_set(&bg_nand_read_time_ns, 0);
	atomic64_set(&bg_nand_write_time_ns, 0);
	atomic64_set(&bg_nand_erase_time_ns, 0);
	atomic64_set(&bg_nand_read_ops, 0);
	atomic64_set(&bg_nand_write_ops, 0);
	atomic64_set(&bg_nand_erase_ops, 0);
	atomic64_set(&read_priority_bypass_ops, 0);
	atomic64_set(&read_priority_bypass_ns, 0);
	atomic64_set(&read_priority_ch_bypass_ops, 0);
	atomic64_set(&read_priority_ch_bypass_ns, 0);
	atomic64_set(&read_priority_pcie_bypass_ops, 0);
	atomic64_set(&read_priority_pcie_bypass_ns, 0);
	atomic64_set(&lp_host_write_ops, 0);
	atomic64_set(&lp_host_write_time_ns, 0);
	return 0;
}
static const struct kernel_param_ops die_stats_reset_ops = {
	.set = die_stats_set_reset,
	.get = param_get_int,
};
module_param_cb(die_stats_reset, &die_stats_reset_ops, &die_stats_reset, 0664);

static char die_stats_buf[4096];
static int die_stats_get(char *buf, const struct kernel_param *kp) {
	int i, len = 0;
	for (i = 0; i < DIE_STAT_MAX; i++) {
		long total = atomic_long_read(&die_read_total[i]);
		if (total == 0) continue;
		len += scnprintf(buf + len, PAGE_SIZE - len,
			"die%d reads=%ld conflicts=%ld wait_ns=%ld\n",
			i, total,
			atomic_long_read(&die_read_conflict[i]),
			atomic_long_read(&die_read_wait_ns[i]));
	}
	return len;
}
static const struct kernel_param_ops die_stats_show_ops = {
	.set = die_stats_set_reset,
	.get = die_stats_get,
};
module_param_cb(die_stats, &die_stats_show_ops, &die_stats_buf, 0664);

static char bg_nand_stats_buf[256];
static int bg_nand_stats_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE,
			 "busy_ns=%lld read_ns=%lld write_ns=%lld erase_ns=%lld read_ops=%lld write_ops=%lld erase_ops=%lld read_prio_bypass_ops=%lld read_prio_bypass_ns=%lld read_prio_ch_bypass_ops=%lld read_prio_ch_bypass_ns=%lld read_prio_pcie_bypass_ops=%lld read_prio_pcie_bypass_ns=%lld lp_host_write_ops=%lld lp_host_write_ns=%lld\n",
			 atomic64_read(&bg_nand_busy_time_ns),
			 atomic64_read(&bg_nand_read_time_ns),
			 atomic64_read(&bg_nand_write_time_ns),
			 atomic64_read(&bg_nand_erase_time_ns),
			 atomic64_read(&bg_nand_read_ops),
			 atomic64_read(&bg_nand_write_ops),
			 atomic64_read(&bg_nand_erase_ops),
			 atomic64_read(&read_priority_bypass_ops),
			 atomic64_read(&read_priority_bypass_ns),
			 atomic64_read(&read_priority_ch_bypass_ops),
			 atomic64_read(&read_priority_ch_bypass_ns),
			 atomic64_read(&read_priority_pcie_bypass_ops),
			 atomic64_read(&read_priority_pcie_bypass_ns),
			 atomic64_read(&lp_host_write_ops),
			 atomic64_read(&lp_host_write_time_ns));
}
static const struct kernel_param_ops bg_nand_stats_ops = {
	.set = die_stats_set_reset,
	.get = bg_nand_stats_get,
};
module_param_cb(bg_nand_stats, &bg_nand_stats_ops, &bg_nand_stats_buf, 0664);

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

static int fast_profile_lat(int base)
{
	if (base <= 0)
		return base;
	return 1;
}

void ssd_capture_latency_defaults(struct ssdparams *spp)
{
	int i;

	if (!spp)
		return;

	for (i = 0; i < MAX_CELL_TYPES; i++) {
		spp->base_pg_4kb_rd_lat[i] = spp->pg_4kb_rd_lat[i];
		spp->base_pg_rd_lat[i] = spp->pg_rd_lat[i];
	}

	for (i = 0; i < ARRAY_SIZE(spp->qlc_pg_4kb_rd_lat); i++) {
		spp->base_qlc_pg_4kb_rd_lat[i] = spp->qlc_pg_4kb_rd_lat[i];
		spp->base_qlc_pg_rd_lat[i] = spp->qlc_pg_rd_lat[i];
	}

	spp->base_pg_wr_lat = spp->pg_wr_lat;
	spp->base_blk_er_lat = spp->blk_er_lat;
	spp->base_qlc_pg_wr_lat = spp->qlc_pg_wr_lat;
	spp->base_qlc_blk_er_lat = spp->qlc_blk_er_lat;
	spp->base_migration_lat = spp->migration_lat;
	spp->base_fw_4kb_rd_lat = spp->fw_4kb_rd_lat;
	spp->base_fw_rd_lat = spp->fw_rd_lat;
	spp->base_fw_wbuf_lat0 = spp->fw_wbuf_lat0;
	spp->base_fw_wbuf_lat1 = spp->fw_wbuf_lat1;
	spp->base_fw_ch_xfer_lat = spp->fw_ch_xfer_lat;
	spp->latency_profile = SSD_LATENCY_PROFILE_NORMAL;
}

const char *ssd_latency_profile_name(enum ssd_latency_profile profile)
{
	switch (profile) {
	case SSD_LATENCY_PROFILE_INIT_FAST:
		return "init-fast";
	case SSD_LATENCY_PROFILE_NORMAL:
	default:
		return "normal";
	}
}

static void ssd_apply_latency_profile(struct ssdparams *spp, enum ssd_latency_profile profile)
{
	int i;

	if (!spp)
		return;

	switch (profile) {
	case SSD_LATENCY_PROFILE_INIT_FAST:
		for (i = 0; i < MAX_CELL_TYPES; i++) {
			spp->pg_4kb_rd_lat[i] = fast_profile_lat(spp->base_pg_4kb_rd_lat[i]);
			spp->pg_rd_lat[i] = fast_profile_lat(spp->base_pg_rd_lat[i]);
		}
		for (i = 0; i < ARRAY_SIZE(spp->qlc_pg_4kb_rd_lat); i++) {
			spp->qlc_pg_4kb_rd_lat[i] =
				fast_profile_lat(spp->base_qlc_pg_4kb_rd_lat[i]);
			spp->qlc_pg_rd_lat[i] = fast_profile_lat(spp->base_qlc_pg_rd_lat[i]);
		}
		spp->pg_wr_lat = fast_profile_lat(spp->base_pg_wr_lat);
		spp->blk_er_lat = fast_profile_lat(spp->base_blk_er_lat);
		spp->qlc_pg_wr_lat = fast_profile_lat(spp->base_qlc_pg_wr_lat);
		spp->qlc_blk_er_lat = fast_profile_lat(spp->base_qlc_blk_er_lat);
		spp->migration_lat = fast_profile_lat(spp->base_migration_lat);
		spp->fw_4kb_rd_lat = fast_profile_lat(spp->base_fw_4kb_rd_lat);
		spp->fw_rd_lat = fast_profile_lat(spp->base_fw_rd_lat);
		spp->fw_wbuf_lat0 = fast_profile_lat(spp->base_fw_wbuf_lat0);
		spp->fw_wbuf_lat1 = fast_profile_lat(spp->base_fw_wbuf_lat1);
		spp->fw_ch_xfer_lat = fast_profile_lat(spp->base_fw_ch_xfer_lat);
		break;
	case SSD_LATENCY_PROFILE_NORMAL:
	default:
		for (i = 0; i < MAX_CELL_TYPES; i++) {
			spp->pg_4kb_rd_lat[i] = spp->base_pg_4kb_rd_lat[i];
			spp->pg_rd_lat[i] = spp->base_pg_rd_lat[i];
		}
		for (i = 0; i < ARRAY_SIZE(spp->qlc_pg_4kb_rd_lat); i++) {
			spp->qlc_pg_4kb_rd_lat[i] = spp->base_qlc_pg_4kb_rd_lat[i];
			spp->qlc_pg_rd_lat[i] = spp->base_qlc_pg_rd_lat[i];
		}
		spp->pg_wr_lat = spp->base_pg_wr_lat;
		spp->blk_er_lat = spp->base_blk_er_lat;
		spp->qlc_pg_wr_lat = spp->base_qlc_pg_wr_lat;
		spp->qlc_blk_er_lat = spp->base_qlc_blk_er_lat;
		spp->migration_lat = spp->base_migration_lat;
		spp->fw_4kb_rd_lat = spp->base_fw_4kb_rd_lat;
		spp->fw_rd_lat = spp->base_fw_rd_lat;
		spp->fw_wbuf_lat0 = spp->base_fw_wbuf_lat0;
		spp->fw_wbuf_lat1 = spp->base_fw_wbuf_lat1;
		spp->fw_ch_xfer_lat = spp->base_fw_ch_xfer_lat;
		break;
	}

	spp->latency_profile = profile;
}

void ssd_set_latency_profile(struct ssd *ssd, enum ssd_latency_profile profile)
{
	if (!ssd)
		return;

	ssd_apply_latency_profile(&ssd->sp, profile);
}

uint64_t __get_ioclock(struct ssd *ssd)
{
	return cpu_clock(ssd->cpu_nr_dispatcher);
}

/* __get_ioclock 函数已移至 ssd.h */

void buffer_init(struct buffer *buf, size_t size)
{
	spin_lock_init(&buf->lock);
	buf->size = size;
	buf->remaining = size;
}

uint32_t buffer_allocate(struct buffer *buf, size_t size)
{
	while (!spin_trylock(&buf->lock)) {
		cpu_relax();
	}

	if (buf->remaining < size) {
		size = 0;
	}

	buf->remaining -= size;

	spin_unlock(&buf->lock);
	return size;
}

bool buffer_release(struct buffer *buf, size_t size)
{
	while (!spin_trylock(&buf->lock))
		;
	{
		size_t headroom = buf->size - buf->remaining;
		if (size > headroom) {
			NVMEV_ERROR("buffer_release: request %zu exceeds headroom %zu\n",
				   size, headroom);
			size = headroom;
		}
		buf->remaining += size;
	}
	spin_unlock(&buf->lock);

	return true;
}

void buffer_refill(struct buffer *buf)
{
	while (!spin_trylock(&buf->lock))
		;
	buf->remaining = buf->size;
	spin_unlock(&buf->lock);
}

static void check_params(struct ssdparams *spp)
{
	/*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

	//ftl_assert(is_power_of_2(spp->luns_per_ch));
	//ftl_assert(is_power_of_2(spp->nchs));
}

void ssd_init_params(struct ssdparams *spp, uint64_t capacity, uint32_t nparts)
{
	uint64_t blk_size, total_size;

	spp->secsz = 512;
	spp->secs_per_pg = 8;
	spp->pgsz = spp->secsz * spp->secs_per_pg;

	spp->nchs = NAND_CHANNELS;
	spp->pls_per_lun = PLNS_PER_LUN;
	spp->luns_per_ch = LUNS_PER_NAND_CH;
	spp->cell_mode = CELL_MODE;

	/* partitioning SSD by dividing channel*/
	NVMEV_ASSERT((spp->nchs % nparts) == 0);
	spp->nchs /= nparts;
	capacity /= nparts;

	if (BLKS_PER_PLN > 0) {
		/* flashpgs_per_blk depends on capacity */
		spp->blks_per_pl = BLKS_PER_PLN;
		blk_size = DIV_ROUND_UP(capacity, spp->blks_per_pl * spp->pls_per_lun *
							  spp->luns_per_ch * spp->nchs);
	} else {
		NVMEV_ASSERT(BLK_SIZE > 0);
		blk_size = BLK_SIZE;
		{
			uint64_t total_planes =
				(uint64_t)spp->pls_per_lun * spp->luns_per_ch * spp->nchs;
			uint64_t weighted_num =
				(uint64_t)QLC_BLOCK_CAPACITY_FACTOR * SLC_LINE_RATIO_NUM;
			uint64_t weighted_den = (uint64_t)SLC_BLOCK_CAPACITY_FACTOR *
						       QLC_LINE_RATIO_NUM +
					       weighted_num;
			uint64_t avg_cap_factor_num =
				weighted_num * SLC_BLOCK_CAPACITY_FACTOR +
				(weighted_den - weighted_num) * QLC_BLOCK_CAPACITY_FACTOR;
			uint64_t denom = (uint64_t)blk_size * total_planes * avg_cap_factor_num;
			uint64_t numer = capacity * weighted_den;

			NVMEV_ASSERT(total_planes);
			spp->blks_per_pl = DIV_ROUND_UP(numer, denom);
			if (!spp->blks_per_pl)
				spp->blks_per_pl = 1;
		}
	}

	NVMEV_ASSERT((ONESHOT_PAGE_SIZE % spp->pgsz) == 0 && (FLASH_PAGE_SIZE % spp->pgsz) == 0);
	NVMEV_ASSERT((ONESHOT_PAGE_SIZE % FLASH_PAGE_SIZE) == 0);

	spp->pgs_per_oneshotpg = ONESHOT_PAGE_SIZE / (spp->pgsz);
	spp->oneshotpgs_per_blk = DIV_ROUND_UP(blk_size, ONESHOT_PAGE_SIZE);

	spp->pgs_per_flashpg = FLASH_PAGE_SIZE / (spp->pgsz);
	spp->flashpgs_per_blk = (ONESHOT_PAGE_SIZE / FLASH_PAGE_SIZE) * spp->oneshotpgs_per_blk;

	spp->slc_pgs_per_blk = spp->pgs_per_oneshotpg * spp->oneshotpgs_per_blk;
	spp->pgs_per_blk = spp->slc_pgs_per_blk;
	spp->qlc_pgs_per_blk = spp->slc_pgs_per_blk * QLC_PAGE_PATTERN;

	compute_line_distribution(spp->blks_per_pl, &spp->slc_blks_per_pl,
				  &spp->qlc_blks_per_pl);

	spp->write_unit_size = WRITE_UNIT_SIZE;

	spp->pg_4kb_rd_lat[CELL_TYPE_LSB] = NAND_4KB_READ_LATENCY_LSB;
	spp->pg_4kb_rd_lat[CELL_TYPE_MSB] = NAND_4KB_READ_LATENCY_MSB;
	spp->pg_4kb_rd_lat[CELL_TYPE_CSB] = NAND_4KB_READ_LATENCY_CSB;
	spp->pg_rd_lat[CELL_TYPE_LSB] = NAND_READ_LATENCY_LSB;
	spp->pg_rd_lat[CELL_TYPE_MSB] = NAND_READ_LATENCY_MSB;
	spp->pg_rd_lat[CELL_TYPE_CSB] = NAND_READ_LATENCY_CSB;
	spp->pg_wr_lat = NAND_PROG_LATENCY;
	spp->blk_er_lat = NAND_ERASE_LATENCY;
	spp->max_ch_xfer_size = MAX_CH_XFER_SIZE;

	/* QLC 延迟参数初始化 - 基于图片参考，QLC 比 SLC 慢 */
	/* QLC 读延迟：约为 SLC 的 1.5-4.5 倍 (TOP:1.5x, UPPER:2.5x, MIDDLE:3.5x, LOWER:4.5x) */
	spp->qlc_pg_4kb_rd_lat[0] = QLC_4KB_READ_LATENCY_TOP;
	spp->qlc_pg_4kb_rd_lat[1] = QLC_4KB_READ_LATENCY_UPPER;
	spp->qlc_pg_4kb_rd_lat[2] = QLC_4KB_READ_LATENCY_MIDDLE;
	spp->qlc_pg_4kb_rd_lat[3] = QLC_4KB_READ_LATENCY_LOWER;
	
	spp->qlc_pg_rd_lat[0] = QLC_READ_LATENCY_TOP;
	spp->qlc_pg_rd_lat[1] = QLC_READ_LATENCY_UPPER;
	spp->qlc_pg_rd_lat[2] = QLC_READ_LATENCY_MIDDLE;
	spp->qlc_pg_rd_lat[3] = QLC_READ_LATENCY_LOWER;
	
	/* QLC 写延迟：约为 SLC 的 4 倍 */
	spp->qlc_pg_wr_lat = QLC_PROG_LATENCY;
	
	/* QLC 擦除延迟：约为 SLC 的 3 倍 */
	spp->qlc_blk_er_lat = QLC_ERASE_LATENCY;
	
	/* 迁移延迟：读取 SLC + 写入 QLC */
	spp->migration_lat = MIGRATION_LATENCY;

	spp->fw_4kb_rd_lat = FW_4KB_READ_LATENCY;
	spp->fw_rd_lat = FW_READ_LATENCY;
	spp->fw_ch_xfer_lat = FW_CH_XFER_LATENCY;
	spp->fw_wbuf_lat0 = FW_WBUF_LATENCY0;
	spp->fw_wbuf_lat1 = FW_WBUF_LATENCY1;
	ssd_capture_latency_defaults(spp);

	spp->ch_bandwidth = NAND_CHANNEL_BANDWIDTH;
	spp->pcie_bandwidth = PCIE_BANDWIDTH;

	spp->write_buffer_size = GLOBAL_WB_SIZE;
	spp->write_early_completion = WRITE_EARLY_COMPLETION;

	/* calculated values */
	spp->secs_per_blk = spp->secs_per_pg * spp->slc_pgs_per_blk;
	{
		uint64_t slc_pages = (uint64_t)spp->slc_blks_per_pl * spp->slc_pgs_per_blk;
		uint64_t qlc_pages = (uint64_t)spp->qlc_blks_per_pl * spp->qlc_pgs_per_blk;
		spp->pgs_per_pl = slc_pages + qlc_pages;
	}
	spp->secs_per_pl = spp->pgs_per_pl * spp->secs_per_pg;
	spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
	spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
	spp->tt_secs = spp->secs_per_ch * spp->nchs;

	spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
	spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
	spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

	spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
	spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
	spp->tt_blks = spp->blks_per_ch * spp->nchs;

	spp->pls_per_ch = spp->pls_per_lun * spp->luns_per_ch;
	spp->tt_pls = spp->pls_per_ch * spp->nchs;

	spp->tt_luns = spp->luns_per_ch * spp->nchs;

	/* line is special, put it at the end */
	spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
	spp->pgs_per_line = spp->blks_per_line * spp->slc_pgs_per_blk;
	spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
	spp->tt_lines = spp->blks_per_lun;
	/* TODO: to fix under multiplanes */ // lun size is super-block(line) size
	
	//66f1 die line option
	spp->blks_per_lun_line = spp->pls_per_lun;
	spp->pgs_per_lun_line = spp->blks_per_lun_line * spp->pgs_per_blk;
	spp->secs_per_lun_line = spp->pgs_per_lun_line * spp->secs_per_pg;
	spp->tt_lun_lines = spp->blks_per_lun_line;
	//66f1


	check_params(spp);

	total_size = (unsigned long long)spp->tt_pgs * spp->secsz * spp->secs_per_pg;
	blk_size = spp->slc_pgs_per_blk * spp->secsz * spp->secs_per_pg;
	NVMEV_INFO(
		"Total Capacity(GiB,MiB)=%llu,%llu chs=%u luns=%lu lines=%lu blk-size(MiB,KiB)=%u,%u line-size(MiB,KiB)=%lu,%lu",
		BYTE_TO_GB(total_size), BYTE_TO_MB(total_size), spp->nchs, spp->tt_luns,
		spp->tt_lines, BYTE_TO_MB(spp->slc_pgs_per_blk * spp->pgsz),
		BYTE_TO_KB(spp->slc_pgs_per_blk * spp->pgsz), BYTE_TO_MB(spp->pgs_per_line * spp->pgsz),
		BYTE_TO_KB(spp->pgs_per_line * spp->pgsz));
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
	int i;
	pg->nsecs = spp->secs_per_pg;
	pg->sec = kmalloc(sizeof(nand_sec_status_t) * pg->nsecs, GFP_KERNEL);
	if (!pg->sec) {
		NVMEV_ERROR("Failed to allocate page sectors memory\n");
		return;
	}
	for (i = 0; i < pg->nsecs; i++) {
		pg->sec[i] = SEC_FREE;
	}
	pg->status = PG_FREE;
	pg->qlc_latency_zone = 0;
	pg->oob_prev_lpn = INVALID_LPN;
}

static void ssd_remove_nand_page(struct nand_page *pg)
{
	kfree(pg->sec);
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp, bool qlc_blk)
{
	int i;
	uint32_t slc_pages = spp->slc_pgs_per_blk;
	uint32_t qlc_pages = spp->qlc_pgs_per_blk;

	blk->is_qlc = qlc_blk;
	blk->npgs = qlc_blk ? qlc_pages : slc_pages;
	blk->pg = kmalloc(sizeof(struct nand_page) * blk->npgs, GFP_KERNEL);
	if (!blk->pg) {
		NVMEV_ERROR("Failed to allocate block pages memory\n");
		return;
	}
	for (i = 0; i < blk->npgs; i++) {
		ssd_init_nand_page(&blk->pg[i], spp);
	}
	blk->ipc = 0;
	blk->vpc = 0;
	blk->erase_cnt = 0;
	blk->wp = 0;
}

static void ssd_remove_nand_blk(struct nand_block *blk)
{
	int i;

	for (i = 0; i < blk->npgs; i++)
		ssd_remove_nand_page(&blk->pg[i]);

	kfree(blk->pg);
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
	int i;
	pl->nblks = spp->blks_per_pl;
	pl->blk = kmalloc(sizeof(struct nand_block) * pl->nblks, GFP_KERNEL);
	if (!pl->blk) {
		NVMEV_ERROR("Failed to allocate plane blocks memory\n");
		return;
	}
	for (i = 0; i < pl->nblks; i++) {
		bool is_qlc = (spp->slc_blks_per_pl && (uint32_t)i >= spp->slc_blks_per_pl);
		ssd_init_nand_blk(&pl->blk[i], spp, is_qlc);
	}
}

static void ssd_remove_nand_plane(struct nand_plane *pl)
{
	int i;

	for (i = 0; i < pl->nblks; i++)
		ssd_remove_nand_blk(&pl->blk[i]);

	kfree(pl->blk);
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
	int i;
	lun->npls = spp->pls_per_lun;
	lun->pl = kmalloc(sizeof(struct nand_plane) * lun->npls, GFP_KERNEL);
	if (!lun->pl) {
		NVMEV_ERROR("Failed to allocate lun planes memory\n");
		return;
	}
	for (i = 0; i < lun->npls; i++) {
		ssd_init_nand_plane(&lun->pl[i], spp);
	}
	lun->next_lun_avail_time = 0;
	lun->hp_next_lun_avail_time = 0;
	lun->lp_next_lun_avail_time = 0;
	lun->busy = false;
}

static void ssd_remove_nand_lun(struct nand_lun *lun)
{
	int i;

	for (i = 0; i < lun->npls; i++)
		ssd_remove_nand_plane(&lun->pl[i]);

	kfree(lun->pl);
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
	int i;
	ch->nluns = spp->luns_per_ch;
	ch->lun = kmalloc(sizeof(struct nand_lun) * ch->nluns, GFP_KERNEL);
	if (!ch->lun) {
		NVMEV_ERROR("Failed to allocate channel luns memory\n");
		return;
	}
	for (i = 0; i < ch->nluns; i++) {
		ssd_init_nand_lun(&ch->lun[i], spp);
	}
	ch->next_ch_avail_time = 0;
	ch->hp_next_ch_avail_time = 0;
	ch->lp_next_ch_avail_time = 0;

	ch->perf_model = kmalloc(sizeof(struct channel_model), GFP_KERNEL);
	if (!ch->perf_model) {
		NVMEV_ERROR("Failed to allocate channel performance model memory\n");
		return;
	}
	if (chmodel_init(ch->perf_model, spp->ch_bandwidth)) {
		kfree(ch->perf_model);
		ch->perf_model = NULL;
		return;
	}

	/* Add firmware overhead */
	ch->perf_model->xfer_lat += (spp->fw_ch_xfer_lat * UNIT_XFER_SIZE / KB(4));
}

static void ssd_remove_ch(struct ssd_channel *ch)
{
	int i;

	if (ch->perf_model) {
		kvfree(ch->perf_model->avail_credits);
		kfree(ch->perf_model);
	}

	for (i = 0; i < ch->nluns; i++)
		ssd_remove_nand_lun(&ch->lun[i]);

	kfree(ch->lun);
}

static void ssd_init_pcie(struct ssd_pcie *pcie, struct ssdparams *spp)
{
	pcie->next_pcie_avail_time = 0;
	pcie->hp_next_pcie_avail_time = 0;
	pcie->lp_next_pcie_avail_time = 0;
	pcie->perf_model = kmalloc(sizeof(struct channel_model), GFP_KERNEL);
	if (!pcie->perf_model) {
		NVMEV_ERROR("Failed to allocate pcie performance model memory\n");
		return;
	}
	if (chmodel_init(pcie->perf_model, spp->pcie_bandwidth)) {
		kfree(pcie->perf_model);
		pcie->perf_model = NULL;
		return;
	}
}

static void ssd_remove_pcie(struct ssd_pcie *pcie)
{
	if (pcie->perf_model) {
		kvfree(pcie->perf_model->avail_credits);
		kfree(pcie->perf_model);
	}
}

void ssd_init(struct ssd *ssd, struct ssdparams *spp, uint32_t cpu_nr_dispatcher)
{
	int i;

	ssd->sp = *spp;
	ssd->cpu_nr_dispatcher = cpu_nr_dispatcher;

	ssd->ch = kmalloc(sizeof(struct ssd_channel) * spp->nchs, GFP_KERNEL); // 40 * 8 = 320
	if (!ssd->ch) {
		NVMEV_ERROR("Failed to allocate SSD channels memory\n");
		return;
	}
	for (i = 0; i < spp->nchs; i++) {
		ssd_init_ch(&ssd->ch[i], spp);
	}

	ssd->pcie = kmalloc(sizeof(struct ssd_pcie), GFP_KERNEL);
	if (!ssd->pcie) {
		NVMEV_ERROR("Failed to allocate SSD PCIe memory\n");
		return;
	}
	ssd_init_pcie(ssd->pcie, spp);

	ssd->write_buffer = kmalloc(sizeof(struct buffer), GFP_KERNEL);
	if (!ssd->write_buffer) {
		NVMEV_ERROR("Failed to allocate SSD write buffer memory\n");
		return;
	}
	buffer_init(ssd->write_buffer, spp->write_buffer_size);
}

void ssd_remove(struct ssd *ssd)
{
	uint32_t i;

	kfree(ssd->write_buffer);
	if (ssd->pcie) {
		if (ssd->pcie->perf_model) {
			kvfree(ssd->pcie->perf_model->avail_credits);
			kfree(ssd->pcie->perf_model);
		}
		kfree(ssd->pcie);
	}

	for (i = 0; i < ssd->sp.nchs; i++) {
		ssd_remove_ch(&(ssd->ch[i]));
	}

	kfree(ssd->ch);
}

static inline uint64_t ssd_pcie_xfer_time(struct ssd_pcie *pcie,
					  uint64_t length)
{
	uint64_t units;

	if (!pcie || !pcie->perf_model || !length)
		return 0;
	units = DIV_ROUND_UP(length, UNIT_XFER_SIZE);
	return (uint64_t)pcie->perf_model->xfer_lat * units;
}

static inline uint64_t ssd_pcie_sched_start(struct ssd_pcie *pcie,
					    uint64_t request_time,
					    bool read_priority,
					    bool low_priority)
{
	if (read_priority)
		return max(pcie->hp_next_pcie_avail_time, request_time);
	if (low_priority)
		return max(max(pcie->hp_next_pcie_avail_time,
			       pcie->lp_next_pcie_avail_time), request_time);
	return max(pcie->next_pcie_avail_time, request_time);
}

static inline void ssd_pcie_commit_normal(struct ssd_pcie *pcie, uint64_t end)
{
	pcie->hp_next_pcie_avail_time = end;
	pcie->lp_next_pcie_avail_time = end;
	pcie->next_pcie_avail_time = end;
}

static inline void ssd_pcie_commit_low_priority(struct ssd_pcie *pcie,
						uint64_t end)
{
	pcie->lp_next_pcie_avail_time = end;
	pcie->next_pcie_avail_time = max(pcie->hp_next_pcie_avail_time,
					 pcie->lp_next_pcie_avail_time);
}

static inline void ssd_pcie_commit_read_priority(struct ssd_pcie *pcie,
						 uint64_t start,
						 uint64_t end)
{
	uint64_t busy = (end > start) ? (end - start) : 0;

	if (busy && pcie->lp_next_pcie_avail_time > start) {
		if (U64_MAX - pcie->lp_next_pcie_avail_time < busy)
			pcie->lp_next_pcie_avail_time = U64_MAX;
		else
			pcie->lp_next_pcie_avail_time += busy;
	}
	pcie->hp_next_pcie_avail_time = max(pcie->hp_next_pcie_avail_time, end);
	pcie->next_pcie_avail_time = max(pcie->hp_next_pcie_avail_time,
					 pcie->lp_next_pcie_avail_time);
}

static uint64_t ssd_advance_pcie_internal(struct ssd *ssd,
					  uint64_t request_time,
					  uint64_t length,
					  bool read_priority,
					  bool low_priority)
{
	struct ssd_pcie *pcie;
	uint64_t start, end;

	if (!ssd || !ssd->pcie || !ssd->pcie->perf_model)
		return request_time;
	pcie = ssd->pcie;

	if (!read_priority && !low_priority) {
		request_time = max(pcie->next_pcie_avail_time, request_time);
		end = chmodel_request(pcie->perf_model, request_time, length);
		ssd_pcie_commit_normal(pcie, end);
		return end;
	}

	start = ssd_pcie_sched_start(pcie, request_time, read_priority,
				     low_priority);
	if (read_priority && pcie->next_pcie_avail_time > start) {
		atomic64_inc(&read_priority_pcie_bypass_ops);
		atomic64_add(pcie->next_pcie_avail_time - start,
			     &read_priority_pcie_bypass_ns);
	}
	end = start + ssd_pcie_xfer_time(pcie, length);
	if (read_priority)
		ssd_pcie_commit_read_priority(pcie, start, end);
	else
		ssd_pcie_commit_low_priority(pcie, end);
	return end;
}

uint64_t ssd_advance_pcie(struct ssd *ssd, uint64_t request_time, uint64_t length)
{
	return ssd_advance_pcie_internal(ssd, request_time, length, false, false);
}

/* Write buffer Performance Model
  Y = A + (B * X)
  Y : latency (ns)
  X : transfer size (4KB unit)
  A : fw_wbuf_lat0
  B : fw_wbuf_lat1 + pcie dma transfer
*/
uint64_t ssd_advance_write_buffer(struct ssd *ssd, uint64_t request_time, uint64_t length)
{
	uint64_t nsecs_latest = request_time;
	struct ssdparams *spp = &ssd->sp;

	nsecs_latest += spp->fw_wbuf_lat0;
	nsecs_latest += spp->fw_wbuf_lat1 * DIV_ROUND_UP(length, KB(4));

	nsecs_latest = ssd_advance_pcie(ssd, nsecs_latest, length);

	return nsecs_latest;
}

uint64_t ssd_advance_write_buffer_low_priority(struct ssd *ssd,
					       uint64_t request_time,
					       uint64_t length)
{
	uint64_t nsecs_latest = request_time;
	struct ssdparams *spp = &ssd->sp;

	nsecs_latest += spp->fw_wbuf_lat0;
	nsecs_latest += spp->fw_wbuf_lat1 * DIV_ROUND_UP(length, KB(4));
	nsecs_latest = ssd_advance_pcie_internal(ssd, nsecs_latest, length,
						 false, true);
	return nsecs_latest;
}

/* 辅助函数：检查块是否为 QLC（需要从 conv_ftl 访问） */
static bool is_qlc_block_ssd(struct ssd *ssd, uint32_t blk_id)
{
	uint32_t slc_blks = ssd->sp.slc_blks_per_pl;

	if (!slc_blks || slc_blks > ssd->sp.blks_per_pl)
		slc_blks = ssd->sp.blks_per_pl / 5;  /* 兜底到旧逻辑 */

	return blk_id >= slc_blks;
}

static inline uint64_t max3_u64(uint64_t a, uint64_t b, uint64_t c)
{
	return max(max(a, b), c);
}

static inline uint64_t ssd_lun_sched_start(struct nand_lun *lun,
					   uint64_t cmd_stime,
					   bool read_priority,
					   bool low_priority)
{
	if (read_priority)
		return max(lun->hp_next_lun_avail_time, cmd_stime);
	if (low_priority)
		return max3_u64(lun->hp_next_lun_avail_time,
				lun->lp_next_lun_avail_time, cmd_stime);
	return max(lun->next_lun_avail_time, cmd_stime);
}

static inline void ssd_lun_commit_normal(struct nand_lun *lun, uint64_t end)
{
	lun->hp_next_lun_avail_time = end;
	lun->lp_next_lun_avail_time = end;
	lun->next_lun_avail_time = end;
}

static inline void ssd_lun_commit_low_priority(struct nand_lun *lun, uint64_t end)
{
	lun->lp_next_lun_avail_time = end;
	lun->next_lun_avail_time = max(lun->hp_next_lun_avail_time,
				       lun->lp_next_lun_avail_time);
}

static inline void ssd_lun_commit_read_priority(struct nand_lun *lun,
						uint64_t start, uint64_t end)
{
	uint64_t busy = (end > start) ? (end - start) : 0;

	if (busy && lun->lp_next_lun_avail_time > start) {
		if (U64_MAX - lun->lp_next_lun_avail_time < busy)
			lun->lp_next_lun_avail_time = U64_MAX;
		else
			lun->lp_next_lun_avail_time += busy;
	}

	lun->hp_next_lun_avail_time = max(lun->hp_next_lun_avail_time, end);
	lun->next_lun_avail_time = max(lun->hp_next_lun_avail_time,
				       lun->lp_next_lun_avail_time);
}

static inline uint64_t ssd_channel_xfer_time(struct ssd_channel *ch,
					     uint64_t length)
{
	uint64_t units;

	if (!ch || !ch->perf_model || !length)
		return 0;
	units = DIV_ROUND_UP(length, UNIT_XFER_SIZE);
	return (uint64_t)ch->perf_model->xfer_lat * units;
}

static inline uint64_t ssd_channel_sched_start(struct ssd_channel *ch,
					       uint64_t request_time,
					       bool read_priority,
					       bool low_priority)
{
	if (read_priority)
		return max(ch->hp_next_ch_avail_time, request_time);
	if (low_priority)
		return max3_u64(ch->hp_next_ch_avail_time,
				ch->lp_next_ch_avail_time, request_time);
	return max(ch->next_ch_avail_time, request_time);
}

static inline void ssd_channel_commit_normal(struct ssd_channel *ch,
					     uint64_t end)
{
	ch->hp_next_ch_avail_time = end;
	ch->lp_next_ch_avail_time = end;
	ch->next_ch_avail_time = end;
}

static inline void ssd_channel_commit_low_priority(struct ssd_channel *ch,
						   uint64_t end)
{
	ch->lp_next_ch_avail_time = end;
	ch->next_ch_avail_time = max(ch->hp_next_ch_avail_time,
				     ch->lp_next_ch_avail_time);
}

static inline void ssd_channel_commit_read_priority(struct ssd_channel *ch,
						    uint64_t start,
						    uint64_t end)
{
	uint64_t busy = (end > start) ? (end - start) : 0;

	if (busy && ch->lp_next_ch_avail_time > start) {
		if (U64_MAX - ch->lp_next_ch_avail_time < busy)
			ch->lp_next_ch_avail_time = U64_MAX;
		else
			ch->lp_next_ch_avail_time += busy;
	}
	ch->hp_next_ch_avail_time = max(ch->hp_next_ch_avail_time, end);
	ch->next_ch_avail_time = max(ch->hp_next_ch_avail_time,
				     ch->lp_next_ch_avail_time);
}

static uint64_t ssd_advance_channel(struct ssd_channel *ch,
				    uint64_t request_time,
				    uint64_t length,
				    bool read_priority,
				    bool low_priority)
{
	uint64_t start, end;

	if (!ch || !ch->perf_model)
		return request_time;

	if (!read_priority && !low_priority) {
		request_time = max(ch->next_ch_avail_time, request_time);
		end = chmodel_request(ch->perf_model, request_time, length);
		ssd_channel_commit_normal(ch, end);
		return end;
	}

	start = ssd_channel_sched_start(ch, request_time, read_priority,
					low_priority);
	if (read_priority && ch->next_ch_avail_time > start) {
		atomic64_inc(&read_priority_ch_bypass_ops);
		atomic64_add(ch->next_ch_avail_time - start,
			     &read_priority_ch_bypass_ns);
	}
	end = start + ssd_channel_xfer_time(ch, length);

	if (read_priority)
		ssd_channel_commit_read_priority(ch, start, end);
	else
		ssd_channel_commit_low_priority(ch, end);
	return end;
}

static uint64_t ssd_advance_nand_internal(struct ssd *ssd, struct nand_cmd *ncmd,
					  bool force_read_priority,
					  bool force_low_priority)
{
	    /* Defensive checks to avoid NULL deref and invalid operations */
	    uint64_t safe_now;
	    if (unlikely(!ssd)) {
	        return 0;
	    }
	    safe_now = __get_ioclock(ssd);
    if (unlikely(!ncmd)) {
        NVMEV_ERROR("ssd_advance_nand: NULL ncmd\n");
        return safe_now;
    }
    if (unlikely(!ncmd->ppa)) {
        NVMEV_ERROR("ssd_advance_nand: NULL ncmd->ppa (cmd=0x%x, xfer=%llu)\n", ncmd->cmd, ncmd->xfer_size);
        /* return a bounded timestamp so callers can continue without crashing */
        return (ncmd->stime == 0) ? safe_now : ncmd->stime;
    }

    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? safe_now : ncmd->stime;

    if (ncmd->type == GC_IO && !gc_nand_timing)
        return cmd_stime;
	uint64_t nand_stime, nand_etime;
	uint64_t chnl_stime, chnl_etime;
	uint64_t remaining, xfer_size, completed_time;
	struct ssdparams *spp;
	struct nand_lun *lun;
	struct ssd_channel *ch;
    struct ppa *ppa = ncmd->ppa;
	uint32_t cell;
	bool is_qlc;
	bool read_priority;
	bool low_priority;
	bool host_low_priority;
	
	NVMEV_DEBUG(
		"SSD: %p, Enter stime: %lld, ch %d lun %d blk %d page %d command %d ppa 0x%llx\n",
		ssd, ncmd->stime, ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg, c, ppa->ppa);

    if (unlikely(ppa->ppa == UNMAPPED_PPA)) {
		NVMEV_ERROR("Error ppa 0x%llx\n", ppa->ppa);
		return cmd_stime;
	}

    /* Validate address range before dereferencing channels/luns */
    {
        int ch_idx = ppa->g.ch;
        int lun_idx = ppa->g.lun;
        int pl_idx = ppa->g.pl;
        int blk_idx = ppa->g.blk;
        int pg_idx = ppa->g.pg;
        struct ssdparams *vspp = &ssd->sp;
	uint32_t max_pg = (blk_idx < (int)vspp->slc_blks_per_pl) ?
			  vspp->slc_pgs_per_blk : vspp->qlc_pgs_per_blk;
        if (unlikely(ch_idx < 0 || ch_idx >= vspp->nchs ||
                     lun_idx < 0 || lun_idx >= vspp->luns_per_ch ||
                     pl_idx < 0 || pl_idx >= vspp->pls_per_lun ||
                     blk_idx < 0 || blk_idx >= vspp->blks_per_pl ||
                     pg_idx < 0 || pg_idx >= (int)max_pg)) {
            NVMEV_ERROR("ssd_advance_nand: invalid PPA ch=%d lun=%d pl=%d blk=%d pg=%d\n",
                        ch_idx, lun_idx, pl_idx, blk_idx, pg_idx);
            return cmd_stime;
        }
    }

	spp = &ssd->sp;
	lun = get_lun(ssd, ppa);
	ch = get_ch(ssd, ppa);
	cell = get_cell(ssd, ppa);
	remaining = ncmd->xfer_size;
	read_priority = force_read_priority && ncmd->type == USER_IO && c == NAND_READ;
	host_low_priority = force_low_priority && ncmd->type == USER_IO;
	low_priority = force_low_priority && !read_priority;
	
	/* 判断是否为 QLC 块 */
	is_qlc = is_qlc_block_ssd(ssd, ppa->g.blk);

	switch (c) {
		case NAND_READ:
			/* read: perform NAND cmd first */
			nand_stime = ssd_lun_sched_start(lun, cmd_stime,
							 read_priority, low_priority);
			if (read_priority && lun->next_lun_avail_time > nand_stime) {
				atomic64_inc(&read_priority_bypass_ops);
				atomic64_add(lun->next_lun_avail_time - nand_stime,
					     &read_priority_bypass_ns);
			}

				if (ncmd->type != GC_IO) {
				int die_idx = ppa->g.lun * spp->nchs + ppa->g.ch;
				if (die_idx >= 0 && die_idx < DIE_STAT_MAX) {
					atomic_long_inc(&die_read_total[die_idx]);
					if (nand_stime > cmd_stime) {
						uint64_t wait_ns = nand_stime - cmd_stime;

						atomic_long_inc(&die_read_conflict[die_idx]);
						atomic_long_add((long)wait_ns,
								&die_read_wait_ns[die_idx]);
						if (ncmd->tracked_read_die_conflicts)
							atomic64_inc(ncmd->tracked_read_die_conflicts);
						if (ncmd->tracked_read_die_wait_ns)
							atomic64_add(wait_ns, ncmd->tracked_read_die_wait_ns);
					}
				}
			}

		if (is_qlc) {
			/* QLC 读延迟 */
			struct nand_page *cur_pg = get_pg(ssd, ppa);
			uint32_t zone = cur_pg ? cur_pg->qlc_latency_zone : 0;
			if (zone >= ARRAY_SIZE(spp->qlc_pg_rd_lat))
				zone = ARRAY_SIZE(spp->qlc_pg_rd_lat) - 1;
			if (ncmd->xfer_size == 4096) {
				nand_etime = nand_stime + spp->qlc_pg_4kb_rd_lat[zone];
			} else {
				nand_etime = nand_stime + spp->qlc_pg_rd_lat[zone];
			}
		} else {
			/* SLC 读延迟 */
			if (ncmd->xfer_size == 4096) {
				nand_etime = nand_stime + spp->pg_4kb_rd_lat[cell];
			} else {
				nand_etime = nand_stime + spp->pg_rd_lat[cell];
			}
		}

		if (ncmd->type == GC_IO) {
			/* GC/迁移：跳过通道模型，仅保留 NAND 延迟 */
			if (low_priority)
				ssd_lun_commit_low_priority(lun, nand_etime);
			else
				ssd_lun_commit_normal(lun, nand_etime);
			completed_time = nand_etime;
		} else {
			/* read: then data transfer through channel */
			chnl_stime = nand_etime;
			chnl_etime = chnl_stime;
			completed_time = chnl_etime;

				while (remaining) {
					xfer_size = min(remaining, (uint64_t)spp->max_ch_xfer_size);
					chnl_etime = ssd_advance_channel(ch, chnl_stime,
									 xfer_size,
									 read_priority,
									 low_priority);

					if (ncmd->interleave_pci_dma) {
						completed_time =
							ssd_advance_pcie_internal(ssd, chnl_etime,
										 xfer_size,
										 read_priority,
										 low_priority);
					} else {
						completed_time = chnl_etime;
					}

				remaining -= xfer_size;
				chnl_stime = chnl_etime;
			}

			if (read_priority)
				ssd_lun_commit_read_priority(lun, nand_stime,
							     chnl_etime);
			else
				ssd_lun_commit_normal(lun, chnl_etime);
		}
		break;

		case NAND_WRITE:
			if (ncmd->type == GC_IO) {
				/* GC/迁移：跳过通道模型，仅保留 NAND 延迟 */
				nand_stime = ssd_lun_sched_start(lun, cmd_stime,
								 false, low_priority);
				} else {
					/* write: transfer data through channel first */
					chnl_stime = ssd_lun_sched_start(lun, cmd_stime,
									 false, low_priority);
					chnl_etime = ssd_advance_channel(ch, chnl_stime,
									 ncmd->xfer_size,
									 false, low_priority);
					nand_stime = chnl_etime;
				}

		if (is_qlc) {
			nand_etime = nand_stime + spp->qlc_pg_wr_lat;
		} else {
			nand_etime = nand_stime + spp->pg_wr_lat;
		}
		
		if (low_priority)
			ssd_lun_commit_low_priority(lun, nand_etime);
		else
			ssd_lun_commit_normal(lun, nand_etime);
		completed_time = nand_etime;
		break;

	case NAND_ERASE:
		/* erase: only need to advance NAND status */
		nand_stime = ssd_lun_sched_start(lun, cmd_stime,
						 false, low_priority);
		
		if (is_qlc) {
			/* QLC 擦除延迟（比 SLC 慢约 3 倍） */
			nand_etime = nand_stime + spp->qlc_blk_er_lat;
		} else {
			/* SLC 擦除延迟 */
			nand_etime = nand_stime + spp->blk_er_lat;
		}
		
		if (low_priority)
			ssd_lun_commit_low_priority(lun, nand_etime);
		else
			ssd_lun_commit_normal(lun, nand_etime);
		completed_time = nand_etime;
		break;

	case NAND_NOP:
		/* no operation: just return last completed time of lun */
		nand_stime = max(lun->next_lun_avail_time, cmd_stime);
		ssd_lun_commit_normal(lun, nand_stime);
		completed_time = nand_stime;
		break;

	default:
		NVMEV_ERROR("Unsupported NAND command: 0x%x\n", c);
		return 0;
	}

	if (ncmd->type == GC_IO && completed_time > cmd_stime) {
		uint64_t delta = completed_time - cmd_stime;

		atomic64_add(delta, &bg_nand_busy_time_ns);
		switch (c) {
		case NAND_READ:
			atomic64_add(delta, &bg_nand_read_time_ns);
			atomic64_inc(&bg_nand_read_ops);
			break;
		case NAND_WRITE:
			atomic64_add(delta, &bg_nand_write_time_ns);
			atomic64_inc(&bg_nand_write_ops);
			break;
		case NAND_ERASE:
			atomic64_add(delta, &bg_nand_erase_time_ns);
			atomic64_inc(&bg_nand_erase_ops);
			break;
		default:
			break;
		}
	}
	if (host_low_priority && c == NAND_WRITE && completed_time > cmd_stime) {
		atomic64_inc(&lp_host_write_ops);
		atomic64_add(completed_time - cmd_stime, &lp_host_write_time_ns);
	}

	return completed_time;
}

uint64_t ssd_advance_nand(struct ssd *ssd, struct nand_cmd *ncmd)
{
	return ssd_advance_nand_internal(ssd, ncmd, false, false);
}

uint64_t ssd_advance_nand_read_priority(struct ssd *ssd, struct nand_cmd *ncmd)
{
	return ssd_advance_nand_internal(ssd, ncmd, true, false);
}

uint64_t ssd_advance_nand_low_priority(struct ssd *ssd, struct nand_cmd *ncmd)
{
	return ssd_advance_nand_internal(ssd, ncmd, false, true);
}

uint64_t ssd_next_idle_time(struct ssd *ssd)
{
	struct ssdparams *spp = &ssd->sp;
	uint32_t i, j;
	uint64_t latest = __get_ioclock(ssd);

	for (i = 0; i < spp->nchs; i++) {
		struct ssd_channel *ch = &ssd->ch[i];

		latest = max(latest, ch->next_ch_avail_time);

		for (j = 0; j < spp->luns_per_ch; j++) {
			struct nand_lun *lun = &ch->lun[j];
			latest = max(latest, lun->next_lun_avail_time);
		}
	}
	if (ssd->pcie)
		latest = max(latest, ssd->pcie->next_pcie_avail_time);

	return latest;
}

uint64_t ssd_lun_next_idle_time(struct ssd *ssd, unsigned int ch, unsigned int lun)
{
	if (!ssd || ch >= (unsigned int)ssd->sp.nchs ||
	    lun >= (unsigned int)ssd->sp.luns_per_ch)
		return ssd ? __get_ioclock(ssd) : 0;

	return ssd->ch[ch].lun[lun].next_lun_avail_time;
}

uint64_t ssd_pcie_next_idle_time(struct ssd *ssd)
{
	if (!ssd || !ssd->pcie)
		return ssd ? __get_ioclock(ssd) : 0;

	return ssd->pcie->next_pcie_avail_time;
}

void adjust_ftl_latency(int target, int lat)
{
/* TODO ..*/
#if 0
    struct ssdparams *spp;
    int i;

    for (i = 0; i < SSD_PARTITIONS; i++) {
        spp = &(g_conv_ftls[i].sp);
        NVMEV_INFO("Before latency: %d %d %d, change to %d\n", spp->pg_rd_lat, spp->pg_wr_lat, spp->blk_er_lat, lat);
        switch (target) {
            case NAND_READ:
                spp->pg_rd_lat = lat;
                break;

            case NAND_WRITE:
                spp->pg_wr_lat = lat;
                break;

            case NAND_ERASE:
                spp->blk_er_lat = lat;
                break;

            default:
                NVMEV_ERROR("Unsupported NAND command\n");
        }
        NVMEV_INFO("After latency: %d %d %d\n", spp->pg_rd_lat, spp->pg_wr_lat, spp->blk_er_lat);
    }
#endif
}
