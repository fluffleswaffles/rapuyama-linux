/* simprobe: read-only check of /dev/rapuyama-sim (drivers/misc/rapuyama_sim.c).
 * Reads upstream's diagnostic SIM registers (4..0x14, 0x28), the PMIC registers on the SIM write
 * allowlist, ADC channels 2/8/9 and the pending interrupt mask. Writes nothing,
 * enables no interrupt, does not power the SIM. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/ioctl.h>

struct isi_sim_reg { uint32_t offset, size, value, write; };
struct isi_sim_pmic { uint32_t device, reg, value, write; };
struct isi_sim_adc { uint32_t channel, value; };
#define ISI_SIM_REG _IOWR('S', 0, struct isi_sim_reg)
#define ISI_SIM_PMIC _IOWR('S', 1, struct isi_sim_pmic)
#define ISI_SIM_ADC _IOWR('S', 3, struct isi_sim_adc)

int main(void)
{
	int fd = open("/dev/rapuyama-sim", O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror("/dev/rapuyama-sim");
		return 1;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	/* upstream's diagnostic set (sim_runtime.c hardware_snapshot); 0x20 is the
	 * data FIFO: never read it here (a read would consume a byte) */
	printf("SIM regs (pa 0x20038300):");
	const uint32_t offs[] = {4, 8, 0xc, 0x10, 0x14, 0x28};
	for (unsigned k = 0; k < sizeof(offs) / sizeof(offs[0]); k++) {
		uint32_t off = offs[k];
		struct isi_sim_reg r = {off, off == 0x28 ? 2 : 4, 0, 0};
		if (ioctl(fd, ISI_SIM_REG, &r)) {
			printf(" %02x:err(%s)", off, strerror(errno));
			continue;
		}
		printf(" %02x:%08x", off, r.value);
	}
	printf("\n");
	const uint32_t pm[][2] = {{1, 0x1c}, {1, 0x1d}, {1, 0x1e}, {2, 0x01}, {2, 0x03}};
	printf("PMIC:");
	for (unsigned i = 0; i < sizeof(pm) / sizeof(pm[0]); i++) {
		struct isi_sim_pmic p = {pm[i][0], pm[i][1], 0, 0};
		if (ioctl(fd, ISI_SIM_PMIC, &p))
			printf(" %u.%02x:err(%s)", p.device, p.reg, strerror(errno));
		else
			printf(" %u.%02x:%04x", p.device, p.reg, p.value);
	}
	printf("\n");
	/* watchdog reg: must be refused. 0x30 = the normal feed value, harmless even if it went through */
	struct isi_sim_pmic deny = {1, 0x17, 0x30, 1};
	printf("PMIC write 1.17 (not on the allowlist): %s\n",
	       ioctl(fd, ISI_SIM_PMIC, &deny) ? strerror(errno) : "ALLOWED (bad)");
	printf("ADC (2 ntc, 8 vbat, 9 charger):");
	const uint32_t chans[] = {2, 8, 9};
	for (unsigned i = 0; i < 3; i++) {
		uint32_t ch = chans[i];
		struct isi_sim_adc a = {ch, 0};
		if (ioctl(fd, ISI_SIM_ADC, &a))
			printf(" %u:err", ch);
		else
			printf(" %u:%u", ch, a.value);
	}
	printf("\n");
	uint32_t pending = 0;
	ssize_t n = read(fd, &pending, 4);
	printf("pending read: %zd (%s) mask %08x\n", n, n < 0 ? strerror(errno) : "ok", pending);
	close(fd);
	return 0;
}
