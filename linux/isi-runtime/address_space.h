// SPDX-License-Identifier: GPL-2.0-only
#ifndef RYISI_ADDRESS_SPACE_H
#define RYISI_ADDRESS_SPACE_H
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>

// firmware code/ram/peripheral copies live in c6000000-c7200000. the kernel's first
// mmap(NULL) allocations land right below the stack gap (~c6f8xxxx on e6), so an early
// malloc can sit inside a firmware region. reserve the window before anything allocates
#define ISI_WINDOW_START 0xc6000000u
#define ISI_WINDOW_SIZE 0x1200000u

static inline int isi_window_reserve(void){
	return mmap((void *)ISI_WINDOW_START,ISI_WINDOW_SIZE,PROT_NONE,
		MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED_NOREPLACE,-1,0)==MAP_FAILED ? -1 : 0;
}

static inline void isi_window_release(uint32_t address,size_t length){
	if(address>=ISI_WINDOW_START && length<=ISI_WINDOW_SIZE && address-ISI_WINDOW_START<=ISI_WINDOW_SIZE-length){
		munmap((void *)(uintptr_t)address,length);
	}
}
#endif
