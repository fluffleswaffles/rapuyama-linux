// SPDX-License-Identifier: GPL-2.0-only
/* Upstream's linux/isi-runtime/os_timer_runtime.c on the E5 firmware (fw 101.003).
 * E5 differences (docs/devices/e5-00/cellular-map.md, "OS timers"):
 *  - copied timer pool: 240 slots of 16 bytes {ticks, callback, arg, u16 next} after an
 *    8-byte header {count, free, head, tail} at c683496c (E6: 279 x 20 B, header at -8,
 *    an extra microsecond word at +0); ticks are 8192 Hz on both
 *  - timer create returns the handle only (E6: delay<<32 | handle)
 *  - OS timer table c6832e8c, 237 ids (E6 c681fd48, 276), same 28-byte entries
 *  - Linux-owned energy timers belong to task 28 (E6 29); the watchdogs stopped at attach are
 *    id 172 of task 26 (E6 212 of task 27) and id 219 of task 34 (E6 266 of task 38: a 1 s
 *    supervision of a DSP request, c63c8951 sends 0x12 to dev 0x7c, a 0x12/0xda reply stops it,
 *    expiry = reset 9)
 *  - selftest timer 29 of task 2 (E6 62), energy timer 183 (E6 223)
 * The SMS timeout selftest is not ported (its SMS addresses are not mapped yet). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include "thumb_redirect.h"
#include "os_timer_runtime.h"

#define POOL 240u		/* copied timer pool slots */
#define IDS 237u		/* OS timer ids (< 0xed) */
#define CODE_LO 0xc6300000u
#define CODE_HI 0xc66ae000u
#define DATA_LO 0xc6800000u
#define DATA_HI 0xc6a80000u
#define OS_TABLE 0xc6832e8cu	/* 28-byte entries: +0x10 owner task, +0x11 flags, +0x12 state */
#define COPIED_POOL 0xc683496cu
#define SCHEDULER 0xc6835c3cu
#define OS_START 0xc63ba68fu
#define OS_STOP 0xc63ba94fu
#define ENERGY_TASK 28u
#define WATCHDOG_ID 172u
#define WATCHDOG_TASK 26u
#define DSP_WATCHDOG_ID 219u
#define DSP_WATCHDOG_TASK 34u
#define TEST_ID 29u
#define ENERGY_ID 183u
#define BX_LR_CALLBACK 0xc63b9d17u	/* the c6033340 helper, patched to bx lr */

struct os_timer{uint64_t deadline,serial;uint32_t callback,arg;int active;};
static struct os_timer timers[POOL];
static uint64_t now,baseline,sequence;
static int live_clock,trace,offline;
void isi_os_timer_offline(void){offline=1;}
static void sync_clock(void);
static _Noreturn void invalid(const char *reason){fprintf(stderr,"os timer: %s\n",reason);_exit(123);}

static uint32_t remaining(uint32_t handle){
	sync_clock();
	handle&=65535;
	if(handle>=POOL){invalid("invalid handle");}
	return timers[handle].active && timers[handle].deadline>now ? timers[handle].deadline-now : 0;
}

static uint32_t create(uint32_t delay,uint32_t callback,uint32_t arg){
	sync_clock();
	if(callback<CODE_LO || callback>=CODE_HI){invalid("invalid callback");}
	for(unsigned i=0;i<POOL;i++){
		if(!timers[i].active){
			timers[i]=(struct os_timer){now+delay,++sequence,callback,arg,1};
			return i;
		}
	}
	invalid("pool exhausted");
}

static uint32_t cancel(uint32_t handle){
	handle&=65535;
	if(handle>=POOL || !timers[handle].active){invalid("cancel inactive handle");}
	uint32_t value=remaining(handle);timers[handle].active=0;return value;
}

static uint32_t next_remaining(void){
	sync_clock();
	uint64_t earliest=UINT64_MAX;
	for(unsigned i=0;i<POOL;i++){if(timers[i].active && timers[i].deadline<earliest){earliest=timers[i].deadline;}}
	uint64_t value=earliest==UINT64_MAX || earliest<=now ? 0 : earliest-now;
	return value>UINT32_MAX ? UINT32_MAX : value;
}

static uint32_t elapsed(void){sync_clock();return (now-baseline)/5000;}
static void rebase(void){sync_clock();baseline=now;}
static void sync_clock(void){
	if(live_clock){
		struct timespec time;if(clock_gettime(CLOCK_MONOTONIC,&time)){invalid("monotonic clock");}
		now=(uint64_t)time.tv_sec*1000000+time.tv_nsec/1000;
	}
}

