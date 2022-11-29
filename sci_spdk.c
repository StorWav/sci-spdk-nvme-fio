/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2022 Colin Zhu. All rights reserved.
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
 *   sci_spdk.c
 * 
 *   Frontend interceptor of system calls and memory allocation calls. The
 *   interception mechanism, which takes use of syscall_intercept libraries,
 *   is to redirect those calls to the backend high performance SPDK/NVMe
 *   IO interfaces, foster significant IO performance boost without any
 *   modification to existing applications.
 *
 *   For details about syscall_intercept please visit
 *   https://github.com/pmem/syscall_intercept
 *
 *   Author: Colin Zhu <czhu@nexnt.com>
 */

#include "libsyscall_intercept_hook_point.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>

#include <linux/aio_abi.h>

#include "sci_spdk_nvme.h"

#define likely(x)		__builtin_expect(!!(x), 1)
#define unlikely(x)		__builtin_expect(!!(x), 0)

static int log_fd = -1;
static int hook_counter = 0;
static long flags = -1;
static long my_pid = -1;

struct io_entry_t {
	bool is_allocated;
	struct iocb *cb_obj; /* saved from io_submit(), copied back to io_getevents() */
	int len;
};
struct ctx_entry_t {
	aio_context_t ctx_id;
	int rsp_q_in;
	int rsp_q_out;
	int cpu;
	int spdk_dev_idx;

	struct io_entry_t ios[MAX_Q_DEPTH];	
	struct io_event evs[MAX_RING_CNT];

	bool is_allocated;
	int q_depth;
	pthread_mutex_t io_lock;
	pthread_mutex_t rsp_lock;
};
static struct ctx_entry_t ctxs[MAX_JOB_CNT];
static pthread_mutex_t ctx_entry_lock;
static char ctx_id_holders[MAX_JOB_CNT];
static pthread_mutex_t mem_alloc_lock;;
static uint64_t fio_pool_size = 0;

static uint32_t sci_debug_level = DBG_ERR|DBG_INFO;

static inline int
sci_mask_match(uint32_t level)
{
	return (level > 0) && (level & sci_debug_level) == level;
}
void SCI_SPDK_LOG(uint32_t level, int fd, const char *fmt, ...)
{
	int len;
	va_list ap;

	if (!sci_mask_match(level))
		return;

	va_start(ap, fmt);
	len = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);

	if (len <= 0)
		return;

	char buf[len + 1];

	va_start(ap, fmt);
	len = vsprintf(buf, fmt, ap);
	va_end(ap);
	
	struct timeval tv;
	struct tm newtime;
	syscall_no_intercept(SYS_gettimeofday, &tv, NULL);
	time_t ltime = tv.tv_sec;
	localtime_r(&ltime, &newtime);
    
	char log_buf[len + 64];
	sprintf(log_buf, "%04d.%02d.%02d_%02d:%02d:%02d_%06luus ",
		newtime.tm_year+1900, newtime.tm_mon, newtime.tm_mday,
		newtime.tm_hour, newtime.tm_min, newtime.tm_sec, tv.tv_usec);
	strcat(log_buf, buf);

	if (fd < 0 && log_fd >= 0)
		syscall_no_intercept(SYS_write, log_fd, log_buf, strlen(log_buf));
	else 
		syscall_no_intercept(SYS_write, fd, log_buf, strlen(log_buf));
}
static inline struct ctx_entry_t *find_ctx_entry(aio_context_t ctx)
{
	aio_context_t ctx0 = (aio_context_t)&ctx_id_holders[0];
	int idx = (int)(ctx-ctx0);
	if (idx < 0 || idx >= MAX_JOB_CNT)
		return NULL;
	else 
		return &ctxs[idx];
}
static inline int alloc_ctx_entry(int nr, aio_context_t *ctxp)
{
	/* ATTN: shall use lock? */
	int rval = -1;
	
	if (nr > MAX_Q_DEPTH)
		return -1;
	
	pthread_mutex_lock(&ctx_entry_lock);
	for (int i=0; i<MAX_JOB_CNT; i++)
	{
		if (ctxs[i].is_allocated)
			continue;
		
		ctxs[i].is_allocated = true;
		ctxs[i].q_depth = nr;
		*ctxp = ctxs[i].ctx_id;
		rval = 0;
		break;
	}
	pthread_mutex_unlock(&ctx_entry_lock);
	
	return rval;
}
static inline int get_ctx(aio_context_t ctx_id)
{
	aio_context_t ctx0 = (aio_context_t)&ctx_id_holders[0];
	int idx = (int)(ctx_id-ctx0);
	return idx;
}

