// SPDX-License-Identifier: GPL-2.0-only
#ifndef RYISI_SIM_RUNTIME_H
#define RYISI_SIM_RUNTIME_H

#include <signal.h>

int isi_sim_active(void);
int isi_sim_open(const char *device,int embedded);
int isi_sim_embedded(void);
int isi_sim_fd(void);
unsigned int isi_sim_timeout(void);
void isi_sim_attach(void);
void isi_sim_service(void);
int isi_sim_backend_selftest(void);
int isi_sim_map(void);
int isi_sim_fault(siginfo_t *info,void *context);
void isi_sim_patches(void);
void isi_sim_report(void);
int isi_sim_run(int reset,int cycle);
#endif