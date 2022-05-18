/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Copyright (c) 2019-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*-
 *   This file is part of sci-spdk-nvme library.
 * 
 *   spdk_nvme.c
 * 
 *   Backend SPDK/NVMe IO interfaces for frontend intercepted system calls
 * 
 *   Part of this file comes from spdk/examples/nvme/perf/perf.c which is the
 *   SPDK's built-in performance evaluation tool, therefore its associated
 *   BSD LICENSE and Copyright Notice is retained as required (see above).
 *
 *   For details about SPDK please visit https://spdk.io/
 *
 *   Author: Colin Zhu <czhu@nexnt.com>
 */

#pragma GCC diagnostic ignored "-Wmissing-declarations"

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/fd.h"
#include "spdk/nvme.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/nvme_intel.h"
#include "spdk/histogram_data.h"
#include "spdk/endian.h"
#include "spdk/dif.h"
#include "spdk/util.h"
#include "spdk/likely.h"
#include "spdk/sock.h"

#include <linux/aio_abi.h>
#include <sys/sysinfo.h>

#include "sci_spdk_nvme.h"

struct ctrlr_entry {
	struct spdk_nvme_ctrlr				*ctrlr;
	enum spdk_nvme_transport_type		trtype;
	struct spdk_nvme_qpair				**unused_qpairs;
	TAILQ_ENTRY(ctrlr_entry)			link;
	char								name[1024];
};

enum entry_type {
	ENTRY_TYPE_NVME_NS,
	ENTRY_TYPE_AIO_FILE,
	ENTRY_TYPE_URING_FILE,
};

struct ns_fn_table;

struct ns_entry {
	enum entry_type					type;
	const struct ns_fn_table		*fn_table;

	union {
		struct {
			struct spdk_nvme_ctrlr	*ctrlr;
			struct spdk_nvme_ns		*ns;
		} nvme;
	} u;

	TAILQ_ENTRY(ns_entry)			link;
	uint32_t						io_size_blocks;
	uint32_t						num_io_requests;
	uint64_t						size_in_ios;
	uint32_t						block_size;
	uint32_t						md_size;
	bool							md_interleave;
	unsigned int					seed;
	bool							pi_loc;
	enum spdk_nvme_pi_type			pi_type;
	uint32_t						io_flags;
	char							name[1024];
	char							*dev_fn;	/* device file name */
};

struct ns_worker_ctx {
	struct ns_entry						*entry;
	uint64_t							current_queue_depth;
	uint64_t							offset_in_ios;
	bool								is_draining;

	union {
		struct {
			int							num_active_qpairs;
			int							num_all_qpairs;
			struct spdk_nvme_qpair		**qpair;
			struct spdk_nvme_poll_group	*group;
			int							last_qpair;
		} nvme;
	} u;

	TAILQ_ENTRY(ns_worker_ctx)			link;
	
	struct sci_ns_rings					*rings;
	uint64_t							ctx_id;
	int									cpu;
	int									pool_idx;
};

struct perf_task {
	struct ns_worker_ctx	*ns_ctx;
	struct iovec			*iovs;		/* array of iovecs to transfer. */
	int						iovcnt;		/* Number of iovecs in iovs array. */
	int						iovpos; 	/* Current iovec position. */
	uint32_t				iov_offset; /* Offset in current iovec. */
	uint64_t				submit_tsc;
	bool					is_read;
	struct spdk_dif_ctx		dif_ctx;

	void *user_buf;
	void *user_iocb;
	int io_size;
	uint64_t offset;
	void (*callback)(void *iocb, uint64_t reserved_1, uint64_t reserved_2);
	
	uint64_t reserved_1;	/* aio_context_t ctx_id */
	uint64_t reserved_2;
	
	int state;
#define SCI_TASK_FREE		0
#define SCI_TASK_REQ_IN		1
#define SCI_TASK_REQ_OUT	2
#define SCI_TASK_RSP_IN		3
#define SCI_TASK_RSP_OUT	4
};

struct worker_thread {
	TAILQ_HEAD(, ns_worker_ctx)	ns_ctx;
	TAILQ_ENTRY(worker_thread)	link;
	unsigned			lcore;
};

struct ns_fn_table {
	void (*setup_payload)(struct perf_task *task, uint8_t pattern);
	int (*submit_io)(struct perf_task *task, struct ns_worker_ctx *ns_ctx,
			     struct ns_entry *entry, uint64_t offset_in_ios);
	int64_t (*check_io)(struct ns_worker_ctx *ns_ctx);
	void (*verify_io)(struct perf_task *task, struct ns_entry *entry);
	int (*init_ns_worker_ctx)(struct ns_worker_ctx *ns_ctx);
	void (*cleanup_ns_worker_ctx)(struct ns_worker_ctx *ns_ctx);
	void (*dump_transport_stats)(uint32_t lcore, struct ns_worker_ctx *ns_ctx);
};

static TAILQ_HEAD(, ctrlr_entry) g_controllers = TAILQ_HEAD_INITIALIZER(g_controllers);
static TAILQ_HEAD(, ns_entry) g_namespaces = TAILQ_HEAD_INITIALIZER(g_namespaces);
static int g_num_namespaces;
static TAILQ_HEAD(, worker_thread) g_workers = TAILQ_HEAD_INITIALIZER(g_workers);
static int g_num_workers = 0;
static uint32_t g_main_core;
static pthread_barrier_t g_worker_sync_barrier;

static uint64_t g_tsc_rate;

static uint32_t g_max_io_md_size;
static uint32_t g_max_io_size_blocks;
static uint32_t g_metacfg_pract_flag;
static uint32_t g_metacfg_prchk_flags;
static int g_nr_io_queues_per_ns = 1;
static int g_nr_unused_io_queues;
static uint32_t g_max_completions;
static uint32_t g_disable_sq_cmb;
static bool g_warn;
static bool g_header_digest;
static bool g_data_digest;
static bool g_no_shn_notification;
/* The flag is used to exit the program while keep alive fails on the transport */
static bool g_exit;
/* Default to 10 seconds for the keep alive value. This value is arbitrary. */
static uint32_t g_keep_alive_timeout_in_ms = 10000;
static uint32_t g_quiet_count = 1;

/* When user specifies -Q, some error messages are rate limited.  When rate
 * limited, we only print the error message every g_quiet_count times the
 * error occurs.
 *
 * Note: the __count is not thread safe, meaning the rate limiting will not
 * be exact when running perf with multiple thread with lots of errors.
 * Thread-local __count would mean rate-limiting per thread which doesn't
 * seem as useful.
 */
#define RATELIMIT_LOG(...) \
	{								\
		static uint64_t __count = 0;				\
		if ((__count % g_quiet_count) == 0) {			\
			if (__count > 0 && g_quiet_count > 1) {		\
				fprintf(stderr, "Message suppressed %" PRIu32 " times: ",	\
					g_quiet_count - 1);		\
			}						\
			fprintf(stderr, __VA_ARGS__);			\
		}							\
		__count++;						\
	}

