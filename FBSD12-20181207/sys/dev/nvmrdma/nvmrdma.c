/**********
 TODO:
 1) Use the command cid array to track the allocated containers and use the
    STAILQ field to implement a queue of free containers.  Right now the
    STAILQ field tracks all allocated containers.
 2) !!!Remove field nvmrq_next2use whose only use is testing MR lifecycle!!!
 3) We should be able to remove ib_mr,s or their containing command-containers
    from circulation if they cannot be invalidated correctly
 **********/

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/uuid.h>
#include <sys/conf.h>
#include <sys/systm.h>
#include <sys/module.h>

#include <netinet/in.h>

#include <linux/kernel.h>
#include <linux/netdevice.h>

#include <rdma/rdma_cm.h>
#include <rdma/ib_verbs.h>

#include <dev/nvme/nvme_shared.h>

#define MAX_NVME_RDMA_SEGMENTS 256

typedef struct {
	uint64_t nvmrk_address;
	uint8_t  nvmrk_length[3];
	uint32_t nvmrk_key;
	uint8_t  nvmrk_sgl_identifier;
} __packed nvmr_ksgl_t;


typedef struct {
	uint8_t     nvmf_opc;
	uint8_t     nvmf_sgl_fuse;
	uint16_t    nvmf_cid;
	union {
		struct {
			uint8_t     nvmf_fctype;
			uint8_t     nvmf_resvf1[19];
		};
		struct {
			uint32_t    nvmf_nsid;
			uint8_t     nvmf_resvn1[16];
		};
	};
	nvmr_ksgl_t nvmf_ksgl;
} __packed nvmf_prfx_t;

typedef struct {
	nvmf_prfx_t nvmrsb_nvmf;
	uint8_t     nvmrsb_resv1[24];
} __packed nvmr_stub_t;
CTASSERT(sizeof(nvmr_stub_t) == sizeof(struct nvme_command));

typedef struct {
	nvmf_prfx_t nvmrcn_nvmf;
	uint16_t    nvmrcn_recfmt;
	uint16_t    nvmrcn_qid;
	uint16_t    nvmrcn_sqsize;
	uint8_t     nvmrcn_cattr;
	uint8_t     nvmrcn_resv2;
	uint32_t    nvmrcn_kato;
	uint8_t     nvmrcn_resv3[12];
} __packed nvmr_connect_t;
CTASSERT(sizeof(nvmr_connect_t) == sizeof(struct nvme_command));

typedef struct {
	nvmf_prfx_t nvmrpg_nvmf;
	uint8_t     nvmrpg_attrib;
	uint8_t     nvmrpg_resv1[3];
	uint32_t    nvmrpg_ofst;
	uint8_t     nvmrpg_resv2[16];
} __packed nvmr_propget_t;
CTASSERT(sizeof(nvmr_propget_t) == sizeof(struct nvme_command));

typedef struct {
	nvmf_prfx_t nvmrps_nvmf;
	uint8_t     nvmrps_attrib;
	uint8_t     nvmrps_resv1[3];
	uint32_t    nvmrps_ofst;
	uint64_t    nvmrps_value;
	uint8_t     nvmrps_resv2[8];
} __packed nvmr_propset_t;
CTASSERT(sizeof(nvmr_propset_t) == sizeof(struct nvme_command));

typedef struct {
	nvmf_prfx_t nvmrid_nvmf;
	uint8_t     nvmrid_cns;
	uint8_t     nvmrid_resv1;
	uint16_t    nvmrid_cntid;
	uint8_t     nvmrid_resv2[20];
} __packed nvmr_identify_t;
CTASSERT(sizeof(nvmr_identify_t) == sizeof(struct nvme_command));

typedef union {
	struct nvme_command nvmrcu_nvme;
	nvmr_connect_t      nvmrcu_conn;
	nvmr_propget_t      nvmrcu_prgt;
	nvmr_propset_t      nvmrcu_prst;
	nvmr_identify_t     nvmrcu_idnt;
	nvmr_stub_t         nvmrcu_stub;
} nvmr_communion_t;
CTASSERT(sizeof(nvmr_communion_t) == sizeof(struct nvme_command));

struct nvmr_ncmplcont {
	struct ib_cqe			nvmrsp_cqe;
	STAILQ_ENTRY(nvmr_ncmplcont)	nvmrsp_next;
	struct nvme_completion	        nvmrsp_nvmecmpl;
	u64				nvmrsp_dmaddr;
};
typedef struct nvmr_ncmplcont nvmr_ncmplcon_t;

struct nvmr_ncommcont {
	struct ib_cqe		     nvmrsnd_cqe;
	struct ib_cqe		     nvmrsnd_regcqe;
	STAILQ_ENTRY(nvmr_ncommcont) nvmrsnd_nextfree;
	nvmr_stub_t                 *nvmrsnd_nvmecomm;
	struct ib_mr                *nvmrsnd_mr;
	u64			     nvmrsnd_dmaddr;
	uint16_t                     nvmrsnd_cid;
	struct nvme_request         *nvmrsnd_req;
	bool                         nvmrsnd_rspndd;
	bool                         nvmrsnd_rkeyvalid;
};
typedef struct nvmr_ncommcont nvmr_ncommcon_t;

typedef struct {
	uint16_t nvmrcr_recfmt;
	uint16_t nvmrcr_qid;
	uint16_t nvmrcr_hrqsize;
	uint16_t nvmrcr_hsqsize;
	uint8_t  nvmrcr_resv0[24];
} __packed nvmr_rdma_cm_request_t;
CTASSERT(sizeof(nvmr_rdma_cm_request_t) == 32);

typedef struct {
	uint16_t nvmrcrj_recfmt;
	uint16_t nvmrcrj_sts;
} __packed nvmr_rdma_cm_reject_t;
CTASSERT(sizeof(nvmr_rdma_cm_reject_t) == 4);

static void
nvmr_qphndlr(struct ib_event *ev, void *ctx)
{
	ERRSPEW("Event \"%s\" on QP:%p\n", ib_event_msg(ev->event), ctx);
}


char nvrdma_host_uuid_string[80];
struct uuid nvrdma_host_uuid;

#define MAX_SGS	2
#define CTASSERT_MAX_SGS_LARGE_ENOUGH_FOR(data_structure) \
    CTASSERT(MAX_SGS >= ((sizeof(data_structure)/PAGE_SIZE)+1))

#define MAX_NQN_LEN 255
struct nvmrdma_connect_data {
	struct uuid nvmrcd_hostid;
	uint16_t    nvmrcd_cntlid;
	uint8_t     nvmrcd_resv0[238];
	uint8_t     nvmrcd_subnqn[MAX_NQN_LEN+1];
	uint8_t     nvmrcd_hostnqn[MAX_NQN_LEN+1];
	uint8_t     nvmrcd_resv1[256];
} __packed;
CTASSERT(sizeof(struct nvmrdma_connect_data) == 1024);
CTASSERT_MAX_SGS_LARGE_ENOUGH_FOR(struct nvmrdma_connect_data);

#define NVMR_FOURK (4096)
#define NVMR_DYNANYCNTLID 0xFFFF

#define HOSTNQN_TEMPLATE "nqn.2014-08.org.nvmexpress:uuid:%s"
#define DISCOVERY_SUBNQN "nqn.2014-08.org.nvmexpress.discovery"

#define NVMR_DEFAULT_KATO 0x1D4C0000
#define NVMR_DISCOVERY_KATO 0x0 /* DISCOVERY KATO has to be 0 */
#define NVMF_FCTYPE_PROPSET 0x0
#define NVMF_FCTYPE_CONNECT 0x1
#define NVMF_FCTYPE_PROPGET 0x4
#define NVMR_PSDT_SHIFT 6
#define NVMF_SINGLE_BUF_SGL (0x1 << NVMR_PSDT_SHIFT)
#define NVMF_MULT_SEG_SGL   (0x2 << NVMR_PSDT_SHIFT)
#define NVMF_KEYED_SGL_NO_INVALIDATE 0x40
#define NVMF_KEYED_SGL_INVALIDATE    0x4F

static void
nvmr_rg_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct ib_cqe *rcve;

	rcve = wc->wr_cqe;

	/*
	DBGSPEW("rcve:%p, wc_status:\"%s\"\n", rcve,
	    ib_wc_status_msg(wc->status));
	 */
}

static void
nvmr_snd_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct ib_cqe *rcve;

	rcve = wc->wr_cqe;

	/*
	DBGSPEW("rcve:%p, wc_status:\"%s\"\n", rcve,
	    ib_wc_status_msg(wc->status));
	 */
}




#define NUMIPV4OCTETS 4
typedef uint8_t nvmripv4_t[NUMIPV4OCTETS];

struct nvmr_cntrlr_tag;
typedef void (*nvmr_crtcntrlrcb_t)(struct nvmr_cntrlr_tag *cntrlr);

typedef struct {
	int      nvmrqp_numqueues;
	int      nvmrqp_numsndqe;
	int      nvmrqp_numrcvqe;
	uint32_t nvmrqp_kato;
} nvmr_qprof_t;

typedef enum {
	NVMR_QTYPE_ADMIN = 0,
	NVMR_QTYPE_IO,
	NVMR_NUM_QTYPES
} nvmr_qndx_t;

static char *nvmr_qndxnames[NVMR_NUM_QTYPES] = {
	[NVMR_QTYPE_ADMIN] = "Admin Q",
	[NVMR_QTYPE_IO] = "I/O Q",
};

static inline char *
nvmr_qndx2name(nvmr_qndx_t qndx)
{
	KASSERT(qndx < NVMR_NUM_QTYPES, ("qndx:%d", qndx));
	return nvmr_qndxnames[qndx];
}

typedef struct {
	nvmr_qprof_t	   nvmrp_qprofs[NVMR_NUM_QTYPES];
} nvmr_cntrlrprof_t;

typedef enum {
	NVMRQ_PRE_INIT = 0,
	NVMRQ_PRE_ADDR_RESOLV,
	NVMRQ_ADDR_RESOLV_FAILED,
	NVMRQ_ADDR_RESOLV_SUCCEEDED,
	NVMRQ_PRE_ROUTE_RESOLV,
	NVMRQ_ROUTE_RESOLV_FAILED,
	NVMRQ_ROUTE_RESOLV_SUCCEEDED,
	NVMRQ_PRE_CONNECT,
	NVMRQ_CONNECT_FAILED,
	NVMRQ_CONNECT_SUCCEEDED,
} nvmr_queue_state_t;

typedef struct {
	struct rdma_cm_id             *nvmrq_cmid;
	struct nvmr_cntrlr_tag        *nvmrq_cntrlr; /* Owning Controller     */
	struct ib_pd                  *nvmrq_ibpd;
	struct ib_cq                  *nvmrq_ibcq;
	struct ib_qp                  *nvmrq_ibqp;
	nvmr_qprof_t                  *nvmrq_prof;
	nvmr_ncommcon_t              **nvmrq_commcid; /* CID > nvmr_ncommcont */
	STAILQ_HEAD(, nvmr_ncommcont)  nvmrq_comms;
	STAILQ_HEAD(, nvmr_ncmplcont)  nvmrq_cmpls;
	STAILQ_HEAD(, nvme_request)    nvmrq_defreqs;
	volatile nvmr_queue_state_t    nvmrq_state;  /* nvmrctr_nvmec.lockc */
	int                            nvmrq_last_cm_status;
	uint16_t                       nvmrq_numsndqe;/* nvmrq_comms count    */
	uint16_t                       nvmrq_numrcvqe;/* nvmrq_cmpls count    */
	uint16_t                       nvmrq_numFsndqe;/* nvmrq_comms count   */
	uint16_t                       nvmrq_numFrcvqe;/* nvmrq_cmpls count   */
	struct scatterlist             nvmrq_scl[MAX_NVME_RDMA_SEGMENTS];

	struct nvme_qpair              nvmrq_gqp;
	nvmr_ncommcon_t               *nvmrq_next2use; /* !! REMOVE THIS !!   */
} *nvmr_qpair_t;


/*
 * An NVMr controller requires multiple memory allocations and n/w
 * interactions to be fully setup.  An initial admin QP is created which
 * is used to interrogate the properties of the remote NVMr controller.
 * Thereafter additional IO QPs are created. As soon as the admin QP is even
 * partly setup it can receive async events from the RDMA stack about
 * disconnects or RNIC device removals.  These removals should trigger a
 * teardown of the controller that could still be initializing.
 *
 * The same events can be generated once the controller has initialized.
 *
 * To deal with all the resulting scenarios a state variable has been introduced
 *
 * A taskqueue has been introduced to handle destroying a controller.
 * Controllers will not be tossed into this queue on a destruction event
 * if it is determined that it is still being initialized (NVMRC_PRE_INIT).
 * However they will be moved to the NVMRC_CONDEMNED state.
 * The NVMRC_CONDEMNED serves two purposes:
 * 1) Destruction/condemn events can work out that a previous condemn event has
 *    happened on a controller and that nothing more has to be done to
 *    condemn it.
 * 2) The initialization logic in nvmr_cntrlr_create() monitors the state to
 *    check if a condemn event has happened on a controller it is initializing.
 *    If so the initialization is aborted and the initialization logic
 *    tosses the controller onto the condemned taskqueue for destruction
 */
typedef enum {
	NVMRC_PRE_INIT = 0,
	NVMRC_INITED,
	NVMRC_CONDEMNED
} nvmr_cntrlr_state_t;

