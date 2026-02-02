// SPDX-License-Identifier: GPL-2.0-only

#include "nvmev.h"
#include "conv_ftl.h"
#if 0  // Force disabled ZNS support
#include "zns_ftl.h"
#endif

#define sq_entry(entry_id) \
	queue->nvme_sq[SQ_ENTRY_TO_PAGE_NUM(entry_id)][SQ_ENTRY_TO_PAGE_OFFSET(entry_id)]
#define cq_entry(entry_id) \
	queue->nvme_cq[CQ_ENTRY_TO_PAGE_NUM(entry_id)][CQ_ENTRY_TO_PAGE_OFFSET(entry_id)]

#include <linux/mm.h> /* for pfn_valid, page_address */
#include <linux/dma-mapping.h>
#include <linux/iommu.h>

#define NVMEV_ADMIN_LOG(string, args...) NVMEV_INFO("admin: " string, ##args)
#define NVMEV_ADMIN_ERR(string, args...) NVMEV_ERROR("admin: " string, ##args)

/* Safe PRP translation:
 * - Translates DMA addr -> phys (IOMMU aware)
 * - Guards overflow on addr+len
 * - Returns NULL on any check failure so callers can bail early
 */
static inline bool nvmev_dma_to_phys(dma_addr_t dma, phys_addr_t *phys_out)
{
#ifdef CONFIG_IOMMU_API
	struct iommu_domain *domain;

	if (nvmev_vdev && nvmev_vdev->pdev) {
		domain = iommu_get_domain_for_dev(&nvmev_vdev->pdev->dev);
		if (domain) {
			phys_addr_t phys = iommu_iova_to_phys(domain, dma);
			if (!phys && dma)
				return false;
			*phys_out = phys;
			return true;
		}
	}
#endif
	*phys_out = (phys_addr_t)dma;
	return true;
}

static inline void *nvmev_prp_address_offset(u64 prp, unsigned int page_off, size_t len)
{
	dma_addr_t dma, dma_end;
	phys_addr_t phys;
	u64 phys_end;
	struct page *page;
	void *kaddr;

	if (!nvmev_vdev || !nvmev_vdev->pdev)
		return NULL;

	dma = prp + ((u64)page_off << PAGE_SHIFT);
	dma_end = dma + len - 1;

	/* overflow or zero length */
	if (len == 0 || dma_end < dma) {
		NVMEV_ADMIN_ERR("prp invalid len/overflow: prp=0x%llx off=%u len=%zu\n",
				(unsigned long long)prp, page_off, len);
		return NULL;
	}

	if (!nvmev_dma_to_phys(dma, &phys)) {
		NVMEV_ADMIN_ERR("prp dma->phys fail: prp=0x%llx off=%u len=%zu dma=0x%llx\n",
				(unsigned long long)prp, page_off, len,
				(unsigned long long)dma);
		return NULL;
	}
	phys_end = phys + len - 1;
	if (phys_end < phys) {
		NVMEV_ADMIN_ERR("prp phys overflow: prp=0x%llx off=%u len=%zu phys=0x%llx\n",
				(unsigned long long)prp, page_off, len,
				(unsigned long long)phys);
		return NULL;
	}

	if (!pfn_valid(phys >> PAGE_SHIFT)) {
		NVMEV_ADMIN_ERR("prp pfn invalid: prp=0x%llx off=%u len=%zu phys=0x%llx pfn=0x%llx\n",
				(unsigned long long)prp, page_off, len,
				(unsigned long long)phys,
				(unsigned long long)(phys >> PAGE_SHIFT));
		return NULL;
	}

	page = pfn_to_page(phys >> PAGE_SHIFT);
	kaddr = page_address(page);
	if (!kaddr) {
		NVMEV_ADMIN_ERR("prp page_address null: prp=0x%llx off=%u len=%zu phys=0x%llx\n",
				(unsigned long long)prp, page_off, len,
				(unsigned long long)phys);
		return NULL;
	}

	NVMEV_ADMIN_LOG("prp ok: prp=0x%llx off=%u len=%zu dma=0x%llx phys=0x%llx kaddr=%px ref=%d\n",
			(unsigned long long)prp, page_off, len,
			(unsigned long long)dma, (unsigned long long)phys,
			kaddr, page_count(page));
	return kaddr + (phys & ~PAGE_MASK);
}

