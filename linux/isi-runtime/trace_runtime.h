// SPDX-License-Identifier: GPL-2.0-only
#ifndef RYISI_TRACE_RUNTIME_H
#define RYISI_TRACE_RUNTIME_H

#include <stdint.h>

int isi_trace_open(const char *path);
void isi_trace_record(unsigned kind,const uint32_t descriptor[4],const void *payload,unsigned size);
#endif