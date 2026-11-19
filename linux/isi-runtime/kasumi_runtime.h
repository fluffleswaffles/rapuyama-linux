// SPDX-License-Identifier: GPL-2.0-only
#ifndef RYISI_KASUMI_RUNTIME_H
#define RYISI_KASUMI_RUNTIME_H
#include <stdint.h>

void isi_kasumi_patches(void);
void isi_kasumi_diagnostics(int enabled);
int isi_kasumi_selftest(void);
int isi_kasumi_fault(void *context);
void isi_kasumi_watch(int enabled);
void isi_kasumi_watch_rearm(void);
int isi_kasumi_watch_fault(uint32_t fault,void *context);
int isi_kasumi_watch_selftest(void);
int isi_kasumi_uplink_match(unsigned sn,const unsigned char bytes[4]);
#endif