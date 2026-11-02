// SPDX-License-Identifier: GPL-2.0-only
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include "thumb_redirect.h"
#include "os_timer_runtime.h"

struct os_timer{uint64_t deadline,serial;uint32_t callback,arg;int active;};
static struct os_timer timers[279];
static uint64_t now,baseline,sequence;
static int live_clock;
static void sync_clock(void);
static _Noreturn void invalid(const char *reason){fprintf(stderr,"os timer: %s\n",reason);_exit(123);}

static uint32_t remaining(uint32_t handle){
	sync_clock();
	handle&=65535;
	if(handle>=279){invalid("invalid handle");}
	return timers[handle].active && timers[handle].deadline>now ? timers[handle].deadline-now : 0;
}

static uint64_t create(uint32_t delay,uint32_t callback,uint32_t arg){
	sync_clock();
	if(callback<0xc6200000 || callback>=0xc660c000){invalid("invalid callback");}
	for(unsigned i=0;i<279;i++){
		if(!timers[i].active){
			timers[i]=(struct os_timer){now+delay,++sequence,callback,arg,1};
			return (uint64_t)delay<<32|i;
		}
	}
	invalid("pool exhausted");
}

static uint32_t cancel(uint32_t handle){
	handle&=65535;
	if(handle>=279 || !timers[handle].active){invalid("cancel inactive handle");}
	uint32_t value=remaining(handle);timers[handle].active=0;return value;
}