typedef struct nvmr_cntrlr_tag {
	char			very_first_field[NVME_VFFSTRSZ+1];
	char              *nvmrctr_subnqn;
	nvmr_qpair_t       nvmrctr_adminqp;
	nvmr_qpair_t      *nvmrctr_ioqarr;  /* Array size determined by prof */
	nvmr_cntrlrprof_t *nvmrctr_prof;
	nvmripv4_t         nvmrctr_ipv4;
	int                nvmrctr_numioqs; /* count not always fixed in prof */
	uint16_t           nvmrctr_port;
	struct nvme_controller nvmrctr_nvmec;
	volatile nvmr_cntrlr_state_t nvmrctr_state;  /* nvmrctr_nvmec.lockc */
	TAILQ_ENTRY(nvmr_cntrlr_tag) nvmrctr_nxt;
	boolean_t          nvmrctr_nvmereg;
} *nvmr_cntrlr_t;

#define NVMR_NUMSNDSGE (1 + 1) /* NVMe Command  + Inline data */
#define NVMR_NUMRCVSGE 1       /* NVMe Completion */

#define MAX_ADMINQ_ELEMENTS 32
#define MAX_IOQ_ELEMENTS 32

nvmr_cntrlrprof_t nvmr_regularprof = {
	.nvmrp_qprofs[NVMR_QTYPE_ADMIN] = {
		.nvmrqp_numqueues = 1,
		.nvmrqp_numsndqe = MAX_ADMINQ_ELEMENTS - 1,
		.nvmrqp_numrcvqe = MAX_ADMINQ_ELEMENTS,
		.nvmrqp_kato = NVMR_DEFAULT_KATO,
	},
	.nvmrp_qprofs[NVMR_QTYPE_IO] = {
		.nvmrqp_numqueues = 1,
		.nvmrqp_numsndqe = MAX_IOQ_ELEMENTS - 1,
		.nvmrqp_numrcvqe = MAX_IOQ_ELEMENTS,
		.nvmrqp_kato = NVMR_DEFAULT_KATO,
	},
};

static MALLOC_DEFINE(M_NVMR, "nvmr", "nvmr");

int nvmr_command_async(nvmr_qpair_t q, struct nvme_request *req);
void
nvmr_ctrlr_post_failed_request(nvmr_cntrlr_t cntrlr, struct nvme_request *req);

void nvmr_cleanup_q_next(nvmr_qpair_t q, struct nvmr_ncommcont *commp);
void
nvmr_cleanup_q_next(nvmr_qpair_t q, struct nvmr_ncommcont *commp)
{
	int retval;
	struct nvme_request *req;

	nvme_free_request(commp->nvmrsnd_req);
	commp->nvmrsnd_req = NULL;

	mtx_lock(&q->nvmrq_gqp.qlock);

	STAILQ_INSERT_HEAD(&q->nvmrq_comms, commp, nvmrsnd_nextfree);

	retval = EDOOFUS;
	while (retval != 0) {
		req = STAILQ_FIRST(&q->nvmrq_defreqs);
		if (req == NULL) {
			break;
		}

		STAILQ_REMOVE(&q->nvmrq_defreqs, req, nvme_request, stailq);
		retval = nvmr_command_async(q, req);
		if (retval != 0) {
			mtx_unlock(&q->nvmrq_gqp.qlock);
			ERRSPEW("Failing req: %d\n", retval);
			nvmr_ctrlr_post_failed_request(q->nvmrq_cntrlr, req);
			mtx_lock(&q->nvmrq_gqp.qlock);
		}
	}

	mtx_unlock(&q->nvmrq_gqp.qlock);
}


static void
nvmr_localinv_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct nvmr_ncommcont *commp;
	nvmr_qpair_t q;

	q = cq->cq_context;
	commp = container_of(wc->wr_cqe, struct nvmr_ncommcont, nvmrsnd_regcqe);

	if (wc->status != IB_WC_SUCCESS) {
		ERRSPEW("Local rKey invalidation failed q:%p commp:%p r:%x\n",
		    q, commp, commp->nvmrsnd_mr->rkey);
		/* Bail for now, wake waiters */
	}

	nvmr_cleanup_q_next(q, commp);
}


void nvmr_nreq_compl(nvmr_qpair_t q, struct nvmr_ncommcont *commp, struct nvme_completion *c);
/**********
 TODO:
 1) Retry logic
 2) Time out and its cancellation
 3) Track in-use comm
 **********/
void
nvmr_nreq_compl(nvmr_qpair_t q, struct nvmr_ncommcont *commp, struct nvme_completion *c)
{
	struct nvme_request *req;
	boolean_t retry, error;

	req = commp->nvmrsnd_req;
	error = nvme_completion_is_error(c);
	retry = error && nvme_completion_is_retry(c); /* Need to check count */

	if (error) {
		nvme_qpair_print_command(&q->nvmrq_gqp, &req->cmd);
		nvme_qpair_print_completion(&q->nvmrq_gqp, c);
	}

	if (retry) {
		ERRSPEW("Not retrying: unimplemented yet! c:%p\n", commp);
	}
	
	if (req->cb_fn) {
		req->cb_fn(req->cb_arg, c);
	}
}

void nvmr_qfail_freecmpl(nvmr_qpair_t q, struct nvmr_ncmplcont *cmplp);
void
nvmr_qfail_freecmpl(nvmr_qpair_t q, struct nvmr_ncmplcont *cmplp)
{
	mtx_lock(&q->nvmrq_gqp.qlock);
	STAILQ_INSERT_HEAD(&q->nvmrq_cmpls, cmplp, nvmrsp_next);
	q->nvmrq_numFrcvqe++;
	if (q->nvmrq_numFrcvqe == q->nvmrq_numrcvqe) {
		wakeup(&q->nvmrq_numFrcvqe);
	}
	mtx_unlock(&q->nvmrq_gqp.qlock);
}


#define Q_IS_FAILED(q) (atomic_load_acq_int(&q->nvmrq_gqp.qis_enabled) == FALSE)

static void
nvmr_recv_cmplhndlr(struct ib_cq *cq, struct ib_wc *wc);

static int
nvmr_post_cmpl(nvmr_qpair_t q, struct nvmr_ncmplcont *cmplp)
{
	struct nvme_completion *ncmp;
	struct ib_recv_wr rcvwr, *badrcvwrp;
	struct ib_sge sgl;
	struct ib_device *ibd;
	int retval;
	struct ib_pd *ibpd;

	if (Q_IS_FAILED(q)) {
		retval = ESHUTDOWN;
		DBGSPEW("Q:%p has failed, freeing %p\n", q, cmplp);
		nvmr_qfail_freecmpl(q, cmplp);
		goto out;
	}

	ibd = q->nvmrq_cmid->device;
	ibpd = q->nvmrq_ibpd;

	ncmp = &cmplp->nvmrsp_nvmecmpl;
	memset(&rcvwr, 0, sizeof(rcvwr));
	memset(&sgl, 0, sizeof(sgl));

	sgl.addr   = cmplp->nvmrsp_dmaddr;
	sgl.length = sizeof(*ncmp);
	sgl.lkey   = ibpd->local_dma_lkey;

	cmplp->nvmrsp_cqe.done = nvmr_recv_cmplhndlr;

	rcvwr.sg_list = &sgl;
	rcvwr.num_sge = 1;
	rcvwr.wr_cqe  = &cmplp->nvmrsp_cqe;
	rcvwr.next    = NULL;

	ib_dma_sync_single_for_device(ibd, cmplp->nvmrsp_dmaddr,
	    sizeof(cmplp->nvmrsp_nvmecmpl), DMA_FROM_DEVICE);
	retval = ib_post_recv(q->nvmrq_ibqp, &rcvwr, &badrcvwrp);

out:
	return retval;
}


static void
nvmr_recv_cmplhndlr(struct ib_cq *cq, struct ib_wc *wc)
{
	struct nvmr_ncmplcont *cmplp;
	struct nvmr_ncommcont *commp;
	struct nvme_completion *c;
	struct ib_send_wr invwr, *badinvwrp;
	nvmr_qpair_t q;
	int retval;

	q = cq->cq_context;
	cmplp = container_of(wc->wr_cqe, struct nvmr_ncmplcont, nvmrsp_cqe);

	switch(wc->status) {
	case IB_WC_SUCCESS:
	case IB_WC_WR_FLUSH_ERR:
		break;
	default:
		ERRSPEW("cmplp:%p, wc_status:\"%s\"\n", cmplp,
		    ib_wc_status_msg(wc->status));
		break;
	}

	ib_dma_sync_single_for_cpu(q->nvmrq_cmid->device, cmplp->nvmrsp_dmaddr,
	    sizeof cmplp->nvmrsp_nvmecmpl, DMA_FROM_DEVICE);

	if (wc->status == IB_WC_WR_FLUSH_ERR) {
		nvmr_qfail_freecmpl(q, cmplp);
		goto out_no_repost;
	}

	if (wc->status != IB_WC_SUCCESS) {
		goto out;
	}

	c = &cmplp->nvmrsp_nvmecmpl;

	if ((c->cid == 0) || (c->cid > q->nvmrq_numsndqe)) {
		ERRSPEW("Returned CID in NVMe completion is not valid "
		    "cid:%hu max:%hu\n", c->cid, q->nvmrq_numsndqe);
		goto out;
	}

	commp = q->nvmrq_commcid[c->cid];
	nvme_completion_swapbytes(c);

	KASSERT(commp->nvmrsnd_req != NULL, ("%s@%d c:%p r:%p", __func__,
	    __LINE__, commp, commp->nvmrsnd_req));
	nvmr_nreq_compl(q, commp, c);

	/*
	 * The NVMeoF spec allows the host to ask the target to send over an
	 * rkey(remote-key)-invalidate-request after it has RDMAed into/from
	 * the host memory.  This improves latency I suppose but is optional
	 */
	if (wc->wc_flags & IB_WC_WITH_INVALIDATE) {
		/*
		 * The target sent over an NVMe completion along with a request
		 * to our RDMA stack to invalidate the rkey we setup for the
		 * corresponding NVMe command.  Confirm the keys match.
		 */
		if (commp->nvmrsnd_mr->rkey != wc->ex.invalidate_rkey) {
			ERRSPEW("Key's don't match! q:%p commp:%p cid:%d "
			    "wkey:%x okey:%x\n", q, commp, c->cid,
			    wc->ex.invalidate_rkey, commp->nvmrsnd_mr->rkey);

			/* Bail for now, wake waiters */
		}
	} else if (commp->nvmrsnd_rkeyvalid) {
		/*
		 * We didn't request invalidation of our RDMA rkey by the
		 * target or it was ignored.  Invalidate the rkey ourselves.
		 */
		memset(&invwr, 0, sizeof(invwr));
		invwr.num_sge = 0;
		invwr.next = NULL;
		invwr.opcode = IB_WR_LOCAL_INV;
		invwr.ex.invalidate_rkey = commp->nvmrsnd_mr->rkey;
		invwr.send_flags = IB_SEND_SIGNALED;
		invwr.wr_cqe = &commp->nvmrsnd_regcqe;
		commp->nvmrsnd_regcqe.done = nvmr_localinv_done;

		retval = ib_post_send(q->nvmrq_ibqp, &invwr, &badinvwrp);
		if (retval != 0) {
			ERRSPEW("Local Invalidate failed! commp:%p cid:%d\n",
			    commp, c->cid);
			/* Bail for now, wake waiters */
		} else {
			DBGSPEW("Not waking so as to invalidate %p %d\n",
			    commp, c->cid);
			/* Continue in nvmr_localinv_done() */
			goto out;
		}
	}

	nvmr_cleanup_q_next(q, commp);

out:
	nvmr_post_cmpl(q, cmplp);

out_no_repost:
	return;
}

void nvmr_qfail_drain(nvmr_qpair_t q, uint16_t *counter, uint16_t count);
void
nvmr_qfail_drain(nvmr_qpair_t q, uint16_t *counter, uint16_t count)
{
	KASSERT(Q_IS_FAILED(q), ("q:%p is still enabled!\n", q));

	KASSERT((counter == &q->nvmrq_numFsndqe) ||
	    (counter == &q->nvmrq_numFrcvqe), ("%s@%d %p is neither of q:%p\n",
	    __func__, __LINE__, counter, q));

	mtx_lock(&q->nvmrq_gqp.qlock);
	while(*counter != count) {
		DBGSPEW("Sleeping for q:%p field:%p to drain\n", q, counter);
		mtx_sleep(counter, &q->nvmrq_gqp.qlock, 0, "qdrain", HZ);
	}
	mtx_unlock(&q->nvmrq_gqp.qlock);
}


