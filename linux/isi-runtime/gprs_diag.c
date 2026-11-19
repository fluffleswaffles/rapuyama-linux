#define _GNU_SOURCE
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#ifdef __arm__
#include <ucontext.h>
#endif
#include "gprs_diag.h"

#define TRAP 0xdeee
static const struct {
	uint32_t address;
	uint16_t expected[3];
} sites[] = {
	{0xc636c65c, {0x20c9, 0x7160, 0x2001}},
	{0xc636d254, {0x22c9, 0x2300, 0xf7fe}},
	{0xc63be2fc, {0x79a8, 0x4626, 0x3608}}
};
static int active;
static struct isi_gprs_diag_state state;
static unsigned reported;
static int overflow_reported;

static uint32_t increment(uint32_t value)
{
	return value == UINT32_MAX ? value : value + 1;
}

int isi_gprs_diag_attach(int enabled)
{
	if (!enabled)
		return active ? -1 : 0;
	if (active)
		return -1;
	for (unsigned i = 0; i < sizeof(sites) / sizeof(sites[0]); i++)
		if (memcmp((void *)(uintptr_t)sites[i].address,
			   sites[i].expected, sizeof(sites[i].expected)))
			return -1;
	memset(&state, 0, sizeof(state));
	for (unsigned i = 0; i < sizeof(sites) / sizeof(sites[0]); i++) {
		*(volatile uint16_t *)(uintptr_t)sites[i].address = TRAP;
		__builtin___clear_cache((char *)(uintptr_t)sites[i].address,
				       (char *)(uintptr_t)(sites[i].address + 2));
	}
	active = 1;
	return 0;
}

int isi_gprs_diag_step(struct isi_gprs_diag_cpu *cpu)
{
	if (!active || !(cpu->cpsr & 0x20))
		return 0;
	unsigned site;
	for (site = 0; site < sizeof(sites) / sizeof(sites[0]); site++)
		if (cpu->r[15] == sites[site].address)
			break;
	if (site == sizeof(sites) / sizeof(sites[0]) ||
	    *(volatile uint16_t *)(uintptr_t)sites[site].address != TRAP)
		return 0;
	uint32_t cause = 0xc9;
	if (site == 2) {
		uint32_t payload = cpu->r[5];
		if (payload < 0xc6800000 || payload > 0xc6c4d000 - 54)
			return 0;
		cause = *(volatile const uint8_t *)(uintptr_t)(payload + 6);
		cpu->r[0] = cause;
	} else {
		cpu->r[site == 0 ? 0 : 2] = 0xc9;
		cpu->cpsr &= ~UINT32_C(0xc0000000);
	}
	cpu->r[15] += 2;
	state.sequence = increment(state.sequence);
	state.hits[site] = increment(state.hits[site]);
	if (state.count < ISI_GPRS_DIAG_CAPACITY) {
		struct isi_gprs_diag_record *record = &state.records[state.count];
		record->sequence = state.sequence;
		record->kind = site + 1;
		record->cause = cause;
		state.count++;
	} else {
		state.dropped = increment(state.dropped);
	}
	return 1;
}

const struct isi_gprs_diag_state *isi_gprs_diag_snapshot(void)
{
	return &state;
}

void isi_gprs_diag_report(void)
{
	if (!active)
		return;
	while (reported < state.count) {
		const struct isi_gprs_diag_record *record = &state.records[reported++];
		fprintf(stderr, "GPDS_DIAG sequence=%u site=%u cause=%02x\n",
			record->sequence, record->kind, record->cause);
	}
	if (state.dropped && !overflow_reported) {
		overflow_reported = 1;
		fprintf(stderr, "GPDS_DIAG overflow: further records discarded\n");
	}
}

#ifdef __arm__
int isi_gprs_diag_fault(int signal, void *context)
{
	if (signal != SIGILL)
		return 0;
	ucontext_t *uc = context;
	struct isi_gprs_diag_cpu cpu = {0};
	cpu.r[0] = uc->uc_mcontext.arm_r0;
	cpu.r[2] = uc->uc_mcontext.arm_r2;
	cpu.r[5] = uc->uc_mcontext.arm_r5;
	cpu.r[15] = uc->uc_mcontext.arm_pc;
	cpu.cpsr = uc->uc_mcontext.arm_cpsr;
	if (!isi_gprs_diag_step(&cpu))
		return 0;
	uc->uc_mcontext.arm_r0 = cpu.r[0];
	uc->uc_mcontext.arm_r2 = cpu.r[2];
	uc->uc_mcontext.arm_pc = cpu.r[15];
	uc->uc_mcontext.arm_cpsr = cpu.cpsr;
	return 1;
}
#endif
