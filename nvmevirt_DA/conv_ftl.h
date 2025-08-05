// SPDX-License-Identifier: GPL-2.0-only

#ifndef _NVMEVIRT_CONV_FTL_H
#define _NVMEVIRT_CONV_FTL_H

#include <linux/types.h>
#include "pqueue/pqueue.h"
#include "ssd_config.h"
#include "ssd.h"

struct convparams {
	uint32_t gc_thres_lines;
	uint32_t gc_thres_lines_high;
	bool enable_gc_delay;

	double op_area_pcent;
	int pba_pcent; /* (physical space / logical space) * 100*/
};

struct line {
	int id; /* line id, the same as corresponding block id */
	int ipc; /* invalid page count in this line */
	int vpc; /* valid page count in this line */
	struct list_head entry;
	/* position in the priority queue for victim lines */
	size_t pos;
};

/* wp: record next write addr */
struct write_pointer {
	struct line *curline;
	uint32_t ch;
	uint32_t lun;
	uint32_t pg;
	uint32_t blk;
	uint32_t pl;
};

struct line_mgmt {
	struct line *lines;

	/* free line list, we only need to maintain a list of blk numbers */
	struct list_head free_line_list;
	pqueue_t *victim_line_pq;
	struct list_head full_line_list;

	uint32_t tt_lines;
	uint32_t free_line_cnt;
	uint32_t victim_line_cnt;
	uint32_t full_line_cnt;
};

struct write_flow_control {
	uint32_t write_credits;
	uint32_t credits_to_refill;
};

/* 热数据跟踪结构 - 用于 SLC 到 QLC 迁移 */
struct heat_tracking {
	uint64_t *access_count;     /* 每个 LPN 的访问计数 */
	uint64_t *last_access_time; /* 每个 LPN 的最后访问时间 */
	uint32_t migration_threshold; /* 迁移阈值 */
};

/* 迁移管理结构 */
struct migration_mgmt {
	struct list_head migration_queue; /* 待迁移页面队列 */
	uint32_t pending_migrations;      /* 待迁移页面数 */
	bool migration_in_progress;       /* 迁移进行标志 */
};

struct conv_ftl {
	struct ssd *ssd;

	struct convparams cp;
	struct ppa *maptbl; /* page level mapping table */
	uint64_t *rmap; /* reverse mapptbl, assume it's stored in OOB */
	struct write_pointer wp;
	struct write_pointer gc_wp;
	struct line_mgmt lm;
	struct write_flow_control wfc;
	//66f1
	struct line_mgmt *lunlm;
	struct write_pointer *lunwp;
	uint32_t lunpointer;
	//66f1

	/* SLC/QLC 混合存储相关字段 */
	bool *is_slc_block;          /* 标记块是否为 SLC */
	uint32_t slc_blks_per_pl;    /* 每个 plane 的 SLC 块数 */
	uint32_t qlc_blks_per_pl;    /* 每个 plane 的 QLC 块数 */
	uint32_t qlc_region_size;    /* QLC 区域大小（块数） */
	
	/* SLC 写指针 - 使用 DA (Die Affinity) */
	struct write_pointer slc_wp;
	struct line_mgmt slc_lm;
	
	/* QLC 写指针 - 使用顺序写入 */
	struct write_pointer qlc_wp[QLC_REGIONS]; /* QLC 区域写指针 */
	struct line_mgmt qlc_lm[QLC_REGIONS];     /* QLC 区域的 line 管理 */
	uint32_t current_qlc_region;              /* 当前写入的 QLC 区域 */
	
	/* 热数据跟踪和迁移管理 */
	struct heat_tracking heat_track;
	struct migration_mgmt migration;
	
	/* 页面元数据 - 记录页面是否在 SLC 中 */
	bool *page_in_slc;           /* 标记页面是否在 SLC 中 */
	
	/* 统计信息 */
	uint64_t slc_write_cnt;      /* SLC 写入计数 */
	uint64_t qlc_write_cnt;      /* QLC 写入计数 */
	uint64_t migration_cnt;      /* 迁移计数 */
};

void conv_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			 uint32_t cpu_nr_dispatcher);

void conv_remove_namespace(struct nvmev_ns *ns);

bool conv_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req,
			   struct nvmev_result *ret);

#endif
