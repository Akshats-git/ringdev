/* M1: open/GET_INFO/independent contexts/bad ioctl. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../ringdev_uapi.h"

static int failures;
#define CHECK(c) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #c, errno); \
	failures++; } } while (0)

int main(void)
{
	struct ringdev_info a, b;
	int fd1 = open("/dev/ringdev", O_RDWR);
	int fd2 = open("/dev/ringdev", O_RDWR);

	CHECK(fd1 >= 0 && fd2 >= 0);
	CHECK(ioctl(fd1, RINGDEV_IOC_GET_INFO, &a) == 0);
	CHECK(ioctl(fd2, RINGDEV_IOC_GET_INFO, &b) == 0);
	CHECK(a.version == RINGDEV_VERSION);
	CHECK(a.ring_entries && !(a.ring_entries & (a.ring_entries - 1)));
	CHECK(a.data_bytes && a.data_bytes % (u_int64_t)sysconf(_SC_PAGESIZE) == 0);
	CHECK(a.tick_us == b.tick_us && a.data_bytes == b.data_bytes);
	printf("info: ring_entries=%u data_bytes=%llu tick_us=%u\n", a.ring_entries,
	       (unsigned long long)a.data_bytes, a.tick_us);

	CHECK(ioctl(fd1, RINGDEV_IOC_GET_INFO, (void *)1) == -1 && errno == EFAULT);
	CHECK(ioctl(fd1, _IO('R', 99)) == -1 && errno == ENOTTY);
	CHECK(close(fd1) == 0);
	CHECK(ioctl(fd2, RINGDEV_IOC_GET_INFO, &b) == 0);	/* fd2 unaffected */
	CHECK(close(fd2) == 0);

	puts(failures ? "test_basic: FAILED" : "test_basic: PASS");
	return failures != 0;
}
