/* regpeek: read-only, bus-fault-safe register reads through /dev/mem.
 *   regpeek <phys page> <offset> [offset...]    (hex; one 32-bit read each)
 *   regpeek <phys page> all                     (the whole 4 KB page)
 * An unclocked/unpowered block raises a bus error: that read prints "fault"
 * (SIGBUS caught in userspace) instead of oopsing the kernel. */
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static sigjmp_buf jump;
static void bus(int sig) { siglongjmp(jump, sig); }

static void peek(volatile uint32_t *base, unsigned off)
{
	int sig = sigsetjmp(jump, 1);
	if (sig) {
		printf(" %03x:fault(%d)", off, sig);
		return;
	}
	uint32_t v = base[off / 4];
	printf(" %03x:%08x", off, v);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: regpeek <phys page> <offset...|all>\n");
		return 2;
	}
	unsigned long page = strtoul(argv[1], NULL, 16) & ~0xfffUL;
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	volatile uint32_t *base = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, page);
	if (base == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	struct sigaction sa = {.sa_handler = bus};
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);
	printf("%08lx:", page);
	if (!strcmp(argv[2], "all")) {
		for (unsigned off = 0; off < 4096; off += 4) {
			if (off && !(off % 32))
				printf("\n%08lx:", page + off);
			peek(base, off);
		}
	} else {
		for (int i = 2; i < argc; i++)
			peek(base, strtoul(argv[i], NULL, 16));
	}
	printf("\n");
	return 0;
}