static void
nvmr_queue_destroy(nvmr_qpair_t q)
{
	nvmr_ncmplcon_t *cmplp, *tcmplp;
	nvmr_ncommcon_t *commp, *tcommp;
	struct ib_device *ibd;
	int count;

	if (q == NULL) {
		goto out;
	}

	atomic_store_rel_int(&q->nvmrq_gqp.qis_enabled, FALSE);

	ibd = q->nvmrq_cmid->device;

	if (q->nvmrq_state >= NVMRQ_CONNECT_SUCCEEDED) {
		DBGSPEW("Invoking rdma_disconnect(%p)...\n", q->nvmrq_cmid);
		rdma_disconnect(q->nvmrq_cmid);
	}
	if (q->nvmrq_ibqp != NULL) {
		DBGSPEW("Invoking ib_drain_qp(%p)...\n", q->nvmrq_ibqp);
		ib_drain_qp(q->nvmrq_ibqp);

		DBGSPEW("Invoking rdma_destroy_qp(%p)..\n", q->nvmrq_cmid);
		rdma_destroy_qp(q->nvmrq_cmid);
		q->nvmrq_ibqp = NULL;
	}

	if (q->nvmrq_ibcq != NULL) {
		DBGSPEW("Invoking ib_free_cq(%p)...\n", q->nvmrq_ibcq);
		ib_free_cq(q->nvmrq_ibcq);
		q->nvmrq_ibcq = NULL;
	}

	nvmr_qfail_drain(q, &q->nvmrq_numFrcvqe, q->nvmrq_numrcvqe);
	KASSERT(q->nvmrq_numFrcvqe == q->nvmrq_numrcvqe, ("%s@%d q:%p\n",
	    __func__, __LINE__, q));

	/*
	 * Loop through the list of completion container structures unmapping
	 * the NVMe completion structure and then free'ing the containers
	 * themselves
	 */
	count = 0;
	STAILQ_FOREACH_SAFE(cmplp, &q->nvmrq_cmpls, nvmrsp_next, tcmplp) {
		if (cmplp->nvmrsp_dmaddr != 0) {
			ib_dma_sync_single_for_cpu(ibd, cmplp->nvmrsp_dmaddr,
			    sizeof(cmplp->nvmrsp_nvmecmpl), DMA_FROM_DEVICE);
			ib_dma_unmap_single(ibd, cmplp->nvmrsp_dmaddr,
			    sizeof(cmplp->nvmrsp_nvmecmpl), DMA_FROM_DEVICE);
		}
		free(cmplp, M_NVMR);
		count++;
	}
	KASSERT(count == q->nvmrq_numrcvqe, ("%s@%d count:%d numrcvqe:%d",
	    __func__, __LINE__, count, q->nvmrq_numrcvqe));
	DBGSPEW("Freed %d completion containers\n", q->nvmrq_numrcvqe);

	free(q->nvmrq_commcid, M_NVMR);

	nvmr_qfail_drain(q, &q->nvmrq_numFsndqe, q->nvmrq_numsndqe);
	KASSERT(q->nvmrq_numFsndqe == q->nvmrq_numsndqe, ("%s@%d q:%p\n",
	    __func__, __LINE__, q));

	count = 0;
	STAILQ_FOREACH_SAFE(commp, &q->nvmrq_comms, nvmrsnd_nextfree, tcommp) {
		if (commp->nvmrsnd_mr != NULL) {
			ib_dereg_mr(commp->nvmrsnd_mr);
		}
		if (commp->nvmrsnd_dmaddr != 0) {
			ib_dma_sync_single_for_cpu(ibd, commp->nvmrsnd_dmaddr,
			    sizeof(*commp->nvmrsnd_nvmecomm), DMA_FROM_DEVICE);
			ib_dma_unmap_single(ibd, commp->nvmrsnd_dmaddr,
			    sizeof(*commp->nvmrsnd_nvmecomm), DMA_FROM_DEVICE);
		}
		free(commp, M_NVMR);
		count++;
	}
	KASSERT(count == q->nvmrq_numsndqe, ("%s@%d count:%d numsndqe:%d",
	    __func__, __LINE__, count, q->nvmrq_numsndqe));
	DBGSPEW("Freed %d command containers\n", q->nvmrq_numsndqe);

	if (q->nvmrq_ibpd != NULL) {
		DBGSPEW("ib_dealloc_pd(%p)...\n", q->nvmrq_ibpd);
		ib_dealloc_pd(q->nvmrq_ibpd);
		q->nvmrq_ibpd = NULL;
	}

	if (q->nvmrq_cmid != NULL) {
		DBGSPEW("rdma_destroy_id(%p)...\n", q->nvmrq_cmid);
		rdma_destroy_id(q->nvmrq_cmid);
		q->nvmrq_cmid = NULL;
	}

	DBGSPEW("free(%p)ing Q\n", q);
	free(q, M_NVMR);

out:
	return;
}


/* Tracks the number of NVMr controllers */
int nvmr_cntrlrs_count;

TASKQUEUE_DEFINE_THREAD(nvmr_cntrlrs_reaper);

void nvmr_destroy_cntrlrs(void *arg, int pending);
struct task nvmr_destroy_cntrlrs_task =
    TASK_INITIALIZER(0, nvmr_destroy_cntrlrs, NULL);


TAILQ_HEAD(, nvmr_cntrlr_tag) nvmr_cntrlrs =
    TAILQ_HEAD_INITIALIZER(nvmr_cntrlrs);

TAILQ_HEAD(, nvmr_cntrlr_tag) nvmr_condemned_cntrlrs =
    TAILQ_HEAD_INITIALIZER(nvmr_condemned_cntrlrs);

/* Protects the two lists above, as well as nvmr_cntrlrs_count */
struct mtx nvmr_cntrlrs_lstslock;
MTX_SYSINIT(nvmr_cntrlrs_lstslock, &nvmr_cntrlrs_lstslock,
    "NVMr Lists of Controllers Lock", MTX_DEF);


typedef enum {
	NVMRCD_ENQUEUE = 0,
	NVMRCD_SKIP_ENQUEUE
}nvmr_condemned_disposition_t;

/*
 * Atomically, looks at the state of the controller and switches it to the
 * condemned state while deciding to enqueue right away or later
 */
nvmr_condemned_disposition_t nvmr_cntrlr_mark_condemned(nvmr_cntrlr_t cntrlr);
nvmr_condemned_disposition_t
nvmr_cntrlr_mark_condemned(nvmr_cntrlr_t cntrlr)
{
	nvmr_condemned_disposition_t retval;

	mtx_assert(&cntrlr->nvmrctr_nvmec.lockc, MA_OWNED);

	switch(cntrlr->nvmrctr_state) {
	case NVMRC_PRE_INIT:
	case NVMRC_INITED:
		atomic_store_rel_int(&cntrlr->nvmrctr_state, NVMRC_CONDEMNED);
		switch (cntrlr->nvmrctr_state) {
		case NVMRC_PRE_INIT:
			retval = NVMRCD_SKIP_ENQUEUE;
		default:
			retval = NVMRCD_ENQUEUE;
			break;
		}
		break;

	case NVMRC_CONDEMNED:
		retval = NVMRCD_SKIP_ENQUEUE;
	}

	return retval;
}


/*
 * Routine that blindly adds a controller to the condemned queue for destruction
 */
void nvmr_cntrlr_condemn_enqueue(nvmr_cntrlr_t cntrlr);
void
nvmr_cntrlr_condemn_enqueue(nvmr_cntrlr_t cntrlr)
{
	DBGSPEW("Queueing controller:%p for destruction\n", cntrlr);
	mtx_lock(&nvmr_cntrlrs_lstslock);
	TAILQ_REMOVE(&nvmr_cntrlrs, cntrlr, nvmrctr_nxt);
	TAILQ_INSERT_TAIL(&nvmr_condemned_cntrlrs, cntrlr, nvmrctr_nxt);
	DBGSPEW("ctrlr:%p queued for destruction\n", cntrlr);
	mtx_unlock(&nvmr_cntrlrs_lstslock);

	taskqueue_enqueue(taskqueue_nvmr_cntrlrs_reaper,
	    &nvmr_destroy_cntrlrs_task);
}


/*
 * High-level routine invoked when something bad has been detected on the
 * controller and it needs to be destroyed.  It might enqueue for immediate
 * destruction or not based on the current state.
 */
void nvmr_cntrlr_destroy_init(nvmr_cntrlr_t cntrlr);
void
nvmr_cntrlr_destroy_init(nvmr_cntrlr_t cntrlr)
{
	nvmr_condemned_disposition_t retval;

	DBGSPEW("Condemning controller:%p\n", cntrlr);
	mtx_lock(&cntrlr->nvmrctr_nvmec.lockc);
	retval = nvmr_cntrlr_mark_condemned(cntrlr);
	mtx_unlock(&cntrlr->nvmrctr_nvmec.lockc);

	if (retval == NVMRCD_ENQUEUE) {
		nvmr_cntrlr_condemn_enqueue(cntrlr);
	}
}


/*
 * Moves a controller from the pre-init state to the initialized state unless
 * it has been condemned
 */
void nvmr_cntrlr_inited(nvmr_cntrlr_t cntrlr);
void
nvmr_cntrlr_inited(nvmr_cntrlr_t cntrlr)
{
	mtx_lock(&cntrlr->nvmrctr_nvmec.lockc);
	switch(cntrlr->nvmrctr_state) {
	case NVMRC_PRE_INIT:
		atomic_store_rel_int(&cntrlr->nvmrctr_state, NVMRC_INITED);
		break;
	case NVMRC_INITED:
		panic("%s@%d Controller:%p already marked initialized!\n",
		    __func__, __LINE__, cntrlr);
		break;
	case NVMRC_CONDEMNED:
		break;
	}
	mtx_unlock(&cntrlr->nvmrctr_nvmec.lockc);
}


/*
 * This routine runs through the controllers that have not been condemned
 * and queues them for destruction if they are not in the process of being
 * initialized.
 */
void nvmr_all_cntrlrs_condemn_init(void);
void
nvmr_all_cntrlrs_condemn_init(void)
{
	nvmr_cntrlr_t cntrlr, tcntrlr;
	nvmr_condemned_disposition_t retval;

	mtx_lock(&nvmr_cntrlrs_lstslock);
	TAILQ_FOREACH_SAFE(cntrlr, &nvmr_cntrlrs, nvmrctr_nxt, tcntrlr) {
		mtx_lock(&cntrlr->nvmrctr_nvmec.lockc);
		retval = nvmr_cntrlr_mark_condemned(cntrlr);
		mtx_unlock(&cntrlr->nvmrctr_nvmec.lockc);

		if (retval == NVMRCD_SKIP_ENQUEUE) {
			continue;
		}

		TAILQ_REMOVE(&nvmr_cntrlrs, cntrlr, nvmrctr_nxt);
		TAILQ_INSERT_TAIL(&nvmr_condemned_cntrlrs, cntrlr, nvmrctr_nxt);
		DBGSPEW("Queued ctrlr:%p for destruction\n", cntrlr);
	}
	mtx_unlock(&nvmr_cntrlrs_lstslock);

	taskqueue_enqueue(taskqueue_nvmr_cntrlrs_reaper,
	    &nvmr_destroy_cntrlrs_task);
}


static void
nvmr_cntrlr_destroy(nvmr_cntrlr_t cntrlr)
{
	int count;

	DBGSPEW("Controller:%p being destroyed\n", cntrlr);

	atomic_store_rel_int(&cntrlr->nvmrctr_nvmec.is_failed, TRUE);

	if (cntrlr->nvmrctr_nvmereg) {
		nvme_unregister_controller(&cntrlr->nvmrctr_nvmec);
	}

	nvme_notify_fail_consumers(&cntrlr->nvmrctr_nvmec);

	for (count = 0; count < NVME_MAX_NAMESPACES; count++) {
		nvme_ns_destruct(&cntrlr->nvmrctr_nvmec.cns[count]);
	}

	if (cntrlr->nvmrctr_nvmec.ccdev) {
		destroy_dev(cntrlr->nvmrctr_nvmec.ccdev);
	}

	if (cntrlr->nvmrctr_nvmec.taskqueue) {
		taskqueue_free(cntrlr->nvmrctr_nvmec.taskqueue);
	}

	if (cntrlr->nvmrctr_ioqarr != NULL) {
		for (count = 0; count < cntrlr->nvmrctr_numioqs; count++) {
			nvmr_queue_destroy(cntrlr->nvmrctr_ioqarr[count]);
			cntrlr->nvmrctr_ioqarr[count] = NULL;
		}
		DBGSPEW("free(%p)ing Q array\n", cntrlr->nvmrctr_ioqarr);
		free(cntrlr->nvmrctr_ioqarr, M_NVMR);
	}
	nvmr_queue_destroy(cntrlr->nvmrctr_adminqp);

	DBGSPEW("Controller:%p destroyed, but not freed\n", cntrlr);

	return;
}


void
nvmr_destroy_cntrlrs(void *arg, int pending)
{
	nvmr_cntrlr_t cntrlr;
	int count;

	DBGSPEW("Controller destruction task woken up\n");
	count = 0;
	while (cntrlr = TAILQ_FIRST(&nvmr_condemned_cntrlrs), cntrlr != NULL) {
		count++;
		nvmr_cntrlr_destroy(cntrlr);

		mtx_lock(&nvmr_cntrlrs_lstslock);
		TAILQ_REMOVE(&nvmr_condemned_cntrlrs, cntrlr, nvmrctr_nxt);
		nvmr_cntrlrs_count--;
		if (nvmr_cntrlrs_count == 0) {
			DBGSPEW("nvmr_cntrlrs_count is 0\n");
			wakeup(&nvmr_cntrlrs_count);
		}
		mtx_unlock(&nvmr_cntrlrs_lstslock);

		DBGSPEW("free(%p)ing NVMr controller\n", cntrlr);
		free(cntrlr, M_NVMR);
	}
	DBGSPEW("%d controllers destroyed\n", count);
}


static int
nvmr_connmgmt_handler(struct rdma_cm_id *cmid, struct rdma_cm_event *event)
{
	nvmr_qpair_t q;
	nvmr_queue_state_t qstate;
	const nvmr_rdma_cm_reject_t *ps;

	DBGSPEW("Event \"%s\" returned status \"%d\" for cmid:%p\n",
	    rdma_event_msg(event->event), event->status, cmid);

	/* Every cmid is associated with a controller or a queue? */
	q = cmid->context;
	q->nvmrq_last_cm_status = event->status;

	switch(event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_RESOLVED:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_ESTABLISHED:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		switch(event->event) {
		case RDMA_CM_EVENT_ADDR_RESOLVED:
			qstate = NVMRQ_ADDR_RESOLV_SUCCEEDED;
			break;

		case RDMA_CM_EVENT_ADDR_ERROR:
			qstate = NVMRQ_ADDR_RESOLV_FAILED;
			break;

		case RDMA_CM_EVENT_ROUTE_RESOLVED:
			qstate = NVMRQ_ROUTE_RESOLV_SUCCEEDED;
			break;

		case RDMA_CM_EVENT_ROUTE_ERROR:
			qstate = NVMRQ_ROUTE_RESOLV_FAILED;
			break;

		case RDMA_CM_EVENT_ESTABLISHED:
			qstate = NVMRQ_CONNECT_SUCCEEDED;
			break;

		case RDMA_CM_EVENT_UNREACHABLE:
			qstate = NVMRQ_CONNECT_FAILED;
			break;

		case RDMA_CM_EVENT_REJECTED:
			ps = (const nvmr_rdma_cm_reject_t *)
			    event->param.conn.private_data;
			DBGSPEW("Reject reason recfmt:%hu sts:%hu\n",
			    ps->nvmrcrj_recfmt, ps->nvmrcrj_sts);
			qstate = NVMRQ_CONNECT_FAILED;
			break;
		default:
			panic("%s@%d: Unhandled Conn Manager event Q:%p e:%d\n",
			    __func__, __LINE__, q, event->event);
		}
		mtx_lock(&q->nvmrq_cntrlr->nvmrctr_nvmec.lockc);
		q->nvmrq_state = qstate;
		mtx_unlock(&q->nvmrq_cntrlr->nvmrctr_nvmec.lockc);
		wakeup(&q->nvmrq_cmid);
		/* No touching q beyond this point */
		break;
	case RDMA_CM_EVENT_DISCONNECTED:
	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		nvmr_cntrlr_destroy_init(q->nvmrq_cntrlr);
		break;

	default:
		dump_stack();
		break;
	}

	/* No touching q beyond this point */
	if (event->status != 0) {
		dump_stack();
	}

	return 0;
}