static uint32_t next_remaining(void){
	sync_clock();
	uint64_t earliest=UINT64_MAX;
	for(unsigned i=0;i<279;i++){if(timers[i].active && timers[i].deadline<earliest){earliest=timers[i].deadline;}}
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

static unsigned advance(uint64_t delta){
	uint64_t target=now+delta;unsigned fired=0;
	for(;;){
		int selected=-1;
		for(unsigned i=0;i<279;i++){
			if(!timers[i].active || timers[i].deadline>target){continue;}
			if(selected<0 || timers[i].deadline<timers[selected].deadline || (timers[i].deadline==timers[selected].deadline && timers[i].serial<timers[selected].serial)){selected=i;}
		}
		if(selected<0){break;}
		if(++fired>10000){invalid("callback budget exceeded");}
		struct os_timer timer=timers[selected];timers[selected].active=0;
		now=timer.deadline;counter();((void (*)(uint32_t))(uintptr_t)timer.callback)(timer.arg);
	}
	now=target;counter();return fired;

}

static unsigned adopt(void){
	uint32_t base=*(uint32_t *)0xc62ca794;
	if(base<0xc6800008 || base>0xc6c4d000-279*20){invalid("copied timer pool range");}
	const uint16_t *meta=(void *)(uintptr_t)(base-8);
	unsigned count=meta[0],head=meta[2];uint64_t ticks=0;unsigned char seen[279]={0};
	if(count>279){invalid("copied timer count");}
	for(unsigned n=0;n<count;n++){
		if(head>=279 || seen[head]){invalid("copied timer chain");}
		seen[head]=1;
		const uint32_t *entry=(void *)(uintptr_t)(base+head*20);
		if(entry[1]==UINT32_MAX || entry[2]<0xc6200000 || entry[2]>=0xc660c000){invalid("copied timer entry");}
		ticks+=entry[1];
		timers[head]=(struct os_timer){now+(ticks*1000000+8191)/8192+entry[0],++sequence,entry[2],entry[3],1};
		head=*(const uint16_t *)(entry+4);
	}
	return count;
}

void isi_os_timer_patches(void){
	const uintptr_t entries[][2]={
		{0xc62ca416,(uintptr_t)create},{0xc62ca4e0,(uintptr_t)cancel},
		{0xc62ca642,(uintptr_t)remaining},{0xc62ca922,(uintptr_t)next_remaining},
		{0xc62c13dc,(uintptr_t)elapsed},{0xc62c2868,(uintptr_t)rebase},
	};
	for(unsigned i=0;i<sizeof(entries)/sizeof(entries[0]);i++){isi_thumb_redirect((void *)entries[i][0],entries[i][1]);}
}

int isi_os_timer_selftest(void){
	void (*start)(uint32_t,uint32_t)=(void *)0xc62c1673;
	uint32_t (*stop)(uint32_t)=(void *)0xc62c18b7;
	uint32_t base=*(uint32_t *)0xc62c162c;
	if(base<0xc6800000 || base>0xc6c4d000-279*28){return 1;}
	volatile unsigned char *state=(void *)(uintptr_t)(base+62*28+0x12);
	for(unsigned ticks=1;ticks<=100;ticks++){
		start(62,ticks);
		if(next_remaining()!=ticks*5000 || advance(ticks*5000-1) || advance(1)!=1 || *state!=3 || next_remaining()){return 1;}
	}
	for(unsigned ticks=1;ticks<=100;ticks++){
		start(62,ticks);advance(1);
		if(stop(62)>ticks || advance(ticks*5000) || next_remaining()){return 1;}
		start(62,ticks+1);
		if(advance((ticks+1)*5000)!=1 || *state!=3){return 1;}
	}
	uint64_t (*arm)(uint32_t,uint32_t,uint32_t)=(void *)0xc62ca417;
	uint32_t (*left)(uint32_t)=(void *)0xc62ca643,(*remove)(uint32_t)=(void *)0xc62ca4e1;
	uint32_t handles[279];
	for(unsigned i=0;i<279;i++){
		uint64_t result=arm(1000000+i,0xc62bb821,0);
		handles[i]=(uint32_t)result;
		if(result>>32!=1000000+i || handles[i]>=279 || left(handles[i])!=1000000+i){return 1;}
		for(unsigned j=0;j<i;j++){if(handles[j]==handles[i]){return 1;}}
	}
	for(unsigned i=0;i<279;i++){if(remove(handles[i])!=1000000+i){return 1;}}
	if(next_remaining() || advance(2000000)){return 1;}
	rebase();advance(0x100000000ULL+12345);
	if(elapsed()!=(0x100000000ULL+12345)/5000){return 1;}
	unsigned adopted=adopt(),active=0;
	for(unsigned i=0;i<279;i++){active+=timers[i].active!=0;}
	if(!adopted || active!=adopted){return 1;}
	uint64_t result=arm(10,0xc62bb821,0);unsigned handle=(uint32_t)result;
	if(remove(handle)!=10){return 1;}
	active=0;for(unsigned i=0;i<279;i++){active+=timers[i].active!=0;}
	if(active!=adopted){return 1;}
	printf("native os timers: 100 expiries, 100 cancel/rearms, EABI, 279 slots, long clock, adopted %u pending timers passed\n",adopted);return 0;
}

static int sms_frames(unsigned wanted){
	void *(*receive)(void)=(void *)0xc62be89d;void (*release)(void *)=(void *)0xc62c8579;
	unsigned char *queue=(void *)0xc681c11c;int matched=0;
	for(unsigned i=0;queue[16]!=queue[17] && i<64;i++){
		unsigned char *p=receive();
		if((uintptr_t)p<0xc6800000 || (uintptr_t)p>0xc6c4d000-18){invalid("SMS output pointer");}
		unsigned size=((unsigned)p[4]|(unsigned)p[5]<<8)+6;
		if(size<10 || (uintptr_t)p+size>0xc6c4d000){invalid("SMS output size");}
		if(size>=12 && p[3]==2 && p[6]==0xa1 && p[9]==0x22 && p[10]==wanted){matched=1;}
		release(p);
	}
	if(queue[16]!=queue[17]){invalid("SMS queue budget");}return matched;
}

static void sms_request(uint8_t resource,uint8_t tid,const uint8_t *body,unsigned length){
	void *(*allocate)(unsigned)=(void *)0xc62c8027;void (*send)(void *)=(void *)0xc6252ba7;
	unsigned char *p=allocate(length+9);if(!p){invalid("SMS packet allocation");}
	unsigned char header[]={0x15,0,0x6c,resource,(length+3)&255,(length+3)>>8,0,0xa1,tid};
	memcpy(p,header,sizeof(header));memcpy(p+9,body,length);send(p);((void (*)(void))0xc62bc1c7)();
}

int isi_os_sms_timeout_selftest(void){
	adopt();
	const uint8_t subscribe[]={0x12,1,0,0,0,0,2},activate[]={0x41,1,0};
	const uint8_t submit[]={0x02,0x00,0x00,0x00,0x00,0x00,0x03,0x00,0x31,0x00,0x08,0x00,0x02,0x00,0x00,0x00,0x1c,0x00,0x20,0x18,0x00,0x21,0x00,0x08,0x91,0x51,0x55,0x10,0x00,0x00,0x00,0x0e,0xf4,0x74,0xbb,0x2c,0x07,0xcd,0xcb,0x6c,0x33,0xbd,0x3c,0xa7,0x03,0x00,0x00,0x00,0x82,0x00,0x0c,0x02,0x06,0x05,0x91,0x51,0x55,0x10,0x10};
	sms_request(0x10,0,subscribe,sizeof(subscribe));sms_frames(255);
	sms_request(2,0x40,activate,sizeof(activate));sms_frames(255);
	sms_request(2,0x42,submit,sizeof(submit));if(!sms_frames(2)){return 1;}
	int selected=-1;
	for(unsigned i=0;i<279;i++){
		if(timers[i].active && timers[i].callback==0xc62c1199 && timers[i].arg==144){
			if(selected>=0 || remaining(i)!=60000000){return 1;}selected=i;
		}
	}
	if(selected<0){return 1;}
	for(unsigned i=0;i<279;i++){if(i!=(unsigned)selected){timers[i].active=0;}}
	if(advance(59999999) || advance(1)!=1){return 1;}
	((void (*)(void))0xc62bc1c7)();
	if(!sms_frames(1)){return 1;}
	unsigned rearmed=0;
	for(unsigned i=0;i<279;i++){rearmed+=timers[i].active && timers[i].arg==144 && remaining(i)==60000000;}
	if(rearmed!=1){return 1;}
	puts("native SMS timeout: original 60s expiry, client indication and network-wait rearm passed; no radio peer");return 0;
}

void isi_os_timer_attach(void){
	for(unsigned i=0;i<279;i++){if(timers[i].active){invalid("attach with active adapter timers");}}
	live_clock=1;sync_clock();baseline=now;unsigned copied=adopt();
	uint32_t base=*(uint32_t *)0xc62c162c;
	if(base<0xc6800000 || base>0xc6c4d000-279*28){invalid("os timer table range");}
	uint32_t scheduler=*(uint32_t *)0xc62bc81c;
	if(scheduler<0xc6800000 || scheduler>0xc6c4d000-0x2c){invalid("scheduler range");}
	uint32_t config=*(uint32_t *)(uintptr_t)(scheduler+0x28);
	if(config<0xc6200000 || config>0xc660c000-49*16 || *(uint32_t *)(uintptr_t)(config+29*16)!=0xc621eadb || *(uint32_t *)(uintptr_t)(config+27*16)!=0xc62cc8e1 || *(uint32_t *)(uintptr_t)(config+38*16)!=0xc62bb371){invalid("unsupported task ownership");}
	uint32_t (*stop)(uint32_t)=(void *)0xc62c18b7;unsigned energy=0,watchdogs=0;
	for(unsigned i=0;i<279;i++){
		const unsigned char *entry=(void *)(uintptr_t)(base+i*28);
		if(entry[18]!=2){continue;}
		if(entry[16]==29){stop(i);energy++;}
		else if((i==212 && entry[16]==27) || (i==266 && entry[16]==38)){stop(i);watchdogs++;}
	}
	printf("os timers attached: copied=%u linux energy=%u watchdogs=%u\n",copied,energy,watchdogs);
}

unsigned int isi_os_timer_service(void){
	if(!live_clock){return 0;}

	uint32_t base=*(uint32_t *)0xc62c162c;
	for(unsigned i=0;i<279;i++){
		const unsigned char *entry=(void *)(uintptr_t)(base+i*28);
		if(entry[16]==29 && entry[18]==2){((uint32_t (*)(uint32_t))0xc62c18b7)(i);}
	}
	unsigned fired=0;
	for(;fired<64;fired++){
		sync_clock();int selected=-1;
		for(unsigned i=0;i<279;i++){
			if(!timers[i].active || timers[i].deadline>now){continue;}
			if(selected<0 || timers[i].deadline<timers[selected].deadline || (timers[i].deadline==timers[selected].deadline && timers[i].serial<timers[selected].serial)){selected=i;}
		}
		if(selected<0){break;}
		struct os_timer timer=timers[selected];timers[selected].active=0;
		((void (*)(uint32_t))(uintptr_t)timer.callback)(timer.arg);
	}
	return fired;
}

int isi_os_timer_live_selftest(void){
	isi_os_timer_attach();
	uint32_t base=*(uint32_t *)0xc62c162c;
	volatile unsigned char *state=(void *)(uintptr_t)(base+62*28+0x12);
	((void (*)(uint32_t,uint32_t))0xc62c1673)(223,1);
	((void (*)(uint32_t,uint32_t))0xc62c1673)(62,1);
	uint64_t started=now;unsigned fired=0;
	while(now-started<100000 && *state!=3){poll(NULL,0,1);fired+=isi_os_timer_service();sync_clock();}
	if(!fired || *state!=3 || now-started<4000 || *(unsigned char *)(uintptr_t)(base+223*28+0x12)==2){fprintf(stderr,"os clock selftest: fired=%u state=%u elapsed=%llu energy_state=%u\n",fired,*state,(unsigned long long)(now-started),*(unsigned char *)(uintptr_t)(base+223*28+0x12));return 1;}
	puts("native live os clock: linux ownership handoff and original real-time expiry passed");return 0;
}
