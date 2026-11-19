/* opt-in diagnostic */
#ifndef ISI_GPRS_DIAG_H
#define ISI_GPRS_DIAG_H
#include <stdint.h>

#define ISI_GPRS_DIAG_CAPACITY 32
struct isi_gprs_diag_record {
	uint32_t sequence, kind, cause;
};
struct isi_gprs_diag_state {
	uint32_t sequence, hits[3], count, dropped;
	struct isi_gprs_diag_record records[ISI_GPRS_DIAG_CAPACITY];
};
struct isi_gprs_diag_cpu {
	uint32_t r[16], cpsr;
};

int isi_gprs_diag_attach(int enabled);
int isi_gprs_diag_step(struct isi_gprs_diag_cpu *cpu);
const struct isi_gprs_diag_state *isi_gprs_diag_snapshot(void);

void isi_gprs_diag_report(void);
#ifdef __arm__

int isi_gprs_diag_fault(int signal, void *context);
#endif
#endif
