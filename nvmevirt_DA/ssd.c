// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ktime.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "ssd.h"

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
	buf->remaining += size;
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
		spp->blks_per_pl = DIV_ROUND_UP(capacity, blk_size * spp->pls_per_lun *
								  spp->luns_per_ch * spp->nchs);
	}

	NVMEV_ASSERT((ONESHOT_PAGE_SIZE % spp->pgsz) == 0 && (FLASH_PAGE_SIZE % spp->pgsz) == 0);
	NVMEV_ASSERT((ONESHOT_PAGE_SIZE % FLASH_PAGE_SIZE) == 0);

	spp->pgs_per_oneshotpg = ONESHOT_PAGE_SIZE / (spp->pgsz);
	spp->oneshotpgs_per_blk = DIV_ROUND_UP(blk_size, ONESHOT_PAGE_SIZE);

	spp->pgs_per_flashpg = FLASH_PAGE_SIZE / (spp->pgsz);
	spp->flashpgs_per_blk = (ONESHOT_PAGE_SIZE / FLASH_PAGE_SIZE) * spp->oneshotpgs_per_blk;

	spp->pgs_per_blk = spp->pgs_per_oneshotpg * spp->oneshotpgs_per_blk;

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

	spp->ch_bandwidth = NAND_CHANNEL_BANDWIDTH;
	spp->pcie_bandwidth = PCIE_BANDWIDTH;

	spp->write_buffer_size = GLOBAL_WB_SIZE;
	spp->write_early_completion = WRITE_EARLY_COMPLETION;

	/* calculated values */
	spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
	spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
	spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
	spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
	spp->tt_secs = spp->secs_per_ch * spp->nchs;

	spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
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
	spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
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

	total_size = (unsigned long)spp->tt_luns * spp->blks_per_lun * spp->pgs_per_blk *
		     spp->secsz * spp->secs_per_pg;
	blk_size = spp->pgs_per_blk * spp->secsz * spp->secs_per_pg;
	NVMEV_INFO(
		"Total Capacity(GiB,MiB)=%llu,%llu chs=%u luns=%lu lines=%lu blk-size(MiB,KiB)=%u,%u line-size(MiB,KiB)=%lu,%lu",
		BYTE_TO_GB(total_size), BYTE_TO_MB(total_size), spp->nchs, spp->tt_luns,
		spp->tt_lines, BYTE_TO_MB(spp->pgs_per_blk * spp->pgsz),
		BYTE_TO_KB(spp->pgs_per_blk * spp->pgsz), BYTE_TO_MB(spp->pgs_per_line * spp->pgsz),
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
}

static void ssd_remove_nand_page(struct nand_page *pg)
{
	kfree(pg->sec);
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
	int i;
	blk->npgs = spp->pgs_per_blk;
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
		ssd_init_nand_blk(&pl->blk[i], spp);
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

	ch->perf_model = kmalloc(sizeof(struct channel_model), GFP_KERNEL);
	if (!ch->perf_model) {
		NVMEV_ERROR("Failed to allocate channel performance model memory\n");
		return;
	}
	chmodel_init(ch->perf_model, spp->ch_bandwidth);

	/* Add firmware overhead */
	ch->perf_model->xfer_lat += (spp->fw_ch_xfer_lat * UNIT_XFER_SIZE / KB(4));
}

static void ssd_remove_ch(struct ssd_channel *ch)
{
	int i;

	kfree(ch->perf_model);

	for (i = 0; i < ch->nluns; i++)
		ssd_remove_nand_lun(&ch->lun[i]);

	kfree(ch->lun);
}

static void ssd_init_pcie(struct ssd_pcie *pcie, struct ssdparams *spp)
{
	pcie->perf_model = kmalloc(sizeof(struct channel_model), GFP_KERNEL);
	chmodel_init(pcie->perf_model, spp->pcie_bandwidth);
}

static void ssd_remove_pcie(struct ssd_pcie *pcie)
{
	kfree(pcie->perf_model);
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
		kfree(ssd->pcie->perf_model);
		kfree(ssd->pcie);
	}

	for (i = 0; i < ssd->sp.nchs; i++) {
		ssd_remove_ch(&(ssd->ch[i]));
	}

	kfree(ssd->ch);
}

