// SPDX-License-Identifier: GPL-2.0-only
#ifndef ISI_IDENTITY_RUNTIME_H
#define ISI_IDENTITY_RUNTIME_H

int isi_identity_load(const char *path);
int isi_identity_attach(void);
int isi_identity_selftest(void);
void isi_identity_report(void);
#endif