#define prp_address_offset(prp, offset) nvmev_prp_address_offset((prp), (offset), PAGE_SIZE)
#define prp_address_len(prp, len)      nvmev_prp_address_offset((prp), 0, (len))

static inline void nvmev_admin_complete(struct nvmev_admin_queue *queue, int cq_head,
					int entry_id, u16 status)
{
	cq_entry(cq_head).command_id = sq_entry(entry_id).common.command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = entry_id;
	cq_entry(cq_head).status = queue->phase | (status << 1);

	NVMEV_ADMIN_LOG("cqe: opcode=0x%x cid=%u sq_head=%d cq_head=%d phase=%d status=0x%x\n",
			sq_entry(entry_id).common.opcode,
			sq_entry(entry_id).common.command_id,
			entry_id, cq_head, queue->phase, status);
}

static inline void nvmev_admin_complete_err(struct nvmev_admin_queue *queue, int cq_head,
					    int entry_id, u16 status)
{
	nvmev_admin_complete(queue, cq_head, entry_id, status);
	cq_entry(cq_head).result0 = 0;
}

static void __nvmev_admin_create_cq(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = nvmev_vdev->admin_q;
	struct nvmev_completion_queue *cq;
	struct nvme_create_cq *cmd = &sq_entry(eid).create_cq;
	unsigned int num_pages, i;
	int dbs_idx;

	cq = kzalloc(sizeof(struct nvmev_completion_queue), GFP_KERNEL);

	cq->qid = cmd->cqid;

	cq->irq_enabled = cmd->cq_flags & NVME_CQ_IRQ_ENABLED ? true : false;
	if (cq->irq_enabled) {
		cq->irq_vector = cmd->irq_vector;
	}
	cq->interrupt_ready = false;

	cq->queue_size = cmd->qsize + 1;
	cq->phase = 1;

	cq->cq_head = 0;
	cq->cq_tail = -1;

	spin_lock_init(&cq->entry_lock);
	spin_lock_init(&cq->irq_lock);

	/* TODO Physically non-contiguous prp list */
	cq->phys_contig = cmd->cq_flags & NVME_QUEUE_PHYS_CONTIG ? true : false;
	WARN_ON(!cq->phys_contig);

	num_pages = DIV_ROUND_UP(cq->queue_size * sizeof(struct nvme_completion), PAGE_SIZE);
	cq->cq = kzalloc(sizeof(struct nvme_completion *) * num_pages, GFP_KERNEL);
	for (i = 0; i < num_pages; i++) {
		cq->cq[i] = prp_address_offset(cmd->prp1, i);
		if (!cq->cq[i]) {
			NVMEV_ERROR("CREATE_CQ prp invalid: prp1=0x%llx page=%u\n",
				    cmd->prp1, i);
			kfree(cq->cq);
			kfree(cq);
			nvmev_admin_complete(queue, cq_head, eid, NVME_SC_INVALID_FIELD);
			return;
		}
	}

	nvmev_vdev->cqes[cq->qid] = cq;

	NVMEV_ERROR("CREATE_CQ qid=%u qsize=%u irq_en=%d irq_vec=%u\n",
		    cmd->cqid, cmd->qsize + 1,
		    !!(cmd->cq_flags & NVME_CQ_IRQ_ENABLED), cmd->irq_vector);

	dbs_idx = cq->qid * 2 + 1;
	nvmev_vdev->dbs[dbs_idx] = nvmev_vdev->old_dbs[dbs_idx] = 0;

	cq_entry(cq_head).command_id = cmd->command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;
	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
}

static void __nvmev_admin_delete_cq(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = nvmev_vdev->admin_q;
	struct nvmev_completion_queue *cq;
	unsigned int qid;

	qid = sq_entry(eid).delete_queue.qid;

	cq = nvmev_vdev->cqes[qid];
	nvmev_vdev->cqes[qid] = NULL;

	if (cq) {
		kfree(cq->cq);
		kfree(cq);
	}

	cq_entry(cq_head).command_id = sq_entry(eid).delete_queue.command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;
	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
}

