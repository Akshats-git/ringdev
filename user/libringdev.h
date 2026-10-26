/* SPDX-License-Identifier: GPL-2.0 */
/* Thin header-only helpers around /dev/ringdev. All functions keep the
 * ioctl/read convention: -1 with errno set on failure. */
#ifndef LIBRINGDEV_H
#define LIBRINGDEV_H

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "../ringdev_uapi.h"

struct rd_dev {
	int fd;
	struct ringdev_info info;
	uint8_t *buf;		/* mmap'd data buffer, NULL until rd_map() */
};

static inline uint64_t rd_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static inline int rd_open(struct rd_dev *d, int flags)
{
	d->buf = NULL;
	d->fd = open("/dev/ringdev", O_RDWR | flags);
	if (d->fd < 0)
		return -1;
	if (ioctl(d->fd, RINGDEV_IOC_GET_INFO, &d->info) < 0) {
		close(d->fd);
		return -1;
	}
	return 0;
}

static inline int rd_map(struct rd_dev *d)
{
	void *p = mmap(NULL, d->info.data_bytes, PROT_READ | PROT_WRITE,
		       MAP_SHARED, d->fd, 0);

	if (p == MAP_FAILED)
		return -1;
	d->buf = p;
	return 0;
}

static inline void rd_close(struct rd_dev *d)
{
	if (d->buf)
		munmap(d->buf, d->info.data_bytes);
	close(d->fd);
}

/* Returns the number of jobs accepted (may be < n), or -1/EAGAIN if full. */
static inline int rd_submit(struct rd_dev *d, const struct ringdev_job *jobs,
			    uint32_t n)
{
	struct ringdev_submit s = { .jobs_ptr = (uintptr_t)jobs, .count = n };

	if (ioctl(d->fd, RINGDEV_IOC_SUBMIT, &s) < 0)
		return -1;
	return s.count;
}

/* Blocking read of up to max completions; returns how many, or -1. */
static inline ssize_t rd_reap(struct rd_dev *d, struct ringdev_cqe *cqes,
			      size_t max)
{
	ssize_t r = read(d->fd, cqes, max * sizeof(*cqes));

	return r < 0 ? r : r / (ssize_t)sizeof(*cqes);
}

static inline struct ringdev_job rd_fill(uint32_t id, uint64_t dst, uint32_t len,
					 uint32_t pattern)
{
	return (struct ringdev_job){ .id = id, .op = RINGDEV_OP_FILL,
				     .dst = dst, .len = len, .arg = pattern };
}

static inline struct ringdev_job rd_copy(uint32_t id, uint64_t src, uint64_t dst,
					 uint32_t len)
{
	return (struct ringdev_job){ .id = id, .op = RINGDEV_OP_COPY,
				     .src0 = src, .dst = dst, .len = len };
}

static inline struct ringdev_job rd_vadd(uint32_t id, uint64_t a, uint64_t b,
					 uint64_t dst, uint32_t len)
{
	return (struct ringdev_job){ .id = id, .op = RINGDEV_OP_VADD_U32,
				     .src0 = a, .src1 = b, .dst = dst, .len = len };
}

static inline struct ringdev_job rd_checksum(uint32_t id, uint64_t src,
					     uint32_t len)
{
	return (struct ringdev_job){ .id = id, .op = RINGDEV_OP_CHECKSUM,
				     .src0 = src, .len = len };
}

#endif /* LIBRINGDEV_H */