static void counter(void){
	*(volatile uint32_t *)0xc603334c=(uint32_t)(now*384/10);
	*(volatile uint32_t *)0xc6033350=(uint32_t)(now*32768/1000000);
}

static int earliest_due(uint64_t limit){
	int selected=-1;
	for(unsigned i=0;i<POOL;i++){
		if(!timers[i].active || timers[i].deadline>limit){continue;}
		if(selected<0 || timers[i].deadline<timers[selected].deadline || (timers[i].deadline==timers[selected].deadline && timers[i].serial<timers[selected].serial)){selected=i;}
	}
	return selected;
}

static unsigned advance(uint64_t delta){
	uint64_t target=now+delta;unsigned fired=0;
	for(int selected;(selected=earliest_due(target))>=0;){
		if(++fired>10000){invalid("callback budget exceeded");}
		struct os_timer timer=timers[selected];timers[selected].active=0;
		now=timer.deadline;counter();((void (*)(uint32_t))(uintptr_t)timer.callback)(timer.arg);
	}
	now=target;counter();return fired;
}

/* the snapshot's pending firmware timers (delta list in 8192 Hz ticks) into the adapter */
static unsigned adopt(void){
	const uint16_t *meta=(void *)(uintptr_t)COPIED_POOL;
	unsigned count=meta[0],head=meta[2];uint64_t ticks=0;unsigned char seen[POOL]={0};
	if(count>POOL){invalid("copied timer count");}
	for(unsigned n=0;n<count;n++){
		if(head>=POOL || seen[head]){invalid("copied timer chain");}
		seen[head]=1;
		const uint32_t *entry=(void *)(uintptr_t)(COPIED_POOL+8+head*16);
		if(entry[0]==UINT32_MAX || entry[1]<CODE_LO || entry[1]>=CODE_HI){invalid("copied timer entry");}
		ticks+=entry[0];
		timers[head]=(struct os_timer){now+(ticks*1000000+8191)/8192,++sequence,entry[1],entry[2],1};
		head=*(const uint16_t *)(entry+3);
	}
	return count;
}

static const unsigned char *os_entry(unsigned id){return (const unsigned char *)(uintptr_t)(OS_TABLE+id*28);}

void isi_os_timer_patches(void){
	const uintptr_t entries[][2]={
		{0xc63bb89c,(uintptr_t)create},{0xc63bb954,(uintptr_t)cancel},
		{0xc63bba60,(uintptr_t)remaining},{0xc63bbb9e,(uintptr_t)next_remaining},
		{0xc63ba434,(uintptr_t)elapsed},{0xc63bb852,(uintptr_t)rebase},
	};
	for(unsigned i=0;i<sizeof(entries)/sizeof(entries[0]);i++){isi_thumb_redirect((void *)entries[i][0],entries[i][1]);}
}

int isi_os_timer_selftest(void){
	void (*start)(uint32_t,uint32_t)=(void *)OS_START;
	uint32_t (*stop)(uint32_t)=(void *)OS_STOP;
	volatile unsigned char *state=(void *)(uintptr_t)(OS_TABLE+TEST_ID*28+0x12);
	for(unsigned ticks=1;ticks<=100;ticks++){
		start(TEST_ID,ticks);
		if(next_remaining()!=ticks*5000 || advance(ticks*5000-1) || advance(1)!=1 || *state!=3 || next_remaining()){fprintf(stderr,"expiry %u: state %u\n",ticks,*state);return 1;}
	}
	for(unsigned ticks=1;ticks<=100;ticks++){
		start(TEST_ID,ticks);advance(1);
		if(stop(TEST_ID)>ticks || advance(ticks*5000) || next_remaining()){fprintf(stderr,"cancel %u\n",ticks);return 1;}
		start(TEST_ID,ticks+1);
		if(advance((ticks+1)*5000)!=1 || *state!=3){fprintf(stderr,"rearm %u\n",ticks);return 1;}
	}
	uint32_t (*arm)(uint32_t,uint32_t,uint32_t)=(void *)0xc63bb89d;
	uint32_t (*left)(uint32_t)=(void *)0xc63bba61,(*remove)(uint32_t)=(void *)0xc63bb955;
	uint32_t handles[POOL];
	for(unsigned i=0;i<POOL;i++){
		handles[i]=arm(1000000+i,BX_LR_CALLBACK,0);
		if(handles[i]>=POOL || left(handles[i])!=1000000+i){fprintf(stderr,"arm %u\n",i);return 1;}
		for(unsigned j=0;j<i;j++){if(handles[j]==handles[i]){return 1;}}
	}
	for(unsigned i=0;i<POOL;i++){if(remove(handles[i])!=1000000+i){fprintf(stderr,"remove %u\n",i);return 1;}}
	if(next_remaining() || advance(2000000)){return 1;}
	rebase();advance(0x100000000ULL+12345);
	if(elapsed()!=(0x100000000ULL+12345)/5000){fprintf(stderr,"long clock\n");return 1;}
	unsigned adopted=adopt(),active=0;
	for(unsigned i=0;i<POOL;i++){active+=timers[i].active!=0;}
	if(!adopted || active!=adopted){fprintf(stderr,"adopt %u active %u\n",adopted,active);return 1;}
	unsigned handle=arm(10,BX_LR_CALLBACK,0);
	if(remove(handle)!=10){return 1;}
	active=0;for(unsigned i=0;i<POOL;i++){active+=timers[i].active!=0;}
	if(active!=adopted){return 1;}
	printf("native os timers: 100 expiries, 100 cancel/rearms, EABI, %u slots, long clock, adopted %u pending timers passed\n",POOL,adopted);return 0;
}