static void __nvmev_admin_create_sq(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = nvmev_vdev->admin_q;
	struct nvmev_submission_queue *sq;
	struct nvme_create_sq *cmd = &sq_entry(eid).create_sq;
	unsigned int num_pages, i;
	int dbs_idx;

	sq = kzalloc(sizeof(struct nvmev_submission_queue), GFP_KERNEL);

	sq->qid = cmd->sqid;
	sq->cqid = cmd->cqid;

	sq->sq_priority = cmd->sq_flags & 0xFFFE;
	sq->queue_size = cmd->qsize + 1;
	atomic_set(&sq->refcnt, 1);
	init_waitqueue_head(&sq->ref_wait);
	sq->deleting = false;

	/* TODO Physically non-contiguous prp list */
	sq->phys_contig = (cmd->sq_flags & NVME_QUEUE_PHYS_CONTIG) ? true : false;
	WARN_ON(!sq->phys_contig);

	num_pages = DIV_ROUND_UP(sq->queue_size * sizeof(struct nvme_command), PAGE_SIZE);
	sq->sq = kzalloc(sizeof(struct nvme_command *) * num_pages, GFP_KERNEL);

	for (i = 0; i < num_pages; i++) {
		sq->sq[i] = prp_address_offset(cmd->prp1, i);
		if (!sq->sq[i]) {
			NVMEV_ERROR("CREATE_SQ prp invalid: prp1=0x%llx page=%u\n",
				    cmd->prp1, i);
			kfree(sq->sq);
			kfree(sq);
			nvmev_admin_complete(queue, cq_head, eid, NVME_SC_INVALID_FIELD);
			return;
		}
	}
	nvmev_vdev->sqes[sq->qid] = sq;

	dbs_idx = sq->qid * 2;
	nvmev_vdev->dbs[dbs_idx] = 0;
	nvmev_vdev->old_dbs[dbs_idx] = 0;

	NVMEV_DEBUG("%s: %d\n", __func__, sq->qid);
	NVMEV_ERROR("CREATE_SQ qid=%u cqid=%u qsize=%u flags=0x%x\n",
		    cmd->sqid, cmd->cqid, cmd->qsize + 1, cmd->sq_flags);

	cq_entry(cq_head).command_id = cmd->command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;
	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
}

static void __nvmev_admin_delete_sq(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = nvmev_vdev->admin_q;
	struct nvmev_submission_queue *sq;
	unsigned int qid;

	qid = sq_entry(eid).delete_queue.qid;

	sq = nvmev_vdev->sqes[qid];
	if (sq) {
		WRITE_ONCE(sq->deleting, true);
		smp_mb();
	}
	nvmev_vdev->sqes[qid] = NULL;

	if (sq) {
		nvmev_sq_put(sq);
		wait_event(sq->ref_wait, atomic_read(&sq->refcnt) == 0);
		kfree(sq->sq);
		kfree(sq);
	}

	cq_entry(cq_head).command_id = sq_entry(eid).delete_queue.command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;
	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
}

static void __nvmev_admin_identify_ctrl(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = nvmev_vdev->admin_q;
	struct nvme_id_ctrl *ctrl;

	ctrl = prp_address_len(sq_entry(eid).identify.prp1, sizeof(*ctrl));
	if (!ctrl)
		goto invalid_prp;
	NVMEV_ADMIN_LOG("identify ctrl: prp1=0x%llx kaddr=%px len=%zu\n",
			(unsigned long long)sq_entry(eid).identify.prp1,
			ctrl, sizeof(*ctrl));
	memset(ctrl, 0x00, sizeof(*ctrl));

	ctrl->nn = nvmev_vdev->nr_ns;
	ctrl->oncs = 0; //optional command
	ctrl->acl = 3; //minimum 4 required, 0's based value
	ctrl->vwc = 0;
	snprintf(ctrl->sn, sizeof(ctrl->sn), "CSL_Virt_SN_%02d", 1);
	snprintf(ctrl->mn, sizeof(ctrl->mn), "CSL_Virt_MN_%02d", 1);
	snprintf(ctrl->fr, sizeof(ctrl->fr), "CSL_%03d", 2);
	ctrl->mdts = nvmev_vdev->mdts;
	ctrl->sqes = 0x66;
	ctrl->cqes = 0x44;

	nvmev_admin_complete(queue, cq_head, eid, NVME_SC_SUCCESS);
	return;

invalid_prp:
	NVMEV_ERROR("Invalid PRP for identify ctrl: prp1=0x%llx\n",
		    sq_entry(eid).identify.prp1);
	nvmev_admin_complete(queue, cq_head, eid, NVME_SC_INVALID_FIELD);
}

