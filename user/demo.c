/* Walkthrough of the ringdev API: map the buffer, queue four jobs that depend
 * on each other, wait with poll(), check the answers. */
#include <stdio.h>
#include <stdlib.h>

#include "libringdev.h"

#define N 1024		/* u32 elements per vector */

int main(void)
{
	struct rd_dev d;
	struct ringdev_job jobs[4];
	struct ringdev_cqe c;
	uint32_t *a, *b, *sum, *copy;
	uint64_t want = 0;
	static const char *names[] = { "", "FILL", "COPY", "VADD_U32", "CHECKSUM" };
	int i, done = 0, bad = 0;

	if (rd_open(&d, 0) || rd_map(&d)) {
		perror("ringdev (is the module loaded?)");
		return 1;
	}
	printf("device: %u ring entries, %llu byte buffer, %u us per job\n",
	       d.info.ring_entries, (unsigned long long)d.info.data_bytes, d.info.tick_us);

	/* Layout of the shared buffer: a | b | sum | copy, N u32 each. */
	a = (uint32_t *)d.buf;
	b = a + N;
	sum = b + N;
	copy = sum + N;
	for (i = 0; i < N; i++)
		a[i] = i;

	/* The engine runs a context's jobs in submit order, so later jobs may
	 * use the results of earlier ones. */
	jobs[0] = rd_fill(1, N * 4, N * 4, 1000);			/* b[] = 1000 */
	jobs[1] = rd_vadd(2, 0, N * 4, 2 * N * 4, N * 4);		/* sum = a + b */
	jobs[2] = rd_copy(3, 2 * N * 4, 3 * N * 4, N * 4);		/* copy = sum */
	jobs[3] = rd_checksum(4, 3 * N * 4, N * 4);			/* byte sum of copy */

	if (rd_submit(&d, jobs, 4) != 4) {
		perror("submit");
		return 1;
	}

	while (done < 4) {
		struct pollfd p = { .fd = d.fd, .events = POLLIN };

		if (poll(&p, 1, 2000) != 1) {
			fprintf(stderr, "timed out waiting for completions\n");
			return 1;
		}
		while (done < 4 && rd_reap(&d, &c, 1) == 1) {
			printf("  job %u %-9s status=%d latency=%.1f us", c.id,
			       names[jobs[c.id - 1].op], c.status, c.latency_ns / 1e3);
			if (jobs[c.id - 1].op == RINGDEV_OP_CHECKSUM)
				printf(" result=%llu", (unsigned long long)c.result);
			putchar('\n');
			done++;
		}
	}

	for (i = 0; i < N; i++) {
		bad += sum[i] != (uint32_t)i + 1000 || copy[i] != sum[i];
		want += (uint8_t)(copy[i]) + (uint8_t)(copy[i] >> 8) +
			(uint8_t)(copy[i] >> 16) + (uint8_t)(copy[i] >> 24);
	}
	printf("%s: %d bad elements, checksum expected %llu\n",
	       bad ? "MISMATCH" : "OK", bad, (unsigned long long)want);

	rd_close(&d);
	return bad != 0;
}
