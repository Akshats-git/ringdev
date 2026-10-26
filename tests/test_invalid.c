/* M3: malformed calls fail the ioctl; bad jobs fail in the CQE; flow control. */
#include <stdio.h>
#include <stdlib.h>

#include "check.h"
#include "../user/libringdev.h"

static struct rd_dev d;

/* Keeps the compiler from proving the pointer bad at build time. */
static __attribute__((noinline)) void *bad_ptr(void)
{
	return (void *)0x10;
}

/* Run one job that must fail with `want`, and leave the buffer untouched. */
static void expect_status(struct ringdev_job j, int want, int line)
{
	struct ringdev_cqe c = { 0 };

	if (rd_submit(&d, &j, 1) != 1 || rd_reap(&d, &c, 1) != 1) {
		fprintf(stderr, "FAIL line %d: submit/reap\n", line);
		failures++;
		return;
	}
	if (c.status != want || c.id != j.id) {
		fprintf(stderr, "FAIL line %d: status %d want %d\n", line, c.status, want);
		failures++;
	}
}
#define EXPECT(j, want) expect_status(j, want, __LINE__)

int main(void)
{
	struct ringdev_job j, many[256];
	struct ringdev_submit s;
	struct ringdev_cqe c, cqes[256];
	uint64_t db, huge = ~0ull - 3;
	uint32_t ring, i;
	int fd2;

	CHECK(rd_open(&d, 0) == 0 && rd_map(&d) == 0);
	db = d.info.data_bytes;
	ring = d.info.ring_entries;
	memset(d.buf, 0x5a, db);

	/* --- the ioctl itself fails --- */
	j = rd_checksum(1, 0, 8);
	s = (struct ringdev_submit){ .jobs_ptr = (uintptr_t)&j, .count = 0 };
	CHECK(ioctl(d.fd, RINGDEV_IOC_SUBMIT, &s) == -1 && errno == EINVAL);
	s.count = ring + 1;
	CHECK(ioctl(d.fd, RINGDEV_IOC_SUBMIT, &s) == -1 && errno == EINVAL);
	s.count = 1; s.reserved = 1;
	CHECK(ioctl(d.fd, RINGDEV_IOC_SUBMIT, &s) == -1 && errno == EINVAL);
	s.reserved = 0; s.jobs_ptr = 0;
	CHECK(ioctl(d.fd, RINGDEV_IOC_SUBMIT, &s) == -1 && errno == EFAULT);
	s.jobs_ptr = 0x1000;
	CHECK(ioctl(d.fd, RINGDEV_IOC_SUBMIT, &s) == -1 && errno == EFAULT);
	CHECK(ioctl(d.fd, RINGDEV_IOC_SUBMIT, NULL) == -1 && errno == EFAULT);
	CHECK(ioctl(d.fd, RINGDEV_IOC_GET_INFO, NULL) == -1 && errno == EFAULT);
	CHECK(ioctl(d.fd, RINGDEV_IOC_GET_STATS, NULL) == -1 && errno == EFAULT);
	CHECK(ioctl(d.fd, _IO('R', 77)) == -1 && errno == ENOTTY);
	CHECK(ioctl(d.fd, _IO('X', 1)) == -1 && errno == ENOTTY);

	/* --- short read buffer --- */
	j = rd_checksum(2, 0, 8);
	CHECK(rd_submit(&d, &j, 1) == 1);
	CHECK(read(d.fd, &c, sizeof(c) - 1) == -1 && errno == EINVAL);
	CHECK(read(d.fd, &c, 0) == -1 && errno == EINVAL);
	CHECK(read(d.fd, bad_ptr(), sizeof(c)) == -1 && errno == EFAULT);
	CHECK(rd_reap(&d, &c, 1) == 1 && c.id == 2);	/* EFAULT lost nothing */

	/* --- bad jobs complete with an error status --- */
	EXPECT(rd_checksum(10, db, 1), -ERANGE);		/* one past the end */
	EXPECT(rd_checksum(11, db - 4, 8), -ERANGE);		/* straddles the end */
	EXPECT(rd_checksum(12, huge, 16), -ERANGE);		/* offset+len overflows u64 */
	EXPECT(rd_checksum(13, 0, 0xffffffffu), -ERANGE);	/* huge len */
	EXPECT(rd_fill(14, huge, 4, 1), -ERANGE);		/* aligned, but past the end */
	EXPECT(rd_fill(31, huge + 1, 4, 1), -EINVAL);		/* unaligned */
	EXPECT(rd_fill(15, 0, 0xfffffffcu, 1), -ERANGE);
	EXPECT(rd_copy(16, huge, 0, 16), -ERANGE);
	EXPECT(rd_copy(17, 0, huge, 16), -ERANGE);
	EXPECT(rd_vadd(18, 0, 4096, huge - 3, 8), -EINVAL);	/* misaligned */
	EXPECT(rd_vadd(19, 0, 4096, db - 4, 8), -ERANGE);
	EXPECT(rd_vadd(20, 2, 4096, 8192, 8), -EINVAL);
	EXPECT(rd_fill(21, 0, 7, 1), -EINVAL);			/* len % 4 */
	EXPECT(rd_fill(22, 2, 8, 1), -EINVAL);			/* dst % 4 */
	EXPECT(rd_copy(23, 0, 8, 16), -EINVAL);			/* overlap */
	EXPECT(rd_copy(24, 8, 0, 16), -EINVAL);
	j = rd_checksum(25, 0, 8); j.op = 0;
	EXPECT(j, -EINVAL);
	j.op = 99; j.id = 26;
	EXPECT(j, -EINVAL);
	j.op = 0xffffffffu; j.id = 27;
	EXPECT(j, -EINVAL);
	j = rd_checksum(28, 0, 8); j.flags = 1;
	EXPECT(j, -EINVAL);

	/* none of the failed jobs touched the buffer */
	for (i = 0; i < db; i += 4096)
		CHECK(d.buf[i] == 0x5a);
	CHECK(d.buf[db - 1] == 0x5a && d.buf[db - 4] == 0x5a);

	/* a valid job right after errors still works */
	EXPECT(rd_checksum(30, 0, 8), 0);

	/* --- mmap validation on a second fd --- */
	fd2 = open("/dev/ringdev", O_RDWR);
	CHECK(fd2 >= 0);
	CHECK(mmap(NULL, db / 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0) == MAP_FAILED && errno == EINVAL);
	CHECK(mmap(NULL, db, PROT_READ, MAP_PRIVATE, fd2, 0) == MAP_FAILED && errno == EINVAL);
	CHECK(mmap(NULL, db, PROT_READ, MAP_SHARED, fd2, 4096) == MAP_FAILED && errno == EINVAL);
	close(fd2);

	/* --- flow control: outstanding never exceeds ring_entries --- */
	for (i = 0; i < 256; i++)
		many[i] = rd_checksum(1000 + i, 0, 8);
	CHECK(rd_submit(&d, many, ring - 24) == (int)(ring - 24));
	CHECK(rd_submit(&d, many, 64) == 24);			/* partial acceptance */
	CHECK(rd_submit(&d, many, 1) == -1 && errno == EAGAIN);	/* full */
	usleep(ring * (d.info.tick_us + 100) + 100000);		/* all completed, none read */
	CHECK(rd_submit(&d, many, 1) == -1 && errno == EAGAIN);	/* unread CQEs still count */
	{
		struct ringdev_stats st;

		CHECK(ioctl(d.fd, RINGDEV_IOC_GET_STATS, &st) == 0);
		CHECK(st.ctx_outstanding == ring);
	}
	CHECK(rd_reap(&d, cqes, 1) == 1);
	CHECK(rd_submit(&d, many, 4) == 1);			/* exactly one slot freed */

	/* RESET discards queued jobs and unread completions */
	usleep(100000);
	CHECK(ioctl(d.fd, RINGDEV_IOC_RESET) == 0);
	{
		struct ringdev_stats st;

		CHECK(ioctl(d.fd, RINGDEV_IOC_GET_STATS, &st) == 0);
		CHECK(st.ctx_outstanding == 0);
	}
	d.buf[0] = 0;
	{
		int fl = fcntl(d.fd, F_GETFL);

		fcntl(d.fd, F_SETFL, fl | O_NONBLOCK);
		CHECK(read(d.fd, &c, sizeof(c)) == -1 && errno == EAGAIN);
	}
	CHECK(rd_submit(&d, many, ring) == (int)ring);		/* full capacity again */

	rd_close(&d);
	TEST_DONE("test_invalid");
}