uint64_t ssd_advance_pcie(struct ssd *ssd, uint64_t request_time, uint64_t length)
{
	struct channel_model *perf_model = ssd->pcie->perf_model;
	return chmodel_request(perf_model, request_time, length);
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

/* 辅助函数：检查块是否为 QLC（需要从 conv_ftl 访问） */
static bool is_qlc_block_ssd(struct ssd *ssd, uint32_t blk_id)
{
	/* 简单的判断：前 20% 是 SLC，后 80% 是 QLC */
	uint32_t slc_blks = ssd->sp.blks_per_pl / 5;  /* 0.2 = 1/5 */
	return blk_id >= slc_blks;
}

uint64_t ssd_advance_nand(struct ssd *ssd, struct nand_cmd *ncmd)
{
	int c = ncmd->cmd;
	uint64_t cmd_stime = (ncmd->stime == 0) ? __get_ioclock(ssd) : ncmd->stime;
	uint64_t nand_stime, nand_etime;
	uint64_t chnl_stime, chnl_etime;
	uint64_t remaining, xfer_size, completed_time;
	struct ssdparams *spp;
	struct nand_lun *lun;
	struct ssd_channel *ch;
	struct ppa *ppa = ncmd->ppa;
	uint32_t cell;
	bool is_qlc;
	
	NVMEV_DEBUG(
		"SSD: %p, Enter stime: %lld, ch %d lun %d blk %d page %d command %d ppa 0x%llx\n",
		ssd, ncmd->stime, ppa->g.ch, ppa->g.lun, ppa->g.blk, ppa->g.pg, c, ppa->ppa);

	if (ppa->ppa == UNMAPPED_PPA) {
		NVMEV_ERROR("Error ppa 0x%llx\n", ppa->ppa);
		return cmd_stime;
	}

	spp = &ssd->sp;
	lun = get_lun(ssd, ppa);
	ch = get_ch(ssd, ppa);
	cell = get_cell(ssd, ppa);
	remaining = ncmd->xfer_size;
	
	/* 判断是否为 QLC 块 */
	is_qlc = is_qlc_block_ssd(ssd, ppa->g.blk);

	switch (c) {
	case NAND_READ:
		/* read: perform NAND cmd first */
		nand_stime = max(lun->next_lun_avail_time, cmd_stime);

		if (is_qlc) {
			/* QLC 读延迟 */
			uint32_t qlc_cell = ppa->g.pg % 4;  /* QLC 有配置定义的单元类型 */
			if (ncmd->xfer_size == 4096) {
				nand_etime = nand_stime + spp->qlc_pg_4kb_rd_lat[qlc_cell];
			} else {
				nand_etime = nand_stime + spp->qlc_pg_rd_lat[qlc_cell];
			}
		} else {
			/* SLC 读延迟 */
			if (ncmd->xfer_size == 4096) {
				nand_etime = nand_stime + spp->pg_4kb_rd_lat[cell];
			} else {
				nand_etime = nand_stime + spp->pg_rd_lat[cell];
			}
		}

		/* read: then data transfer through channel */
		chnl_stime = nand_etime;

		while (remaining) {
			xfer_size = min(remaining, (uint64_t)spp->max_ch_xfer_size);
			chnl_etime = chmodel_request(ch->perf_model, chnl_stime, xfer_size);

			if (ncmd->interleave_pci_dma) { /* overlap pci transfer with nand ch transfer*/
				completed_time = ssd_advance_pcie(ssd, chnl_etime, xfer_size);
			} else {
				completed_time = chnl_etime;
			}

			remaining -= xfer_size;
			chnl_stime = chnl_etime;
		}

		lun->next_lun_avail_time = chnl_etime;
		break;

	case NAND_WRITE:
		/* write: transfer data through channel first */
		chnl_stime = max(lun->next_lun_avail_time, cmd_stime);

		chnl_etime = chmodel_request(ch->perf_model, chnl_stime, ncmd->xfer_size);

		/* write: then do NAND program */
		nand_stime = chnl_etime;
		
		if (is_qlc) {
			/* QLC 写延迟（比 SLC 慢约 4 倍） */
			nand_etime = nand_stime + spp->qlc_pg_wr_lat;
		} else {
			/* SLC 写延迟 */
			nand_etime = nand_stime + spp->pg_wr_lat;
		}
		
		lun->next_lun_avail_time = nand_etime;
		completed_time = nand_etime;
		break;

	case NAND_ERASE:
		/* erase: only need to advance NAND status */
		nand_stime = max(lun->next_lun_avail_time, cmd_stime);
		
		if (is_qlc) {
			/* QLC 擦除延迟（比 SLC 慢约 3 倍） */
			nand_etime = nand_stime + spp->qlc_blk_er_lat;
		} else {
			/* SLC 擦除延迟 */
			nand_etime = nand_stime + spp->blk_er_lat;
		}
		
		lun->next_lun_avail_time = nand_etime;
		completed_time = nand_etime;
		break;

	case NAND_NOP:
		/* no operation: just return last completed time of lun */
		nand_stime = max(lun->next_lun_avail_time, cmd_stime);
		lun->next_lun_avail_time = nand_stime;
		completed_time = nand_stime;
		break;

	default:
		NVMEV_ERROR("Unsupported NAND command: 0x%x\n", c);
		return 0;
	}

	return completed_time;
}

uint64_t ssd_next_idle_time(struct ssd *ssd)
{
	struct ssdparams *spp = &ssd->sp;
	uint32_t i, j;
	uint64_t latest = __get_ioclock(ssd);

	for (i = 0; i < spp->nchs; i++) {
		struct ssd_channel *ch = &ssd->ch[i];

		for (j = 0; j < spp->luns_per_ch; j++) {
			struct nand_lun *lun = &ch->lun[j];
			latest = max(latest, lun->next_lun_avail_time);
		}
	}

	return latest;
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
