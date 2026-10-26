/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* Shared by the kernel module and user space. No implicit padding. */
#ifndef RINGDEV_UAPI_H
#define RINGDEV_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define RINGDEV_MAGIC		'R'
#define RINGDEV_VERSION		1
#define RINGDEV_MAX_RING	4096	/* upper bound for the ring_entries module parameter */

enum ringdev_op {
	RINGDEV_OP_FILL = 1,
	RINGDEV_OP_COPY,
	RINGDEV_OP_VADD_U32,
	RINGDEV_OP_CHECKSUM,
};

/* src/dst are byte offsets into the mmap'd data buffer. */
struct ringdev_job {
	__u32 id;
	__u32 op;
	__u64 src0;
	__u64 src1;
	__u64 dst;
	__u32 len;
	__u32 flags;		/* must be 0 */
	__u64 arg;		/* FILL pattern (low 32 bits) */
};

/* Job-level errors come back as -errno in status. */
struct ringdev_cqe {
	__u32 id;
	__s32 status;
	__u64 result;		/* CHECKSUM sum */
	__u64 latency_ns;	/* submit -> completion */
};

struct ringdev_info {
	__u32 version;
	__u32 ring_entries;
	__u64 data_bytes;
	__u32 tick_us;
	__u32 reserved;
};

struct ringdev_submit {
	__u64 jobs_ptr;
	__u32 count;		/* in: requested, out: accepted */
	__u32 reserved;
};

struct ringdev_stats {
	/* this context */
	__u64 ctx_submitted;
	__u64 ctx_completed;
	__u64 ctx_failed;
	__u64 ctx_outstanding;	/* queued + in flight + unread */
	/* whole device */
	__u64 submitted;
	__u64 completed;
	__u64 failed;
	__u64 busy_ns;		/* engine time spent executing jobs */
	__u32 active_ctx;
	__u32 reserved;
};

#define RINGDEV_IOC_GET_INFO	_IOR(RINGDEV_MAGIC, 1, struct ringdev_info)
#define RINGDEV_IOC_SUBMIT	_IOWR(RINGDEV_MAGIC, 2, struct ringdev_submit)
#define RINGDEV_IOC_RESET	_IO(RINGDEV_MAGIC, 3)
#define RINGDEV_IOC_GET_STATS	_IOR(RINGDEV_MAGIC, 4, struct ringdev_stats)

#endif /* RINGDEV_UAPI_H */
