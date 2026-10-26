/* M2: strict mmap validation, and the mapping really is the kernel's buffer. */
#include <stdio.h>

#include "check.h"
#include "../user/libringdev.h"

static uint64_t kernel_sum(struct rd_dev *d, uint64_t off, uint32_t len)
{
	struct ringdev_job j = rd_checksum(1, off, len);
	struct ringdev_cqe c = { .status = -1 };

	CHECK(rd_submit(d, &j, 1) == 1 && rd_reap(d, &c, 1) == 1);
	CHECK(c.status == 0);
	return c.result;
}

int main(void)
{
	struct rd_dev a, b;
	size_t len, pg = sysconf(_SC_PAGESIZE);
	uint8_t *q;
	struct ringdev_job j;
	struct ringdev_cqe c;

	CHECK(rd_open(&a, 0) == 0 && rd_open(&b, 0) == 0);
	len = a.info.data_bytes;

	/* validation: size, offset and sharing are all enforced */
	CHECK(mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, a.fd, 0) == MAP_FAILED && errno == EINVAL);
	CHECK(mmap(NULL, len - pg, PROT_READ | PROT_WRITE, MAP_SHARED, a.fd, 0) == MAP_FAILED && errno == EINVAL);
	CHECK(mmap(NULL, len + pg, PROT_READ | PROT_WRITE, MAP_SHARED, a.fd, 0) == MAP_FAILED && errno == EINVAL);
	CHECK(mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, a.fd, pg) == MAP_FAILED && errno == EINVAL);

	CHECK(rd_map(&a) == 0);

	/* a new buffer is zeroed */
	CHECK(kernel_sum(&a, 0, len) == 0);

	/* user writes through the mapping are what the kernel sees... */
	memset(a.buf, 1, len);
	CHECK(kernel_sum(&a, 0, len) == len);
	a.buf[0] = 101;
	a.buf[len - 1] = 100;
	CHECK(kernel_sum(&a, 0, len) == len - 2 + 201);

	/* ...and kernel writes are what the user sees */
	j = rd_fill(2, 4096, 4096, 0xa5a5a5a5);
	CHECK(rd_submit(&a, &j, 1) == 1 && rd_reap(&a, &c, 1) == 1 && c.status == 0);
	CHECK(a.buf[4096] == 0xa5 && a.buf[8191] == 0xa5 && a.buf[8192] == 1);

	/* contexts do not share buffers */
	CHECK(kernel_sum(&b, 0, len) == 0);
	q = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, b.fd, 0);
	CHECK(q != MAP_FAILED && q[0] == 0 && q[4096] == 0);

	/* closing the fd while mapped keeps the mapping valid */
	close(a.fd);
	CHECK(a.buf[0] == 101 && a.buf[len / 2] == 1);
	munmap(a.buf, len);
	if (q != MAP_FAILED)
		munmap(q, len);
	close(b.fd);

	TEST_DONE("test_mmap");
}
