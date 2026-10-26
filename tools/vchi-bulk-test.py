#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""exercise the real VCHI bulk sender with synthetic VC receive requests."""
from pathlib import Path
import os
import subprocess
import tempfile
s=(Path(__file__).resolve().parents[1]/'linux/kernel/linux/drivers/soc/rapuyama/videocore.c').read_text()
fn=s[s.index('static int vchi_bulk_tx('):s.index('/* ---- dispcommanddrv ---- */')]
slot=s[s.index('static int vchi_slot_msg('):s.index('static int vchi_pump(', s.index('static int vchi_slot_msg('))]
preamble=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uintptr_t dma_addr_t;
#define VCHI_MAGIC 0xaf05
#define VCHI_SLOT 0x1020
#define VCHI_MAX_MSG 0x800
#define VCHI_TX_MAX_MSG 0x1000
#define VCHI_BULK_PIECE 0x18000
#define VC_FB_SIZE 614400
#define FCC_BULX 0x584c5542
#define DMA_TO_DEVICE 0
#define G_LINE 256
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define min(a,b) ((a)<(b)?(a):(b))
#define dev_err(...) ((void)0)
#define dev_info(...) ((void)0)

static int debug;
static unsigned long jiffies;
static unsigned long msecs_to_jiffies(unsigned long x){return x;}
static bool time_after(unsigned long x,unsigned long y){return x>y;}
struct vc_bulk_req{bool valid; u32 size,rx_size,offset;};
struct vc{u8 bulk_msg[VCHI_TX_MAX_MSG]; u8 tx[VCHI_SLOT]; u8 rx[4096]; u32 tx_cur,credits_owed; bool dma_ok; int transport_error; u8 *push; dma_addr_t push_phys; void *dev; unsigned frames;};
static struct vc_bulk_req request;
static u8 ownership[VC_FB_SIZE];
static int dma_fail;
static u32 dma_calls,sync_device,sync_cpu;
static void dma_sync_single_range_for_device(void *dev,dma_addr_t base,u32 off,u32 n,int dir){
 (void)dev;assert(base==0x18000000 && dir==DMA_TO_DEVICE && n && off+n<=VC_FB_SIZE);
 for(u32 i=off;i<off+n;i++){assert(!ownership[i]);ownership[i]=1;}sync_device++;
}
static void dma_sync_single_range_for_cpu(void *dev,dma_addr_t base,u32 off,u32 n,int dir){
 (void)dev;assert(base==0x18000000 && dir==DMA_TO_DEVICE && n && off+n<=VC_FB_SIZE);
 for(u32 i=off;i<off+n;i++){assert(ownership[i]);ownership[i]=0;}sync_cpu++;
}
static u8 sent[4096]; static u32 sent_n, bulk_n;
static void put_unaligned_le32(u32 v,void *p){memcpy(p,&v,4);}
static void put_unaligned_le16(u16 v,void *p){memcpy(p,&v,2);}
static u32 get32(int x){u32 v;memcpy(&v,sent+x,4);return v;}
static u16 get16(int x){u16 v;memcpy(&v,sent+x,2);return v;}
static struct vc_bulk_req *vchi_find_bulk_req(struct vc *v,u32 f){(void)v;(void)f;return request.valid?&request:NULL;}
static int inject_request;
static int vc_rx(struct vc *v,u8 *buf,u32 max,u32 *n,int timeout){(void)v;(void)buf;(void)max;(void)timeout;if(inject_request){*n=28;return 0;}jiffies+=20;return -EAGAIN;}
static void vchi_handle_slot(struct vc *v,u32 n){(void)v;assert(n==28);request.valid=true;inject_request=0;}
static void vchi_answer_queries(struct vc *v){(void)v;}
static int vchi_slot_msg(struct vc *,u32,const void *,u32);
static int vchi_send(struct vc *v,u32 f,const void *p,u32 n){assert(f==FCC_BULX);v->tx_cur=0;int r=vchi_slot_msg(v,f,p,n);if(r){return r;}assert(n<=4096);memcpy(sent,p,n);sent_n=n;return 0;}
static int vc_tx(struct vc *v,const u8 *p,u32 n,int c){(void)v;(void)p;(void)c;bulk_n+=n;return 0;}
static int vc_tx_dma(struct vc *v,const u8 *p,dma_addr_t a,u32 n,int c){
 u32 off=p-v->push,bytes=n/G_LINE*G_LINE;assert(a==v->push_phys+off);
 for(u32 i=0;i<n;i++){assert(ownership[off+i]==(i<bytes));}
 dma_calls++;return dma_fail?dma_fail:vc_tx(v,p,n,c);
}
'''
main=r'''
int main(void){
 struct vc v={0}; u8 pixels[131072]; memset(pixels,0x5a,sizeof(pixels));
 /* The first failing phone upload was 2704 bytes. VC may give no aligned area. */
 request=(struct vc_bulk_req){true,2704,0,0};
 assert(vchi_bulk_tx(&v,0x4e52484b,pixels,2704)==0);
 assert(!request.valid && sent_n==2732 && get32(4)==2704 && get32(16)==0);
 assert(!sent[22] && get16(24)==2704 && get16(26)==0);
 assert(!memcmp(sent+28,pixels,2704) && bulk_n==0);
 request=(struct vc_bulk_req){true,4068,0,0};
 assert(vchi_bulk_tx(&v,0x4e52484b,pixels,4068)==0);
 assert(sent_n==4096 && v.tx_cur==4112 && get16(24)==4068);
 request=(struct vc_bulk_req){true,4072,0,0};
 assert(vchi_bulk_tx(&v,0x4e52484b,pixels,4072)==-EINVAL);
 /* Existing aligned large transfers and their short inline tail stay valid. */
 request=(struct vc_bulk_req){true,8195,8192,0};
 assert(vchi_bulk_tx(&v,0x4e52484b,pixels,8195)==0);
 assert(sent_n==32 && get32(16)==8192 && sent[22]==1 && get16(24)==0 && get16(26)==3 && bulk_n==8192);
 request=(struct vc_bulk_req){true,2704,2688,16}; bulk_n=0;
 assert(vchi_bulk_tx(&v,0x4e52484b,pixels,2704)==0);
 assert(get32(16)==2688 && get16(20)==(u16)-16 && get16(26)==16 && bulk_n==2688);
 assert(vchi_bulk_tx(&v,0x4e52484b,pixels,128)==0);
 assert(get16(24)==128 && !sent[22]);
 /* An immediately arriving request must not spend a polling window. */
 request=(struct vc_bulk_req){false,8192,8192,0};inject_request=1;jiffies=0;
 assert(vchi_bulk_tx(&v,0x4e52484b,pixels,8192)==0 && jiffies==0);
 request.valid=false;jiffies=0;
 assert(vchi_bulk_tx(&v,0x4e52484b,pixels,8192)==-ETIMEDOUT && jiffies>=2000);
 /* Reusing the staging buffer must return every DMA cache line to CPU.
  * Test several pieces, a PIO-only short piece, a PIO tail, and inline data. */
 static u8 push[VC_FB_SIZE];v.push=push;v.push_phys=0x18000000;v.dma_ok=true;
 const u32 sizes[]={2704,8195,196640,614400};
 for(u32 i=0;i<sizeof(sizes)/sizeof(*sizes);i++){
  u32 n=sizes[i];request=(struct vc_bulk_req){true,n,n&~31u,0};bulk_n=0;
  assert(vchi_bulk_tx(&v,0x4e52484b,push,n)==0 && bulk_n==(n&~31u));
  assert(sync_device==sync_cpu);
  for(u32 j=0;j<VC_FB_SIZE;j++){assert(!ownership[j]);}
  memset(push,0x42,sizeof(push));
  assert(vchi_bulk_tx(&v,0x4e52484b,push,128)==0);
 }
 request=(struct vc_bulk_req){true,2080,2048,0};
 assert(vchi_bulk_tx(&v,0x4e52484b,push,2080)==0);
 request=(struct vc_bulk_req){true,2080,32,0};
 u32 prior=sync_device;
 assert(vchi_bulk_tx(&v,0x4e52484b,push,2080)==0 && sync_device==prior);
 /* An unfinished transfer is never handed back for CPU reuse. */
 request=(struct vc_bulk_req){true,8192,8192,0};dma_fail=-ETIMEDOUT;
 assert(vchi_bulk_tx(&v,0x4e52484b,push,8192)==-ETIMEDOUT);
 assert(sync_device==sync_cpu+1 && dma_calls>5);
 for(u32 j=0;j<8192;j++){assert(ownership[j]);}
 v.transport_error=-EIO;request.valid=true;prior=sync_device;
 assert(vchi_bulk_tx(&v,0x4e52484b,push,128)==-EIO && request.valid && sync_device==prior);
 puts("VCHI bulk: inline/request/size cases and DMA ownership across multiple pieces, PIO tails, CPU reuse and failure passed");
}
'''
with tempfile.TemporaryDirectory(prefix='vchi-bulk-test-') as tmp:
    source=Path(tmp)/'test.c'
    binary=Path(tmp)/'test'
    source.write_text(preamble+slot+fn+main)
    subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-o',str(binary),str(source)],check=True)
    env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
    subprocess.run([str(binary)],env=env,check=True)