#ifdef _VERIFY_WRITE
static void sci_dump_buffer_ascii(uint8_t * b, uint32_t size)
{
	uint32_t cnt;
	uint8_t c;
	char ascii_str[17];

	for (cnt = 0; cnt < size;) {
		c = *b++;
		//SCI_SPDK_LOG(DBG_INFO, log_fd,"%02x",(uint32_t) c);
		
		if (c >= 32 && c <= 126)
			ascii_str[cnt%16] = c;
		else
			ascii_str[cnt%16] = '.';
		
		cnt++;
		if (!(cnt % 16))
		{
			ascii_str[16] = '\0';
			SCI_SPDK_LOG(DBG_INFO, log_fd,"%*c%s", 4, ' ', ascii_str);
			ascii_str[0] = '\0';
			SCI_SPDK_LOG(DBG_INFO, log_fd,"\n");
		}
		else
			SCI_SPDK_LOG(DBG_INFO, log_fd,"  ");
	}
	if (cnt % 16)
	{
		ascii_str[cnt%16] = '\0';
		SCI_SPDK_LOG(DBG_INFO, log_fd,"%*c%s", (16-cnt%16)*4+2, ' ', ascii_str);
		SCI_SPDK_LOG(DBG_INFO, log_fd,"\n");
	}
}
#endif

static int get_events(aio_context_t ctx_id, int nr, struct io_event *events)
{
	int ctx_idx = get_ctx(ctx_id);
	struct ctx_entry_t *ctx = &ctxs[ctx_idx];
	int rval = 0;

	//pthread_mutex_lock(&ctx->rsp_lock);
	while (ctx->rsp_q_in != ctx->rsp_q_out)
	{
		if (rval == nr)
			break;
		
		struct io_event *next_ev = &ctx->evs[ctx->rsp_q_out];
		struct io_event *to_copy_ev = &events[rval];
		to_copy_ev->res = next_ev->res;
		to_copy_ev->res2 = next_ev->res2;
		to_copy_ev->obj = next_ev->obj;

#ifdef _VERIFY_WRITE		
		struct iocb *cb = (struct iocb *)to_copy_ev->obj;
		if (cb->aio_lio_opcode == IOCB_CMD_PREAD)
		{
			void *buf = (void *)cb->aio_buf;
			sci_dump_buffer_ascii(buf, 72);
		}
#endif
		
		rval++;
		ctx->rsp_q_out++;
		if (ctx->rsp_q_out >= MAX_RING_CNT)
			ctx->rsp_q_out = 0;
	}
	//pthread_mutex_unlock(&ctx->rsp_lock);
	
	return rval;	
}

void sci_callback(void *iocb, u_int64_t reserved_1, u_int64_t reserved_2)
{
	aio_context_t ctx_id = (aio_context_t)reserved_1;
	int ctx_idx = get_ctx(ctx_id);
	struct ctx_entry_t *ctx = &ctxs[ctx_idx];

	//pthread_mutex_lock(&ctx->rsp_lock);
	struct io_event *next_ev = &ctx->evs[ctx->rsp_q_in];
	next_ev->res = ((struct iocb *)iocb)->aio_nbytes;
	next_ev->res2 = 0;
	next_ev->obj = (__u64)iocb;
	ctx->rsp_q_in++;
	if (ctx->rsp_q_in >= MAX_RING_CNT)
		ctx->rsp_q_in = 0;
	//pthread_mutex_unlock(&ctx->rsp_lock);
}

