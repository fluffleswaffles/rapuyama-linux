// SPDX-License-Identifier: GPL-2.0-only
/*
 * prints MDI doorbell register (0x20040280) like busybox devmem */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define DOORBELLS	0x20040280UL

int main(void)
{
	long page = sysconf(_SC_PAGESIZE);
	int fd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
	volatile uint32_t *map;

	if (fd < 0 || page <= 0) {
		perror("/dev/mem");
		return 1;
	}
	map = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, DOORBELLS & ~(page - 1));
	close(fd);
	if (map == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	printf("0x%08x\n", map[(DOORBELLS & (page - 1)) / 4]);
	return 0;
}