static pthread_mutex_t g_stats_mutex;

#define MAX_ALLOWED_PCI_DEVICE_NUM 128
static struct spdk_pci_addr g_allowed_pci_addr[MAX_ALLOWED_PCI_DEVICE_NUM];

struct trid_entry {
	struct spdk_nvme_transport_id	trid;
	uint16_t						nsid;
	char							hostnqn[SPDK_NVMF_NQN_MAX_LEN + 1];
	TAILQ_ENTRY(trid_entry)			tailq;
	char *dev_fn;
};

static TAILQ_HEAD(, trid_entry) g_trid_list = TAILQ_HEAD_INITIALIZER(g_trid_list);

static inline void
task_complete(struct perf_task *task);

/* connections with syscall intercept module */
static struct worker_thread *workers_by_cpu[MAX_CPU_CNT];
static void *sci_buf_pool_4_fio[MAX_JOB_CNT];	/* g_num_namespaces count = pool count */

/*****
 * SCI-SPDK parameters shared by sci_spdk.c and spdk_nvme.c
 *****/
static uint32_t fio_io_size = DEFAULT_IO_SIZE;
static int fio_q_depth = MAX_Q_DEPTH;
static int fio_io_align = DEFAULT_IO_ALIGN;
static uint64_t spdk_dma_pool_size = 0;
static int spdk_init_flag = 0;

static struct nvme_dummy_dev_t nvme_devs[MAX_JOB_CNT];

static char fio_cfg_line[BUFSIZ] = { '\0' };
void remove_spaces(char *str)
{
	int count = 0;
	for (int i = 0; str[i]; i++)
		if (!isspace(str[i])) str[count++] = str[i];
	str[count] = '\0';
}
static void spdk_nvme_dev_add_name(int fio_job, const char *fio_fn)
{
	struct nvme_dummy_dev_t *dev = &nvme_devs[fio_job-1];
	snprintf(dev->file_name, MAX_NVME_NAME_LEN, "%s", fio_fn);
	snprintf(dev->pci_name, MAX_NVME_NAME_LEN, "%s", fio_fn);
	for (int i = 0; dev->pci_name[i]; i++)
		if (dev->pci_name[i] == '_') dev->pci_name[i] = ':';	
}
static void spdk_nvme_dev_add_cpu(int fio_job, int cpu)
{
	struct nvme_dummy_dev_t *dev = &nvme_devs[fio_job-1];
	assert(cpu >= 0 && cpu < MAX_CPU_CNT);
	dev->cpu = cpu;
}
static int spdk_get_fio_config(void)
{
	FILE *file = NULL;
	
	file = fopen(FIO_CONF_FILE, "r");
	if (file == NULL)
		return 1;
	
	/* get fio io pattern config */
	while(fgets(fio_cfg_line, sizeof(fio_cfg_line), file) != NULL)
	{
		remove_spaces(fio_cfg_line);

		if (fio_cfg_line[0] == '#')
			continue;
		
		char *p_end;
		
		if (strstr(fio_cfg_line, "bs=") && !strstr(fio_cfg_line, "numjobs="))
			fio_io_size = strtol(fio_cfg_line+3, &p_end, 10);
		else if (strstr(fio_cfg_line, "iodepth="))
			fio_q_depth = strtol(fio_cfg_line+8, &p_end, 10);
		else if (strstr(fio_cfg_line, "iomem_align="))
			fio_io_align = strtol(fio_cfg_line+12, &p_end, 10);
	}
	fclose(file);
	file = NULL;
	
	uintptr_t page_size = sysconf(_SC_PAGESIZE);
	uintptr_t page_mask = page_size - 1;	
	spdk_dma_pool_size = (uint64_t)fio_io_size * (uint64_t)fio_q_depth + fio_io_align + page_mask * 2;
	
	if (fio_q_depth > MAX_Q_DEPTH)
	{
		printf("[SPDK] ERROR: invalid queue_depth %d, must be [1,%d]\n", fio_q_depth, MAX_Q_DEPTH);
		goto err;
	}
	
	if (fio_io_align != 512 && fio_io_align != 4096)
	{
		printf("[SPDK] ERROR: invalid io_align %d, must be either 512 or 4096\n", fio_io_align);
		goto err;
	}
	
	/* get fio job config */
	file = fopen(FIO_CONF_FILE, "r");
	if (file == NULL)
		return 1;
	
	int job_stage = 0;
	int curr_job = -1;
	while(fgets(fio_cfg_line, sizeof(fio_cfg_line), file) != NULL)
	{
		char *p_end;
		remove_spaces(fio_cfg_line);
		
		if (fio_cfg_line[0] == '#')
			continue;
		
		/* [job#] */
		if (strstr(fio_cfg_line, "[job"))
		{
			if (job_stage % 3 != 0)
			{
				printf("[SPDK] ERROR: invalid job config, job(%d) missing %s, stage(%d) #\n",
					curr_job, (job_stage % 3) == 1 ? "filename=" : "cpu_allowed=", job_stage);
				goto err;
			}
			job_stage++;
			curr_job = strtol(fio_cfg_line+4, &p_end, 10);
		}
		else if (strstr(fio_cfg_line, "filename="))
		{
			if (job_stage % 3 != 1)
			{
				printf("[SPDK] ERROR: invalid job config, job(%d) missing %s, stage(%d) ##\n",
					curr_job, (job_stage % 3) == 2 ? "[job#]" : "cpu_allowed=", job_stage);
				goto err;
			}
			job_stage++;
			assert(curr_job >= 1 && curr_job <= MAX_JOB_CNT);
			char *fn = fio_cfg_line+9;
			spdk_nvme_dev_add_name(curr_job, fn);
		}
		else if (strstr(fio_cfg_line, "cpus_allowed="))
		{
			if (job_stage % 3 != 2)
			{
				printf("[SPDK] ERROR: invalid job config, job(%d) missing %s, stage(%d) ###\n",
					curr_job, (job_stage % 3) == 0 ? "[job#]" : "filename=", job_stage);
				goto err;
			}
			job_stage++;			
			assert(curr_job >= 1 && curr_job <= MAX_JOB_CNT);
			int cpu = strtol(fio_cfg_line+13, &p_end, 10);
			spdk_nvme_dev_add_cpu(curr_job, cpu);
		}
	}
	if (job_stage % 3 != 0)
	{
		printf("[SPDK] ERROR: invalid job config, job(%d) missing filename/cpu_allowed, stage(%d) ####\n",
			curr_job, job_stage);
		goto err;
	}

	fclose(file);
	file = NULL;
	
	for (int i=0; i<MAX_JOB_CNT; i++)
	{
		struct nvme_dummy_dev_t *dev = &nvme_devs[i];
		if (dev->cpu < 0)
			continue;
		//printf("[SPDK] nvme fs(%s) pci(%s) cpu(%d)\n",
		//	dev->file_name, dev->pci_name, dev->cpu);
	}
		
	return 0;
	
err:
	if (file != NULL)
		fclose(file);
	exit(1);
}