static inline void sci_open_nvme(const char *file_name, int fd, const char *syscall_str)
{
	struct nvme_dummy_dev_t *nvme_devs = spdk_get_nvme_devices();
	int cpu = -1;
	syscall_no_intercept(SYS_getcpu, &cpu, NULL, NULL);

	for (int i=0; i<MAX_JOB_CNT; i++)
	{
		if (strcmp(file_name, nvme_devs[i].file_name) != 0)
			continue;

		if (fd >= 0)
			nvme_devs[i].fd = fd;

		SCI_SPDK_LOG(DBG_INFO, log_fd, ">>> %s: filename(%s), fd=%d, cpu(%d)\n",
			syscall_str, file_name, fd, cpu);
		
		break;
	}
}

static inline struct nvme_dummy_dev_t *sci_find_nvme(const char *file_name)
{
	if (strlen(file_name) == 0)
		return NULL;
	
	struct nvme_dummy_dev_t *nvme_devs = spdk_get_nvme_devices();
	
	for (int i=0; i<MAX_JOB_CNT; i++)
	{
		if (strcmp(file_name, nvme_devs[i].file_name) != 0)
			continue;
		
		return &nvme_devs[i];
	}
	
	return NULL;
}

static inline void sci_associate_ctx_with_spdk_dev(uint64_t ctx_id, int fd, struct ctx_entry_t *ctx_entry)
{
	if (ctx_entry->spdk_dev_idx != -1)
		return;
	
	struct nvme_dummy_dev_t *nvme_devs = spdk_get_nvme_devices();
	
	for (int i=0; i<MAX_JOB_CNT; i++)
	{
		if (fd != nvme_devs[i].fd)
			continue;
		
		ctx_entry->spdk_dev_idx = i;
		nvme_devs[i].ctx_id = ctx_id;
		break;
	}	
}

static inline void sci_set_dev_stat(struct nvme_dummy_dev_t *dev, struct stat *stat)
{
	stat->st_size = dev->ns_size;
	stat->st_blksize = dev->sector_size;
	stat->st_blocks = dev->ns_size/512;
}