static void __nvmev_admin_get_log_page(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = nvmev_vdev->admin_q;
	struct nvme_get_log_page_command *cmd = &sq_entry(eid).get_log_page;
	void *page;
	uint32_t len = ((((uint32_t)cmd->numdu << 16) | cmd->numdl) + 1) << 2;

	page = prp_address_len(cmd->prp1, len);
	if (!page) {
		NVMEV_ERROR("Invalid PRP for log page: prp1=0x%llx len=%u\n",
			    cmd->prp1, len);
		goto invalid_prp;
	}
	NVMEV_ADMIN_LOG("get_log_page: lid=0x%x prp1=0x%llx len=%u kaddr=%px\n",
			cmd->lid, (unsigned long long)cmd->prp1, len, page);

	switch (cmd->lid) {
	case NVME_LOG_SMART: {
		static const struct nvme_smart_log smart_log = {
			.critical_warning = 0,
			.spare_thresh = 20,
			.host_reads[0] = cpu_to_le64(0),
			.host_writes[0] = cpu_to_le64(0),
			.num_err_log_entries[0] = cpu_to_le64(0),
			.temperature[0] = 0 & 0xff,
			.temperature[1] = (0 >> 8) & 0xff,
		};

		NVMEV_INFO("Handling NVME_LOG_SMART\n");

		NVMEV_ADMIN_LOG("log_page copy: lid=0x%x len=%u kaddr=%px\n",
				cmd->lid, len, page);
		__memcpy(page, &smart_log, len);
		break;
	}
	case NVME_LOG_CMD_EFFECTS: {
		static const struct nvme_effects_log effects_log = {
			.acs = {
				[nvme_admin_get_log_page] = cpu_to_le32(NVME_CMD_EFFECTS_CSUPP),
				[nvme_admin_identify] = cpu_to_le32(NVME_CMD_EFFECTS_CSUPP),
				// [nvme_admin_abort_cmd] = cpu_to_le32(NVME_CMD_EFFECTS_CSUPP),
				[nvme_admin_set_features] = cpu_to_le32(NVME_CMD_EFFECTS_CSUPP),
				[nvme_admin_get_features] = cpu_to_le32(NVME_CMD_EFFECTS_CSUPP),
				[nvme_admin_async_event] = cpu_to_le32(NVME_CMD_EFFECTS_CSUPP),
				// [nvme_admin_keep_alive] = cpu_to_le32(NVME_CMD_EFFECTS_CSUPP),
			},
			.iocs = {
#if 0  // Force disabled ZNS support
				/*
				 * Zone Append is unsupported at the moment, but we fake it so that
				 * Linux device driver doesn't lock it to R/O.
				 *
				 * A zone append command will result in device failure.
				 */
				[nvme_cmd_zone_append] = cpu_to_le32(NVME_CMD_EFFECTS_CSUPP),
				[nvme_cmd_zone_mgmt_send] = cpu_to_le32(NVME_CMD_EFFECTS_CSUPP | NVME_CMD_EFFECTS_LBCC),
				[nvme_cmd_zone_mgmt_recv] = cpu_to_le32(NVME_CMD_EFFECTS_CSUPP),
#endif
			},
			.resv = { 0, },
		};

		NVMEV_INFO("Handling NVME_LOG_CMD_EFFECTS\n");

		NVMEV_ADMIN_LOG("log_page copy: lid=0x%x len=%u kaddr=%px\n",
				cmd->lid, len, page);
		__memcpy(page, &effects_log, len);
		break;
	}
	default:
		/*
		 * The NVMe protocol mandates several commands (lid) to be implemented, but some
		 * aren't in NVMeVirt.
		 *
		 * As the NVMe host device driver will always assume that the device will return
		 * the correct values, blindly memset'ing the return buffer will always result in
		 * heavy system malfunction due to incorrect memory dereferences.
		 *
		 * Warn the users and make it perfectly clear that this needs to be implemented.
		 */
		NVMEV_ERROR("Unimplemented log page identifier: 0x%hhx,"
			    "the system will be unstable!\n",
			    cmd->lid);
		NVMEV_ADMIN_LOG("log_page memset: lid=0x%x len=%u kaddr=%px\n",
				cmd->lid, len, page);
		__memset(page, 0, len);
		break;
	}

	nvmev_admin_complete(queue, cq_head, eid, NVME_SC_SUCCESS);
	return;

invalid_prp:
	nvmev_admin_complete(queue, cq_head, eid, NVME_SC_INVALID_FIELD);
}