/*****
 * Exported functions to be called by sci_spdk.c
 *****/
void spdk_collect_fio_config(int *io_size, int *q_depth, int *io_align, uint64_t *pool_size)
{
	*io_size = fio_io_size;
	*q_depth = fio_q_depth;
	*io_align = fio_io_align;
	*pool_size = spdk_dma_pool_size;
}
int spdk_check_init_status(void)
{
	return spdk_init_flag;
}
struct nvme_dummy_dev_t *spdk_get_nvme_devices(void)
{
	return nvme_devs;
}

/***
 * ns_ctx requset and response rings
 ***/
struct sci_ns_rings {
	pthread_mutex_t req_q_lock;
	pthread_mutex_t rsp_q_lock;
	int req_q_in;
	int req_q_next_fill;
	struct perf_task *req_q[MAX_RING_CNT];
};
static void sci_req_q_claim_entry(struct perf_task *task)
{
	struct ns_worker_ctx *ns_ctx = task->ns_ctx;
	
	//pthread_mutex_lock(&ns_ctx->rings->req_q_lock);
	task->state = SCI_TASK_FREE;
	int claim_idx = ns_ctx->rings->req_q_next_fill;
	assert(ns_ctx->rings->req_q[claim_idx] == NULL);
	ns_ctx->rings->req_q[claim_idx] = task;
	ns_ctx->rings->req_q_next_fill++;
	if (ns_ctx->rings->req_q_next_fill >= MAX_RING_CNT)
		ns_ctx->rings->req_q_next_fill = 0;
	//pthread_mutex_unlock(&ns_ctx->rings->req_q_lock);
}
static inline void submit_single_io(struct perf_task *task);
static int sci_req_q_send_task(struct ns_worker_ctx *ns_ctx, int io_size, 
	bool is_read, uint64_t offset, char wr_char, 
	void *user_buf, void *user_iocb, void *callback,
	uint64_t reserved_1, uint64_t reserved_2)
{
	int rval = 0;
	struct perf_task *task = NULL;
	
	//pthread_mutex_lock(&ns_ctx->rings->req_q_lock);
	
	int ring_idx = ns_ctx->rings->req_q_in;
	
	/* not free task entry filled yet */
	if (ns_ctx->rings->req_q[ring_idx] == NULL)
	{
		printf("ERROR: sci_req_q_send_task NULL ring entry, ring_idx(%d)\n", ring_idx);
		//pthread_mutex_unlock(&ns_ctx->rings->req_q_lock);
		return -1;
	}
	
	task = ns_ctx->rings->req_q[ring_idx];
	if (task->state != SCI_TASK_FREE)	/* this shall not happen */
	{
		printf("ERROR: sci_req_q_in_task non-free ring entry (%d)\n", task->state);
		//pthread_mutex_unlock(&ns_ctx->rings->req_q_lock);
		return -1;		
	}
	
	task->io_size = io_size;
	task->is_read = is_read;
	task->offset = offset;
	task->user_buf = user_buf;
	task->user_iocb = user_iocb;
	task->state = SCI_TASK_REQ_IN;
	task->callback = callback;
	task->reserved_1 = reserved_1;
	task->reserved_2 = reserved_2;
	
	ns_ctx->rings->req_q[ring_idx] = NULL; /* removed out of ring */
	
	ns_ctx->rings->req_q_in++;
	if (ns_ctx->rings->req_q_in >= MAX_RING_CNT)
		ns_ctx->rings->req_q_in = 0;
	
	//pthread_mutex_unlock(&ns_ctx->rings->req_q_lock);
	
	if (task != NULL)
		submit_single_io(task);
	
	return rval;	
}
/* Linux AIO interface with SPDK */
int sci_io_submit(int spdk_dev_idx, struct iocb *cb, void *callback, aio_context_t ctx_id)
{
	int rval = 0;
	
	assert(spdk_dev_idx >= 0 && spdk_dev_idx < MAX_JOB_CNT);
	struct nvme_dummy_dev_t *dev = &nvme_devs[spdk_dev_idx];
	int cpu = dev->cpu;
	
	int io_size = cb->aio_nbytes;
	uint64_t offset = cb->aio_offset;
	bool is_read = 0;
	
	if (cb->aio_lio_opcode == IOCB_CMD_PREAD)
		is_read = 1;
	
	struct worker_thread *worker = NULL;
	worker = workers_by_cpu[cpu];
	assert(worker != NULL);
	
	struct ns_worker_ctx *ns_ctx = NULL;
	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link)
	{
		if (ctx_id == ns_ctx->ctx_id)
			break;
		
		/* first io assign ctx_id to ns_ctx */
		if (strcmp(ns_ctx->entry->dev_fn, dev->file_name) == 0)
		{
			ns_ctx->ctx_id = ctx_id;
			break;
		}
	}
	assert(ns_ctx != NULL);
	
	void *buf = (void *)cb->aio_buf;
	
	rval = sci_req_q_send_task(ns_ctx, io_size, is_read, offset, 'A', buf, cb, callback, (uint64_t)ctx_id, 0);
	
	return rval;
}
void sci_io_getevents(int spdk_dev_idx, aio_context_t ctx_id)
{
	assert(spdk_dev_idx >= 0 && spdk_dev_idx < MAX_JOB_CNT);
	struct nvme_dummy_dev_t *dev = &nvme_devs[spdk_dev_idx];
	int cpu = dev->cpu;

	struct worker_thread *worker = NULL;
	worker = workers_by_cpu[cpu];
	assert(worker != NULL);

	struct ns_worker_ctx *ns_ctx = NULL;
	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link)
	{
		if (ctx_id == ns_ctx->ctx_id)
			break;
	}
	assert(ns_ctx != NULL);

	ns_ctx->entry->fn_table->check_io(ns_ctx);
}

static void io_complete(void *ctx, const struct spdk_nvme_cpl *cpl);

static int
nvme_submit_io(struct perf_task *task, struct ns_worker_ctx *ns_ctx,
	       struct ns_entry *entry, uint64_t offset_in_ios)
{
	uint64_t lba;
	int qp_num;

	lba = offset_in_ios * entry->io_size_blocks;

	qp_num = ns_ctx->u.nvme.last_qpair;
	ns_ctx->u.nvme.last_qpair++;
	if (ns_ctx->u.nvme.last_qpair == ns_ctx->u.nvme.num_active_qpairs) {
		ns_ctx->u.nvme.last_qpair = 0;
	}
	
	/* TODO: support just one iovcnt for now, add readv/writev later */
	assert(task->iovcnt == 1);
	task->iovs[0].iov_base = task->user_buf;
	
