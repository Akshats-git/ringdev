/*
 * M4: N processes x M threads.
 *
 * Phase 1: every thread owns a context and keeps a CPU shadow of its buffer.
 * One engine runs a context's jobs in submit order, so the shadow predicts
 * every status, every CHECKSUM result and the final buffer contents.
 * Phase 2: all threads of a process share ONE fd (one context), each working
 * in its own region; completions are consumed by whichever thread reads them.
 *
 * usage: test_stress [-p procs] [-t threads] [-n jobs-per-thread]
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

#include "check.h"
#include "../user/libringdev.h"

#define AREA	(64 * 1024)	/* part of the buffer the random jobs use */
#define MAXLEN	4096
#define P2_REGION 8192

static int nprocs = 4, nthreads = 4, njobs = 1000, p2_iters;

struct worker {
	int idx;
	unsigned seed;
	int errors;
};

struct expect {
	int status;
	uint64_t result;
};

static uint32_t rnd(unsigned *s)
{
	*s = *s * 1664525u + 1013904223u;
	return *s >> 8;
}

#define WFAIL(w, ...) do { fprintf(stderr, "[w%d] ", (w)->idx); \
	fprintf(stderr, __VA_ARGS__); (w)->errors++; } while (0)

static struct ringdev_job gen_job(struct worker *w, uint32_t id, uint64_t data_bytes)
{
	uint32_t len = (rnd(&w->seed) % (MAXLEN / 4 + 1)) * 4;
	uint64_t a = (rnd(&w->seed) % ((AREA - MAXLEN) / 4)) * 4;
	uint64_t b = (rnd(&w->seed) % ((AREA - MAXLEN) / 4)) * 4;
	uint64_t c = (rnd(&w->seed) % ((AREA - MAXLEN) / 4)) * 4;
	uint32_t pat = rnd(&w->seed) * 2654435761u;

	switch (rnd(&w->seed) % 9) {
	case 0: case 1:
		return rd_fill(id, a, len, pat);
	case 2: case 3:
		return rd_copy(id, a, b, len);
	case 4: case 5:
		return rd_vadd(id, a, b, c, len);
	case 6: case 7:
		return rd_checksum(id, a, len);
	default:	/* deliberately runs past the end of the buffer */
		return rd_checksum(id, data_bytes - (rnd(&w->seed) % 64), 64 + len);
	}
}

/* The model: what the device must do to the buffer and report. */
static struct expect apply(const struct ringdev_job *j, uint8_t *shadow,
			   uint64_t data_bytes)
{
	uint32_t *sw = (uint32_t *)shadow;
	struct expect e = { 0, 0 };
	uint32_t i;

	switch (j->op) {
	case RINGDEV_OP_FILL:
		for (i = 0; i < j->len / 4; i++)
			sw[j->dst / 4 + i] = (uint32_t)j->arg;
		break;
	case RINGDEV_OP_COPY:
		if (j->len && j->src0 < j->dst + j->len && j->dst < j->src0 + j->len)
			e.status = -EINVAL;
		else
			memcpy(shadow + j->dst, shadow + j->src0, j->len);
		break;
	case RINGDEV_OP_VADD_U32:
		for (i = 0; i < j->len / 4; i++)
			sw[j->dst / 4 + i] = sw[j->src0 / 4 + i] + sw[j->src1 / 4 + i];
		break;
	case RINGDEV_OP_CHECKSUM:
		if (j->src0 + j->len > data_bytes) {
			e.status = -ERANGE;
			break;
		}
		for (i = 0; i < j->len; i++)
			e.result += shadow[j->src0 + i];
		break;
	}
	return e;
}

static void *phase1(void *arg)
{
	struct worker *w = arg;
	struct rd_dev d;
	struct ringdev_job *jobs = malloc(sizeof(*jobs) * njobs);
	struct expect *exp = calloc(njobs, sizeof(*exp));
	uint8_t *seen = calloc(njobs, 1);
	uint8_t *shadow = calloc(1, AREA);
	struct ringdev_cqe cqes[16];
	int sub = 0, got = 0, next_id = 0;

	if (rd_open(&d, 0) || rd_map(&d)) {
		WFAIL(w, "open/map: %m\n");
		return NULL;
	}

	while (got < njobs) {
		int want = 1 + rnd(&w->seed) % 16, k, acc;
		ssize_t n;

		if (want > njobs - sub)
			want = njobs - sub;
		for (k = 0; k < want; k++)
			jobs[sub + k] = gen_job(w, sub + k, d.info.data_bytes);
		acc = want ? rd_submit(&d, jobs + sub, want) : 0;
		if (acc < 0 && errno != EAGAIN) {
			WFAIL(w, "submit: %m\n");
			break;
		}
		/* Only the accepted prefix exists as far as the model goes. */
		for (k = 0; k < acc; k++)
			exp[sub + k] = apply(&jobs[sub + k], shadow, d.info.data_bytes);
		if (acc > 0)
			sub += acc;

		n = rd_reap(&d, cqes, 16);
		if (n < 0) {
			WFAIL(w, "read: %m\n");
			break;
		}
		for (k = 0; k < n; k++) {
			uint32_t id = cqes[k].id;

			if ((int)id >= sub || seen[id]++) {
				WFAIL(w, "bad/duplicate completion id %u\n", id);
				continue;
			}
			if ((int)id != next_id++)
				WFAIL(w, "out of order completion %u\n", id);
			if (cqes[k].status != exp[id].status)
				WFAIL(w, "id %u op %u status %d want %d\n", id, jobs[id].op,
				      cqes[k].status, exp[id].status);
			if (!cqes[k].status && jobs[id].op == RINGDEV_OP_CHECKSUM &&
			    cqes[k].result != exp[id].result)
				WFAIL(w, "id %u checksum %llu want %llu\n", id,
				      (unsigned long long)cqes[k].result,
				      (unsigned long long)exp[id].result);
		}
		got += n;
	}

	if (got == njobs && memcmp(d.buf, shadow, AREA))
		WFAIL(w, "final buffer differs from shadow\n");
	rd_close(&d);
	free(jobs); free(exp); free(seen); free(shadow);
	return NULL;
}