static void __nvmev_admin_identify_namespace(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = nvmev_vdev->admin_q;
	struct nvme_id_ns *ns;
	struct nvme_identify *cmd = &sq_entry(eid).identify;
	size_t nsid = cmd->nsid - 1;
	NVMEV_DEBUG("[%s] \n", __FUNCTION__);

	ns = prp_address_len(cmd->prp1, PAGE_SIZE);
	if (!ns)
		goto invalid_prp;
	NVMEV_ADMIN_LOG("identify ns: nsid=%u prp1=0x%llx kaddr=%px len=%lu\n",
			cmd->nsid, (unsigned long long)cmd->prp1, ns,
			(unsigned long)PAGE_SIZE);
	memset(ns, 0x0, PAGE_SIZE);

	ns->lbaf[0].ms = 0;
	ns->lbaf[0].ds = 9;
	ns->lbaf[0].rp = NVME_LBAF_RP_GOOD;

	ns->lbaf[1].ms = 8;
	ns->lbaf[1].ds = 9;
	ns->lbaf[1].rp = NVME_LBAF_RP_GOOD;

	ns->lbaf[2].ms = 16;
	ns->lbaf[2].ds = 9;
	ns->lbaf[2].rp = NVME_LBAF_RP_GOOD;

	ns->lbaf[3].ms = 0;
	ns->lbaf[3].ds = 12;
	ns->lbaf[3].rp = NVME_LBAF_RP_BEST;

	ns->lbaf[4].ms = 8;
	ns->lbaf[4].ds = 12;
	ns->lbaf[4].rp = NVME_LBAF_RP_BEST;

	ns->lbaf[5].ms = 64;
	ns->lbaf[5].ds = 12;
	ns->lbaf[5].rp = NVME_LBAF_RP_BEST;

	ns->lbaf[6].ms = 128;
	ns->lbaf[6].ds = 12;
	ns->lbaf[6].rp = NVME_LBAF_RP_BEST;

	ns->nsze = (nvmev_vdev->ns[nsid].size >> ns->lbaf[ns->flbas].ds);

	ns->ncap = ns->nsze;
	ns->nuse = ns->nsze;
	ns->nlbaf = 6;
	ns->flbas = 0;
	ns->dps = 0;

	nvmev_admin_complete(queue, cq_head, eid, NVME_SC_SUCCESS);
	return;

invalid_prp:
	NVMEV_ERROR("Invalid PRP for identify ns: prp1=0x%llx\n",
		    cmd->prp1);
	nvmev_admin_complete(queue, cq_head, eid, NVME_SC_INVALID_FIELD);
}