	if (task->is_read) {
			return spdk_nvme_ns_cmd_read(entry->u.nvme.ns, ns_ctx->u.nvme.qpair[qp_num],
					task->iovs[0].iov_base,
					lba,
					entry->io_size_blocks, io_complete,
					task, 0);
	} else {
			return spdk_nvme_ns_cmd_write(entry->u.nvme.ns, ns_ctx->u.nvme.qpair[qp_num],
					task->iovs[0].iov_base,
					lba,
					entry->io_size_blocks, io_complete,
					task, 0);
	}
}

static void
perf_disconnect_cb(struct spdk_nvme_qpair *qpair, void *ctx)
{

}

static int64_t
nvme_check_io(struct ns_worker_ctx *ns_ctx)
{
	int64_t rc;

	rc = spdk_nvme_poll_group_process_completions(ns_ctx->u.nvme.group, g_max_completions,
			perf_disconnect_cb);
	if (rc < 0) {
		fprintf(stderr, "NVMe io qpair process completion error\n");
		exit(1);
	}
	return rc;
}

/*
 * TODO: If a controller has multiple namespaces, they could all use the same queue.
 *  For now, give each namespace/thread combination its own queue.
 */
static int
nvme_init_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	struct spdk_nvme_io_qpair_opts opts;
	struct ns_entry *entry = ns_ctx->entry;
	struct spdk_nvme_poll_group *group;
	struct spdk_nvme_qpair *qpair;
	int i;

	ns_ctx->u.nvme.num_active_qpairs = g_nr_io_queues_per_ns;
	ns_ctx->u.nvme.num_all_qpairs = g_nr_io_queues_per_ns + g_nr_unused_io_queues;
	ns_ctx->u.nvme.qpair = calloc(ns_ctx->u.nvme.num_all_qpairs, sizeof(struct spdk_nvme_qpair *));
	if (!ns_ctx->u.nvme.qpair) {
		return -1;
	}

	spdk_nvme_ctrlr_get_default_io_qpair_opts(entry->u.nvme.ctrlr, &opts, sizeof(opts));
	if (opts.io_queue_requests < entry->num_io_requests) {
		opts.io_queue_requests = entry->num_io_requests;
	}
	opts.delay_cmd_submit = true;
	opts.create_only = true;

	ns_ctx->u.nvme.group = spdk_nvme_poll_group_create(NULL, NULL);
	if (ns_ctx->u.nvme.group == NULL) {
		goto poll_group_failed;
	}

	group = ns_ctx->u.nvme.group;
	for (i = 0; i < ns_ctx->u.nvme.num_all_qpairs; i++) {
		ns_ctx->u.nvme.qpair[i] = spdk_nvme_ctrlr_alloc_io_qpair(entry->u.nvme.ctrlr, &opts,
					  sizeof(opts));
		qpair = ns_ctx->u.nvme.qpair[i];
		if (!qpair) {
			printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair failed\n");
			goto qpair_failed;
		}

		if (spdk_nvme_poll_group_add(group, qpair)) {
			printf("ERROR: unable to add I/O qpair to poll group.\n");
			spdk_nvme_ctrlr_free_io_qpair(qpair);
			goto qpair_failed;
		}

		if (spdk_nvme_ctrlr_connect_io_qpair(entry->u.nvme.ctrlr, qpair)) {
			printf("ERROR: unable to connect I/O qpair.\n");
			spdk_nvme_ctrlr_free_io_qpair(qpair);
			goto qpair_failed;
		}
	}

	return 0;

qpair_failed:
	for (; i > 0; --i) {
		spdk_nvme_ctrlr_free_io_qpair(ns_ctx->u.nvme.qpair[i - 1]);
	}

	spdk_nvme_poll_group_destroy(ns_ctx->u.nvme.group);
poll_group_failed:
	free(ns_ctx->u.nvme.qpair);
	return -1;
}

static void
nvme_cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	int i;

	for (i = 0; i < ns_ctx->u.nvme.num_all_qpairs; i++) {
		spdk_nvme_ctrlr_free_io_qpair(ns_ctx->u.nvme.qpair[i]);
	}

	spdk_nvme_poll_group_destroy(ns_ctx->u.nvme.group);
	free(ns_ctx->u.nvme.qpair);
}

static const struct ns_fn_table nvme_fn_table = {
	.setup_payload			= NULL,
	.submit_io				= nvme_submit_io,
	.check_io				= nvme_check_io,
	.verify_io				= NULL,
	.init_ns_worker_ctx		= nvme_init_ns_worker_ctx,
	.cleanup_ns_worker_ctx	= nvme_cleanup_ns_worker_ctx,
	.dump_transport_stats	= NULL
};

static int
build_nvme_name(char *name, size_t length, struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_transport_id *trid;
	int res = 0;

	trid = spdk_nvme_ctrlr_get_transport_id(ctrlr);

	switch (trid->trtype) {
	case SPDK_NVME_TRANSPORT_PCIE:
		res = snprintf(name, length, "PCIE (%s)", trid->traddr);
		break;
	default:
		fprintf(stderr, "Unknown transport type %d\n", trid->trtype);
		break;
	}
	return res;
}

static void
build_nvme_ns_name(char *name, size_t length, struct spdk_nvme_ctrlr *ctrlr, uint32_t nsid)
{
	int res = 0;

	res = build_nvme_name(name, length, ctrlr);
	if (res > 0) {
		snprintf(name + res, length - res, " NSID %u", nsid);
	}

}

