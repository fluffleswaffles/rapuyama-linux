#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""exercise the actual DMA submit and interrupt routines with modeled MMIO."""
from pathlib import Path
import os
import subprocess
import tempfile
source=(Path(__file__).resolve().parents[1]/'linux/kernel/linux/drivers/soc/rapuyama/videocore.c').read_text()
def function(name):
    start=source.index(name)
    return source[start:source.index('\n}',start)+2]
functions='\n'.join(function(n) for n in ['static irqreturn_t vc_dma_interrupt(', 'static void vc_dma_irq_stop(', 'static int vc_transport_fail(', 'static int vc_tx(', 'static int vc_tx_dma(', 'static int vc_gfx_prime(', 'static int vchi_slot_send('])
registers='\n'.join(line for line in source.splitlines() if line.startswith('#define G_'))
model=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uintptr_t dma_addr_t;typedef int irqreturn_t;
#define IRQ_NONE 0
#define IRQ_HANDLED 1
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define VC_STAT 0x14
#define VC_DMACTL 0x40
#define VC_DMACNT 0x44
#define VCHI_SLOT 0x1020
#define VCHI_MAGIC 0xaf05
#define VCHI_PAD 0x50fa
#define FCC_PAD 0x5f5f5f5f
#define DMA_TO_DEVICE 0
#define dev_warn_ratelimited(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_err(...) log_args(__VA_ARGS__)
static void log_args(void *dev,const char *format,...){(void)dev;(void)format;}
#define dev_warn(...) ((void)0)
struct completion{bool done;};
struct vc{void *gfx;bool dma_log,dma_ok;void *dev;bool gfx_irq_enabled,dma_waiting;int gfx_irq,transport_error,tx_credits;u8 tx[VCHI_SLOT];u32 tx_cur,dma_status;struct completion dma_done;};
static u32 regs[256],dmactl,go_reads,go_writes,pio_words,channels,polls,irq_calls,disables;
static bool priming,clear_go,timeout,polled,in_irq,early_irq,error_irq,missing_irq,late_irq,map_error;
static int chan_fail,pio_fail,idle_fail;
static unsigned maps,unmaps,bus_snapshots;
static u32 error_status=0x40;
static struct vc *active;
static void trigger_irq(void);
static u32 readl(void *p){
 u32 index=((u32 *)p)-regs;
 assert(!priming || index==G_IRQ/4);
 assert(!polled || (index==G_IRQ/4 && (in_irq || disables)));
 if(index==G_GO/4){go_reads++;if(clear_go){regs[index]&=~0x20u;}}
 return regs[index];
}
static void writel(u32 v,void *p){
 u32 index=((u32 *)p)-regs;
 if(index==G_GO/4){assert(!polled);go_writes++;}
 if(index==G_IRQ/4){regs[index]&=~v;}else{regs[index]=v;}
}
static void udelay(unsigned int us){assert(us==20 && regs[G_GO/4]==0x18129 && regs[G_MODE/4]==0x700000);}
static void reinit_completion(struct completion *c){c->done=false;}
static void complete(struct completion *c){c->done=true;}
static unsigned long msecs_to_jiffies(unsigned int ms){return ms;}
static unsigned long wait_for_completion_timeout(struct completion *,unsigned long);
static void disable_irq(int irq){assert(irq==146);disables++;if(late_irq){trigger_irq();}}
static int vc_chan(struct vc *vc,u32 c){(void)vc;(void)c;channels++;return chan_fail && channels==(u32)chan_fail?-EIO:0;}
static int vc_wr16(struct vc *vc,u32 v){(void)vc;(void)v;pio_words++;return pio_fail && pio_words==(u32)pio_fail?-EIO:0;}
static int vc_idle(struct vc *vc){(void)vc;return idle_fail?-EIO:0;}
static u32 vc_rd(struct vc *vc,u32 r){
 (void)vc;
 if(r==VC_STAT || r==VC_DMACNT){
  /* The failed transfer must still be enabled when its receiver is sampled. */
  assert((dmactl&0x11)==0x11 && (timeout || error_irq || idle_fail));
  bus_snapshots++;return r==VC_STAT?0x100:2048;
 }
 assert(r==VC_DMACTL);return dmactl;
}
static void vc_wr(struct vc *vc,u32 r,u32 v){(void)vc;if(r==VC_DMACTL){dmactl=v;if((v&0x11) && early_irq){trigger_irq();}}else{assert(r==0x44 && v==2048);}}
static void put_unaligned_le16(u16 v,void *p){memcpy(p,&v,2);}
static void put_unaligned_le32(u32 v,void *p){memcpy(p,&v,4);}
static dma_addr_t dma_map_single(void *d,void *p,u32 n,int dir){(void)d;(void)p;(void)dir;assert(n==VCHI_SLOT/G_LINE*G_LINE);maps++;return 0x1b5bc000;}
static bool dma_mapping_error(void *d,dma_addr_t p){(void)d;(void)p;return map_error;}
static void dma_unmap_single(void *d,dma_addr_t p,u32 n,int dir){(void)d;(void)p;(void)n;(void)dir;unmaps++;}
static int vchi_pump(struct vc *v,unsigned int ms,bool *cond){(void)v;(void)ms;(void)cond;assert(!"unexpected credit wait");return -EIO;}
#define readl_poll_timeout_atomic(addr,st,cond,delay,limit) (polled=true,polls++,(st)=timeout?0:(error_irq?error_status:1),(cond)?0:-ETIMEDOUT)
'''
main=r'''
static void trigger_irq(void){
 in_irq=true;regs[G_IRQ/4]=error_irq?error_status:0x81;
 assert(vc_dma_interrupt(146,active)==IRQ_HANDLED);irq_calls++;in_irq=false;
}
static unsigned long wait_for_completion_timeout(struct completion *c,unsigned long ticks){
 assert(ticks==100);polled=true;
 if(!c->done && !timeout && !missing_irq && !late_irq){
  /* A non-completion IRQ must be acknowledged without waking the worker. */
  in_irq=true;regs[G_IRQ/4]=0x80;assert(vc_dma_interrupt(146,active)==IRQ_HANDLED);in_irq=false;
  assert(!c->done);trigger_irq();
 }
 if(missing_irq){regs[G_IRQ/4]=0x81;}
 return c->done?1:0;
}
int main(void){
 /* Startup pulse must stop before polling, never read the running counter,
  * and leave an unsuccessful engine disabled without extra MMIO reads. */
 for(int scenario=0;scenario<3;scenario++){
  memset(regs,0,sizeof(regs));priming=true;polled=false;timeout=scenario==1;error_irq=scenario==2;
  struct vc vc={.gfx=regs};
  int ret=vc_gfx_prime(&vc);
  assert(ret==(timeout?-ETIMEDOUT:error_irq?-EIO:0));
  assert(regs[G_GO/4]==0x18128 && !go_reads && !regs[G_IRQ/4]);
  assert(regs[G_1C/4]==(ret?0:0x5641));
  assert(regs[G_MODE/4]==(ret?0x700000:0));
 }
 priming=false;
 for(int scenario=0;scenario<12;scenario++){
  bus_snapshots=0;
  memset(regs,0,sizeof(regs));dmactl=0x64;go_reads=go_writes=pio_words=channels=polls=irq_calls=disables=0;
  error_status=scenario==10?0x8000:scenario==11?0x41:0x40;clear_go=scenario==1;timeout=scenario==2 || scenario==5;polled=in_irq=false;early_irq=scenario==4;error_irq=scenario==6 || scenario>=9;missing_irq=scenario==7;late_irq=scenario==8;
  struct vc vc={.gfx=regs,.dma_log=true,.dma_ok=true,.gfx_irq_enabled=scenario>=3 && scenario<9,.gfx_irq=146};active=&vc;u8 data[4128]={0};
  int ret=vc_tx_dma(&vc,data,0x1b5bc000,sizeof(data),0);
  assert(go_reads==2 && go_writes==3);
  assert(regs[G_GO/4]==(clear_go?0x8909u:0x8929u));
  assert(regs[G_ADDR/4]==0x1b5bc000 && regs[G_SIZE/4]==0x7f000f);
  assert(dmactl==0x64 && !vc.dma_waiting);
  assert(bus_snapshots==((timeout || error_irq)?2u:0u));
  if(timeout || error_irq){
   assert(ret==(timeout?-ETIMEDOUT:-EIO) && !vc.dma_ok && !pio_words && vc.transport_error==ret);
   u32 old_channels=channels,old_go=go_writes;
   assert(vc_tx_dma(&vc,data,0x1b5bc000,sizeof(data),0)==ret);
   assert(vc_tx(&vc,data,sizeof(data),0)==ret);
   assert(vchi_slot_send(&vc)==ret);
   assert(channels==old_channels && go_writes==old_go && !pio_words);
  }
  else{assert(!ret && vc.dma_ok && pio_words==16 && channels==2);}
  if(scenario>=3 && scenario<9){assert(!polls);assert(disables==(timeout||error_irq||missing_irq||late_irq));assert(irq_calls==(!timeout && !missing_irq));}
  else{assert(polls==1 && !irq_calls);}
 }
 /* A failed slot must never be replayed or consume another credit. */
 for(int scenario=0;scenario<7;scenario++){
  memset(regs,0,sizeof(regs));dmactl=0x64;go_reads=go_writes=pio_words=channels=polls=irq_calls=disables=0;
  polled=in_irq=early_irq=error_irq=missing_irq=late_irq=clear_go=false;
  timeout=scenario==0;map_error=scenario==1;chan_fail=scenario==2?1:scenario==3?2:0;
  pio_fail=scenario==4?1:0;idle_fail=scenario==5;maps=unmaps=0;
  struct vc vc={.gfx=regs,.dma_ok=true,.gfx_irq=146,.tx_credits=3};active=&vc;
  int ret=vchi_slot_send(&vc);
  if(scenario<6){
   int expected=timeout?-ETIMEDOUT:-EIO;
   assert(ret==expected && vc.transport_error==expected && !vc.dma_ok);
   u32 prior_words=pio_words,prior_channels=channels,prior_go=go_writes;
   assert(vchi_slot_send(&vc)==expected);
   assert(vc_tx(&vc,vc.tx,VCHI_SLOT,0)==expected);
   assert(pio_words==prior_words && channels==prior_channels && go_writes==prior_go);
   assert(vc.tx_credits==2 && maps==1 && unmaps==!map_error);
  }else{assert(!ret && vc.dma_ok && vc.tx_credits==2 && maps==1 && unmaps==1 && pio_words==16);}
 }
 /* Separate error and completion IRQs before the worker runs must retain
  * the error regardless of delivery order. */
 for(int reverse=0;reverse<2;reverse++){
  struct vc vc={.gfx=regs,.dma_waiting=true};
  polled=false;
  for(int i=0;i<2;i++){
   regs[G_IRQ/4]=(i==reverse)?0x40:1;
   assert(vc_dma_interrupt(146,&vc)==IRQ_HANDLED);
  }
  assert(vc.dma_done.done && vc.dma_status==0x41);
 }
 /* Explicit PIO remains usable without DMA resources. */
 chan_fail=pio_fail=idle_fail=0;pio_words=channels=0;
 struct vc pio={.tx_credits=1};
 assert(vchi_slot_send(&pio)==0 && pio.tx_credits==0 && pio_words==VCHI_SLOT/2 && channels==2);
 /* PIO can also fail after sending part of a slot. No later slot or
  * channel delimiter may be sent on that corrupted stream. */
 for(int scenario=0;scenario<3;scenario++){
  struct vc v={.tx_credits=3};channels=pio_words=0;
  chan_fail=scenario==0?1:scenario==1?2:0;pio_fail=scenario==2?7:0;
  assert(vchi_slot_send(&v)==-EIO && v.transport_error==-EIO);
  u32 prior_words=pio_words,prior_channels=channels;
  assert(vchi_slot_send(&v)==-EIO && v.tx_credits==2);
  assert(vc_tx(&v,v.tx,VCHI_SLOT,0)==-EIO);
  assert(pio_words==prior_words && channels==prior_channels);
 }
 puts("DMA + PIO: partial PIO failures poison transport; bounded startup pulse success/error/timeout without counter reads, GO readback, IRQ wake before/during wait, unrelated IRQ, split error/completion IRQ status retention, missing/late IRQ recovery, stalled engine, polled error detection, poisoned transport, no slot replay, mapping/channel/tail/idle failures and explicit PIO passed");
}
'''
with tempfile.TemporaryDirectory(prefix='e6-dma-test-') as tmp:
    c=Path(tmp)/'test.c';binary=Path(tmp)/'test'
    c.write_text(registers+'\n'+model+functions+main)
    subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'),check=True)