static int collect_fio_config_done = 0;
static int
hook(long syscall_number,
	long arg0, long arg1,
	long arg2, long arg3,
	long arg4, long arg5,
	long *result)
{
	(void) arg0;
	(void) arg2;
	(void) arg3;
	(void) arg4;
	(void) arg5;
	(void) result;

	/* Use whatever first syscall to collect mem pool size to redirect malloc()
	   to spdk_nvme_zalloc(). This was done in init() constructor and it seems
	   work fine with Ubuntu 20.04, but then in Ubuntu 22.04 can't use sleep()
	   int constructor any more, so had to move those logics into syscall */
	if (unlikely(!collect_fio_config_done))
	{
		int io_size = 0;
		int q_depth = 0;
		int io_align = 0;
	
		spdk_collect_fio_config(&io_size, &q_depth, &io_align, &fio_pool_size);
		if (fio_pool_size == 0)
		{
			SCI_SPDK_LOG(DBG_INFO, 1, "[SCI] ERROR: failed to get fio_pool_size, io_size(%d), q_depth(%d), io_align(%d)\n",
				io_size, q_depth, io_align);
			syscall_no_intercept(SYS_exit_group, 4);
		}
		else 
		{
			SCI_SPDK_LOG(DBG_INFO, log_fd, "[SCI] io_size(%d), q_depth(%d), io_align(%d), fio_pool_size=%lu\n",
				io_size, q_depth, io_align, fio_pool_size);
		}
		collect_fio_config_done = 1;
	}

	if (syscall_number == SYS_io_submit)
	{
		struct ctx_entry_t *ctx_entry = find_ctx_entry(arg0);
		if (ctx_entry == NULL)
		{
			SCI_SPDK_LOG(DBG_IO, log_fd, "io_submit: Unable to find ctx(%016lx)\n", arg0);
			*result = syscall_no_intercept(SYS_io_submit, arg0, arg1, arg2);
			return 0;				
		}
			
		int io_submitted = 0;
		for (int i=0; i<arg1; i++)
		{
			struct iocb **cbs = (struct iocb **)arg2;
			struct iocb *cb = cbs[i];

#ifdef _VERIFY_WRITE
			cb->aio_offset=536870900ULL*512;
			if (cb->aio_lio_opcode == IOCB_CMD_PWRITE)
			{
				char quote[] = "You pray for rain, you gotta deal with the mud too. That's a part of it!";
				memcpy((void *)cb->aio_buf, (void *)quote, strlen(quote));
			}
#endif

			//SCI_SPDK_LOG(DBG_IO, log_fd, ">>> SYS_io_submit: fd(%d) ctx(%016llx) spdk_idx(%d)\n",
			//	cb->aio_fildes, arg0, ctx_entry->spdk_dev_idx);
			
			sci_associate_ctx_with_spdk_dev(arg0, cb->aio_fildes, ctx_entry);
						
			int rval = sci_io_submit(/*cpu,*/ctx_entry->spdk_dev_idx, cb, sci_callback, arg0);
			if (rval != -1)
				io_submitted++;
		}
		*result = io_submitted;
		return 0;
	}
	else if (syscall_number == SYS_io_getevents)
	{
		struct io_event *events = (struct io_event *)arg3;
		struct ctx_entry_t *ctx_entry = find_ctx_entry(arg0);
		if (ctx_entry == NULL)
		{
			SCI_SPDK_LOG(DBG_IO, log_fd, "io_getevents: Unable to find ctx(%016lx)\n", arg0);
			*result = syscall_no_intercept(SYS_io_submit, arg0, arg1, arg2);
			return 0;				
		}

		sci_io_getevents(ctx_entry->spdk_dev_idx, arg0);
		*result = get_events(arg0, arg2, events);
		return 0;
	}
	else if (syscall_number == SYS_io_setup ||
			 syscall_number == SYS_io_destroy ||
			 syscall_number == SYS_io_cancel)
	{
		SCI_SPDK_LOG(DBG_INFO, log_fd, ">>> syscall(%ld), arg0(0x%lx)\n", syscall_number, arg0);

		if (syscall_number == SYS_io_setup)
		{
			int cpu = -1;
			int status;
			status = syscall_no_intercept(SYS_getcpu, &cpu, NULL, NULL);
			
			aio_context_t *ctxp = (aio_context_t *)arg1;
			*result = alloc_ctx_entry((int)arg0, ctxp);
			
			if (*result >= 0)
			{
				aio_context_t *ctxp1 = (aio_context_t *)arg1;
				SCI_SPDK_LOG(DBG_INFO, log_fd, "[SYS_io_setup] aio_context_t=%016llx, cpu(%d)\n", (uint64_t)*ctxp1, cpu);
			}
			else 
			{
				SCI_SPDK_LOG(DBG_ERR, log_fd, "[SYS_io_setup] ERROR alloc ctx\n");
			}
			
			return 0;
		}
		else if (syscall_number == SYS_io_destroy)
		{
			struct ctx_entry_t *ctx_entry = find_ctx_entry(arg0);
			if (ctx_entry == NULL)
			{
				SCI_SPDK_LOG(DBG_ERR, log_fd, "io_destroy: Unable to find ctx(%016lx)\n", arg0);
				*result = syscall_no_intercept(SYS_io_destroy, arg0);
				return 0;				
			}
			
			*result = 0;
			return 0;
		}
	}
	else if (syscall_number == SYS_open)
	{
		*result = syscall_no_intercept(arg0, arg1, arg2);
		sci_open_nvme((char *)arg0, *result, "SYS_open");
		return 0;
	}
	else if (syscall_number == SYS_openat)
	{
		*result = syscall_no_intercept(SYS_openat, arg0, arg1, arg2, arg3);
		sci_open_nvme((char *)arg1, *result, "SYS_openat");
		return 0;
	}
	else if (syscall_number == SYS_stat || syscall_number == SYS_lstat)
	{
		struct nvme_dummy_dev_t *dev = sci_find_nvme((char *)arg0);
		if (dev != NULL)
		{
			int cpu = -1;
			struct stat *stat;
			syscall_no_intercept(SYS_getcpu, &cpu, NULL, NULL);
			*result = syscall_no_intercept(syscall_number, arg0, arg1);
			if (*result == 0)
			{
				stat = (struct stat *)arg1;
				sci_set_dev_stat(dev, stat);
			}
			SCI_SPDK_LOG(DBG_INFO, log_fd, ">>> %s: filename(%s), cpu(%d), stats(%lu,%lu,%lu)\n", 
				syscall_number == SYS_stat ? "SYS_stat" : "SYS_lstat",
				(char *)arg0, cpu, stat->st_size, stat->st_blksize, stat->st_blocks);
			return 0;
		}
	}
	else if (syscall_number == SYS_fstat)
	{
		int cpu = -1;
		syscall_no_intercept(SYS_getcpu, &cpu, NULL, NULL);
		//SCI_SPDK_LOG(DBG_INFO, log_fd, ">>> SYS_fstat: fd=%ld, cpu(%d)\n", arg0, cpu);
	}
	else if (syscall_number == SYS_newfstatat)
	{
		struct nvme_dummy_dev_t *dev = sci_find_nvme((char *)arg1);
		if (dev != NULL)
		{
			int cpu = -1;
			struct stat *stat;
			syscall_no_intercept(SYS_getcpu, &cpu, NULL, NULL);
			*result = syscall_no_intercept(syscall_number, arg0, arg1, arg2, arg3);
			if (*result == 0)
			{
				stat = (struct stat *)arg2;
				sci_set_dev_stat(dev, stat);
			}
			SCI_SPDK_LOG(DBG_INFO, log_fd, ">>> SYS_newfstatat: filename(%s), cpu(%d), stats(%lu,%lu,%lu)\n", 
				(char *)arg1, cpu, stat->st_size, stat->st_blksize, stat->st_blocks);
			return 0;
		}
	}
	else if (syscall_number == SYS_clone || syscall_number == SYS_clone3)
	{
		int cpu = -1;
		int status = syscall_no_intercept(SYS_getcpu, &cpu, NULL, NULL);
		
		while (!spdk_check_init_status())
		{
			SCI_SPDK_LOG(DBG_INFO, log_fd, ">>> %s: waiting...\n", syscall_number == SYS_clone ? "SYS_clone" : "SYS_clone3");
			struct timespec t;
			t.tv_sec = 2;
			t.tv_nsec = 0;		
			syscall_no_intercept(SYS_nanosleep, &t, NULL);
		}

		if (arg1 != 0)
			flags = arg0;
		
		SCI_SPDK_LOG(DBG_INFO, log_fd, ">>> SYS_clone: pid(%p) child(%p), cpu(%d)\n", (void *)arg2, (void *)arg3, cpu);
	}

	return 1;
}