static void
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns, char *dev_fn)
{
	struct ns_entry *entry;
	const struct spdk_nvme_ctrlr_data *cdata;
	uint32_t max_xfer_size, entries, sector_size;
	uint64_t ns_size;
	struct spdk_nvme_io_qpair_opts opts;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (!spdk_nvme_ns_is_active(ns)) {
		printf("Controller %-20.20s (%-20.20s): Skipping inactive NS %u\n",
		       cdata->mn, cdata->sn,
		       spdk_nvme_ns_get_id(ns));
		g_warn = true;
		return;
	}

	ns_size = spdk_nvme_ns_get_size(ns);
	sector_size = spdk_nvme_ns_get_sector_size(ns);

	if (ns_size < fio_io_size || sector_size > fio_io_size) {
		printf("WARNING: controller %-20.20s (%-20.20s) ns %u has invalid "
		       "ns size %" PRIu64 " / block size %u for I/O size %u\n",
		       cdata->mn, cdata->sn, spdk_nvme_ns_get_id(ns),
		       ns_size, spdk_nvme_ns_get_sector_size(ns), fio_io_size);
		g_warn = true;
		return;
	}

	max_xfer_size = spdk_nvme_ns_get_max_io_xfer_size(ns);
	spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &opts, sizeof(opts));
	/* NVMe driver may add additional entries based on
	 * stripe size and maximum transfer size, we assume
	 * 1 more entry be used for stripe.
	 */
	entries = (fio_io_size - 1) / max_xfer_size + 2;
	if ((fio_q_depth * entries) > opts.io_queue_size) {
		printf("controller IO queue size %u less than required\n",
		       opts.io_queue_size);
		printf("Consider using lower queue depth or small IO size because "
		       "IO requests may be queued at the NVMe driver.\n");
	}
	/* For requests which have children requests, parent request itself
	 * will also occupy 1 entry.
	 */
	entries += 1;

	entry = calloc(1, sizeof(struct ns_entry));
	if (entry == NULL) {
		perror("ns_entry malloc");
		exit(1);
	}

	entry->type = ENTRY_TYPE_NVME_NS;
	entry->fn_table = &nvme_fn_table;
	entry->u.nvme.ctrlr = ctrlr;
	entry->u.nvme.ns = ns;
	entry->num_io_requests = fio_q_depth * entries;

	entry->size_in_ios = ns_size / fio_io_size;
	entry->io_size_blocks = fio_io_size / sector_size;

	entry->block_size = spdk_nvme_ns_get_extended_sector_size(ns);
	entry->md_size = spdk_nvme_ns_get_md_size(ns);
	entry->md_interleave = spdk_nvme_ns_supports_extended_lba(ns);
	entry->pi_loc = spdk_nvme_ns_get_data(ns)->dps.md_start;
	entry->pi_type = spdk_nvme_ns_get_pi_type(ns);
	
	//printf("ns_size=%lu, sector_size=%d, entry->block_size=%d max_xfer_size=%d\n",
	//	ns_size, sector_size, entry->block_size, max_xfer_size);

	if (spdk_nvme_ns_get_flags(ns) & SPDK_NVME_NS_DPS_PI_SUPPORTED) {
		entry->io_flags = g_metacfg_pract_flag | g_metacfg_prchk_flags;
		printf("WARNING: PI not supported\n");
		g_warn = true;
		free(entry);
		return;
	}

	/* If metadata size = 8 bytes, PI is stripped (read) or inserted (write),
	 *  and so reduce metadata size from block size.  (If metadata size > 8 bytes,
	 *  PI is passed (read) or replaced (write).  So block size is not necessary
	 *  to change.)
	 */
	if ((entry->io_flags & SPDK_NVME_IO_FLAGS_PRACT) && (entry->md_size == 8)) {
		entry->block_size = spdk_nvme_ns_get_sector_size(ns);
	}

	if (fio_io_size % entry->block_size != 0) {
		printf("WARNING: IO size %u (-o) is not a multiple of nsid %u sector size %u."
		       " Removing this ns from test\n", fio_io_size, spdk_nvme_ns_get_id(ns), entry->block_size);
		g_warn = true;
		free(entry);
		return;
	}

	if (g_max_io_md_size < entry->md_size) {
		g_max_io_md_size = entry->md_size;
	}

	if (g_max_io_size_blocks < entry->io_size_blocks) {
		g_max_io_size_blocks = entry->io_size_blocks;
	}

	entry->dev_fn = dev_fn;
	build_nvme_ns_name(entry->name, sizeof(entry->name), ctrlr, spdk_nvme_ns_get_id(ns));
	
	//printf("[SPDK] dev_fn(%s) entry_name(%s)\n", dev_fn, entry->name);

	g_num_namespaces++;
	TAILQ_INSERT_TAIL(&g_namespaces, entry, link);
}

static void
unregister_namespaces(void)
{
	struct ns_entry *entry, *tmp;

	TAILQ_FOREACH_SAFE(entry, &g_namespaces, link, tmp) {
		TAILQ_REMOVE(&g_namespaces, entry, link);
		free(entry);
	}
}

static void
register_ctrlr(struct spdk_nvme_ctrlr *ctrlr, struct trid_entry *trid_entry)
{
	struct spdk_nvme_ns *ns;
	struct ctrlr_entry *entry = malloc(sizeof(struct ctrlr_entry));
	uint32_t nsid;

	if (entry == NULL) {
		perror("ctrlr_entry malloc");
		exit(1);
	}

	build_nvme_name(entry->name, sizeof(entry->name), ctrlr);

	entry->ctrlr = ctrlr;
	entry->trtype = trid_entry->trid.trtype;
	TAILQ_INSERT_TAIL(&g_controllers, entry, link);

	if (trid_entry->nsid == 0) {
		for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
		     nsid != 0; nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
			ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
			if (ns == NULL) {
				continue;
			}
			//printf("[SPDK] WARN: multiple ns for one trid_entry(%s)\n", trid_entry->dev_fn);
			register_ns(ctrlr, ns, trid_entry->dev_fn);
		}
	} else {
		ns = spdk_nvme_ctrlr_get_ns(ctrlr, trid_entry->nsid);
		if (!ns) {
			perror("Namespace does not exist.");
			exit(1);
		}

		register_ns(ctrlr, ns, trid_entry->dev_fn);
	}
}

static inline void
submit_single_io(struct perf_task *task)
{
	uint64_t		offset_in_ios;
	int			rc;
	struct ns_worker_ctx	*ns_ctx = task->ns_ctx;
	struct ns_entry		*entry = ns_ctx->entry;

	offset_in_ios = task->offset / fio_io_size;

	task->submit_tsc = spdk_get_ticks();

	/* task->is_read already set, no need randomness settings */

	rc = entry->fn_table->submit_io(task, ns_ctx, entry, offset_in_ios);

	if (spdk_unlikely(rc != 0)) {
		RATELIMIT_LOG("starting I/O failed\n");
		free(task->iovs);
		free(task);
	} else {
		//pthread_mutex_lock(&ns_ctx->rings->req_q_lock);
		ns_ctx->current_queue_depth++;
		//pthread_mutex_unlock(&ns_ctx->rings->req_q_lock);
	}
}

static inline void
task_complete(struct perf_task *task)
{
	struct ns_worker_ctx	*ns_ctx;

	ns_ctx = task->ns_ctx;
	//pthread_mutex_lock(&ns_ctx->rings->req_q_lock);
	ns_ctx->current_queue_depth--;
	//pthread_mutex_unlock(&ns_ctx->rings->req_q_lock);

	/*
	 * is_draining indicates when time has expired for the test run
	 * and we are just waiting for the previously submitted I/O
	 * to complete.  In this case, do not submit a new I/O to replace
	 * the one just completed.
	 */
	if (spdk_unlikely(ns_ctx->is_draining)) {
		printf("task_complete: draining\n");
		free(task->iovs);
		free(task);
	} else {
		if (task->user_iocb != NULL && task->callback != NULL)
			task->callback(task->user_iocb, task->reserved_1, task->reserved_2);
		sci_req_q_claim_entry(task);		
	}
}

static void
io_complete(void *ctx, const struct spdk_nvme_cpl *cpl)
{
	struct perf_task *task = ctx;

	if (spdk_unlikely(spdk_nvme_cpl_is_error(cpl))) {
		if (task->is_read) {
			RATELIMIT_LOG("Read completed with error (sct=%d, sc=%d)\n",
				      cpl->status.sct, cpl->status.sc);
		} else {
			RATELIMIT_LOG("Write completed with error (sct=%d, sc=%d)\n",
				      cpl->status.sct, cpl->status.sc);
		}
		if (cpl->status.sct == SPDK_NVME_SCT_GENERIC &&
		    cpl->status.sc == SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT) {
			/* The namespace was hotplugged.  Stop trying to send I/O to it. */
			task->ns_ctx->is_draining = true;
		}
	}

	task_complete(task);
}

