/* M3: read() blocks, wakes on completion, honours O_NONBLOCK and signals. */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>

#include "check.h"
#include "../user/libringdev.h"

static struct rd_dev d;
static struct ringdev_cqe got;
static ssize_t got_n;
static uint64_t t_return;

static void *reader(void *arg)
{
	got_n = rd_reap(&d, &got, 1);
	t_return = rd_now_ns();
	return arg;
}

static void on_alarm(int sig) { (void)sig; }

int main(void)
{
	struct ringdev_cqe c;
	struct ringdev_job j = rd_checksum(7, 0, 16);
	struct sigaction sa = { .sa_handler = on_alarm };	/* no SA_RESTART */
	struct rd_dev nb;
	pthread_t th;
	uint64_t t_submit;

	CHECK(rd_open(&d, 0) == 0);

	/* O_NONBLOCK on an empty CQ */
	CHECK(rd_open(&nb, O_NONBLOCK) == 0);
	CHECK(read(nb.fd, &c, sizeof(c)) == -1 && errno == EAGAIN);
	rd_close(&nb);

	/* a blocked reader stays blocked until a job completes */
	CHECK(pthread_create(&th, NULL, reader, NULL) == 0);
	usleep(150 * 1000);
	CHECK(got_n == 0);			/* still blocked */
	t_submit = rd_now_ns();
	CHECK(rd_submit(&d, &j, 1) == 1);
	pthread_join(th, NULL);
	CHECK(got_n == 1 && got.id == 7 && got.status == 0);
	CHECK(t_return - t_submit < 50 * 1000 * 1000ull);	/* woke promptly */
	CHECK(got.latency_ns >= (uint64_t)d.info.tick_us * 1000);

	/* a signal interrupts a blocked read */
	sigaction(SIGALRM, &sa, NULL);
	alarm(1);
	CHECK(read(d.fd, &c, sizeof(c)) == -1 && errno == EINTR);

	/* a buffer for several entries yields only what is ready */
	{
		struct ringdev_job two[2] = { rd_checksum(1, 0, 4), rd_checksum(2, 0, 4) };
		struct ringdev_cqe four[4];
		ssize_t n, total = 0;

		CHECK(rd_submit(&d, two, 2) == 2);
		while (total < 2) {
			n = rd_reap(&d, four + total, 4 - total);
			CHECK(n > 0);
			if (n <= 0)
				break;
			total += n;
		}
		CHECK(total == 2 && four[0].id == 1 && four[1].id == 2);
	}

	rd_close(&d);
	TEST_DONE("test_blocking");
}