static void __nvmev_admin_identify_namespaces(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = nvmev_vdev->admin_q;
	struct nvme_identify *cmd = &sq_entry(eid).identify;
	unsigned int *ns;
	int i;

	NVMEV_DEBUG("[%s] ns %d\n", __FUNCTION__, cmd->nsid);

	ns = prp_address_len(cmd->prp1, PAGE_SIZE * 2);
	if (!ns)
		goto invalid_prp;
	NVMEV_ADMIN_LOG("identify ns list: nsid=%u prp1=0x%llx kaddr=%px len=%lu\n",
			cmd->nsid, (unsigned long long)cmd->prp1, ns,
			(unsigned long)(PAGE_SIZE * 2));
	memset(ns, 0x00, PAGE_SIZE * 2);

	for (i = 1; i <= nvmev_vdev->nr_ns; i++) {
		if (i > cmd->nsid) {
			NVMEV_DEBUG("[%s] ns %d %px\n", __FUNCTION__, i, ns);
			*ns = i;
			ns++;
		}
	}

	nvmev_admin_complete(queue, cq_head, eid, NVME_SC_SUCCESS);
	return;

invalid_prp:
	NVMEV_ERROR("Invalid PRP for identify ns list: prp1=0x%llx\n",
		    cmd->prp1);
	nvmev_admin_complete(queue, cq_head, eid, NVME_SC_INVALID_FIELD);
}

static void __nvmev_admin_identify_namespace_desc(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = nvmev_vdev->admin_q;
	struct nvme_identify *cmd = &sq_entry(eid).identify;
	struct nvme_id_ns_desc *ns_desc;
	int nsid = cmd->nsid - 1;

	NVMEV_DEBUG("[%s] ns %d\n", __FUNCTION__, cmd->nsid);

	ns_desc = prp_address_len(cmd->prp1, sizeof(*ns_desc));
	if (!ns_desc)
		goto invalid_prp;
	NVMEV_ADMIN_LOG("identify ns desc: nsid=%u prp1=0x%llx kaddr=%px len=%zu\n",
			cmd->nsid, (unsigned long long)cmd->prp1, ns_desc,
			sizeof(*ns_desc));
	memset(ns_desc, 0x00, sizeof(*ns_desc));

	ns_desc->nidt = NVME_NIDT_CSI;
	ns_desc->nidl = 1;

	ns_desc->nid[0] = nvmev_vdev->ns[nsid].csi; // Zoned Name Space Command Set

	nvmev_admin_complete(queue, cq_head, eid, NVME_SC_SUCCESS);
	return;

invalid_prp:
	NVMEV_ERROR("Invalid PRP for identify ns desc: prp1=0x%llx\n",
		    cmd->prp1);
	nvmev_admin_complete(queue, cq_head, eid, NVME_SC_INVALID_FIELD);
}

#if 0  // Force disabled ZNS support - ZNS functions completely removed
static void __nvmev_admin_identify_zns_namespace(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = nvmev_vdev->admin_q;
	struct nvme_identify *cmd = &sq_entry(eid).identify;
	struct nvme_id_zns_ns *ns;
	int nsid = cmd->nsid - 1;
	struct zns_ftl *zns_ftl = (struct zns_ftl *)nvmev_vdev->ns[nsid].ftls;
	struct znsparams *zpp = &zns_ftl->zp;

	NVMEV_ASSERT(nvmev_vdev->ns[nsid].csi == NVME_CSI_ZNS);
	NVMEV_DEBUG("%s\n", __func__);

	ns = prp_address(cmd->prp1);
	memset(ns, 0x00, sizeof(*ns));

	ns->zoc = 0; //currently not support variable zone capacity
	ns->ozcs = 0;
	ns->mar = zpp->nr_active_zones - 1; // 0-based

	ns->mor = zpp->nr_open_zones - 1; // 0-based

	/* zrwa enabled */
	if (zpp->nr_zrwa_zones > 0) {
		ns->ozcs |= OZCS_ZRWA; //Support ZRWA

		ns->numzrwa = zpp->nr_zrwa_zones - 1;

		ns->zrwafg = zpp->zrwafg_size;

		ns->zrwasz = zpp->zrwa_size;

		ns->zrwacap = 0; // explicit zrwa flush
		ns->zrwacap |= ZRWACAP_EXPFLUSHSUP;
	}
	// Zone Size
	ns->lbaf[0].zsze = BYTE_TO_LBA(zpp->zone_size);

	// Zone Descriptor Extension Size
	ns->lbaf[0].zdes = 0; // currently not support

	cq_entry(cq_head).command_id = sq_entry(eid).features.command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;
	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
}

