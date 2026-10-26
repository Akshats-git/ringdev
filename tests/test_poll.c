/* M3: poll()/POLLIN semantics of the completion queue. */
#include <pthread.h>
#include <stdio.h>

#include "check.h"
#include "../user/libringdev.h"

static struct rd_dev d;

static void *late_submit(void *arg)
{
	struct ringdev_job j = rd_checksum(9, 0, 8);

	usleep(100 * 1000);
	rd_submit(&d, &j, 1);
	return arg;
}

int main(void)
{
	struct pollfd p;
	struct ringdev_cqe c;
	struct ringdev_job j = rd_checksum(1, 0, 8);
	pthread_t th;
	uint64_t t0;

	CHECK(rd_open(&d, 0) == 0);
	p.fd = d.fd;
	p.events = POLLIN;

	CHECK(poll(&p, 1, 0) == 0);			/* empty */

	CHECK(rd_submit(&d, &j, 1) == 1);
	CHECK(poll(&p, 1, 2000) == 1 && (p.revents & POLLIN));
	CHECK(poll(&p, 1, 0) == 1);			/* level-triggered */
	CHECK(rd_reap(&d, &c, 1) == 1);
	CHECK(poll(&p, 1, 0) == 0);			/* drained */

	/* poll blocks until a completion arrives from another thread */
	CHECK(pthread_create(&th, NULL, late_submit, NULL) == 0);
	t0 = rd_now_ns();
	CHECK(poll(&p, 1, 3000) == 1);
	CHECK(rd_now_ns() - t0 >= 90 * 1000 * 1000ull);
	CHECK(rd_now_ns() - t0 < 1000 * 1000 * 1000ull);
	pthread_join(th, NULL);
	CHECK(rd_reap(&d, &c, 1) == 1 && c.id == 9);

	rd_close(&d);
	TEST_DONE("test_poll");
}