typedef struct {
	char *nvmra_ipaddr;
	char *nvmra_port;
	char *nvmra_subnqn;
} nvmr_addr_t;

typedef enum {
	NVMR_QCMD_INVALID = 0,
	NVMR_QCMD_RO,
	NVMR_QCMD_WO,
}  nvmr_ksgl_perm_t;

#define NVMRTO 3000

#define NVMR_STRING "NVMe over RDMA"
#define CONFIRMRDMACONTROLLER KASSERT(strncmp(cntrlr->very_first_field, \
    NVMR_STRING, sizeof(cntrlr->very_first_field)) == 0, \
    ("%s@%d NOT an RDMA controller!\n", __func__, __LINE__))
#define KASSERT_NVMR_CNTRLR(c) KASSERT((c)->nvmec_ttype == NVMET_RDMA, \
    ("%s@%d c:%p t:%d\n", __func__, __LINE__, (c), (c)->nvmec_ttype))

/*
 * TODO: Unify into nvme_ctrlr_post_failed_request(): It's the same
 */
void
nvmr_ctrlr_post_failed_request(nvmr_cntrlr_t cntrlr, struct nvme_request *req)
{
	struct nvme_controller *ctrlr;

	CONFIRMRDMACONTROLLER;
	ctrlr = &cntrlr->nvmrctr_nvmec;

	mtx_lock(&ctrlr->lockc);
	STAILQ_INSERT_TAIL(&ctrlr->fail_req, req, stailq);
	mtx_unlock(&ctrlr->lockc);
	taskqueue_enqueue(ctrlr->taskqueue, &ctrlr->fail_req_task);
}


/*
 *    Based on iser_bio_to_sg()
 */
int
nvmr_map_bio(nvmr_qpair_t q, nvmr_ncommcon_t *commp, int *np);
int
nvmr_map_bio(nvmr_qpair_t q, nvmr_ncommcon_t *commp, int *np)
{
	uintptr_t offset;
	int n, error;
	size_t len, translen;
	struct scatterlist *s, *scl;
	struct bio *bio;

	mtx_assert(&q->nvmrq_gqp.qlock, MA_OWNED);

	*np = 0;
	bio = commp->nvmrsnd_req->u.bio;
	translen = bio->bio_bcount;
	if (translen == 0) {
		error = ENOTSUPP;
		ERRSPEW("0 length NVMe request data not supported\n");
		goto out;
	}
	offset = bio->bio_ma_offset;
	scl = q->nvmrq_scl;

	for (n = 0; (0 < translen) && (n < MAX_NVME_RDMA_SEGMENTS); n++,
	    translen -= len) {
		memset(scl + n, 0, sizeof(*scl));
		s = scl + n;
		len = min(PAGE_SIZE - offset, translen);
		sg_set_page(s, bio->bio_ma[n], len, offset);
		offset = 0;
	}

	if (translen != 0) {
		ERRSPEW("Could not complete translation of Data. n:%d "
		    "translen:%zu\n", n, translen);
		error = E2BIG;
		goto out;
	} else {
		/*
		DBGSPEW("data is 0x%p\n", commp->nvmrsnd_req->u.payload);
		for (count = 0; count < n; count++) {
			DBGSPEW("scl[%d](hex) p:%16lX o:%8X l:%8X a:%16lX\n",
			count,
			scl[count].page_link,
			scl[count].offset,
			scl[count].length,
			scl[count].address);
		}
		 */
	}
	sg_mark_end(s);

	*np = n;
	error = 0;

out:
	return error;
}

/*
 *    Based on iser_buf_to_sg()
 */
int
nvmr_map_vaddr(nvmr_qpair_t q, nvmr_ncommcon_t *commp, int *np);
int
nvmr_map_vaddr(nvmr_qpair_t q, nvmr_ncommcon_t *commp, int *np)
{
	uintptr_t offset;
	int n, error;
	size_t len, translen;
	void *cbuf;
	struct scatterlist *s, *scl;
	uint32_t type;

	mtx_assert(&q->nvmrq_gqp.qlock, MA_OWNED);

	*np = 0;
	scl = q->nvmrq_scl;
	type = commp->nvmrsnd_req->type;

	switch (type) {
	case NVME_REQUEST_BIO:
		translen = commp->nvmrsnd_req->u.bio->bio_bcount;
		cbuf = commp->nvmrsnd_req->u.bio->bio_data;
		break;
	case NVME_REQUEST_VADDR:
		translen = commp->nvmrsnd_req->payload_size;
		cbuf = commp->nvmrsnd_req->u.payload;
		break;
	default:
		panic("%s@%d Unsupported NVMe request type:%d c:%p\n", __func__,
		    __LINE__, type, commp);
		break;
	}

	if (cbuf == NULL) {
		error = EINVAL;
		ERRSPEW("NULL NVMe request data not supported, t:%u\n", type);
		goto out;
	}
	if (translen == 0) {
		error = ENOTSUPP;
		ERRSPEW("0 length NVMe request data not supported\n");
		goto out;
	}

	for (n = 0; (0 < translen) && (n < MAX_NVME_RDMA_SEGMENTS); n++,
	    translen -= len) {
		memset(scl + n, 0, sizeof(*scl));
		s = scl + n;
		offset = ((uintptr_t)cbuf) & ~PAGE_MASK;
		len = min(PAGE_SIZE - offset, translen);
		sg_set_buf(s, cbuf, len);
		cbuf = (void *)(((u64)cbuf) + (u64)len);
	}

	if (translen != 0) {
		ERRSPEW("Could not complete translation of Data. n:%d "
		    "translen:%zu\n", n, translen);
		error = E2BIG;
		goto out;
	} else {
		/*
		DBGSPEW("data is 0x%p\n", commp->nvmrsnd_req->u.payload);
		for (count = 0; count < n; count++) {
			DBGSPEW("scl[%d](hex) p:%16lX o:%8X l:%8X a:%16lX\n",
			count,
			scl[count].page_link,
			scl[count].offset,
			scl[count].length,
			scl[count].address);
		}
		 */
	}
	sg_mark_end(s);

	*np = n;
	error = 0;

out:
	return error;
}

int
nvmr_map_data(nvmr_qpair_t q, nvmr_ncommcon_t *commp, nvmr_ksgl_t *k,
    struct ib_reg_wr *regwrp);
int
nvmr_map_data(nvmr_qpair_t q, nvmr_ncommcon_t *commp, nvmr_ksgl_t *k,
    struct ib_reg_wr *regwrp)
{
	int n, nn, nnn, error, retval;
	struct ib_mr *mr;
	struct scatterlist *scl;
	enum dma_data_direction dir;
	struct ib_device *ibd;
	uint8_t dirbits;
	nvmr_communion_t *cmd;
	uint16_t bf;

	mtx_assert(&q->nvmrq_gqp.qlock, MA_OWNED);

	KASSERT(commp->nvmrsnd_req->type != NVME_REQUEST_NULL, ("%s@%d r:%p "
	    "t:%d\n", __func__, __LINE__, commp->nvmrsnd_req,
	    commp->nvmrsnd_req->type));

	ibd = q->nvmrq_cmid->device;
	/*
	 * 1) Translate the command data into a scatterlist array
	 *    for ib_dma_map_sg() to use a la iser_buf_to_sg()
	 */
	switch (commp->nvmrsnd_req->type) {
	case NVME_REQUEST_VADDR:
		retval = nvmr_map_vaddr(q, commp, &n);
		break;
	case NVME_REQUEST_BIO:
		bf = commp->nvmrsnd_req->u.bio->bio_flags;
		switch(bf & (BIO_VLIST|BIO_UNMAPPED)) {
		case 0:
			retval = nvmr_map_vaddr(q, commp, &n);
			break;
		case BIO_UNMAPPED:
			retval = nvmr_map_bio(q, commp, &n);
			break;
		case BIO_VLIST:
		default:
			retval = ENOTSUPP;
			break;

		}
		break;
	default:
		panic("%s@%d Unsupported NVMe req memory type r:%p t:0x%x",
		    __func__, __LINE__, commp->nvmrsnd_req,
		    commp->nvmrsnd_req->type);
	}
	if (retval != 0) {
		error = retval;
		ERRSPEW("nvmr_map_(q:%p commp:%p) failed:%d\n", q, commp,
		    retval);
		goto out;
	}

	scl = q->nvmrq_scl; /* Now contains RDMA segments */

	/* 2) Map the scatterlist array per the direction to/from IB device */
	cmd = (nvmr_communion_t *)&commp->nvmrsnd_req->cmd;
	if (cmd->nvmrcu_stub.nvmrsb_nvmf.nvmf_opc == NVME_OPC_FABRIC_COMMAND) {
		dirbits = cmd->nvmrcu_stub.nvmrsb_nvmf.nvmf_fctype;
	} else {
		dirbits = cmd->nvmrcu_stub.nvmrsb_nvmf.nvmf_opc;
	}
	dirbits &= 0x3;
	switch(dirbits) {
	case 0x1:
		dir = DMA_TO_DEVICE;
		break;
	case 0x2:
		dir = DMA_FROM_DEVICE;
		break;
	default:
		panic("%s@%d Bad direction in NVMe command r:%p d:0x%x",
		    __func__, __LINE__, commp->nvmrsnd_req, dirbits);
	}

	nn = ib_dma_map_sg(ibd, scl, n, dir);
	if (nn < 1) {
		ERRSPEW("ib_dma_map_sg() failed with count:%d\n", nn);
		error = E2BIG;
		goto out;
	}
	/* DBGSPEW("ib_dma_map_sg() returned a count of %d\n", nn); */

	/* 3) Map the scatterlist elements to the MR */
	mr = commp->nvmrsnd_mr;
	nnn = ib_map_mr_sg(mr, scl, nn, NULL, NVMR_FOURK);
	if (nnn < nn) {
		ERRSPEW("ib_map_mr_sg() failed. nnn:%d < nn:%d\n", nnn, nn);
		error = E2BIG;
		goto out;
	}
	/* DBGSPEW("ib_map_mr_sg() returned a count of %d\n", nnn); */
	ib_update_fast_reg_key(mr, ib_inc_rkey(mr->rkey));

	/* 4) Craft the memory registration work-request but don't post it */
	commp->nvmrsnd_regcqe.done = nvmr_rg_done;
	memset(regwrp, 0, sizeof(*regwrp));
	/* NB the Registration work-request contains a Send work-request */
	regwrp->wr.num_sge = 0; /* No send/recv buffers are being posted */
	regwrp->wr.send_flags = IB_SEND_SIGNALED; /* Invoke .done when done */
	regwrp->wr.opcode = IB_WR_REG_MR;
	regwrp->wr.wr_cqe = &commp->nvmrsnd_regcqe;
	regwrp->wr.next = NULL;
	regwrp->access = (dir == DMA_TO_DEVICE) ?
	    IB_ACCESS_REMOTE_READ : IB_ACCESS_REMOTE_WRITE;
	regwrp->access |= IB_ACCESS_LOCAL_WRITE;
	regwrp->key = mr->rkey;
	regwrp->mr = mr;

	k->nvmrk_address = htole64(mr->iova);
	k->nvmrk_length[0] = htole32(mr->length) & 0xFF;
	k->nvmrk_length[1] = (htole32(mr->length)>>8) & 0xFF;
	k->nvmrk_length[2] = (htole32(mr->length)>>16) & 0xFF;
	k->nvmrk_key = htole32(mr->rkey);
	k->nvmrk_sgl_identifier = NVMF_KEYED_SGL_INVALIDATE;

	commp->nvmrsnd_rkeyvalid = true;
	
	error = 0;

out:
	return error;
}


