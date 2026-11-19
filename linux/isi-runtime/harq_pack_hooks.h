/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ISI_HARQ_PACK_HOOKS_H
#define ISI_HARQ_PACK_HOOKS_H
#include "harq_pack_capture.h"
#include "thumb_redirect.h"

static struct isi_harq_pack_capture harq_pack_capture;
static unsigned harq_allocation_records;
static uint32_t harq_native_allocate(uint32_t base,uint32_t bytes)
{
	uint32_t (*original)(uint32_t,uint32_t)=(void *)0xc64c2bdb;
	uint32_t result=original(base,bytes);
	int status=isi_harq_capture_allocation(&harq_pack_capture,base,bytes,harq_firmware_read,NULL);
	if(harq_allocation_records<8){
		unsigned slots=0;
		for(unsigned i=0;i<8;i++)
			slots+=harq_pack_capture.allocation[i].generation==harq_pack_capture.generation;
		fprintf(stderr,"HARQ_ALLOC record=%u generation=%u buffer_bytes=%u validated=%u slots_validated=%u\n",
			++harq_allocation_records,harq_pack_capture.generation,bytes,status==0,slots);
	}
	return result;
}
static uint32_t harq_native_pack(uint32_t output,uint32_t state)
{
	isi_harq_capture_packing(&harq_pack_capture,output,state,harq_firmware_read,NULL);
	uint32_t (*original)(uint32_t,uint32_t)=(void *)0xc64c2689;
	return original(output,state);
}
static int isi_harq_pack_hooks_install(void)
{
	static const struct { uint32_t site; uint16_t first,second; } sites[]={
		{0xc64c14cc,0xf001,0xfb85},
		{0xc64c0ecc,0xf001,0xfbdc},
		{0xc64c12e0,0xf001,0xf9d2},
	};
	for(unsigned i=0;i<3;i++){
		uint16_t original[2];
		memcpy(original,(void *)(uintptr_t)sites[i].site,sizeof(original));
		if(original[0]!=sites[i].first || original[1]!=sites[i].second) return -1;
	}
	isi_window_release(0xc6610000,4096);
	void *page=mmap((void *)0xc6610000,4096,PROT_READ|PROT_WRITE,
		MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
	if(page==MAP_FAILED) return -1;
	isi_thumb_redirect(page,(uintptr_t)harq_native_allocate);
	isi_thumb_redirect((uint8_t *)page+16,(uintptr_t)harq_native_pack);
	__builtin___clear_cache(page,(char *)page+32);
	if(mprotect(page,4096,PROT_READ|PROT_EXEC)) { munmap(page,4096);return -1; }
	for(unsigned i=0;i<3;i++){
		uint32_t target=0xc6610000+(i?16:0);
		int32_t offset=(int32_t)(target-sites[i].site-4);
		uint16_t branch[2]={0xf000|((uint32_t)offset>>12&0x7ff),
			0xf800|((uint32_t)offset>>1&0x7ff)};
		memcpy((void *)(uintptr_t)sites[i].site,branch,sizeof(branch));
	}
	return 0;
}
#endif