static void
sci_nvme_setup_payload(struct perf_task *task, int q_index, struct ns_worker_ctx *ns_ctx)
{
	task->iovcnt = 1;
	task->iovs = calloc(task->iovcnt, sizeof(struct iovec));
	if (!task->iovs) {
		fprintf(stderr, "perf task failed to allocate iovs\n");
		exit(1);
	}
	/* task->iovs[0].iov_base will be assigned in io_submit() */
}

static struct perf_task *
allocate_task(struct ns_worker_ctx *ns_ctx, int queue_depth)
{
	struct perf_task *task;

	task = calloc(1, sizeof(*task));
	if (task == NULL) {
		fprintf(stderr, "Out of memory allocating tasks\n");
		exit(1);
	}

	sci_nvme_setup_payload(task, queue_depth, ns_ctx);

	task->ns_ctx = ns_ctx;	

	return task;
}

static int
init_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	return ns_ctx->entry->fn_table->init_ns_worker_ctx(ns_ctx);
}

static void
cleanup_ns_worker_ctx(struct ns_worker_ctx *ns_ctx)
{
	ns_ctx->entry->fn_table->cleanup_ns_worker_ctx(ns_ctx);
}

extern void sci_spdk_print_stats(const char *str);

static int
work_fn(void *arg)
{
	struct worker_thread *worker = (struct worker_thread *) arg;
	struct ns_worker_ctx *ns_ctx = NULL;
	int rc;
	
	//printf("   >>> work_fn entered: %p, lcore(%d)\n", worker, worker->lcore);

	/* Allocate queue pairs for each namespace. */
	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link) {
		if (init_ns_worker_ctx(ns_ctx) != 0) {
			printf("ERROR: init_ns_worker_ctx() failed\n");
			/* Wait on barrier to avoid blocking of successful workers */
			pthread_barrier_wait(&g_worker_sync_barrier);
			return 1;
		}
	}

	rc = pthread_barrier_wait(&g_worker_sync_barrier);
	if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
		printf("ERROR: failed to wait on thread sync barrier\n");
		return 1;
	}

	TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link)
	{
		int queue_depth = fio_q_depth;
		while (queue_depth-- > 0)
		{
			struct perf_task *task = allocate_task(ns_ctx, queue_depth);
			sci_req_q_claim_entry(task);
		}		
		//break; DO NOT break
	}

	//printf("worker(%p) initial alloc task ready\n", worker);
	return 0;
}

static void usage(char *param)
{
	printf("ERROR: invalid parameter for %s", param);
}

static void
unregister_trids(void)
{
	struct trid_entry *trid_entry, *tmp;

	TAILQ_FOREACH_SAFE(trid_entry, &g_trid_list, tailq, tmp) {
		TAILQ_REMOVE(&g_trid_list, trid_entry, tailq);
		free(trid_entry);
	}
}

static int
add_trid(const char *trid_str, char *dev_fn)
{
	struct trid_entry *trid_entry;
	struct spdk_nvme_transport_id *trid;

	trid_entry = calloc(1, sizeof(*trid_entry));
	if (trid_entry == NULL) {
		return -1;
	}

	trid = &trid_entry->trid;
	trid->trtype = SPDK_NVME_TRANSPORT_PCIE;
	snprintf(trid->subnqn, sizeof(trid->subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);
	trid_entry->dev_fn = dev_fn;

	if (spdk_nvme_transport_id_parse(trid, trid_str) != 0) {
		fprintf(stderr, "Invalid transport ID format '%s'\n", trid_str);
		free(trid_entry);
		return 1;
	}

	TAILQ_INSERT_TAIL(&g_trid_list, trid_entry, tailq);
	return 0;
}

static char core_mask_str[24];
static int
parse_args(struct spdk_env_opts *env_opts)
{
	env_opts->mem_size = -1;
	
	/* check active cores and construct core_mask */
	int core_cnt = get_nprocs();
	unsigned long long core_mask = (1ULL<<core_cnt) - 1;
	snprintf(core_mask_str, 24, "%llx", core_mask);
	env_opts->core_mask = core_mask_str;
	
	//printf("[SPDK] core_mask=%s\n", env_opts->core_mask);
	
	for (int i=0; i<MAX_JOB_CNT; i++)
	{
		struct nvme_dummy_dev_t *dev = &nvme_devs[i];
		if (dev->cpu < 0)
			continue;
		
		/* device name and cpu assigned from fio conf early on */
		char trtype_str[64];
		snprintf(trtype_str, 64, "trtype:PCIe traddr:%s", dev->pci_name);
		
		if (add_trid(trtype_str, dev->file_name)) {
			printf("ERROR add_trid(%s)\n", trtype_str);
			return 1;
		}
		
		//printf("[SPDK] add_trid(%s)\n", trtype_str);		
	}

	if (!g_nr_io_queues_per_ns) {
		usage("g_nr_io_queues_per_ns\n");
		return 1;
	}

	if (!g_quiet_count) {
		usage("-Q (--skip-errors) value must be greater than 0\n");
		return 1;
	}
	
	env_opts->no_pci = false;

	return 0;
}

static int
register_workers(void)
{
	uint32_t i;
	struct worker_thread *worker;

	SPDK_ENV_FOREACH_CORE(i) {
		worker = calloc(1, sizeof(*worker));
		if (worker == NULL) {
			fprintf(stderr, "Unable to allocate worker\n");
			return -1;
		}

		TAILQ_INIT(&worker->ns_ctx);
		worker->lcore = i;
		workers_by_cpu[i] = worker;
		TAILQ_INSERT_TAIL(&g_workers, worker, link);
		g_num_workers++;
	}

	return 0;
}

static void
unregister_workers(void)
{
	struct worker_thread *worker, *tmp_worker;
	struct ns_worker_ctx *ns_ctx, *tmp_ns_ctx;

	/* Free namespace context and worker thread */
	TAILQ_FOREACH_SAFE(worker, &g_workers, link, tmp_worker) {
		TAILQ_REMOVE(&g_workers, worker, link);

		TAILQ_FOREACH_SAFE(ns_ctx, &worker->ns_ctx, link, tmp_ns_ctx) {
			TAILQ_REMOVE(&worker->ns_ctx, ns_ctx, link);
			free(ns_ctx);
		}

		workers_by_cpu[worker->lcore] = NULL;
		
		free(worker);
	}
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	struct trid_entry *trid_entry = cb_ctx;

	if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE) {
		if (g_disable_sq_cmb) {
			opts->use_cmb_sqs = false;
		}
		if (g_no_shn_notification) {
			opts->no_shn_notification = true;
		}
	}

	if (trid->trtype != trid_entry->trid.trtype &&
	    strcasecmp(trid->trstring, trid_entry->trid.trstring)) {
		return false;
	}

	/* Set io_queue_size to UINT16_MAX, NVMe driver
	 * will then reduce this to MQES to maximize
	 * the io_queue_size as much as possible.
	 */
	opts->io_queue_size = UINT16_MAX;

	/* Set the header and data_digest */
	opts->header_digest = g_header_digest;
	opts->data_digest = g_data_digest;
	opts->keep_alive_timeout_ms = g_keep_alive_timeout_in_ms;
	memcpy(opts->hostnqn, trid_entry->hostnqn, sizeof(opts->hostnqn));

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct trid_entry	*trid_entry = cb_ctx;
	struct spdk_pci_addr	pci_addr;
	struct spdk_pci_device	*pci_dev;
	struct spdk_pci_id	pci_id;

	if (trid->trtype != SPDK_NVME_TRANSPORT_PCIE) {
		printf("Attached to NVMe over Fabrics controller at %s:%s: %s\n",
		       trid->traddr, trid->trsvcid,
		       trid->subnqn);
	} else {
		if (spdk_pci_addr_parse(&pci_addr, trid->traddr)) {
			return;
		}

		pci_dev = spdk_nvme_ctrlr_get_pci_device(ctrlr);
		if (!pci_dev) {
			return;
		}

		pci_id = spdk_pci_device_get_id(pci_dev);

		printf("[SPDK] Attached to NVMe Controller at %s [%04x:%04x]\n",
		       trid->traddr,
		       pci_id.vendor_id, pci_id.device_id);
	}

	register_ctrlr(ctrlr, trid_entry);
}

