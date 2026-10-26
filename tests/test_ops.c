/* M3: every op, checked against a CPU computation. */
#include <stdio.h>
#include <stdlib.h>

#include "check.h"
#include "../user/libringdev.h"

#define N 4096		/* bytes per region */

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
	rng = rng * 1664525u + 1013904223u;
	return rng >> 8;
}

/* Submit one job, wait for its completion. */
static struct ringdev_cqe run(struct rd_dev *d, struct ringdev_job j)
{
	struct ringdev_cqe c = { .status = 12345 };

	CHECK(rd_submit(d, &j, 1) == 1);
	CHECK(rd_reap(d, &c, 1) == 1);
	CHECK(c.id == j.id);
	return c;
}

int main(void)
{
	struct rd_dev d;
	struct ringdev_cqe c;
	uint32_t *w, i;
	uint64_t sum;

	CHECK(rd_open(&d, 0) == 0 && rd_map(&d) == 0);
	w = (uint32_t *)d.buf;

	/* FILL */
	c = run(&d, rd_fill(1, 0, N, 0xdeadbeef));
	CHECK(c.status == 0);
	for (i = 0; i < N / 4; i++)
		CHECK(w[i] == 0xdeadbeef);
	CHECK(w[N / 4] == 0);			/* did not run past len */

	/* COPY */
	for (i = 0; i < N; i++)
		d.buf[2 * N + i] = rnd();
	c = run(&d, rd_copy(2, 2 * N, 3 * N, N));
	CHECK(c.status == 0 && memcmp(d.buf + 2 * N, d.buf + 3 * N, N) == 0);

	/* VADD_U32, including wraparound */
	for (i = 0; i < N / 4; i++) {
		w[4 * N / 4 + i] = rnd() * 0x10001u;
		w[5 * N / 4 + i] = 0xffffffffu - (rnd() & 0xff);
	}
	c = run(&d, rd_vadd(3, 4 * N, 5 * N, 6 * N, N));
	CHECK(c.status == 0);
	for (i = 0; i < N / 4; i++)
		CHECK(w[6 * N / 4 + i] == (uint32_t)(w[4 * N / 4 + i] + w[5 * N / 4 + i]));

	/* CHECKSUM */
	for (i = 0, sum = 0; i < N; i++)
		sum += d.buf[2 * N + i];
	c = run(&d, rd_checksum(4, 2 * N, N));
	CHECK(c.status == 0 && c.result == sum);
	c = run(&d, rd_checksum(5, 0, 0));
	CHECK(c.status == 0 && c.result == 0);

	/* Last bytes of the buffer are reachable. */
	c = run(&d, rd_fill(6, d.info.data_bytes - 8, 8, 0x01010101));
	CHECK(c.status == 0 && d.buf[d.info.data_bytes - 1] == 1);
	c = run(&d, rd_checksum(7, d.info.data_bytes - 8, 8));
	CHECK(c.status == 0 && c.result == 8);

	/* A batch of mixed jobs: all ids come back once, latency is plausible. */
	{
		struct ringdev_job jobs[32];
		struct ringdev_cqe cqes[32];
		int seen[32] = { 0 }, got = 0;

		for (i = 0; i < 32; i++)
			jobs[i] = rd_checksum(100 + i, 0, 64 * (i + 1));
		CHECK(rd_submit(&d, jobs, 32) == 32);
		while (got < 32) {
			ssize_t n = rd_reap(&d, cqes + got, 32 - got);

			CHECK(n > 0);
			if (n <= 0)
				break;
			got += n;
		}
		for (i = 0; i < (uint32_t)got; i++) {
			CHECK(cqes[i].id >= 100 && cqes[i].id < 132 && !seen[cqes[i].id - 100]++);
			CHECK(cqes[i].status == 0);
			CHECK(cqes[i].latency_ns >= (uint64_t)d.info.tick_us * 1000);
		}
		/* one engine thread, one ctx: completions are in submit order */
		for (i = 0; i < (uint32_t)got; i++)
			CHECK(cqes[i].id == 100 + i);
	}

	rd_close(&d);
	TEST_DONE("test_ops");
}