int
nvmr_command_async(nvmr_qpair_t q, struct nvme_request *req)
{
	nvmr_stub_t *cp;
	nvmr_ksgl_t *k;
	nvmr_ncommcon_t *commp;
	struct nvme_qpair *gq;
	nvmr_cntrlr_t cntrlr;
	int error, retval;
	struct ib_reg_wr regwr;
	u64 dmaddr;
	struct ib_sge sgl;
	struct ib_send_wr sndwr, *sndwrp, *badsndwrp;
	struct ib_device *ibd;

	gq = &q->nvmrq_gqp;
	cntrlr = q->nvmrq_cntrlr;

	mtx_assert(&gq->qlock, MA_OWNED);

	switch(req->type) {
	case NVME_REQUEST_BIO:
	case NVME_REQUEST_VADDR:
	case NVME_REQUEST_NULL:
		break;
	default:
		error = ENOTSUP;
		goto out;
	}

	commp = STAILQ_FIRST(&q->nvmrq_comms);
	if (commp == NULL) {
		if (cntrlr->nvmrctr_nvmec.is_failed) {
			ERRSPEW("Controller:%p:%p has failed\n", cntrlr,
			    &cntrlr->nvmrctr_nvmec);
			error = ESHUTDOWN;
		} else {
			STAILQ_INSERT_TAIL(&q->nvmrq_defreqs, req, stailq);
			error = 0;
		}
		goto out;
	}
	STAILQ_REMOVE(&q->nvmrq_comms, commp, nvmr_ncommcont, nvmrsnd_nextfree);

	cp = (nvmr_stub_t *)&req->cmd;
	ibd = q->nvmrq_cmid->device;
	k = &cp->nvmrsb_nvmf.nvmf_ksgl;

	cp->nvmrsb_nvmf.nvmf_sgl_fuse = NVMF_SINGLE_BUF_SGL;
	memset(&sndwr, 0, sizeof(sndwr));
	sndwr.next  = NULL;

	commp->nvmrsnd_rkeyvalid = false;
	commp->nvmrsnd_nvmecomm = cp;
	commp->nvmrsnd_req = req;

	memset(k, 0, sizeof(*k));
	switch(req->type) {
	case NVME_REQUEST_BIO:
	case NVME_REQUEST_VADDR:
		retval = nvmr_map_data(q, commp, k, &regwr);
		if (retval != 0) {
			error = retval;
			ERRSPEW("nvmr_map_data(c:%p q:%p):%d\n", commp,
			    q, retval);
			goto out;
		}
		break;
	case NVME_REQUEST_NULL:
		k->nvmrk_sgl_identifier = NVMF_KEYED_SGL_NO_INVALIDATE;
		break;
	default:
		panic("%s@%d Unimplemented t:%d req:%p\n", __func__, __LINE__,
		    req->type, req);
	}


	cp->nvmrsb_nvmf.nvmf_cid = commp->nvmrsnd_cid;
	dmaddr = ib_dma_map_single(ibd, cp, sizeof(*cp), DMA_TO_DEVICE);
	if (ib_dma_mapping_error(ibd, dmaddr) != 0) {
		ERRSPEW("ib_dma_map_single() failed for %p\n", cp);
		error = ENOENT;
		goto out;
	}
	commp->nvmrsnd_dmaddr = dmaddr;
	commp->nvmrsnd_rspndd = false;
	/* Transfer ownership of command structure to device */
	ib_dma_sync_single_for_device(ibd, dmaddr, sizeof(*cp), DMA_TO_DEVICE);

	memset(&sgl, 0, sizeof(sgl));

	sgl.addr   = dmaddr;
	sgl.length = sizeof(*(commp->nvmrsnd_nvmecomm));
	sgl.lkey   = q->nvmrq_ibpd->local_dma_lkey;

	commp->nvmrsnd_cqe.done = nvmr_snd_done;

	sndwr.wr_cqe = &commp->nvmrsnd_cqe;
	sndwr.sg_list = &sgl;
	sndwr.num_sge = 1;
	sndwr.opcode = IB_WR_SEND;
	sndwr.send_flags = IB_SEND_SIGNALED;
	sndwr.next = NULL;

	if (req->type == NVME_REQUEST_NULL) {
		sndwrp = &sndwr;
	} else {
		/* If there's data to be RDMAed chain in the registration */
		regwr.wr.next = &sndwr;
		sndwrp = &regwr.wr;
	}

	badsndwrp = NULL;
	retval = ib_post_send(q->nvmrq_ibqp, sndwrp, &badsndwrp);
	if (retval != 0) {
		ERRSPEW("ib_post_send(%p) failed with %d, badsndwrp:%p\n",
		    sndwrp, retval, badsndwrp);
		error = retval;
		goto out;
	}

	error = 0;
out:
	return error;
}


void
nvmr_submit_req(nvmr_qpair_t q, struct nvme_request *req);
void
nvmr_submit_req(nvmr_qpair_t q, struct nvme_request *req)
{
	nvmr_cntrlr_t cntrlr;
	struct nvme_qpair *gqp;
	int retval;

	cntrlr = q->nvmrq_cntrlr;

	gqp = &q->nvmrq_gqp;
	req->rqpair = gqp;

	mtx_lock(&gqp->qlock);
	if (Q_IS_FAILED(q)) {
		retval = ESHUTDOWN;
	} else {
		retval = nvmr_command_async(q, req);
	}
	mtx_unlock(&gqp->qlock);

	if (retval != 0) {
		ERRSPEW("Failing req: %d\n", retval);
		nvmr_ctrlr_post_failed_request(cntrlr, req);
	}
}


void
nvmr_submit_adm_req(struct nvme_controller *ctrlr, struct nvme_request *req);
void
nvmr_submit_adm_req(struct nvme_controller *ctrlr, struct nvme_request *req)
{
	nvmr_cntrlr_t cntrlr;
	nvmr_qpair_t q;

	KASSERT_NVMR_CNTRLR(ctrlr);
	cntrlr = ctrlr->nvmec_tsp;
	CONFIRMRDMACONTROLLER;

	q = cntrlr->nvmrctr_adminqp;
	nvmr_submit_req(q, req);
}


void
nvmr_submit_io_req(struct nvme_controller *ctrlr, struct nvme_request *req);
void
nvmr_submit_io_req(struct nvme_controller *ctrlr, struct nvme_request *req)
{
	nvmr_cntrlr_t cntrlr;
	nvmr_qpair_t q;

	KASSERT_NVMR_CNTRLR(ctrlr);
	cntrlr = ctrlr->nvmec_tsp;
	CONFIRMRDMACONTROLLER;

	q = cntrlr->nvmrctr_ioqarr[0]; /* Single Q until I/O queues are up */
	nvmr_submit_req(q, req);
}


#define PRE_ASYNC_CM_INVOCATION(pre_state) \
	q->nvmrq_state = (pre_state);

#define POST_ASYNC_CM_INVOCATION(routine, pre_state, success_state)        \
	if (retval != 0) {                                                 \
		ERRSPEW("Failed, %s()> %d\n", routine, retval);            \
		error = retval;                                            \
		goto out;                                                  \
	}                                                                  \
	/* DBGSPEW("Successfully invoked %s()\n", routine); */             \
	mtx_lock(&cntrlr->nvmrctr_nvmec.lockc);                            \
	if (q->nvmrq_state == (pre_state)) {                               \
		DBGSPEW("Sleeping with message \"%s\"\n",                  \
		    __stringify(__LINE__));                                \
		retval = mtx_sleep(&q->nvmrq_cmid,                         \
		    &cntrlr->nvmrctr_nvmec.lockc,                          \
		    0, __stringify(__LINE__), NVMRTO+1000);                \
		mtx_unlock(&cntrlr->nvmrctr_nvmec.lockc);                  \
		switch (retval) {                                          \
		case 0:                                                    \
			break;                                             \
		case EWOULDBLOCK:                                          \
			ERRSPEW("No response after %d ms\n",  NVMRTO+1000);\
		default:                                                   \
			error = retval;                                    \
			goto out;                                          \
		}                                                          \
	} else {                                                           \
		mtx_unlock(&cntrlr->nvmrctr_nvmec.lockc);                  \
	}                                                                  \
	if (q->nvmrq_state < (success_state)) {                            \
		error = q->nvmrq_last_cm_status;                           \
		goto out;                                                  \
	}                                                                  \

#define ISSUE_WAIT_CHECK_REQ                                          \
	status.done = 0;                                              \
	nvme_ctrlr_submit_admin_request(&cntrlr->nvmrctr_nvmec, req); \
	while (!atomic_load_acq_int(&status.done)) {                  \
		pause("nvmr", HZ > 100 ? (HZ/100) : 1);               \
	}                                                             \
	if (nvme_completion_is_error(&status.cpl))