static int
register_controllers(void)
{
	struct trid_entry *trid_entry;

	//printf("Initializing NVMe Controllers\n");

	TAILQ_FOREACH(trid_entry, &g_trid_list, tailq) {
		if (spdk_nvme_probe(&trid_entry->trid, trid_entry, probe_cb, attach_cb, NULL) != 0) {
			fprintf(stderr, "spdk_nvme_probe() failed for transport address '%s'\n",
				trid_entry->trid.traddr);
			return -1;
		}
	}

	return 0;
}

static void
unregister_controllers(void)
{
	struct ctrlr_entry *entry, *tmp;
	struct spdk_nvme_detach_ctx *detach_ctx = NULL;

	TAILQ_FOREACH_SAFE(entry, &g_controllers, link, tmp) {
		TAILQ_REMOVE(&g_controllers, entry, link);

		if (g_nr_unused_io_queues) {
			int i;

			for (i = 0; i < g_nr_unused_io_queues; i++) {
				spdk_nvme_ctrlr_free_io_qpair(entry->unused_qpairs[i]);
			}

			free(entry->unused_qpairs);
		}

		spdk_nvme_detach_async(entry->ctrlr, &detach_ctx);
		free(entry);
	}

	if (detach_ctx) {
		spdk_nvme_detach_poll(detach_ctx);
	}
}

static int
associate_workers_with_ns(void)
{
	struct ns_entry		*entry = TAILQ_FIRST(&g_namespaces);
	//struct worker_thread	*worker = TAILQ_FIRST(&g_workers);
	struct worker_thread	*worker = NULL;
	struct ns_worker_ctx	*ns_ctx;
	int			i, count;

	count = g_num_namespaces > g_num_workers ? g_num_namespaces : g_num_workers;

	for (i = 0; i < count; i++) {
		if (entry == NULL) {
			break;
		}
		
		/* locate cpu assignment from nvme device file array */
		char *dev_fn = entry->dev_fn;
		for (int j=0; j<MAX_JOB_CNT; j++)
		{
			struct nvme_dummy_dev_t *dev = &nvme_devs[j];
			if (strcmp(dev->file_name, dev_fn) != 0)
				continue;
			assert(dev->cpu >= 0 && dev->cpu < MAX_CPU_CNT);
			worker = workers_by_cpu[dev->cpu];
			break;
		}

		assert(worker != NULL);

		ns_ctx = calloc(1, sizeof(struct ns_worker_ctx));
		if (!ns_ctx) {
			return -1;
		}

		//printf("Associating %s with lcore %d\n", entry->name, worker->lcore);
		ns_ctx->entry = entry;

		ns_ctx->rings = calloc(1, sizeof(struct sci_ns_rings));
		if (!ns_ctx->rings)
			return -1;
		ns_ctx->rings->req_q_in = 0;
		ns_ctx->rings->req_q_next_fill = 0;
		pthread_mutex_init(&ns_ctx->rings->req_q_lock, NULL);
		pthread_mutex_init(&ns_ctx->rings->rsp_q_lock, NULL);
		ns_ctx->cpu = worker->lcore;
		ns_ctx->pool_idx = i;
		//printf("pool_idx(%d) assigned to ns_ctx(%p)\n", ns_ctx->pool_idx, ns_ctx);

		TAILQ_INSERT_TAIL(&worker->ns_ctx, ns_ctx, link);

		entry = TAILQ_NEXT(entry, link);
		if (entry == NULL) {
			entry = TAILQ_FIRST(&g_namespaces);
		}
	}

	return 0;
}

static void *
nvme_poll_ctrlrs(void *arg)
{
	struct ctrlr_entry *entry;
	int oldstate;
	int rc;

	spdk_unaffinitize_thread();

	while (true) {
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);

		TAILQ_FOREACH(entry, &g_controllers, link) {
			if (entry->trtype != SPDK_NVME_TRANSPORT_PCIE) {
				rc = spdk_nvme_ctrlr_process_admin_completions(entry->ctrlr);
				if (spdk_unlikely(rc < 0 && !g_exit)) {
					g_exit = true;
				}
			}
		}

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);

		/* This is a pthread cancellation point and cannot be removed. */
		sleep(1);
	}

	return NULL;
}

static void
sig_handler(int signo)
{
	printf("[SPDK] WARN: (%d) received\n", signo);
	g_exit = true;
}

static int
setup_sig_handlers(void)
{
	struct sigaction sigact = {};
	int rc;

	sigemptyset(&sigact.sa_mask);
	sigact.sa_handler = sig_handler;
	rc = sigaction(SIGINT, &sigact, NULL);
	if (rc < 0) {
		fprintf(stderr, "sigaction(SIGINT) failed, errno %d (%s)\n", errno, strerror(errno));
		return -1;
	}

	rc = sigaction(SIGTERM, &sigact, NULL);
	if (rc < 0) {
		fprintf(stderr, "sigaction(SIGTERM) failed, errno %d (%s)\n", errno, strerror(errno));
		return -1;
	}

	return 0;
}

void *spdk_get_buf_pool(int job_idx)
{
	return sci_buf_pool_4_fio[job_idx];
}

