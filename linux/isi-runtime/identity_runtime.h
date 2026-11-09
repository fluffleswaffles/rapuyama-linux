// SPDX-License-Identifier: GPL-2.0-only
#ifndef ISI_IDENTITY_RUNTIME_H
#define ISI_IDENTITY_RUNTIME_H

int isi_identity_load(const char *path);
int isi_identity_attach(int required);
int isi_identity_selftest(void);
#endif