static void
hook_child(void)
{
	assert(flags != -1);
}

static __attribute__((constructor)) void
init(void)
{
	char path[256];
	int i;
	int cpu = -1;

	syscall_no_intercept(SYS_getcpu, &cpu, NULL, NULL);
	
	pthread_mutex_init(&mem_alloc_lock, NULL);
        
	char *debug_str = getenv(SCI_SPDK_DBG_ENV);
	if (debug_str != NULL)
		sci_debug_level = atoi(debug_str);

	my_pid = syscall_no_intercept(SYS_getpid);
	
	sprintf(path, "sci-spdk-nvme-%ld.log", my_pid);

	log_fd = (int)syscall_no_intercept(SYS_open,
			path, O_CREAT | O_RDWR | /*O_APPEND*/O_TRUNC, (mode_t)0744);

	if (log_fd < 0)
	{
		fprintf(stderr, "[SCI] ERROR: failed to open log file(%s)\n", path);
		syscall_no_intercept(SYS_exit_group, 4);
	}

	printf("[SCI] entered constructor with cpu(%d), log_fd(%u), hook_cnt(%d)\n", 
		cpu, (unsigned int)log_fd, hook_counter++);
	
	/* initialize global context stuff */
	memset((void *)ctxs, 0, sizeof(ctxs));
	for (int i=0; i<MAX_JOB_CNT; i++)
	{
		ctxs[i].ctx_id = (aio_context_t)&ctx_id_holders[i];
		pthread_mutex_init(&ctxs[i].io_lock, NULL);
		pthread_mutex_init(&ctxs[i].rsp_lock, NULL);
		ctxs[i].rsp_q_in = 0;
		ctxs[i].rsp_q_out = 0;
		ctxs[i].cpu = -1;
		ctxs[i].spdk_dev_idx = -1;
	}

	pthread_mutex_init(&ctx_entry_lock, NULL);

	intercept_hook_point = hook;
	intercept_hook_point_clone_child = hook_child;
	
	printf("[SCI] exited constructor\n");
}

