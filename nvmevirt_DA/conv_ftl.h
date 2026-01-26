// SPDX-License-Identifier: GPL-2.0-only

#ifndef _NVMEVIRT_CONV_FTL_H
#define _NVMEVIRT_CONV_FTL_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/seqlock.h>
#include <linux/atomic.h>
#include "pqueue/pqueue.h"
#include "ssd_config.h"
#include "ssd.h"

struct dentry;

#define QLC_ZONE_COUNT 4

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
	uint32_t zone_written[QLC_ZONE_COUNT];
};

/* wp: record next write addr */
struct write_pointer {
	struct line *curline;
	uint32_t ch;
	uint32_t lun;
	uint32_t pg;
	uint32_t blk;
	uint32_t pl;
	uint32_t *die_pg;
	uint32_t die_pg_size;
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
	uint64_t *write_epoch;      /* 最近写入序号 */
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
	seqlock_t maptbl_lock; /* serialize maptbl/rmap updates with low-overhead readers */
	struct write_flow_control wfc;
	//66f1 - 删除了冲突的旧 line_mgmt 结构
	uint32_t lunpointer;
	uint32_t die_count;

	/* SLC/QLC 混合存储相关字段 */
	bool *is_slc_block;          /* 标记块是否为 SLC */
	uint32_t slc_blks_per_pl;    /* 每个 plane 的 SLC 块数 */
	uint32_t qlc_blks_per_pl;    /* 每个 plane 的 QLC 块数 */
	uint32_t slc_pgs_per_blk;    /* SLC block 的页数 */
	uint32_t qlc_pgs_per_blk;    /* QLC block 的页数（可为 SLC 的多倍） */
	
	struct line_mgmt slc_lm;              /* legacy aggregated view (unused in DA path) */
	struct line_mgmt *slc_lunlm;          /* per-die SLC line pools */
	struct write_pointer *slc_lunwp;
	struct write_pointer *gc_slc_lunwp;
	struct write_pointer slc_wp;
	struct write_pointer gc_wp;
	
	struct line_mgmt *qlc_lunlm;          /* per-die QLC line pools */
	struct write_pointer *qlc_lunwp;      /* per-die QLC write pointers */
	struct write_pointer *gc_qlc_lunwp;   /* per-die QLC GC write pointers */
	uint32_t qlc_zone_offsets[QLC_ZONE_COUNT];/* 每个zone在block中的起始页 */
	uint32_t qlc_zone_limits[QLC_ZONE_COUNT]; /* 每个zone允许写入的页数 */
	uint32_t qlc_zone_rr_cursor;             /* 无机制版本：线性填充所用的轮询游标 */
	struct write_pointer qlc_wp;
	struct write_pointer qlc_gc_wp;

	uint32_t slc_gc_free_thres_high;
	uint32_t qlc_gc_free_thres_high;
	uint32_t slc_target_watermark;        /* 迁移目标水位线（高水位触发后回落到此值） */
	uint32_t slc_repromote_guard_lines;   /* QLC->SLC 回迁的安全预留行数 */

	/* 热数据跟踪和迁移管理 */
	struct heat_tracking heat_track;
	struct migration_mgmt migration;

	/* 页面元数据 - 记录页面是否在 SLC 中 */
	bool *page_in_slc;           /* 标记页面是否在 SLC 中 */
	uint64_t *qlc_page_wcnt;     /* 每个 LPN 写入到 QLC 的次数 */
	uint64_t qlc_total_wcnt;     /* 写入到 QLC 的总次数 */
	uint64_t qlc_unique_pages;   /* 曾写入 QLC 的唯一 LPN 数 */
	uint64_t qlc_threshold_q1_q2;/* Q1/Q2 分界线 */
	uint64_t qlc_threshold_q2_q3;/* Q2/Q3 分界线 */
	uint64_t qlc_threshold_q3_q4;/* Q3/Q4 分界线 */
	uint64_t qlc_resident_read_sum;   /* HLFA：QLC 驻留页累计读次数 */
	uint64_t qlc_resident_page_cnt;   /* HLFA：已统计的 QLC 驻留页数量 */
	spinlock_t qlc_zone_lock;    /* 保护 QLC 区域统计 */
	uint64_t qlc_migration_read_sum;   /* 迁移页累计读次数 */
	uint64_t qlc_migration_page_cnt;   /* 迁移页数量 */
	uint64_t global_read_sum;         /* 全局有效页读次数总和 */
	uint64_t global_valid_pg_cnt;     /* 全局有效页总数 */
	uint64_t migration_read_path_count;    /* 读路径触发迁移次数 */
	uint64_t migration_read_path_time_ns;  /* 读路径迁移耗时累计 */

	/* 统计信息 */
	uint64_t slc_write_cnt;      /* SLC 写入计数 */
	uint64_t qlc_write_cnt;      /* QLC 写入计数 */
	uint64_t migration_cnt;      /* 迁移计数 */
	uint64_t total_host_writes;  /* 总写入块数 */
	atomic64_t slc_resident_page_cnt; /* 当前驻留在 SLC 的页面数 */
	atomic_t slc_recover_lock;        /* 序列化 SLC 回收 */
	struct dentry *debug_dir;          /* per-instance debugfs directory */
	struct dentry *debug_access_count; /* debugfs entry for access counter */
	struct dentry *debug_access_inject; /* debugfs entry for counter injection */
	
	/* 初始化状态标记 */
	bool maptbl_initialized;     /* 映射表是否初始化成功 */
	bool rmap_initialized;       /* 反向映射表是否初始化成功 */
	bool slc_initialized;        /* SLC 是否初始化成功 */
	bool qlc_initialized;        /* QLC 是否初始化成功 */
	bool heat_track_initialized; /* 热跟踪是否初始化成功 */
	/* 账本并发保护（最小范围：仅行/队列/写指针） */
	spinlock_t slc_lock; /* 保护 SLC 的 line_mgmt/free/full/victim、SLC 写指针 */
	spinlock_t qlc_lock; /* 保护 QLC 的 line_mgmt/free/full/victim、QLC 写指针 */
	
	/* 后台线程管理 */
	struct task_struct *migration_thread;    /* 后台迁移线程 */
	struct task_struct *gc_thread;           /* 后台GC线程 */
	wait_queue_head_t migration_wq;          /* 迁移线程等待队列 */
	wait_queue_head_t gc_wq;                 /* GC线程等待队列 */
	atomic_t migration_needed;               /* 是否需要迁移 */
	atomic_t gc_needed;                      /* 是否需要GC */
	bool threads_should_stop;                /* 线程停止标志 */
	
	/* 水位线控制 - 基于剩余空间数量 */
	uint32_t slc_high_watermark;             /* SLC 高水位线: 剩余空间低于此值时触发迁移 */
};

void conv_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			 uint32_t cpu_nr_dispatcher);

void conv_remove_namespace(struct nvmev_ns *ns);

bool conv_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req,
			   struct nvmev_result *ret);

void nvmev_debugfs_cleanup_root(void);

#endif