/* ---- phase 2: one shared, non-blocking fd ---- */

static struct rd_dev shared;
static atomic_int p2_done;
static atomic_uint p2_status_bad, p2_sum_bad;
static _Atomic uint64_t p2_expect[64][256];	/* checksum per (thread, iteration) */

static int p2_consume(void)
{
	struct ringdev_cqe c[8];
	ssize_t n = read(shared.fd, c, sizeof(c));
	int k;

	if (n <= 0)
		return 0;
	for (k = 0; k < n / (ssize_t)sizeof(c[0]); k++) {
		unsigned t = c[k].id >> 16, s = (c[k].id & 0xffff) >> 1;

		if (c[k].status)
			p2_status_bad++;
		else if ((c[k].id & 1) && c[k].result != p2_expect[t][s])
			p2_sum_bad++;
		p2_done++;
	}
	return k;
}

static void *phase2(void *arg)
{
	struct worker *w = arg;
	uint64_t region = (uint64_t)w->idx * P2_REGION;
	int total = nthreads * p2_iters * 2, i;

	for (i = 0; i < p2_iters; i++) {
		uint32_t pat = rnd(&w->seed);
		struct ringdev_job pair[2] = {
			rd_fill((w->idx << 16) | (i << 1), region, 4096, pat),
			rd_checksum((w->idx << 16) | (i << 1) | 1, region, 4096),
		};
		uint64_t sum = 0;
		int k, sent = 0;

		for (k = 0; k < 4; k++)
			sum += (pat >> (8 * k)) & 0xff;
		p2_expect[w->idx][i] = sum * 1024;

		while (sent < 2) {
			int acc = rd_submit(&shared, pair + sent, 2 - sent);
			int got = p2_consume();	/* reap for everyone, frees ring space */

			if (acc > 0)
				sent += acc;
			else if (!got)
				usleep(50);
		}
	}

	while (p2_done < total) {
		struct pollfd p = { .fd = shared.fd, .events = POLLIN };

		if (poll(&p, 1, 50) > 0)
			p2_consume();
	}
	return NULL;
}

static int child(int proc)
{
	pthread_t th[64];
	struct worker w[64];
	int i, errors = 0;

	for (i = 0; i < nthreads; i++) {
		w[i] = (struct worker){ .idx = i, .seed = proc * 1000 + i + 1 };
		pthread_create(&th[i], NULL, phase1, &w[i]);
	}
	for (i = 0; i < nthreads; i++) {
		pthread_join(th[i], NULL);
		errors += w[i].errors;
	}

	if (rd_open(&shared, O_NONBLOCK) || rd_map(&shared)) {
		perror("phase2 open");
		return 1;
	}
	for (i = 0; i < nthreads; i++) {
		w[i].seed = proc * 77 + i + 5;
		pthread_create(&th[i], NULL, phase2, &w[i]);
	}
	for (i = 0; i < nthreads; i++)
		pthread_join(th[i], NULL);
	if (p2_status_bad || p2_sum_bad) {
		fprintf(stderr, "[p%d] phase2: %u bad status, %u bad sums\n", proc,
			p2_status_bad, p2_sum_bad);
		errors++;
	}
	rd_close(&shared);
	return errors != 0;
}

int main(int argc, char **argv)
{
	struct ringdev_stats before, after;
	unsigned long long expected;
	struct rd_dev obs;
	int opt, i, status;
	pid_t pids[64];

	while ((opt = getopt(argc, argv, "p:t:n:")) != -1) {
		if (opt == 'p') nprocs = atoi(optarg);
		else if (opt == 't') nthreads = atoi(optarg);
		else if (opt == 'n') njobs = atoi(optarg);
	}
	if (nprocs < 1 || nprocs > 64 || nthreads < 1 || nthreads > 60 || njobs < 2) {
		fprintf(stderr, "bad arguments\n");
		return 2;
	}
	p2_iters = njobs / 2 < 200 ? njobs / 2 : 200;
	expected = (unsigned long long)nprocs * nthreads * (njobs + 2 * p2_iters);

	CHECK(rd_open(&obs, 0) == 0);
	CHECK(ioctl(obs.fd, RINGDEV_IOC_GET_STATS, &before) == 0);

	for (i = 0; i < nprocs; i++) {
		pids[i] = fork();
		if (!pids[i])
			_exit(child(i));
	}
	for (i = 0; i < nprocs; i++) {
		CHECK(waitpid(pids[i], &status, 0) == pids[i]);
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}

	CHECK(ioctl(obs.fd, RINGDEV_IOC_GET_STATS, &after) == 0);
	CHECK(after.submitted - before.submitted == expected);
	CHECK(after.completed - before.completed == expected);	/* none lost */
	CHECK(after.active_ctx == 1);				/* only the observer */
	printf("%llu jobs submitted, %llu completed, %llu completed with error (expected)\n",
	       (unsigned long long)(after.submitted - before.submitted),
	       (unsigned long long)(after.completed - before.completed),
	       (unsigned long long)(after.failed - before.failed));
	rd_close(&obs);

	TEST_DONE("test_stress");
}