static __attribute__((destructor)) void
deinit(void)
{
	printf("[SCI] entered destructor\n");
	
	pthread_mutex_destroy(&ctx_entry_lock);	
	pthread_mutex_destroy(&mem_alloc_lock);	
	
	printf("[SCI] closing log file fd(%d)\n", log_fd);
	
	if (log_fd != -1)
	{
		syscall_no_intercept(SYS_close, log_fd);
		log_fd = -1;
	}
	
	printf("[SCI] exited destructor\n");
}

/*-
 * Intercepting malloc to redirect to spdk_dma_zalloc()
 */
#include <sys/types.h>
#include <dlfcn.h>

static int job_cnt = 0;

void * malloc(size_t size)
{	
	static void *(*fptr)(size_t) = 0;
 
	if (fptr == 0) {
		fptr = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc");
		if (fptr == NULL) {
			(void) printf("dlopen: %s\n", dlerror());
			return (NULL);
		}
	}
	
	int cpu = -1;
	syscall_no_intercept(SYS_getcpu, &cpu, NULL, NULL);
 	
 	/* only intercepting fio pool buffer malloc */
 	if (size == fio_pool_size)
 	{
 		void *buf = NULL;
 		pthread_mutex_lock(&mem_alloc_lock);
 		buf = spdk_get_buf_pool(job_cnt);
 		job_cnt++;
 		pthread_mutex_unlock(&mem_alloc_lock);
 			
 		if (buf != NULL)
 			return buf;
	}
	return ((*fptr)(size));
}
void free(void *ptr)
{
	static void (*fptr)(void*) = 0;
 
	if (fptr == 0) {
		fptr = (void (*)(void *))dlsym(RTLD_NEXT, "free");
		if (fptr == NULL) {
			(void) printf("dlopen: %s\n", dlerror());
			return;
		}
	}

	int cpu = -1;
	syscall_no_intercept(SYS_getcpu, &cpu, NULL, NULL);
			
	int find = 0;
	for (int i=0; i<MAX_JOB_CNT; i++)
	{
		pthread_mutex_lock(&mem_alloc_lock);
		void *buf = spdk_get_buf_pool(i);
		if (buf == ptr)
				find = 1;
		pthread_mutex_unlock(&mem_alloc_lock);
 				
		if (find)
			break;
	}
			
	if (find)
		return;
	else
		return ((*fptr)(ptr));
}