static int
nvmr_qpair_create(nvmr_cntrlr_t cntrlr, nvmr_qpair_t *qp, uint16_t qid,
    uint16_t cntlid, nvmr_qprof_t *prof)
{
	u64 dmaddr;
	struct ib_mr *mr;
	struct ib_device *ibd;
	struct sockaddr_storage saddr;
	struct sockaddr_in *sin4;
	struct rdma_cm_id *cmid;
	int error, retval, count;
	nvmr_ncmplcon_t *cmplp;
	nvmr_ncommcon_t *commp, **commparrp;
	struct ib_pd *ibpd;
	nvmr_qpair_t q;
	struct ib_qp_init_attr init_attr;
	struct rdma_conn_param conn_param;
	nvmr_rdma_cm_request_t privdata;
	struct ib_cq *ibcq;
	struct nvmrdma_connect_data ncdata;
	size_t msz;

	nvmr_communion_t *cmd;
	struct nvme_request *req;
	struct nvme_completion_poll_status status;


	sin4 = (struct sockaddr_in *)&saddr;

	msz = sizeof *q;
	q = malloc(msz, M_NVMR, M_WAITOK|M_ZERO);
	if (q == NULL) {
		ERRSPEW("NVMr Q allocation sized \"%zu\" failed\n", msz);
		error = ENOMEM;
		goto out;
	}
	DBGSPEW("NVMr Q allocation of size \"%zu\"\n", msz);
	q->nvmrq_cntrlr = cntrlr;
	q->nvmrq_state = NVMRQ_PRE_INIT;
	q->nvmrq_prof = prof;
	q->nvmrq_gqp.qttype = NVMET_RDMA;
	q->nvmrq_gqp.gqctrlr = &cntrlr->nvmrctr_nvmec;
	atomic_store_rel_int(&q->nvmrq_gqp.qis_enabled, FALSE);
	mtx_init(&q->nvmrq_gqp.qlock, "nvme qpair lock", NULL, MTX_DEF);
	STAILQ_INIT(&q->nvmrq_defreqs);
	STAILQ_INIT(&q->nvmrq_comms);
	STAILQ_INIT(&q->nvmrq_cmpls);

	cmid = rdma_create_id(TD_TO_VNET(curthread), nvmr_connmgmt_handler,
	    q, RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(cmid)) {
		ERRSPEW("rdma_create_id() failed:%ld\n", PTR_ERR(cmid));
		error = EINVAL;
		goto out;
	}
	q->nvmrq_cmid = cmid;

	/*
	 * Allocate containers for the Send Q elements which are always
	 * struct nvme_command (or fabric equivalent)
	 */
	for (count = 0; count < prof->nvmrqp_numsndqe; count++) {
		msz = sizeof(*commp);
		commp = malloc(msz,  M_NVMR, M_WAITOK|M_ZERO);
		if (commp == NULL) {
			ERRSPEW("Command Q container allocation failed after"
			    " %d iterations\n", count);
			break;
		}

		STAILQ_INSERT_HEAD(&q->nvmrq_comms, commp, nvmrsnd_nextfree);
		q->nvmrq_numFsndqe++;
		q->nvmrq_numsndqe++;
	}
	DBGSPEW("Alloced %d command Q containers\n", q->nvmrq_numsndqe);
	q->nvmrq_gqp.num_qentries = q->nvmrq_numsndqe;

	/*
	 * Allocate a mapping array for looking up an NVMe command when we get
	 * an NVMe completion
	 */

	msz = sizeof(*commparrp) * (q->nvmrq_numsndqe + 1);
	commparrp = malloc(msz, M_NVMR, M_WAITOK|M_ZERO);
	if (commparrp == NULL) {
		ERRSPEW("NVMr allocation of %zu for mapping array failed \n",
		    msz);
		error = ENOMEM;
		goto out;
	}
	DBGSPEW("NVMr allocation of %zu bytes for mapping array\n", msz);
	commp = STAILQ_FIRST(&q->nvmrq_comms);
	commparrp[0] = (nvmr_ncommcon_t *)(0xDEADD00D8BADBEEFull);
	for (count = 1; count < (q->nvmrq_numsndqe + 1); count++) {
		commparrp[count] = commp;
		commp->nvmrsnd_cid = count;
		commp = STAILQ_NEXT(commp, nvmrsnd_nextfree);
	}
	KASSERT(commp == NULL, ("%s@%d commp:%p, q:%p\n", __func__, __LINE__,
	    commp, q));
	q->nvmrq_commcid = commparrp;
	q->nvmrq_next2use = STAILQ_LAST(&q->nvmrq_comms, nvmr_ncommcont,
	    nvmrsnd_nextfree);

	/*
	 * Allocate containers for the Recv Q elements which are always
	 * struct nvme_completion
	 */
	for (count = 0; count < prof->nvmrqp_numrcvqe; count++) {
		msz = sizeof(*cmplp);
		cmplp = malloc(msz,  M_NVMR, M_WAITOK|M_ZERO);
		if (cmplp == NULL) {
			ERRSPEW("Completion Q container allocation failed after"
			    " %d iterations\n", count);
			break;
		}

		STAILQ_INSERT_HEAD(&q->nvmrq_cmpls, cmplp, nvmrsp_next);
		q->nvmrq_numFrcvqe++;
		q->nvmrq_numrcvqe++;
	}
	if (q->nvmrq_numrcvqe < q->nvmrq_gqp.num_qentries) {
		ERRSPEW("Only allocated %hu of %u required, failing\n",
		    q->nvmrq_numrcvqe, q->nvmrq_gqp.num_qentries);
		error = ENOMEM;
		goto out;
	}


	memset(&saddr, 0, sizeof(saddr));
	sin4->sin_len = sizeof(*sin4);
	sin4->sin_family = AF_INET;
	memcpy((void *)&sin4->sin_addr.s_addr, &cntrlr->nvmrctr_ipv4,
	    sizeof sin4->sin_addr.s_addr);
	sin4->sin_port = cntrlr->nvmrctr_port;

	/*
	 * NB Once rdma_resolve_addr() is called nvmr_connmgmt_handler() can be
	 * invoked.  Keep cntrlr consistent as it can be reached via cmid
	 * asynchronously.  Bump up the reference count for the activity in
	 * the nvmr_connmgmt_handler() contexts.  Assign the allocated q
	 * structure into the qarr so that cleaning up the controller structure
	 * will clean up the q as well.  nvmr_queue_destroy() can no longer be
	 * called except by nvmr_cntrlr_destroy()
	 */
	*qp = q;
	PRE_ASYNC_CM_INVOCATION(NVMRQ_PRE_ADDR_RESOLV);
	retval = rdma_resolve_addr(cmid, NULL, (struct sockaddr *)sin4, NVMRTO);
	POST_ASYNC_CM_INVOCATION(__stringify(rdma_resolve_addr),
	    NVMRQ_PRE_ADDR_RESOLV, NVMRQ_ADDR_RESOLV_SUCCEEDED);

	/*
	 * Once address resoltion is complete the cmid will have the IB device
	 * that our RDMA connection will be using
	 */
	ibd = q->nvmrq_cmid->device;
	if (ibd == NULL) {
		error = EDOOFUS;
		goto out;
	}

	if (!(ibd->attrs.device_cap_flags & IB_DEVICE_MEM_MGT_EXTENSIONS)) {
		ERRSPEW("Memory management extensions not supported. 0x%lX\n",
		    cmid->device->attrs.device_cap_flags);
		error = ENXIO;
		goto out;
	}

	ibpd  = ib_alloc_pd(ibd, 0);
	if (IS_ERR(ibpd)) {
		ERRSPEW("ib_alloc_pd() failed: 0x%lx\n", PTR_ERR(ibpd));
		error = ENOENT;
		goto out;
	}
	q->nvmrq_ibpd = ibpd;

	/*
	 * Allocate MR structures for use by any data that the NVMe commands
	 * posted to the IB SND Q need to describe
	 */
	count = 0;
	STAILQ_FOREACH(commp, &q->nvmrq_comms, nvmrsnd_nextfree) {
		mr = ib_alloc_mr(ibpd, IB_MR_TYPE_MEM_REG,
		    MAX_NVME_RDMA_SEGMENTS);
		if (IS_ERR(mr)) {
			ERRSPEW("ib_alloc_mr() failed with \"%ld\" for "
			    "count #%d\n", PTR_ERR(mr), count);
			error = ENOENT;
			goto out;
		}
		commp->nvmrsnd_mr = mr;
		count++;
	}
	KASSERT(count == q->nvmrq_numsndqe, ("%s@%d count:%d numsndqe:%d",
	    __func__, __LINE__, count, q->nvmrq_numsndqe));

	/*
	 * Loop through the list of completion container structures mapping
	 * the corresponding NVMe completion structure
	 */
	count = 0;
	STAILQ_FOREACH(cmplp, &q->nvmrq_cmpls, nvmrsp_next) {
		dmaddr =  ib_dma_map_single(ibd, &cmplp->nvmrsp_nvmecmpl,
		    sizeof(cmplp->nvmrsp_nvmecmpl), DMA_FROM_DEVICE);
		if (ib_dma_mapping_error(ibd, dmaddr) != 0) {
			ERRSPEW("ib_dma_map_single() failed for #%d\n", count);
			error = ENOMEM;
			goto out;
		}
		cmplp->nvmrsp_dmaddr = dmaddr;
		ib_dma_sync_single_for_cpu(ibd, dmaddr,
		    sizeof(cmplp->nvmrsp_nvmecmpl), DMA_FROM_DEVICE);
		count++;
	}
	KASSERT(count == q->nvmrq_numrcvqe, ("%s@%d count:%d numrcvqe:%d",
	    __func__, __LINE__, count, q->nvmrq_numrcvqe));


	/*
	 * NB Once rdma_resolve_route() is called nvmr_connmgmt_handler() can be
	 * once again invoked.	Keep cntrlr consistent as it can be reached
	 * via cmid asynchronously.  Bump up the reference count for the
	 * activity in the nvmr_connmgmt_handler() contexts.
	 */
	PRE_ASYNC_CM_INVOCATION(NVMRQ_PRE_ROUTE_RESOLV);
	retval = rdma_resolve_route(cmid, NVMRTO);
	POST_ASYNC_CM_INVOCATION(__stringify(rdma_resolve_route),
	    NVMRQ_PRE_ROUTE_RESOLV, NVMRQ_ROUTE_RESOLV_SUCCEEDED);


	/*
	 * Now create the RDMA queue pair that we can post the NVMe command
	 * (Send Q) and NVMe completion (Recv Q) buffers to.
	 */
	memset(&init_attr, 0, sizeof(init_attr));
	/*
	 * Every Send Queue Element can potentially use a Work Request for:
	 * 1) The Send 2) An optional MR Registration 3) An invalidate for
	 * the MR Registration.  So a total of 3 per SendQE.  Add one more
	 * for when ib_drain_qp() is invoked.
	 */
	init_attr.cap.max_send_wr = (q->nvmrq_numsndqe * 3) + 1;

	/* One more for ib_drain_qp() */
	init_attr.cap.max_recv_wr = q->nvmrq_numrcvqe + 1;

	/*
	 * Allocate an RDMA completion Q for receiving the status of Work
	 * Requests (Send/Recv) on the Q pair.  It should be deep enough to
	 * handle completion Q elements from WRs (not QEs) in both Qs in
	 * the pair.
	 */
	ibcq = ib_alloc_cq(ibd, q, init_attr.cap.max_send_wr +
	    init_attr.cap.max_recv_wr, 0 /* hard-coded! completion vector */,
	    IB_POLL_WORKQUEUE);
	if (IS_ERR(ibcq)) {
		ERRSPEW("ib_alloc_cq() failed with 0x%lX\n", PTR_ERR(ibcq));
		error = ESPIPE;
		goto out;
	}
	q->nvmrq_ibcq = ibcq;

	init_attr.cap.max_send_sge = NVMR_NUMSNDSGE;
	init_attr.cap.max_recv_sge = NVMR_NUMRCVSGE;
	init_attr.qp_type = IB_QPT_RC;
	init_attr.send_cq = ibcq;
	init_attr.recv_cq = ibcq;
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;
	init_attr.event_handler = nvmr_qphndlr;
	retval = rdma_create_qp(cmid, ibpd, &init_attr);
	if (retval != 0) {
		ERRSPEW("rdma_create_qp() failed with %d\n", retval);
		error = retval;
		goto out;
	}
	q->nvmrq_ibqp = cmid->qp;

	/* The QP is now open for business */
	atomic_store_rel_int(&q->nvmrq_gqp.qis_enabled, TRUE);

	/*
	 * Post NVMe completion buffers to the RDMA Recv Q, registering their
	 * associated Completion Q elements as well.  Use locks because the QP
	 * is active and the posting here can race with any flushing that
	 * can be triggered asynchronously.
	 */
	count = 0;
	STAILQ_FOREACH(cmplp, &q->nvmrq_cmpls, nvmrsp_next) {
		mtx_lock(&q->nvmrq_gqp.qlock);
		STAILQ_REMOVE(&q->nvmrq_cmpls, cmplp, nvmr_ncmplcont,
		    nvmrsp_next);
		q->nvmrq_numFrcvqe--;
		mtx_unlock(&q->nvmrq_gqp.qlock);

		retval = nvmr_post_cmpl(q, cmplp);
		if (retval != 0) {
			ERRSPEW("ib_post_recv() failed for #%d with %d\n",
			    count, retval);
			error = ENOMSG;
			goto out;
		}
		count++;
	}
	KASSERT(count == q->nvmrq_numrcvqe, ("%s@%d count:%d numrcvqe:%d",
	    __func__, __LINE__, count, q->nvmrq_numrcvqe));

	/*
	 * NB: The conn_param has to pass in an NVMeoF RDMA Private Data
	 * structure for the NVMeoF RDMA target to setup its Q sizes et al.
	 */
	memset(&conn_param, 0, sizeof(conn_param));
	memset(&privdata, 0, sizeof(privdata));
	privdata.nvmrcr_recfmt = 0;
	privdata.nvmrcr_qid = qid;
	privdata.nvmrcr_hrqsize = htole16(q->nvmrq_numrcvqe);
	privdata.nvmrcr_hsqsize = htole16(q->nvmrq_numsndqe);
	conn_param.responder_resources = ibd->attrs.max_qp_rd_atom;
	conn_param.qp_num = q->nvmrq_ibqp->qp_num;
	conn_param.flow_control = 1;
	conn_param.retry_count = 3;
	conn_param.rnr_retry_count = 3;
	conn_param.private_data = &privdata;
	conn_param.private_data_len = sizeof(privdata);

	PRE_ASYNC_CM_INVOCATION(NVMRQ_PRE_CONNECT);
	retval = rdma_connect(cmid, &conn_param);
	POST_ASYNC_CM_INVOCATION(__stringify(rdma_connect),
	    NVMRQ_PRE_CONNECT, NVMRQ_CONNECT_SUCCEEDED);

	/*
	 * Now that a connection has been established send out a CONNECT
	 * NVMeoF command identifying our system and the NVMe subsystem
	 * we're trying to reach via the nvmrcd_subnqn field
	 */
	memset(&ncdata, 0, sizeof(ncdata));
	ncdata.nvmrcd_hostid = nvrdma_host_uuid;
	ncdata.nvmrcd_cntlid = htole16(cntlid);
	snprintf(ncdata.nvmrcd_subnqn, sizeof(ncdata.nvmrcd_subnqn),
	    "%s", cntrlr->nvmrctr_subnqn);
	snprintf(ncdata.nvmrcd_hostnqn, sizeof(ncdata.nvmrcd_hostnqn),
	    HOSTNQN_TEMPLATE, nvrdma_host_uuid_string);

	req = nvme_allocate_request_vaddr(&ncdata, sizeof(ncdata),
	    nvme_completion_poll_cb, &status);
	cmd = (nvmr_communion_t *)&req->cmd;
	cmd->nvmrcu_conn.nvmrcn_nvmf.nvmf_opc = NVME_OPC_FABRIC_COMMAND;
	cmd->nvmrcu_conn.nvmrcn_nvmf.nvmf_fctype = NVMF_FCTYPE_CONNECT;
	cmd->nvmrcu_conn.nvmrcn_recfmt = 0;
	cmd->nvmrcu_conn.nvmrcn_qid = qid;
	cmd->nvmrcu_conn.nvmrcn_sqsize = htole16(q->nvmrq_numsndqe - 1);
	cmd->nvmrcu_conn.nvmrcn_cattr = 0;
	cmd->nvmrcu_conn.nvmrcn_kato = htole32(prof->nvmrqp_kato);

	status.done = 0;
	nvmr_submit_req(q, req);
	while (!atomic_load_acq_int(&status.done)) {
		pause("nvmr", HZ > 100 ? (HZ/100) : 1);
	}                                                             \
	if (nvme_completion_is_error(&status.cpl)) {
		ERRSPEW("CONNECT NVMeoF command to subNQN \"%s\" failed!\n",
		    cntrlr->nvmrctr_subnqn);
		error = ENXIO;
		goto out;
	}

	error = 0;

out:
	if ((error != 0) && (q->nvmrq_state < NVMRQ_PRE_ADDR_RESOLV)) {
		/* Cleanup the Q because nvmr_cntrlr_destroy() won't see it */
		nvmr_queue_destroy(q);
	}

	return error;
}

typedef enum {
	NVMR_PROPLEN_4BYTES = 0,
	NVMR_PROPLEN_8BYTES = 1,
	NVMR_PROPLEN_MAX
} nvmr_proplent_t;

#define MAX_NVMR_PROP_GET 0x12FFU
#define IDENTIFYLEN 4096

int
nvmr_admin_identify(nvmr_cntrlr_t cntrlr, uint16_t cntid, uint32_t nsid,
    uint8_t cns, void *datap, int datalen);
int
nvmr_admin_identify(nvmr_cntrlr_t cntrlr, uint16_t cntid, uint32_t nsid,
    uint8_t cns, void *datap, int datalen)
{
	int error;
	nvmr_communion_t *cmd;
	struct nvme_request *req;
	struct nvme_completion_poll_status status;

	if ((cntrlr == NULL) || (datap == NULL) || (datalen != IDENTIFYLEN)) {
		ERRSPEW("INVALID! cntrlr:%p datap:%p datalen:%d\n", cntrlr,
		    datap, datalen);
		error = EINVAL;
		goto out;
	}

	req = nvme_allocate_request_vaddr(&cntrlr->nvmrctr_nvmec.cdata,
	    sizeof(struct nvme_controller_data), nvme_completion_poll_cb,
	    &status);
	cmd = (nvmr_communion_t *)&req->cmd;
	cmd->nvmrcu_idnt.nvmrid_nvmf.nvmf_opc = NVME_OPC_IDENTIFY;
	cmd->nvmrcu_idnt.nvmrid_nvmf.nvmf_nsid = htole32(nsid);
	cmd->nvmrcu_idnt.nvmrid_cns = cns;
	cmd->nvmrcu_idnt.nvmrid_cntid = htole16(cntid);

	ISSUE_WAIT_CHECK_REQ {
		ERRSPEW("IDENTIFY NVMeoF command to subNQN \"%s\" failed!\n",
		    cntrlr->nvmrctr_subnqn);
		error = ENXIO;
		goto out;
	}

	error = 0;
out:
	return error;
}


int
nvmr_admin_propset(nvmr_cntrlr_t cntrlr, uint32_t offset, uint64_t value,
    nvmr_proplent_t len);