int spdk_init(void)
{
	int rc;
	struct worker_thread *worker, *main_worker;
	struct spdk_env_opts opts;
	pthread_t thread_id = 0;
	
	rc = spdk_get_fio_config();
	if (rc != 0)
		return -1;
	
	spdk_env_opts_init(&opts);
	opts.name = "perf";
	opts.pci_allowed = g_allowed_pci_addr;
	rc = parse_args(&opts);
	if (rc != 0) {
		return rc;
	}
	/* Transport statistics are printed from each thread.
	 * To avoid mess in terminal, init and use mutex */
	rc = pthread_mutex_init(&g_stats_mutex, NULL);
	if (rc != 0) {
		fprintf(stderr, "Failed to init mutex\n");
		return -1;
	}
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		unregister_trids();
		pthread_mutex_destroy(&g_stats_mutex);
		return -1;
	}

	rc = setup_sig_handlers();
	if (rc != 0) {
		rc = -1;
		goto cleanup;
	}

	//printf("Initializing SPDK memory pool\n");
	for (int i=0; i<MAX_JOB_CNT; i++)
	{
		struct nvme_dummy_dev_t *dev = &nvme_devs[i];
		if (dev->cpu < 0)
			continue;
		
		void *buf = spdk_dma_zmalloc(spdk_dma_pool_size, fio_io_align, NULL);
		if (buf != NULL)
			sci_buf_pool_4_fio[i] = buf;	
		else 
		{
			printf("failed allocating %lu fio_buffer %d, buf=%p\n", spdk_dma_pool_size, i, buf);
			goto cleanup;
		}
		//printf("testing allocating %lu buffer %d, buf=%p\n", spdk_dma_pool_size, i, buf);
	}

	g_tsc_rate = spdk_get_ticks_hz();

	if (register_workers() != 0) {
		rc = -1;
		goto cleanup;
	}

	if (register_controllers() != 0) {
		rc = -1;
		goto cleanup;
	}

	if (g_warn) {
		printf("WARNING: Some requested NVMe devices were skipped\n");
	}

	if (g_num_namespaces == 0) {
		fprintf(stderr, "No valid NVMe controllers or AIO or URING devices found\n");
		goto cleanup;
	}

	if (g_num_workers > 1 && g_quiet_count > 1) {
		fprintf(stderr, "Error message rate-limiting enabled across multiple threads.\n");
		fprintf(stderr, "Error suppression count may not be exact.\n");
	}

	rc = pthread_create(&thread_id, NULL, &nvme_poll_ctrlrs, NULL);
	if (rc != 0) {
		fprintf(stderr, "Unable to spawn a thread to poll admin queues.\n");
		goto cleanup;
	}

	if (associate_workers_with_ns() != 0) {
		rc = -1;
		goto cleanup;
	}

	rc = pthread_barrier_init(&g_worker_sync_barrier, NULL, g_num_workers);
	if (rc != 0) {
		fprintf(stderr, "Unable to initialize thread sync barrier\n");
		goto cleanup;
	}

	//printf("Initialization complete. Launching workers.\n");

	/* Launch all of the secondary workers */
	g_main_core = spdk_env_get_current_core();
	main_worker = NULL;
	TAILQ_FOREACH(worker, &g_workers, link) {
		if (worker->lcore != g_main_core) {
			//printf("   >>> pin worker(%p) with lcore(%d) done\n", worker, worker->lcore);
			spdk_env_thread_launch_pinned(worker->lcore, work_fn, worker);
		} else {
			assert(main_worker == NULL);
			main_worker = worker;
			//printf("   >>> main_worker(%p) (%d) assigned done\n", main_worker, main_worker->lcore);
		}
	}
	
	//printf("   >>> issue work_fn for main_worker\n");
	assert(main_worker != NULL);
	rc = work_fn(main_worker);
	
	spdk_init_flag = 1;
	
	while(1)
	{
		if (g_exit)
			break;
		sleep(2);
	}
	
	TAILQ_FOREACH(worker, &g_workers, link)
	{
		struct ns_worker_ctx	*ns_ctx;
		TAILQ_FOREACH(ns_ctx, &worker->ns_ctx, link)
		{
			cleanup_ns_worker_ctx(ns_ctx);
		}
	}

	//for (int i=0; i<MAX_CPU_CNT; i++)
	//{
	//	if (workers_by_cpu[i] != NULL)
	//		printf("workers_by_cpu[%d]: %p\n", i, workers_by_cpu[i]);
	//}

	spdk_env_thread_wait_all();
	pthread_barrier_destroy(&g_worker_sync_barrier);

cleanup:
	if (thread_id && pthread_cancel(thread_id) == 0) {
		pthread_join(thread_id, NULL);
	}
	unregister_trids();
	unregister_namespaces();
	unregister_controllers();
	unregister_workers();
	for (int i=0; i<MAX_JOB_CNT; i++)
	{
		if (sci_buf_pool_4_fio[i] != NULL)
		{
			spdk_dma_free(sci_buf_pool_4_fio[i]);
			sci_buf_pool_4_fio[i] = NULL;
		}
	}

	spdk_env_fini();

	pthread_mutex_destroy(&g_stats_mutex);

	if (rc != 0) {
		fprintf(stderr, "errors occurred\n");
	}

	return rc;
}
void spdk_exit(void)
{
	g_exit = true;
}

static void *spdk_init_thread(void *arg)
{
	int oldstate;
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);
	spdk_init();
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &oldstate);
	return NULL;
}

static pthread_t spdk_init_thread_id = 0;

static __attribute__((constructor)) void
init(void)
{
	/*
	   ATTN: spdk-nvme.so loaded first so all resources exported to sci-spdk.c
	   must setup here
	*/
	int rval;
	
	/* initialize dummy nvme devices */
	memset(nvme_devs, 0, sizeof(nvme_devs));
	for (int i=0; i<MAX_JOB_CNT; i++)
	{
		nvme_devs[i].fd = -1;
		nvme_devs[i].cpu = -1;
	}
	//printf("nvme_devs=%p\n", nvme_devs);
	
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	rval = pthread_create(&spdk_init_thread_id, &attr, &spdk_init_thread, NULL);
	if (rval != 0)
		fprintf(stderr, "[SPDK] ERROR: failed to launch spdk_init_thread\n");
	pthread_attr_destroy(&attr);

	//printf("[SPDK] constructor exited\n");
}

static __attribute__((destructor)) void
deinit(void)
{
	g_exit = true;
		
	if (spdk_init_thread_id && pthread_cancel(spdk_init_thread_id) == 0)
		pthread_join(spdk_init_thread_id, NULL);
	
	int cnt = 3;
	while (cnt--)
	{
		printf("[SPDK] waiting to exit ...\n");
		sleep(1);
	}

	for (int i=0; i<MAX_JOB_CNT; i++)
	{
		if (sci_buf_pool_4_fio[i] != NULL)
		{
			spdk_dma_free(sci_buf_pool_4_fio[i]);
			sci_buf_pool_4_fio[i] = NULL;
		}
	}
}