static void __nvmev_admin_identify_zns_ctrl(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = nvmev_vdev->admin_q;
	struct nvme_identify *cmd = &sq_entry(eid).identify;
	struct nvme_id_zns_ctrl *res;

	NVMEV_DEBUG("%s\n", __func__);

	res = prp_address(cmd->prp1);

	res->zasl = 0; // currently not support zone append command

	cq_entry(cq_head).command_id = cmd->command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;
	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
}
#endif

static void __nvmev_admin_set_features(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = nvmev_vdev->admin_q;

	NVMEV_DEBUG("%s: %x\n", __func__, sq_entry(eid).features.fid);

	switch (sq_entry(eid).features.fid) {
	case NVME_FEAT_ARBITRATION:
	case NVME_FEAT_POWER_MGMT:
	case NVME_FEAT_LBA_RANGE:
	case NVME_FEAT_TEMP_THRESH:
	case NVME_FEAT_ERR_RECOVERY:
	case NVME_FEAT_VOLATILE_WC:
		break;
	case NVME_FEAT_NUM_QUEUES: {
		int req_sq;
		int req_cq;

		// # of sq in 0-base
		req_sq = (sq_entry(eid).features.dword11 & 0xFFFF) + 1;
		nvmev_vdev->nr_sq = min(req_sq, NR_MAX_IO_QUEUE);

		// # of cq in 0-base
		req_cq = ((sq_entry(eid).features.dword11 >> 16) & 0xFFFF) + 1;
		nvmev_vdev->nr_cq = min(req_cq, NR_MAX_IO_QUEUE);

		NVMEV_ERROR("NUM_QUEUES req sq=%d cq=%d -> use sq=%u cq=%u msix=%d\n",
			    req_sq, req_cq, nvmev_vdev->nr_sq, nvmev_vdev->nr_cq,
			    nvmev_vdev->msix_enabled);

		cq_entry(cq_head).result0 =
			((nvmev_vdev->nr_cq - 1) << 16 | (nvmev_vdev->nr_sq - 1));
		break;
	}
	case NVME_FEAT_IRQ_COALESCE:
	case NVME_FEAT_IRQ_CONFIG:
	case NVME_FEAT_WRITE_ATOMIC:
	case NVME_FEAT_ASYNC_EVENT:
	case NVME_FEAT_AUTO_PST:
	case NVME_FEAT_SW_PROGRESS:
	case NVME_FEAT_HOST_ID:
	case NVME_FEAT_RESV_MASK:
	case NVME_FEAT_RESV_PERSIST:
	default:
		break;
	}

	cq_entry(cq_head).command_id = sq_entry(eid).features.command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;
	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
}

static void __nvmev_admin_get_features(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = nvmev_vdev->admin_q;

	NVMEV_ERROR("Get features not implemented: fid=%u\n", sq_entry(eid).features.fid);
	nvmev_admin_complete_err(queue, cq_head, eid, NVME_SC_INVALID_FIELD);
}