int
nvmr_admin_propset(nvmr_cntrlr_t cntrlr, uint32_t offset, uint64_t value,
    nvmr_proplent_t len)
{
	int error;
	nvmr_communion_t *cmd;
	struct nvme_request *req;
	struct nvme_completion_poll_status status;

	if ((cntrlr == NULL) || (offset > MAX_NVMR_PROP_GET) ||
	    (len >= NVMR_PROPLEN_MAX)) {
		ERRSPEW("INVALID! cntrlr:%p offset:0x%X len:%d\n",
		    cntrlr, offset, len);
		error = EINVAL;
		goto out;
	}

	req = nvme_allocate_request_null(nvme_completion_poll_cb, &status);
	cmd = (nvmr_communion_t *)&req->cmd;
	cmd->nvmrcu_prst.nvmrps_nvmf.nvmf_opc = NVME_OPC_FABRIC_COMMAND;
	cmd->nvmrcu_prst.nvmrps_nvmf.nvmf_fctype = NVMF_FCTYPE_PROPSET;
	cmd->nvmrcu_prst.nvmrps_attrib = len;
	cmd->nvmrcu_prst.nvmrps_ofst = offset;
	cmd->nvmrcu_prst.nvmrps_value = value;

	ISSUE_WAIT_CHECK_REQ {
		ERRSPEW("PROPSET NVMeoF command to subNQN \"%s\" failed!\n",
		    cntrlr->nvmrctr_subnqn);
		error = ENXIO;
		goto out;
	}

	error = 0;

out:
	return error;
}


typedef struct {
	uint64_t nvmrcc_mqes  :16;
	uint64_t nvmrcc_cqr   : 1;
	uint64_t nvmrcc_ams   : 2;
	uint64_t nvmrcc_resv1 : 5;
	uint64_t nvmrcc_to    : 8;
	uint64_t nvmrcc_dstrd : 4;
	uint64_t nvmrcc_nssrs : 1;
	uint64_t nvmrcc_css   : 8;
	uint64_t nvmrcc_bps   : 1;
	uint64_t nvmrcc_resv2 : 2;
	uint64_t nvmrcc_mpsmin: 4;
	uint64_t nvmrcc_mpsmax: 4;
	uint64_t nvmrcc_resv3 : 8;
} nvmr_cntrlcap_t;
CTASSERT(sizeof(nvmr_cntrlcap_t) == sizeof(uint64_t));

static void
nvmr_ctrlr_reset_task(void *arg, int pending)
{
	ERRSPEW("Invoked for arg:%p pending:%d\n", arg, pending);
}


static struct cdevsw nvmr_ctrlr_cdevsw = {
	.d_version =	D_VERSION,
	.d_flags =	0,
};

static void
nvmr_delist_cb(struct nvme_controller *ctrlrp)
{
	panic("%s() invoked for ctrlr:%p\n", __func__, ctrlrp);
}

int
nvmr_admin_propget(nvmr_cntrlr_t cntrlr, uint32_t offset, uint64_t *valuep,
    nvmr_proplent_t len);
int
nvmr_admin_propget(nvmr_cntrlr_t cntrlr, uint32_t offset, uint64_t *valuep,
    nvmr_proplent_t len)
{
	int error;
	nvmr_communion_t *cmd;
	struct nvme_request *req;
	struct nvme_completion_poll_status status;

	if ((cntrlr == NULL) || (offset > MAX_NVMR_PROP_GET) ||
	    (len >= NVMR_PROPLEN_MAX) || (valuep == NULL)) {
		ERRSPEW("INVALID! cntrlr:%p offset:0x%X len:%d valuep:%p\n",
		    cntrlr, offset, len, valuep);
		error = EINVAL;
		goto out;
	}

	req = nvme_allocate_request_null(nvme_completion_poll_cb, &status);
	cmd = (nvmr_communion_t *)&req->cmd;
	cmd->nvmrcu_prgt.nvmrpg_nvmf.nvmf_opc = NVME_OPC_FABRIC_COMMAND;
	cmd->nvmrcu_prgt.nvmrpg_nvmf.nvmf_fctype = NVMF_FCTYPE_PROPGET;
	cmd->nvmrcu_prgt.nvmrpg_attrib = len;
	cmd->nvmrcu_prgt.nvmrpg_ofst = offset;

	ISSUE_WAIT_CHECK_REQ {
		ERRSPEW("PROPGET NVMeoF command to subNQN \"%s\" failed!\n",
		    cntrlr->nvmrctr_subnqn);
		error = ENXIO;
		goto out;
	}

	*valuep = (((uint64_t)status.cpl.rsvd1) << 32) | status.cpl.cdw0;
	error = 0;

out:
	return error;
}


#define NVMR_IOQID_ADDEND 1 /* Only a single AdminQ */
#define NVMR_PAYLOAD_UNIT 16U

static void
nvmr_register_cntrlr(nvmr_cntrlr_t cntrlr)
{
	mtx_lock(&nvmr_cntrlrs_lstslock);
	TAILQ_INSERT_HEAD(&nvmr_cntrlrs, cntrlr, nvmrctr_nxt);
	nvmr_cntrlrs_count++;
	mtx_unlock(&nvmr_cntrlrs_lstslock);
}

#define BAIL_IF_CNTRLR_CONDEMNED(c)                                        \
	if (atomic_load_acq_int(&(c)->nvmrctr_state) == NVMRC_CONDEMNED) { \
		error = ESHUTDOWN;                                         \
		goto out;                                                  \
	}


int
nvmr_cntrlr_create(nvmr_addr_t *addr, nvmr_cntrlrprof_t *prof,
    nvmr_cntrlr_t *retcntrlrp);
int
nvmr_cntrlr_create(nvmr_addr_t *addr, nvmr_cntrlrprof_t *prof,
    nvmr_cntrlr_t *retcntrlrp)
{
	int error, retval, count;
	struct make_dev_args md_args;
	nvmr_cntrlr_t cntrlr;
	nvmr_qpair_t *qarr;
	unsigned long tmp;
	nvmr_cntrlcap_t cntrlrcap;
	uint64_t cntrlrconf;
	uint32_t unitnum;
	nvmripv4_t ipv4;
	nvmr_qprof_t *pf;
	uint16_t port;
	/* uint8_t *identp; */
	char *retp;
	struct nvme_controller_data *cd;

	retval = -1;
	if ((addr->nvmra_subnqn == NULL) ||
	    (retval = strlen(addr->nvmra_subnqn), retval > MAX_NQN_LEN)) {
		ERRSPEW("Invalid NVMe Subsystem NQN string passed in: \"%s\" "
		    "of length %d\n", addr->nvmra_subnqn, retval);
		error = EINVAL;
		goto outret;
	}

	if (inet_pton(AF_INET, addr->nvmra_ipaddr, &ipv4) != 1) {
		ERRSPEW("Parsing failed for IPV4 address \"%s\"\n",
		    addr->nvmra_ipaddr);
		error = EINVAL;
		goto outret;
	}

	tmp = strtoul(addr->nvmra_port, &retp, 0);
	if ((*retp != '\0') || (tmp > UINT16_MAX)) {
		ERRSPEW("Parsing failed with %lu for RDMA port \"%s\"\n", tmp,
		    addr->nvmra_port);
		error = EINVAL;
		goto outret;
	}
	port = htons((uint16_t)tmp);

	cntrlr = malloc(sizeof(*cntrlr), M_NVMR, M_WAITOK|M_ZERO);
	if (cntrlr == NULL) {
		ERRSPEW("Controller allocation sized \"%zu\" failed\n",
		    sizeof(*cntrlr));
		error = ENOMEM;
		goto outret;
	}
	DBGSPEW("Controller allocation of size \"%zu\"@%p\n", sizeof(*cntrlr),
	    cntrlr);
	memcpy(&cntrlr->nvmrctr_ipv4, ipv4, sizeof(cntrlr->nvmrctr_ipv4));
	cntrlr->nvmrctr_port = port;
	cntrlr->nvmrctr_prof = prof;
	cntrlr->nvmrctr_subnqn = addr->nvmra_subnqn;
	cntrlr->nvmrctr_state = NVMRC_PRE_INIT;
	cntrlr->nvmrctr_nvmereg = FALSE;
	strncpy(cntrlr->very_first_field, NVMR_STRING, NVME_VFFSTRSZ);
	mtx_init(&cntrlr->nvmrctr_nvmec.lockc, "nvme ctrlr lock", NULL,
	    MTX_DEF);

	nvmr_register_cntrlr(cntrlr);

	/**********
	 Set up the NVMe stack facing fields so we can issue NVMe commands to
	 the target using the stack
	 **********/
	cntrlr->nvmrctr_nvmec.is_resetting = 0;
	cntrlr->nvmrctr_nvmec.notification_sent = 0;
	TASK_INIT(&cntrlr->nvmrctr_nvmec.reset_task, 0,
	    nvmr_ctrlr_reset_task, cntrlr);
	TASK_INIT(&cntrlr->nvmrctr_nvmec.fail_req_task, 0,
	    nvme_ctrlr_fail_req_task, &cntrlr->nvmrctr_nvmec);
	STAILQ_INIT(&cntrlr->nvmrctr_nvmec.fail_req);
	cntrlr->nvmrctr_nvmec.is_failed = FALSE;

	cntrlr->nvmrctr_nvmec.timeout_period = NVME_DEFAULT_TIMEOUT_PERIOD;
	cntrlr->nvmrctr_nvmec.max_xfer_size = NVME_MAX_XFER_SIZE;
	cntrlr->nvmrctr_nvmec.taskqueue = taskqueue_create("nvmr_taskq",
	    M_WAITOK, taskqueue_thread_enqueue,
	    &cntrlr->nvmrctr_nvmec.taskqueue);
	taskqueue_start_threads(&cntrlr->nvmrctr_nvmec.taskqueue, 1, PI_DISK,
	    "nvmr taskq");

	cntrlr->nvmrctr_nvmec.nvmec_tsp = cntrlr;
	cntrlr->nvmrctr_nvmec.nvmec_ttype = NVMET_RDMA;
	cntrlr->nvmrctr_nvmec.nvmec_delist = &nvmr_delist_cb;
	cntrlr->nvmrctr_nvmec.nvmec_subadmreq = &nvmr_submit_adm_req;
	cntrlr->nvmrctr_nvmec.nvmec_subioreq = &nvmr_submit_io_req;

	retval = nvmr_qpair_create(cntrlr, &cntrlr->nvmrctr_adminqp, 0,
	    NVMR_DYNANYCNTLID, &prof->nvmrp_qprofs[NVMR_QTYPE_ADMIN]);
	if (retval != 0) {
		ERRSPEW("%s creation failed with %d\n",
		    nvmr_qndx2name(NVMR_QTYPE_ADMIN), retval);
		error = retval;
		goto out;
	}

	retval = nvmr_admin_propget(cntrlr, 0, (uint64_t *)&cntrlrcap,
	    NVMR_PROPLEN_8BYTES);
	if (retval != 0) {
		error = retval;
		ERRSPEW("nvmr_admin_propget(o:0x%X l:%d) failed:%d\n", 0,
		    NVMR_PROPLEN_8BYTES, retval);
		goto out;
	}

	DBGSPEW("PROPGET CAP:\n\t"
	    "MQES:%lu CQR:%lu CMS:%lu TO:%lu DSTRD:%lu\n\t"
	    "NSSRS:%lu CSS:%lu BPS:%lu MPSMIN:%lu MPSMAX:%lu\n",
	    cntrlrcap.nvmrcc_mqes, cntrlrcap.nvmrcc_cqr, cntrlrcap.nvmrcc_ams,
	    cntrlrcap.nvmrcc_to, cntrlrcap.nvmrcc_dstrd, cntrlrcap.nvmrcc_nssrs,
	    cntrlrcap.nvmrcc_css, cntrlrcap.nvmrcc_bps, cntrlrcap.nvmrcc_mpsmin,
	    cntrlrcap.nvmrcc_mpsmax);

	cntrlrconf = 0;
	retval = nvmr_admin_propget(cntrlr, 20, &cntrlrconf,
	    NVMR_PROPLEN_4BYTES);
	if (retval != 0) {
		error = retval;
		ERRSPEW("nvmr_admin_propget(o:0x%X l:%d) failed:%d\n", 20,
		    NVMR_PROPLEN_4BYTES, retval);
		goto out;
	} else {
		DBGSPEW("cntrlrconf is 0x%08lX\n", cntrlrconf);
	}

	cntrlrconf = 0;
	retval = nvmr_admin_propget(cntrlr, 28, &cntrlrconf,
	    NVMR_PROPLEN_4BYTES);
	if (retval != 0) {
		error = retval;
		ERRSPEW("nvmr_admin_propget(o:0x%X l:%d) failed:%d\n", 28,
		    NVMR_PROPLEN_4BYTES, retval);
		goto out;
	} else {
		DBGSPEW("reg 28 is 0x%08lX\n", cntrlrconf);
	}

	cntrlrconf = 0;
	retval = nvmr_admin_propget(cntrlr, 8, &cntrlrconf,
	    NVMR_PROPLEN_4BYTES);
	if (retval != 0) {
		error = retval;
		ERRSPEW("nvmr_admin_propget(o:0x%X l:%d) failed:%d\n", 8,
		    NVMR_PROPLEN_4BYTES, retval);
		goto out;
	} else {
		DBGSPEW("Reg 8 is 0x%08lX\n", cntrlrconf);
	}

	cntrlrconf = 0;
	retval = nvmr_admin_propget(cntrlr, 0, (uint64_t *)&cntrlrcap,
	    NVMR_PROPLEN_8BYTES);
	if (retval != 0) {
		error = retval;
		ERRSPEW("nvmr_admin_propget(o:0x%X l:%d) failed:%d\n", 0,
		    NVMR_PROPLEN_8BYTES, retval);
		goto out;
	}

	DBGSPEW("PROPGET CAP:\n\t"
	    "MQES:%lu CQR:%lu CMS:%lu TO:%lu DSTRD:%lu\n\t"
	    "NSSRS:%lu CSS:%lu BPS:%lu MPSMIN:%lu MPSMAX:%lu\n",
	    cntrlrcap.nvmrcc_mqes, cntrlrcap.nvmrcc_cqr, cntrlrcap.nvmrcc_ams,
	    cntrlrcap.nvmrcc_to, cntrlrcap.nvmrcc_dstrd, cntrlrcap.nvmrcc_nssrs,
	    cntrlrcap.nvmrcc_css, cntrlrcap.nvmrcc_bps, cntrlrcap.nvmrcc_mpsmin,
	    cntrlrcap.nvmrcc_mpsmax);

	retval = nvmr_admin_propset(cntrlr, 0x14, 0x460001,
	    NVMR_PROPLEN_4BYTES);
	if (retval != 0) {
		error = retval;
		ERRSPEW("nvmr_admin_propset(o:0x%X l:%d) failed:%d\n", 0x14,
		    NVMR_PROPLEN_4BYTES, retval);
		goto out;
	}

	cntrlr->nvmrctr_nvmec.guard0 = cntrlr->nvmrctr_nvmec.guard1 = 
	    0xDEADD00D8BADBEEF;
	retval = nvmr_admin_identify(cntrlr, 0, 0, 1,
	    &cntrlr->nvmrctr_nvmec.cdata, sizeof(cntrlr->nvmrctr_nvmec.cdata));
	if (retval != 0) {
		error = retval;
		ERRSPEW("nvmr_admin_identify() failed:%d\n", retval);
		goto out;
	} else {
		KASSERT((cntrlr->nvmrctr_nvmec.guard0 == 0xDEADD00D8BADBEEF) &&
		    (cntrlr->nvmrctr_nvmec.guard1 == 0xDEADD00D8BADBEEF),
		    ("%s@%d Guards failed! %lx %lx", __func__, __LINE__,
		    cntrlr->nvmrctr_nvmec.guard0,
		    cntrlr->nvmrctr_nvmec.guard1));
		/*
		identp = (uint8_t *)&cntrlr->nvmrctr_nvmec.cdata;
		printf("       ");
		for (count = 0; count < 16; count++) {
			printf(" %02x", count);
		}
		printf("\n");
		printf("       ");
		for (count = 0; count < 16; count++) {
			printf(" vv");
		}
		printf("\n");
		for (count = 0; count < IDENTIFYLEN; count++) {
			if ((count % 16) == 0) {
				printf("0x%04x:", count);
			}
			printf(" %02hhX", identp[count]);
			if ((count % 16) == 15) {
				printf("\n");
			}
		}
		 */
	}
	cd = &cntrlr->nvmrctr_nvmec.cdata;
	DBGSPEW("ioccsz:%u iorcsz:%u icdoff:%hu ctrattr:%hhu msdbd:%hhu\n",
	     cd->nidf_ioccsz, cd->nidf_iorcsz, cd->nidf_icdoff,
	     cd->nidf_ctrattr, cd->nidf_msdbd);
	if ((cd->nidf_ioccsz*NVMR_PAYLOAD_UNIT) > sizeof(nvmr_stub_t)) {
		ERRSPEW("\n\t!!! Unimplemented inline data @%lu possible !!!\n",
		    (cd->nidf_ioccsz*NVMR_PAYLOAD_UNIT) - sizeof(nvmr_stub_t));
	}

	if (cntrlrcap.nvmrcc_dstrd != 0) {
		ERRSPEW("Non-zero Doorbell stride (%lu) unsupported\n",
		    cntrlrcap.nvmrcc_dstrd);
		error = ENOSPC;
		goto out;
	}

	cntrlr->nvmrctr_nvmec.min_page_size =
	    1 << (12 + cntrlrcap.nvmrcc_mpsmin);
	cntrlr->nvmrctr_nvmec.ready_timeout_in_ms = cntrlrcap.nvmrcc_to *
	    500;

	/**********
	 Come up with a unique unitnum that has enough room for
	 NVME_MAX_NAMESPACES sub-unitnums.
	 **********/
	unitnum = (uint32_t)((uint64_t)(&cntrlr->nvmrctr_nvmec)/
	    sizeof(cntrlr->nvmrctr_nvmec));
	unitnum &= INT_MAX;
	unitnum *= NVME_MAX_NAMESPACES;
	unitnum /= NVME_MAX_NAMESPACES;
	cntrlr->nvmrctr_nvmec.nvmec_unit = (int)(uint32_t)unitnum;

	make_dev_args_init(&md_args);
	md_args.mda_devsw = &nvmr_ctrlr_cdevsw;
	md_args.mda_uid = UID_ROOT;
	md_args.mda_gid = GID_WHEEL;
	md_args.mda_mode = 0600;
	md_args.mda_unit = cntrlr->nvmrctr_nvmec.nvmec_unit;/* Security hole? */
	md_args.mda_si_drv1 = (void *)cntrlr;
	retval = make_dev_s(&md_args, &cntrlr->nvmrctr_nvmec.ccdev, "nvme%d",
	    md_args.mda_unit);
	if (retval != 0) {
		ERRSPEW("make_dev_s() for cntrlr:%p returned %d\n", cntrlr,
		    retval);
		error = retval;
		goto out;
	}
	DBGSPEW("NVMe device with unitnum:%d\n",
	    cntrlr->nvmrctr_nvmec.nvmec_unit);

	retval = nvme_ctrlr_construct_namespaces(&cntrlr->nvmrctr_nvmec);
	if (retval != 0) {
		error = retval;
		ERRSPEW("nvme_ctrlr_construct_namespaces(c:%p):%d\n",
		    &cntrlr->nvmrctr_nvmec, error);
		goto out;
	}


	/**********
	 Now set up the IO Qs
	 **********/
	pf = &prof->nvmrp_qprofs[NVMR_QTYPE_IO];

	/* Get smarter about this */
	cntrlr->nvmrctr_numioqs = pf->nvmrqp_numqueues;

	BAIL_IF_CNTRLR_CONDEMNED(cntrlr);

	if (cntrlr->nvmrctr_numioqs != 0) {
		qarr = malloc(cntrlr->nvmrctr_numioqs * sizeof(nvmr_qpair_t),
		    M_NVMR, M_WAITOK|M_ZERO);
		if (qarr == NULL) {
			ERRSPEW("IO Q array allocation sized \"%zu\" failed\n",
			    cntrlr->nvmrctr_numioqs * sizeof(nvmr_qpair_t));
			error = ENOMEM;
			goto out;
		}
		DBGSPEW("IO Q array allocation of size \"%zu\"\n",
		    cntrlr->nvmrctr_numioqs * sizeof(nvmr_qpair_t));
		cntrlr->nvmrctr_ioqarr = qarr;

		/* Allocate IO queues and store pointers to them in the qarr */
		for (count = 0; count < pf->nvmrqp_numqueues; count++) {
			retval = nvmr_qpair_create(cntrlr, &qarr[count],
			    NVMR_IOQID_ADDEND + count,
			    cntrlr->nvmrctr_nvmec.cdata.ctrlr_id, pf);
			if (retval != 0) {
				ERRSPEW("%s#%d creation failed with %d\n",
				    nvmr_qndx2name(NVMR_QTYPE_IO), count,
				    retval);
				error = retval;
				goto out;
			}

			BAIL_IF_CNTRLR_CONDEMNED(cntrlr);
		}
		if (count != pf->nvmrqp_numqueues) {
			error = retval;
			goto out;
		}
		KASSERT(count == cntrlr->nvmrctr_numioqs,
		    ("count:%d numqs:%d", count, cntrlr->nvmrctr_numioqs));
	} else {
		cntrlr->nvmrctr_ioqarr = NULL;
	}

	nvmr_cntrlr_inited(cntrlr);
	BAIL_IF_CNTRLR_CONDEMNED(cntrlr); /* It's important this be here */

	cntrlr->nvmrctr_nvmec.is_initialized = 1;

	nvme_register_controller(&cntrlr->nvmrctr_nvmec);
	cntrlr->nvmrctr_nvmereg = TRUE;
	nvme_notify_new_controller(&cntrlr->nvmrctr_nvmec);

	error = 0;
	*retcntrlrp = cntrlr;

out:
	if (error == 0) {
		goto outret;
	}

	DBGSPEW("Controller:%p init failed:%d, queuing for destruction\n",
	    cntrlr, error);

	nvmr_cntrlr_condemn_enqueue(cntrlr);
	*retcntrlrp = NULL;

outret:
	return error;
}

