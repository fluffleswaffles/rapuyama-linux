// SPDX-License-Identifier: GPL-2.0-only
#ifndef RYISI_OS_TIMER_RUNTIME_H
#define RYISI_OS_TIMER_RUNTIME_H

void isi_os_timer_patches(void);
int isi_os_timer_selftest(void);
int isi_os_sms_timeout_selftest(void);
void isi_os_timer_attach(void);
unsigned int isi_os_timer_service(void);
int isi_os_timer_live_selftest(void);

#endif