static void __nvmev_proc_admin_req(int entry_id)
{
	struct nvmev_admin_queue *queue = nvmev_vdev->admin_q;
	int cq_head = queue->cq_head;
	int cns;

	NVMEV_DEBUG("%s: %x %d %d %d\n", __func__, sq_entry(entry_id).identify.opcode, entry_id,
		    sq_entry(entry_id).common.command_id, cq_head);
	NVMEV_ADMIN_LOG("admin req: opcode=0x%x cid=%u sq_head=%d cq_head=%d phase=%d prp1=0x%llx prp2=0x%llx nsid=%u\n",
			sq_entry(entry_id).common.opcode,
			sq_entry(entry_id).common.command_id,
			entry_id, cq_head, queue->phase,
			(unsigned long long)sq_entry(entry_id).common.prp1,
			(unsigned long long)sq_entry(entry_id).common.prp2,
			sq_entry(entry_id).identify.nsid);

	/* Default to invalid opcode to avoid phantom completions */
	nvmev_admin_complete_err(queue, cq_head, entry_id, NVME_SC_INVALID_OPCODE);

	switch (sq_entry(entry_id).common.opcode) {
	case nvme_admin_delete_sq:
		__nvmev_admin_delete_sq(entry_id, cq_head);
		break;
	case nvme_admin_create_sq:
		__nvmev_admin_create_sq(entry_id, cq_head);
		break;
	case nvme_admin_get_log_page:
		__nvmev_admin_get_log_page(entry_id, cq_head);
		break;
	case nvme_admin_delete_cq:
		__nvmev_admin_delete_cq(entry_id, cq_head);
		break;
	case nvme_admin_create_cq:
		__nvmev_admin_create_cq(entry_id, cq_head);
		break;
	case nvme_admin_identify:
		cns = sq_entry(entry_id).identify.cns;
		switch (cns) {
		case 0x00:
			__nvmev_admin_identify_namespace(entry_id, cq_head);
			break;
		case 0x01:
			__nvmev_admin_identify_ctrl(entry_id, cq_head);
			break;
		case 0x02:
			__nvmev_admin_identify_namespaces(entry_id, cq_head);
			break;
		case 0x03:
			__nvmev_admin_identify_namespace_desc(entry_id, cq_head);
			break;
#if 0  // Force disabled ZNS support
		case 0x05:
			__nvmev_admin_identify_zns_namespace(entry_id, cq_head);
			break;
		case 0x06:
			__nvmev_admin_identify_zns_ctrl(entry_id, cq_head);
			break;
#endif
		default:
			NVMEV_ERROR("I don't know %d\n", cns);
			nvmev_admin_complete_err(queue, cq_head, entry_id, NVME_SC_INVALID_FIELD);
		}
		break;
	case nvme_admin_abort_cmd:
		NVMEV_ERROR("Abort cmd not implemented\n");
		nvmev_admin_complete_err(queue, cq_head, entry_id, NVME_SC_INVALID_OPCODE);
		break;
	case nvme_admin_set_features:
		__nvmev_admin_set_features(entry_id, cq_head);
		break;
	case nvme_admin_get_features:
		__nvmev_admin_get_features(entry_id, cq_head);
		break;
	case nvme_admin_async_event:
		cq_entry(cq_head).command_id = sq_entry(entry_id).features.command_id;
		cq_entry(cq_head).sq_id = 0;
		cq_entry(cq_head).sq_head = entry_id;
		cq_entry(cq_head).result0 = 0;
		cq_entry(cq_head).status = queue->phase | NVME_SC_ASYNC_LIMIT << 1;
		break;
	case nvme_admin_activate_fw:
	case nvme_admin_download_fw:
	case nvme_admin_format_nvm:
	case nvme_admin_security_send:
	case nvme_admin_security_recv:
	default:
		NVMEV_ERROR("Unhandled admin requests: %d", sq_entry(entry_id).common.opcode);
		break;
	}

	if (++cq_head == queue->cq_depth) {
		cq_head = 0;
		queue->phase = !queue->phase;
	}

	queue->cq_head = cq_head;
}

void nvmev_proc_admin_sq(int new_db, int old_db)
{
	struct nvmev_admin_queue *queue = nvmev_vdev->admin_q;
	int num_proc = new_db - old_db;
	int curr = old_db;
	int seq;

	if (!queue || !queue->nvme_sq || !queue->nvme_cq ||
	    queue->sq_depth <= 0 || queue->cq_depth <= 0)
		return;

	if (!nvmev_vdev->bar->cc.en || !nvmev_vdev->bar->csts.rdy)
		return;

	if (num_proc < 0)
		num_proc += queue->sq_depth;

	/* Ensure SQ entries are visible after the host updates doorbell */
	dma_rmb();

	for (seq = 0; seq < num_proc; seq++) {
		__nvmev_proc_admin_req(curr++);

		if (curr == queue->sq_depth) {
			curr = 0;
		}
	}

	/* Ensure PRP writes and CQEs are visible before raising IRQ */
	wmb();
	nvmev_signal_irq(0);
}

void nvmev_proc_admin_cq(int new_db, int old_db)
{
} 
