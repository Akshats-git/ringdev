/*
 * M4: processes die at awkward moments (SIGKILL while jobs are queued or in
 * flight, while blocked in read() or poll(), or exiting with work pending).
 * A survivor context must keep getting correct, timely service, and once the
 * dust settles only the survivor's own context may be left.
 *
 * usage: test_kill [-r rounds] [-k kids-per-round]
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

#include "check.h"
#include "../user/libringdev.h"

#define DEADLINE_MS 2000

static int active_ctx(int fd)
{
	struct ringdev_stats st;

	if (ioctl(fd, RINGDEV_IOC_GET_STATS, &st) < 0)
		return -1;
	return st.active_ctx;
}

/* One survivor job with a known answer, must finish within the deadline. */
static int survivor_job(struct rd_dev *s, uint32_t id)
{
	struct ringdev_job j = rd_checksum(id, 0, 4096);
	struct ringdev_cqe c;
	struct pollfd p = { .fd = s->fd, .events = POLLIN };

	if (rd_submit(s, &j, 1) != 1)
		return -1;
	if (poll(&p, 1, DEADLINE_MS) != 1)
		return -2;			/* engine stalled */
	if (rd_reap(s, &c, 1) != 1 || c.id != id || c.status)
		return -3;
	return c.result == 4096ull * 0x11 ? 0 : -4;
}

static void kid(int mode, int ready_fd, int inherited_fd)
{
	struct rd_dev d;
	struct ringdev_job *jobs;
	struct ringdev_cqe c;
	uint32_t i, n;
	char b = 'r';

	close(inherited_fd);
	if (rd_open(&d, 0) || rd_map(&d))
		_exit(90);
	n = d.info.ring_entries;
	jobs = malloc(n * sizeof(*jobs));
	for (i = 0; i < n; i++)
		jobs[i] = rd_checksum(i, 0, d.info.data_bytes);	/* ~1 MB each: long jobs */

	switch (mode) {
	case 0:				/* flood forever; never read */
		if (write(ready_fd, &b, 1) != 1)
			_exit(91);
		for (;;)
			if (rd_submit(&d, jobs, n) < 0)
				usleep(20);
	case 1:				/* blocked in read() with nothing outstanding */
		if (write(ready_fd, &b, 1) != 1)
			_exit(91);
		(void)!read(d.fd, &c, sizeof(c));
		_exit(92);
	case 2: {			/* blocked in poll() */
		struct pollfd p = { .fd = d.fd, .events = POLLIN };

		if (write(ready_fd, &b, 1) != 1)
			_exit(91);
		poll(&p, 1, -1);
		_exit(92);
	}
	default:			/* exit normally with work still queued */
		if (rd_submit(&d, jobs, n) < 0)
			_exit(93);
		if (write(ready_fd, &b, 1) != 1)
			_exit(91);
		_exit(0);
	}
}

int main(int argc, char **argv)
{
	int rounds = 10, kids = 6, opt, r, i;

	while ((opt = getopt(argc, argv, "r:k:")) != -1) {
		if (opt == 'r') rounds = atoi(optarg);
		else if (opt == 'k') kids = atoi(optarg);
	}
	if (rounds < 1 || kids < 1 || kids > 64)
		return 2;

	struct rd_dev o0;
	struct ringdev_stats st0, st1;

	CHECK(rd_open(&o0, 0) == 0 && ioctl(o0.fd, RINGDEV_IOC_GET_STATS, &st0) == 0);
	rd_close(&o0);		/* must not stay open: it would count as an active context */

	for (r = 0; r < rounds; r++) {
		int pipefd[2], status, left;
		pid_t pids[64];
		struct rd_dev s;
		char b;

		CHECK(pipe(pipefd) == 0);
		/* Open before forking so every kid has to close it again. */
		CHECK(rd_open(&s, 0) == 0 && rd_map(&s) == 0);
		memset(s.buf, 0x11, 4096);

		for (i = 0; i < kids; i++) {
			pids[i] = fork();
			if (!pids[i]) {
				close(pipefd[0]);
				kid(i % 4, pipefd[1], s.fd);
			}
		}
		close(pipefd[1]);
		for (i = 0; i < kids; i++) {
			struct pollfd p = { .fd = pipefd[0], .events = POLLIN };

			CHECK(poll(&p, 1, 5000) == 1 && read(pipefd[0], &b, 1) == 1);
		}
		close(pipefd[0]);

		CHECK(survivor_job(&s, 1) == 0);
		/* Kill in a rotating order, with survivor traffic in between. */
		for (left = kids, i = r % kids; left; left--, i = (i + 1) % kids) {
			usleep((rand() % 4) * 1000);
			kill(pids[i], SIGKILL);	/* mode 3 may already be gone: fine */
			CHECK(survivor_job(&s, 100 + left) == 0);
		}
		for (i = 0; i < kids; i++) {
			CHECK(waitpid(pids[i], &status, 0) == pids[i]);
			if (i % 4 == 3)		/* exited by itself unless we got there first */
				CHECK(WIFEXITED(status) ? WEXITSTATUS(status) == 0
						        : WTERMSIG(status) == SIGKILL);
			else
				CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
		}

		/* every killed context must be reaped; only the survivor is left */
		for (i = 0; i < 200 && active_ctx(s.fd) != 1; i++)
			usleep(10000);
		CHECK(active_ctx(s.fd) == 1);
		CHECK(survivor_job(&s, 999) == 0);
		rd_close(&s);
	}

	/* nobody left: a fresh observer sees exactly itself */
	{
		struct rd_dev o;

		CHECK(rd_open(&o, 0) == 0);
		CHECK(active_ctx(o.fd) == 1);
		CHECK(ioctl(o.fd, RINGDEV_IOC_GET_STATS, &st1) == 0);
		printf("%llu jobs submitted, %llu completed, %llu dropped with their dying context\n",
		       (unsigned long long)(st1.submitted - st0.submitted),
		       (unsigned long long)(st1.completed - st0.completed),
		       (unsigned long long)((st1.submitted - st0.submitted) -
					    (st1.completed - st0.completed)));
		CHECK(st1.submitted - st0.submitted > st1.completed - st0.completed);
		printf("active contexts after %d rounds of kills: %d (observer excluded)\n",
		       rounds, active_ctx(o.fd) - 1);
		rd_close(&o);
	}
	TEST_DONE("test_kill");
}
