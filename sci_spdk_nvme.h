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
 *   sci_spdk_nvme.h
 *
 *   Author: Colin Zhu <czhu@nexnt.com>
 */

#ifndef _SCI_SPDK_NVME_H
#define _SCI_SPDK_NVME_H

const char *FIO_CONF_FILE = "fiocfg";
const char *SCI_SPDK_DBG_ENV = "SCI_SPDK_DEBUG_LEVEL";

#define MAX_JOB_CNT			64		/* one fio job for each nvme, max 64 nvme devices */
#define MAX_NVME_NAME_LEN	31		/* max dummy nvme file name length */
#define MAX_CPU_CNT			64		/* max number of cores, increase the value if needed */

#define MAX_Q_DEPTH			512
#define MAX_RING_CNT		MAX_Q_DEPTH
#define DEFAULT_IO_ALIGN	0x200	/* 512 */
#define DEFAULT_IO_SIZE		0x1000	/* 4K */

extern void spdk_collect_fio_config(int *,			/* io_size */
									int *,			/* q_depth */
									int *,			/* io_align */
									uint64_t *);	/* pool_size */
extern int spdk_check_init_status(void);
extern struct nvme_dummy_dev_t *spdk_get_nvme_devices(void);
extern void *spdk_get_buf_pool(int job_idx);
extern int sci_io_submit(int, struct iocb*, void*, aio_context_t);
extern void sci_io_getevents(int, aio_context_t);

struct nvme_dummy_dev_t {
	char file_name[MAX_NVME_NAME_LEN+1];
	char pci_name[MAX_NVME_NAME_LEN+1];
	int fd;
	int cpu;
	uint64_t ctx_id;	/* assigned from sci front-end */
	uint64_t ns_size;
	uint32_t sector_size;
};

#define DBG_ERR		0x00000001	/* BIT_0 */
#define DBG_INFO	0x00000002	/* BIT_1 */
#define DBG_IO		0x00000004	/* BIG_2 */
extern void SCI_SPDK_LOG(uint32_t level, int fd, const char *fmt, ...);

#endif	/* _SCI_SPDK_NVME_H */