/*
 * Invoked whenever the routine is registered with ib_register_client()
 * below or when an IB interface is added to the system.  In the former case
 * the routine is invoked for every IB interface already known.
 */
static void
nvmr_add_ibif(struct ib_device *ib_device)
{
	DBGSPEW("rdma_node_get_transport(%p)> %d\n", ib_device,
	    rdma_node_get_transport(ib_device->node_type));
}


/*
 * Invoked whenever the routine is unregistered with ib_unregister_client()
 * below or when an IB interface is removed from the system.  In the former case
 * the routine is invoked for every IB interface already known.
 */
static void
nvmr_remove_ibif(struct ib_device *ib_device, void *client_data)
{
	DBGSPEW("%p removed\n", ib_device);
}


static struct ib_client nvmr_ib_client = {
	.name   = "nvmrdma",
	.add    = nvmr_add_ibif,
	.remove = nvmr_remove_ibif
};

#define VELTESTSUBNQN "FromGUS"

nvmr_addr_t r640gent07eno1 = {
	.nvmra_ipaddr = "10.1.87.194",
	.nvmra_port =   "4420",
	.nvmra_subnqn = VELTESTSUBNQN,
};

nvmr_addr_t r640gent07enp94s0f1 = {
	.nvmra_ipaddr = "11.10.10.200",
	.nvmra_port =   "4420",
	.nvmra_subnqn = VELTESTSUBNQN,
};

#define VELADDR r640gent07eno1

static nvmr_cntrlr_t glbl_cntrlr;

static void
veladdr_connect(void)
{
	int retval;
	nvmr_cntrlrprof_t cntrlrprof = {};

	cntrlrprof = nvmr_regularprof;
	retval = nvmr_cntrlr_create(&VELADDR, &cntrlrprof, &glbl_cntrlr);
	if (retval != 0) {
		ERRSPEW("nvmr_cntrlr_create(\"%s\", \"%s\", \"%s\") failed "
		    "with %d\n", VELADDR.nvmra_ipaddr, VELADDR.nvmra_port,
		    VELADDR.nvmra_subnqn, retval);
	}

	return;
}


static void
veladdr_disconnect(void)
{
	if (glbl_cntrlr == NULL) {
		goto out;
	}

	nvmr_cntrlr_destroy_init(glbl_cntrlr);
	glbl_cntrlr = NULL;

out:
	return;
}


#define NVMR_CONNECT_CMD "attach"
#define NVMR_DISCONNECT_CMD "detach"

static int
nvmr_sysctl_veladdr_conn(SYSCTL_HANDLER_ARGS)
{
	int error;
	char buf[40], *src;

	if (glbl_cntrlr == NULL) {
		src = "unconnected";
	} else {
		src = "connected";
	}
	strlcpy(buf, src, sizeof(buf));
	error = sysctl_handle_string(oidp, buf, sizeof(buf), req);
	if ((error == 0) && (req->newptr != NULL)) {
		if (strncmp(buf, NVMR_CONNECT_CMD,
		    sizeof(NVMR_CONNECT_CMD)) == 0) {
			if (glbl_cntrlr != NULL) {
				goto out;
			}
			veladdr_connect();
		} else if (strncmp(buf, NVMR_DISCONNECT_CMD,
		    sizeof(NVMR_DISCONNECT_CMD)) == 0) {
			if (glbl_cntrlr == NULL) {
				goto out;
			}
			veladdr_disconnect();
		} else {
			goto out;
		}
	}

	if (glbl_cntrlr == NULL) {
		src = "unconnected";
	} else {
		src = "connected";
	}
	strlcpy(buf, src, sizeof(buf));
	error = sysctl_handle_string(oidp, buf, sizeof(buf), req);

out:
	return error;
}
static SYSCTL_NODE(_hw, OID_AUTO, nvmrdma, CTLFLAG_RD, 0, "NVMeoRDMA");
SYSCTL_PROC(_hw_nvmrdma, OID_AUTO, veladdr_conn,
	    CTLTYPE_STRING | CTLFLAG_RW,
	    NULL, 0, nvmr_sysctl_veladdr_conn, "A", NULL);


static void
nvmr_init(void)
{
	int retval;

	retval = ib_register_client(&nvmr_ib_client);
	if (retval != 0) {
		ERRSPEW("ib_register_client() for NVMeoF failed, ret:%d\n",
		    retval);
		goto out;
	}

	kern_uuidgen(&nvrdma_host_uuid, 1);
	snprintf_uuid(nvrdma_host_uuid_string, sizeof(nvrdma_host_uuid_string),
	    &nvrdma_host_uuid);
	DBGSPEW("Generated UUID is \"%s\"\n", nvrdma_host_uuid_string);

out:
	return;
}


static void
nvmr_uninit(void)
{
	DBGSPEW("Uninit invoked\n");

	veladdr_disconnect();

	nvmr_all_cntrlrs_condemn_init();

	mtx_lock(&nvmr_cntrlrs_lstslock);
	while (nvmr_cntrlrs_count != 0) {
		ERRSPEW("Waiting for all NVMr controllers to be deallocated\n");
		mtx_sleep(&nvmr_cntrlrs_count, &nvmr_cntrlrs_lstslock, 0,
		    "cdrain", HZ*2);
	}
	mtx_unlock(&nvmr_cntrlrs_lstslock);

	ib_unregister_client(&nvmr_ib_client);
}


SYSINIT(nvmr, SI_SUB_DRIVERS, SI_ORDER_ANY, nvmr_init, NULL);
SYSUNINIT(nvmr, SI_SUB_DRIVERS, SI_ORDER_ANY, nvmr_uninit, NULL);
MODULE_DEPEND(nvmr, linuxkpi, 1, 1, 1);
MODULE_DEPEND(nvmr, ibcore, 1, 1, 1);
MODULE_DEPEND(nvmr, nvme, 1, 1, 1);
MODULE_VERSION(nvmr, 1);
