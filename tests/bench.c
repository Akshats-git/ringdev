/*
 * Latency and throughput of the simulated accelerator.
 *
 * Every process loops: submit one batch of 4 KB CHECKSUM jobs with a single
 * ioctl, then reap that batch. Latency is the kernel's submit -> completion
 * time from each CQE, so jobs late in a batch include the time spent queued
 * behind the earlier ones. Throughput is total jobs over the wall time from
 * the first submit to the last completion.
 *
 * "fair" is the slowest process's rate over the fastest one's (1.00 = even).
 *
 * usage: bench [-n jobs-per-process]
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

#include "../user/libringdev.h"

#define JOB_BYTES 4096

static int njobs = 20000;

struct shared {
	uint64_t t_start[8], t_end[8];
};

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

/* One process: returns 0 and fills lat[0..njobs) and t_start/t_end. */
static int run_one(int batch, uint64_t *lat, uint64_t *t0, uint64_t *t1)
{
	struct rd_dev d;
	struct ringdev_job jobs[64];
	struct ringdev_cqe cqes[64];
	int sub = 0, done = 0, i;

	if (rd_open(&d, 0))
		return 1;
	for (i = 0; i < batch; i++)
		jobs[i] = rd_checksum(i, 0, JOB_BYTES);

	*t0 = rd_now_ns();
	while (done < njobs) {
		int want = batch < njobs - sub ? batch : njobs - sub;
		int acc = rd_submit(&d, jobs, want);

		if (acc != want)	/* ring is empty here, so all must fit */
			return 1;
		sub += acc;
		while (done < sub) {
			ssize_t n = rd_reap(&d, cqes, 64);

			if (n < 0)
				return 1;
			for (i = 0; i < n; i++)
				lat[done + i] = cqes[i].latency_ns;
			done += n;
		}
	}
	*t1 = rd_now_ns();
	rd_close(&d);
	return 0;
}

static void measure(int procs, int batch)
{
	size_t total = (size_t)procs * njobs;
	size_t map_len = total * sizeof(uint64_t) + sizeof(struct shared);
	uint8_t *mem = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
			    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	uint64_t *lat = (uint64_t *)(mem + sizeof(struct shared));
	struct shared *sh = (struct shared *)mem;
	uint64_t first = ~0ull, last = 0;
	double rate, rmin = 1e18, rmax = 0;
	int i, status, bad = 0;

	for (i = 0; i < procs; i++) {
		if (!fork())
			_exit(run_one(batch, lat + (size_t)i * njobs, &sh->t_start[i],
				      &sh->t_end[i]));
	}
	for (i = 0; i < procs; i++) {
		wait(&status);
		bad |= !WIFEXITED(status) || WEXITSTATUS(status);
	}
	if (bad) {
		printf("%5d %5d  FAILED\n", procs, batch);
		exit(1);
	}

	for (i = 0; i < procs; i++) {
		if (sh->t_start[i] < first) first = sh->t_start[i];
		if (sh->t_end[i] > last) last = sh->t_end[i];
		rate = njobs / ((sh->t_end[i] - sh->t_start[i]) / 1e9);
		if (rate < rmin) rmin = rate;
		if (rate > rmax) rmax = rate;
	}
	qsort(lat, total, sizeof(*lat), cmp_u64);
	printf("%5d %5d %12.0f %10.1f %10.1f %10.1f %7.2f\n", procs, batch,
	       total / ((last - first) / 1e9), lat[total / 2] / 1e3,
	       lat[total * 99 / 100] / 1e3, lat[total - 1] / 1e3, rmin / rmax);
	fflush(stdout);
	munmap(mem, map_len);
}

int main(int argc, char **argv)
{
	static const int batches[] = { 1, 4, 16, 64 }, procs[] = { 1, 2, 4 };
	struct rd_dev d;
	unsigned i;
	int opt;

	while ((opt = getopt(argc, argv, "n:")) != -1)
		if (opt == 'n')
			njobs = atoi(optarg);

	if (rd_open(&d, 0)) {
		perror("open /dev/ringdev");
		return 1;
	}
	printf("ring_entries=%u tick_us=%u job=CHECKSUM %d B, %d jobs per process\n\n",
	       d.info.ring_entries, d.info.tick_us, JOB_BYTES, njobs);
	rd_close(&d);

	printf("--- throughput and latency vs batch size (1 process)\n");
	printf("%5s %5s %12s %10s %10s %10s %7s\n", "procs", "batch", "jobs/s",
	       "p50 us", "p99 us", "max us", "fair");
	for (i = 0; i < sizeof(batches) / sizeof(*batches); i++)
		measure(1, batches[i]);

	printf("\n--- vs number of processes (batch 16)\n");
	printf("%5s %5s %12s %10s %10s %10s %7s\n", "procs", "batch", "jobs/s",
	       "p50 us", "p99 us", "max us", "fair");
	for (i = 0; i < sizeof(procs) / sizeof(*procs); i++)
		measure(procs[i], 16);
	return 0;
}