void isi_os_timer_attach(void){
	for(unsigned i=0;i<POOL;i++){if(timers[i].active){invalid("attach with active adapter timers");}}
	const char *t=getenv("E5RT_OS_TRACE");trace=t && !strcmp(t,"1");
	live_clock=1;sync_clock();baseline=now;unsigned copied=adopt();
	uint32_t config=*(uint32_t *)(uintptr_t)(SCHEDULER+0x20);
	if(config<CODE_LO || config>CODE_HI-48*16 || *(uint32_t *)(uintptr_t)(config+ENERGY_TASK*16)!=0xc6329389 || *(uint32_t *)(uintptr_t)(config+WATCHDOG_TASK*16)!=0xc63ca065 || *(uint32_t *)(uintptr_t)(config+DSP_WATCHDOG_TASK*16)!=0xc63c8b75){invalid("unsupported task ownership");}
	uint32_t (*stop)(uint32_t)=(void *)OS_STOP;unsigned energy=0,watchdogs=0;
	for(unsigned i=0;i<IDS;i++){
		const unsigned char *entry=os_entry(i);
		if(entry[18]!=2){continue;}
		if(entry[16]==ENERGY_TASK){stop(i);energy++;}
		else if((i==WATCHDOG_ID && entry[16]==WATCHDOG_TASK) || (i==DSP_WATCHDOG_ID && entry[16]==DSP_WATCHDOG_TASK)){stop(i);watchdogs++;}
	}
	printf("os timers attached: copied=%u linux energy=%u watchdogs=%u\n",copied,energy,watchdogs);
}

unsigned int isi_os_timer_service(void){
	if(!live_clock){return 0;}
	for(unsigned i=0;i<IDS;i++){
		const unsigned char *entry=os_entry(i);
		if(entry[16]==ENERGY_TASK && entry[18]==2){((uint32_t (*)(uint32_t))OS_STOP)(i);}
	}
	/* offline (no DSP to answer task 34's request): its 1 s supervision would reset the firmware */
	if(offline && os_entry(DSP_WATCHDOG_ID)[18]==2){((uint32_t (*)(uint32_t))OS_STOP)(DSP_WATCHDOG_ID);}
	unsigned fired=0;
	for(int selected;fired<64;fired++){
		sync_clock();
		if((selected=earliest_due(now))<0){break;}
		struct os_timer timer=timers[selected];timers[selected].active=0;
		if(trace){fprintf(stderr,"os timer fire: slot %d callback %08x arg %x at +%llu ms\n",selected,timer.callback,timer.arg,(unsigned long long)(now-baseline)/1000);}
		((void (*)(uint32_t))(uintptr_t)timer.callback)(timer.arg);
	}
	return fired;
}

int isi_os_timer_live_selftest(void){
	isi_os_timer_attach();
	volatile unsigned char *state=(void *)(uintptr_t)(OS_TABLE+TEST_ID*28+0x12);
	((void (*)(uint32_t,uint32_t))OS_START)(ENERGY_ID,1);
	((void (*)(uint32_t,uint32_t))OS_START)(TEST_ID,1);
	uint64_t started=now;unsigned fired=0;
	while(now-started<100000 && *state!=3){poll(NULL,0,1);fired+=isi_os_timer_service();sync_clock();}
	unsigned energy_state=os_entry(ENERGY_ID)[18];
	if(!fired || *state!=3 || now-started<4000 || energy_state==2){fprintf(stderr,"os clock selftest: fired=%u state=%u elapsed=%llu energy_state=%u\n",fired,*state,(unsigned long long)(now-started),energy_state);return 1;}
	puts("native live os clock: linux ownership handoff and original real-time expiry passed");return 0;
}
