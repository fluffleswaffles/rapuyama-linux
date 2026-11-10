/* E5 port of upstream linux/isi-runtime/sim_runtime.c (GPL-2.0-only): E6 addresses replaced with
 * E5 fw 101.003 ones (docs/devices/e5-00/cellular-map.md). E5 SIM buffers: data at +4 after the
 * count (E6 +0x20); irq enable/disable = c63d8442/c63d8460. Not yet checked line by line. */
// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <poll.h>
#include "sim_bridge.h"
#include "sim_runtime.h"
#include "thumb_mmio.h"
#include "thumb_redirect.h"

static int device=-1;
static uint32_t handlers[6],enabled,sim_owner,sim_transaction,completion_event,fsm_event;
static int reset_mode,cycle_mode,embedded_mode;
static struct isi_sim_reg last_access;
static uint32_t last_pc,access_count;
static void timer_report(void);
void isi_sim_report(void){
	timer_report();
	fprintf(stderr,"SIM MMIO last: pc=%08x offset=%02x size=%u write=%u value=%08x count=%u\n",last_pc,last_access.offset,last_access.size,last_access.write,last_access.value,access_count);
}

static struct{
	uint32_t regs[12],pmic[2][32],pending,adc[19];
	unsigned char tx[64],rx[262];
	unsigned int tx_count,rx_count,rx_length,tx_length;
	int tx_event,rx_event;
	unsigned int voltage_count;uint32_t voltage_values[4];
} peer;
static _Noreturn void fatal(const char *what){fprintf(stderr,"SIM: %s, errno=%d\n",what,errno);_exit(122);}
int isi_sim_active(void){return device!=-1;}
static uint64_t now_us(void){
	struct timespec time;
	if(clock_gettime(CLOCK_MONOTONIC,&time)){fatal("clock");}
	return (uint64_t)time.tv_sec*1000000+time.tv_nsec/1000;
}

static uint64_t clock_epoch;
static uint32_t clock_base;

static void counter(uint64_t time){
	if(!clock_epoch){clock_epoch=time;clock_base=0;	/* E5: upstream reads its saved 32 kHz count at E6 c6826f78; no E5 twin found */}
	uint64_t elapsed=time-clock_epoch;
	*(volatile uint32_t *)0xc603334c=(uint32_t)(elapsed*384/10);
	*(volatile uint32_t *)0xc6033350=clock_base+(uint32_t)(elapsed*32768/1000000);
}
static void delay(uint32_t microseconds){
	if(microseconds>1000000){fatal("invalid delay");}
	uint64_t end=now_us()+microseconds;
	uint64_t time;
	do{time=now_us();counter(time);}while(time<end);
}
struct sim_timer{uint64_t deadline;uint32_t callback,period;};
static struct sim_timer timers[32];
static unsigned int timer_fires;
static void timer_report(void){
	fprintf(stderr,"SIM timers: fires=%u enabled=%02x now=%llu\n",timer_fires,enabled,(unsigned long long)now_us());
	for(unsigned int i=0;i<32;i++){
		if(timers[i].deadline){fprintf(stderr,"SIM timer %u: callback=%08x period=%u deadline=%llu\n",i,timers[i].callback,timers[i].period,(unsigned long long)timers[i].deadline);}
	}
}
static int firmware_code(uint32_t address){
	return (address>=0xc6300000 && address<0xc66ae000) || (address>=0xc60f0000 && address<0xc60f0db8);
}
static uint64_t sim_timer_create(uint32_t callback,uint32_t repeat,uint32_t microseconds){
	if(!firmware_code(callback) || repeat>1 || !microseconds || microseconds>1000000){fatal("invalid HAL timer");}
	for(unsigned int i=0;i<32;i++){
		if(!timers[i].deadline){
			timers[i]=(struct sim_timer){now_us()+microseconds,callback,repeat ? microseconds : 0};
			return (uint64_t)callback<<32|i;
		}
	}
	fatal("HAL timer pool exhausted");
}
static uint32_t sim_timer_cancel(uint32_t handle){
	handle&=255;
	if(handle>=32 || !timers[handle].deadline){fatal("inactive HAL timer");}
	timers[handle].deadline=0;return 0;
}
static uint32_t sim_timer_remaining(uint32_t handle){
	handle&=255;if(handle>=32){fatal("invalid HAL timer handle");}
	uint64_t now=now_us(),end=timers[handle].deadline;
	return end>now ? end-now : 0;
}
static void timer_service(void){
	uint64_t now=now_us();counter(now);
	for(unsigned int i=0;i<32;i++){
		struct sim_timer *timer=&timers[i];
		if(timer->deadline && timer->deadline<=now){
			void (*callback)(void)=(void *)(uintptr_t)timer->callback;
			timer->deadline=timer->period ? now+timer->period : 0;timer_fires++;callback();now=now_us();counter(now);
		}
	}
}

static unsigned int timer_wait(void){
	uint64_t now=now_us(),wait=100000;
	for(unsigned int i=0;i<32;i++){
		if(timers[i].deadline){
			uint64_t remaining=timers[i].deadline>now ? timers[i].deadline-now : 0;
			if(remaining<wait){wait=remaining;}
		}
	}
	return (wait+999)/1000;
}

static void owner_complete(uint32_t owner,uint32_t event,uint32_t unused){
	(void)unused;if(owner!=sim_owner){fatal("unexpected SIM owner");}completion_event=event;
}

static void transaction_complete(uint32_t transaction,uint32_t event,uint32_t unused){
	(void)unused;if(transaction!=sim_transaction){fatal("unexpected SIM transaction");}fsm_event=event;
}

int isi_sim_embedded(void){return embedded_mode;}
int isi_sim_fd(void){return device>=0 ? device : -1;}
unsigned int isi_sim_timeout(void){unsigned int wait=timer_wait();return wait<2 ? wait : 2;}
int isi_sim_open(const char *path,int embedded){
	embedded_mode=embedded;
	if(!path){
		device=-2;peer.regs[1]=0x0020a102;peer.regs[3]=0xae59;peer.regs[4]=6;peer.regs[5]=0x0f0f;
		peer.pmic[1][3]=0x45fd;peer.pmic[0][0x1c]=0x488;peer.pmic[0][0x1d]=2;peer.pmic[0][0x1e]=2;
		peer.rx[0]=0x90;peer.rx[1]=0;peer.rx_length=2;peer.tx_length=5;return 0;
	}
	device=open(path,O_RDWR|O_NONBLOCK|O_CLOEXEC);
	if(device<0){return -1;}
	if(embedded){
		struct isi_sim_adc probe={19,0};
		if(ioctl(device,ISI_SIM_ADC,&probe)!=-1 || errno!=EINVAL){close(device);device=-1;errno=EPROTONOSUPPORT;return -1;}
	}
	return 0;
}

int isi_sim_map(void){
	return mmap((void *)0xc6038000,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==MAP_FAILED ? -1 : 0;
}

static int reg_io(struct isi_sim_reg *request){
	if(device>=0){return ioctl(device,ISI_SIM_REG,request);}
	if(request->offset>=48 || request->size>4 || !request->size){errno=EINVAL;return -1;}
	if(request->write){
		if(request->offset==0x20){
			if(peer.tx_count==sizeof(peer.tx)){errno=EMSGSIZE;return -1;}
			peer.tx[peer.tx_count++]=request->value;
		}else if(request->offset==8){peer.regs[2]&=~request->value;}
		else{
			uint32_t mask=request->size==4 ? 0xffffffffU : (1U<<(request->size*8))-1;
			peer.regs[request->offset/4]=(peer.regs[request->offset/4]&~mask)|(request->value&mask);
		}
	}else if(request->offset==0x24){
		if(peer.rx_count==peer.rx_length){errno=EIO;return -1;}
		request->value=peer.rx[peer.rx_count++];
	}else{
		request->value=peer.regs[request->offset/4];
		if(request->offset==0x10){request->value=(request->value&~0x38)|2;}
	}
	return 0;
}

static int pmic_io(struct isi_sim_pmic *request){
	if(device>=0){return ioctl(device,ISI_SIM_PMIC,request);}
	/* embedded: reads are unrestricted, like the bridge (rapuyama_sim.c allowlists writes only) */
	if(!(embedded_mode && !request->write && request->device>=1 && request->device<=2 && request->reg<0x20) && !((request->device==2 && (request->reg==3 || ((reset_mode || embedded_mode) && request->reg==1))) || (request->device==1 && (request->reg==0x1c || request->reg==0x1d || (cycle_mode && request->reg==0x1e))))){errno=EPERM;return -1;}
	if(request->write){
		if(request->device==1 && request->reg==0x1e){
			if(peer.voltage_count==4){errno=EMSGSIZE;return -1;}
			peer.voltage_values[peer.voltage_count++]=request->value;
		}
		peer.pmic[request->device-1][request->reg]=request->value;
	}
	else{request->value=peer.pmic[request->device-1][request->reg];}
	return 0;
}

static uint32_t pmic_read(uint32_t dev,uint32_t reg,uint32_t bits){
	if(bits>15){fatal("invalid PMIC width");}
	struct isi_sim_pmic request={dev,reg,0,0};
	if(pmic_io(&request)){fprintf(stderr,"SIM PMIC read dev%u reg%02x caller=%p\n",dev,reg,__builtin_return_address(0));fatal("PMIC read");}
	return request.value&((1U<<(bits+1))-1);
}
static uint32_t pmic_write(uint32_t dev,uint32_t reg,uint32_t bits,uint32_t value){
	if(bits>15){fatal("invalid PMIC width");}
	if(embedded_mode && dev==1 && reg==0x17 && bits==15 && value==0x20){
		static int reported;
		if(!reported){fprintf(stderr,"PMIC watchdog feed delegated to Linux\n");reported=1;}
		return 1;
	}
	struct isi_sim_pmic request={dev,reg,value&((1U<<(bits+1))-1),1};
	if(pmic_io(&request)){fprintf(stderr,"SIM PMIC write dev%u reg%02x value%04x caller=%p\n",dev,reg,value,__builtin_return_address(0));fatal("PMIC write");}
	return 1;
}
static const unsigned char adc_channels[]={255,2,255,255,10,4,1,255,255,3,9,8,255,5,12,255,255,255,255,6,7,11,13,0,9,15,16,17,18};
static uint32_t adc_read(uint32_t channel){
	if(channel>=sizeof(adc_channels)){fatal("invalid firmware ADC channel");}
	unsigned hardware=adc_channels[channel];
	if(hardware==255){return 0;}
	struct isi_sim_adc request={hardware,0};
	if(device>=0){if(ioctl(device,ISI_SIM_ADC,&request)){fatal("Linux ADC read");}}
	else{request.value=peer.adc[hardware];}
	if(request.value>1023){fatal("invalid Linux ADC sample");}
	return request.value;
}


static int mdi_irq(uint32_t number){return embedded_mode && (number==19 || number==21 || number==23);}
static unsigned int irq_index(uint32_t number){if(number<0xb1 || number>0xb6){fprintf(stderr,"SIM IRQ number %u\n",number);fatal("invalid IRQ");}return number-0xb1;}
static uint32_t irq_register(uint32_t number,uint32_t handler){
	if(mdi_irq(number)){
		uint32_t expected=number==19 ? 0xc63c8f6d : number==21 ? 0xc63c9031 : 0xc63c8e0d;
		if((handler&~1U)!=(expected&~1U)){fatal("unexpected MDI handler");}
		return 0;
	}
	unsigned int index=irq_index(number);
	if(!((handler>=0xc6300000 && handler<0xc66ae000) || (handler>=0xc60f0000 && handler<0xc60f0db8))){fatal("invalid IRQ callback");}
	handlers[index]=handler;return 0;
}
static void irq_control(uint32_t number,int enable){

	if(mdi_irq(number)){return;}
	unsigned int index=irq_index(number);
	if(enable){enabled|=1U<<index;}else{enabled&=~(1U<<index);}
	if(device>=0){
		struct isi_sim_irq request={number,enable};
		if(ioctl(device,ISI_SIM_IRQ,&request)){fatal("IRQ control");}
	}else if(enable){
		if(number==0xb6 && peer.tx_count+1==peer.tx_length && !peer.tx_event){peer.pending|=1U<<index;peer.tx_event=1;}
		if(number==0xb4 && peer.tx_count==peer.tx_length && peer.rx_count<peer.rx_length){peer.pending|=1U<<index;peer.rx_event=1;}
	}
}
static void irq_enable(uint32_t number){irq_control(number,1);}
static void irq_disable(uint32_t number){irq_control(number,0);}

static uint32_t sim_irq_bind(uint32_t number,uint32_t handler,uint32_t priority){
	unsigned int index=irq_index(number);
	if(priority!=2){fatal("unsupported SIM IRQ priority");}
	if(!handler){irq_disable(number);handlers[index]=0;return 0;}
	return irq_register(number,handler);
}
static void patch(uint32_t address,uintptr_t target){isi_thumb_redirect((void *)address,target);}
void isi_sim_patches(void){
	const unsigned char irq_entry[]={0xf8,0xb5,0x04,0x46,0x0d,0x46,0xd4,0x4f};	/* E5: same code, own literal offset */
	if(memcmp((void *)0xc63ebf44,irq_entry,sizeof(irq_entry))){fatal("unsupported host IRQ wrapper");}
	if(memcmp((void *)(uintptr_t)*(uint32_t *)0xc63ec5e0,adc_channels,sizeof(adc_channels))){fatal("unsupported ADC channel map");}
	patch(0xc63ec534,(uintptr_t)adc_read);
	patch(0xc63ec6b0,(uintptr_t)pmic_read);patch(0xc63ec64a,(uintptr_t)pmic_write);
	patch(0xc63ebf44,(uintptr_t)sim_irq_bind);
	patch(0xc63d83e8,(uintptr_t)irq_register);patch(0xc63d8442,(uintptr_t)irq_enable);patch(0xc63d8460,(uintptr_t)irq_disable);	/* E5: enable writes INTC +0xc, disable +0x10 */
	patch(0xc63d7ace,(uintptr_t)delay);
	patch(0xc63d6ec0,(uintptr_t)sim_timer_create);patch(0xc63d6fba,(uintptr_t)sim_timer_cancel);patch(0xc63d7084,(uintptr_t)sim_timer_remaining);
	if(!embedded_mode){patch(0xc638bfa0,(uintptr_t)owner_complete);patch(0xc638c270,(uintptr_t)transaction_complete);}
}

int isi_sim_fault(siginfo_t *info,void *context){
	uint32_t address=(uint32_t)(uintptr_t)info->si_addr;
	if(!isi_sim_active() || address<0xc6038300 || address>=0xc6038330){return 0;}
	ucontext_t *uc=context;
	_Static_assert(offsetof(ucontext_t,uc_mcontext.arm_pc)-offsetof(ucontext_t,uc_mcontext.arm_r0)==15*4,"ARM signal register layout");
	uint32_t *regs=(uint32_t *)&uc->uc_mcontext.arm_r0,pc=regs[15];
	if(!(uc->uc_mcontext.arm_cpsr&32) || !((pc>=0xc6300000 && pc<0xc66adffe) || (pc>=0xc60f0000 && pc<0xc60f0db6))){return 0;}
	struct isi_mmio access;
	if(!isi_thumb_mmio(*(uint16_t *)pc,regs,&access) || access.address!=address){fatal("unsupported MMIO instruction");}
	struct isi_sim_reg request={access.address-0xc6038300,access.size,access.value,access.write};
	last_pc=pc;access_count++;last_access=request;
	if(reg_io(&request)){
		fprintf(stderr,"SIM MMIO: pc=%08x offset=%02x size=%u write=%u value=%08x\n",pc,request.offset,request.size,request.write,request.value);
		fatal("register transfer");
	}
	last_access=request;isi_thumb_mmio_finish(&access,regs,request.value);return 1;
}

static void hardware_snapshot(const char *stage){
	if(device<0){return;}
	fprintf(stderr,"SIM hardware %s:",stage);
	const uint32_t offsets[]={4,8,0xc,0x10,0x14,0x28};
	for(unsigned int i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++){
		struct isi_sim_reg request={offsets[i],offsets[i]==0x28 ? 2 : 4,0,0};
		if(reg_io(&request)){fatal("diagnostic register read");}
		fprintf(stderr," %02x=%08x",request.offset,request.value);
	}
	fprintf(stderr," pmic1.1c=%04x pmic1.1d=%04x pmic2.1=%04x pmic2.3=%04x enabled=%02x\n",pmic_read(1,0x1c,15),pmic_read(1,0x1d,15),pmic_read(2,1,15),pmic_read(2,3,15),enabled);
}
static int private_ram(uint32_t address,uint32_t size){return address>=0xc6800000 && address<=0xc6a80000-size;}
static uint32_t word(uint32_t address){return *(uint32_t *)address;}
static int gazoo_poll;

static struct{uint32_t us,what,fifo;}trace[96];
static unsigned int traced;
static void trace_event(uint32_t what){
	if(traced==sizeof(trace)/sizeof(trace[0])){return;}
	struct isi_sim_reg request={0x28,2,0,0};
	if(device>=0 && reg_io(&request)){fatal("trace FIFO read");}
	trace[traced++]=(typeof(trace[0])){(uint32_t)now_us(),what,request.value};
}

static void gazoo_service(void){
	uint32_t pending=pmic_read(2,1,15)&~pmic_read(2,3,15)&0x3800;
	for(unsigned int index=16;index<=18;index++){
		if(!(pending&(0x2000>>(index-16)))){continue;}
		trace_event(0x100|index);
		uint32_t handler=word(0xc683d280+index*4);
		if(!firmware_code(handler)){fatal("Gazoo SIM handler");}
		((void (*)(void))(uintptr_t)handler)();
	}
}

static int rx_batch_ready(uint32_t context){
	uint32_t batch=word(context+0x50),depth=word(context+0x74);
	if(!(enabled&(1U<<3)) || !batch || depth!=32 || batch>=depth){return 0;}
	struct isi_sim_reg request={0x28,2,0,0};
	if(reg_io(&request)){fatal("RX FIFO read");}
	uint32_t producer=request.value&255,consumer=(request.value>>8)&255;
	if(producer>=depth || consumer>=depth){return 0;}
	return ((producer-consumer)&(depth-1))>=batch;
}
static uint32_t embedded_context;
void isi_sim_attach(void){
	uint32_t pointer=word(0xc638d46c);
	if(!private_ram(pointer,4) || !private_ram(embedded_context=word(pointer),0x114)){fatal("embedded SIM context");}
	const uint32_t initial_handlers[]={0xc638d254,0xc638d250,0xc638d240,0xc638d244,0xc638d24c,0xc638d248};
	for(unsigned int i=0;i<6;i++){irq_register(0xb1+i,word(initial_handlers[i]));}

	if(device>=0 && word(embedded_context+0x30)==0){pmic_write(2,1,15,0x1000);}
	fprintf(stderr,"SIM backend attached: context=%08x state=%u ready=%u\n",embedded_context,word(embedded_context+0x30),word(embedded_context+0x78));
}

void isi_sim_service(void){
	if(!embedded_mode){return;}
	timer_service();
	gazoo_service();
	uint32_t pending=0;
	if(device>=0){
		ssize_t count=read(device,&pending,4);
		if(count<0 && errno!=EAGAIN && errno!=EINTR){fatal("embedded IRQ read");}
		if(count>=0 && count!=4){fatal("embedded IRQ size");}
		if(word(embedded_context+0x30)==1 && rx_batch_ready(embedded_context)){pending|=1U<<3;}
	}else{pending=peer.pending;peer.pending=0;}
	for(unsigned int i=0;i<6;i++){
		if((pending&enabled&(1U<<i)) && handlers[i]){
			((void (*)(void))(uintptr_t)handlers[i])();
			if(enabled&(1U<<i)){irq_control(0xb1+i,1);}
		}
	}
}

int isi_sim_backend_selftest(void){
	for(unsigned i=0;i<19;i++){peer.adc[i]=i*51;}
	for(unsigned i=0;i<sizeof(adc_channels);i++){
		uint32_t expected=adc_channels[i]==255 ? 0 : adc_channels[i]*51;
		if(((uint32_t (*)(uint32_t))0xc63ec535)(i)!=expected){fatal("Linux ADC channel mapping");}
	}
	peer.pmic[0][0x17]=0x30;
	if(pmic_write(1,0x17,15,0x20)!=1 || peer.pmic[0][0x17]!=0x30){fatal("Linux watchdog ownership");}
	uint32_t (*bind_irq)(uint32_t,uint32_t,uint32_t)=(void *)0xc63ebf45;
	const uint32_t numbers[]={0xb6,0xb4,0xb5},callbacks[]={0xc60f03d9,0xc60f0127,0xc60f048d};
	for(unsigned i=0;i<3;i++){
		unsigned index=numbers[i]-0xb1;uint32_t saved=handlers[index],was_enabled=enabled&(1U<<index);
		if(bind_irq(numbers[i],callbacks[i],2) || handlers[index]!=callbacks[i]){fatal("SIM host IRQ bind");}

		if(bind_irq(numbers[i],callbacks[i],2)){fatal("SIM host IRQ rebind");}
		irq_enable(numbers[i]);
		if(bind_irq(numbers[i],0,2) || handlers[index] || (enabled&(1U<<index))){fatal("SIM host IRQ unbind");}
		handlers[index]=saved;irq_control(numbers[i],!!was_enabled);
	}
	puts("SIM host IRQ selftest: original wrapper bind/rebind/unbind passed for three captured callbacks");

	/* E5: upstream replays an E6-captured DSP SIM packet here (E6 buffer 0x90c755f0, descriptor 0x001b55f0);
	 * no E5 capture yet, so only the wake MMIO check below */
	uint32_t before=access_count;
	((void (*)(uint32_t))0xc638cf17)(embedded_context);
	if(access_count!=before+2 || last_pc!=0xc638cf36 || last_access.offset!=4 || !last_access.write){fatal("embedded SIM wake MMIO coverage");}
	if(!(peer.regs[1]&1) || peer.tx_count){fprintf(stderr,"SIM backend selftest: missing clock request\n");return 1;}
	puts("SIM backend selftest: exact original SIM wake MMIO read/write passed with upper callbacks intact (no DSP packet replay on the E5)");return 0;
}
static void wait_transaction(uint32_t context,int reset){

	uint64_t end=now_us()+(device>=0 ? 3000000 : 10000000);
	while(word(context+0x30) && now_us()<end){
		timer_service();
		if(gazoo_poll){gazoo_service();}
		uint32_t pending=0;
		if(device>=0){
			struct pollfd descriptor={.fd=device,.events=POLLIN};
			unsigned int wait=timer_wait();
			int n=poll(&descriptor,1,gazoo_poll && wait>2 ? 2 : wait);
			if(n<0){if(errno==EINTR){continue;}fatal("poll");}
			if(!n){
				if(!reset && gazoo_poll && rx_batch_ready(context)){pending=1U<<3;trace_event(0x400|pending);}
				else{continue;}
			}else if(read(device,&pending,4)!=4){if(errno==EAGAIN || errno==EINTR){continue;}fatal("event read");}
		}else{
			if(reset && timer_fires>=2 && peer.rx_count<peer.rx_length && (enabled&(1U<<3))){peer.pending|=1U<<3;}
			pending=peer.pending;peer.pending=0;
			if(!pending){
				if(!reset){fatal("selftest made no progress");}
				delay(1000);continue;
			}
		}
		if(gazoo_poll){trace_event(pending);}
		for(unsigned int i=0;i<6;i++){
			if((pending&enabled&(1U<<i)) && handlers[i]){
				void (*handler)(void)=(void *)(uintptr_t)handlers[i];handler();
				if(enabled&(1U<<i)){irq_control(0xb1+i,1);}
			}
		}
	}
}

static int exchange(uint32_t context,uint32_t tx,uint32_t rx,const unsigned char *command,unsigned int length,unsigned int expected){
	const uint32_t initial_handlers[]={0xc638d254,0xc638d250,0xc638d240,0xc638d244,0xc638d24c,0xc638d248};
	if(device==-2){peer.tx_count=0;peer.rx_count=0;peer.pending=0;peer.tx_event=0;peer.tx_length=length;}
	memcpy((void *)(tx+4),command,length);*(uint32_t *)tx=length;*(uint32_t *)rx=0;
	*(unsigned char *)(sim_transaction+0x1c)=8;*(uint32_t *)(context+0x30)=1;
	completion_event=0;fsm_event=0;
	*(uint32_t *)(context+0x40)=length;*(uint32_t *)(context+0x44)=expected;*(uint32_t *)(context+0x4c)=expected==1 ? 1 : 2;
	for(unsigned int i=0;i<6;i++){irq_register(0xb1+i,word(initial_handlers[i]));}
	void (*wake)(uint32_t)=(void *)0xc638cf17,(*start)(uint32_t)=(void *)0xc638cd1f;

	if(device>=0){pmic_write(2,1,15,0x1000);gazoo_poll=1;}traced=0;trace_event(0x200);
	alarm(5);wake(context);delay(15000);start(context);wait_transaction(context,0);alarm(0);gazoo_poll=0;
	uint32_t count=word(rx);
	if(word(context+0x30) || count<1 || count>259){
		hardware_snapshot("incomplete");isi_sim_report();
		for(unsigned int i=0;i<traced;i++){fprintf(stderr,"SIM trace +%u us %03x fifo %04x\n",trace[i].us-trace[0].us,trace[i].what,trace[i].fifo);}
		fprintf(stderr,"SIM rx bytes %u completion %x fsm %x\n",count,completion_event,fsm_event);
		return -1;
	}
	return count;
}

static int apdu_selftest(uint32_t context,uint32_t tx,uint32_t rx){
	const unsigned char commands[][5]={{0x80,0xf2,0,0,0},{0x80,0xf2,0,0,0x27},{0,0xa4,0,4,2},{0x3f,0},{0,0xa4,0,4,2},{0x2f,0xe2},{0,0xb0,0,0,10}};
	const unsigned int lengths[]={5,5,5,2,5,2,5},counts[]={2,42,1,2,1,2,13};
	for(unsigned int i=0;i<7;i++){
		memset(peer.rx,0,sizeof(peer.rx));peer.rx_length=counts[i];
		if(i==0){peer.rx[0]=0x6c;peer.rx[1]=0x27;}
		else if(i==2 || i==4){peer.rx[0]=0xa4;}
		else{if(i==1 || i==6){peer.rx[0]=commands[i][1];}peer.rx[counts[i]-2]=0x90;}
		unsigned char expected[262];memcpy(expected,peer.rx,counts[i]);
		int count=exchange(context,tx,rx,commands[i],lengths[i],counts[i]);
		if(count!=(int)counts[i] || peer.tx_count!=lengths[i] || memcmp(peer.tx,commands[i],lengths[i]) || memcmp((void *)(rx+4),expected,counts[i]) || peer.rx_count!=counts[i]){return 1;}
	}
	puts("SIM APDU selftest: seven consecutive T=0 phases, INS acknowledgement and short body passed");return 0;
}
static int apdus(uint32_t context,uint32_t tx,uint32_t rx,const char *list){
	unsigned char command[261];
	while(*list){
		unsigned int length=0;
		while(*list==' ' || *list==','){list++;}
		while(*list && *list!=' ' && *list!=','){
			unsigned int byte;
			if(length==sizeof(command) || sscanf(list,"%2x",&byte)!=1){fatal("bad SIM_APDU");}
			command[length++]=byte;list+=2;
		}
		if(!length){break;}
		if(length<5){fatal("short APDU");}
		for(unsigned int tries=0;tries<3;tries++){
			int outgoing=length>5;
			if(outgoing && length!=5U+command[4]){fatal("unsupported APDU length");}
			unsigned int expected=outgoing ? 1 : (command[4] ? command[4] : 256U)+3;
			int count=exchange(context,tx,rx,command,outgoing ? 5 : length,expected);
			if(outgoing && count==1){
				if(*(unsigned char *)(rx+4)!=command[1]){fatal("unsupported T=0 procedure byte");}
				count=exchange(context,tx,rx,command+5,length-5,2);
			}
			if(count<2){fprintf(stderr,"SIM APDU incomplete: state=%u\n",word(context+0x30));return 1;}
			const unsigned char *reply=(const unsigned char *)(rx+4);
			printf("SIM live APDU");
			for(unsigned int i=0;i<length;i++){printf(" %02x",command[i]);}
			printf(" ->");
			for(int i=0;i<count;i++){printf(" %02x",reply[i]);}
			puts("");
			unsigned char sw1=reply[count-2],sw2=reply[count-1];

			if(count==2 && sw1==0x6c && length==5){command[4]=sw2;continue;}
			if(count==2 && sw1==0x61){unsigned char cla=command[0];length=5;memcpy(command,(unsigned char[]){cla,0xc0,0,0,sw2},5);continue;}
			break;
		}
	}
	return 0;
}
int isi_sim_run(int reset,int cycle){
	reset_mode=reset;cycle_mode=cycle;
	if(cycle && !reset){fatal("power cycle requires reset");}
	uint32_t pointer=word(0xc638d46c);
	if(!private_ram(pointer,4)){fatal("SIM context pointer");}
	uint32_t context=word(pointer);
	if(!private_ram(context,0x114) || word(context+0x78)!=1){fatal("SIM is not ready in the snapshot");}
	uint32_t tx=word(context+4),rx=word(context+8);
	if(!private_ram(tx,0x126) || !private_ram(rx,0x126)){fatal("SIM buffers");}
	uint32_t owner=word(context),transaction;
	if(!private_ram(owner,0x10) || !private_ram(transaction=word(owner+0xc),0x20)){fatal("SIM transaction metadata");}
	sim_owner=owner;sim_transaction=transaction;

	*(unsigned char *)(transaction+0x1c)=8;*(uint32_t *)(context+0x30)=1;
	const unsigned char command[]={0x80,0xf2,0,0,0};
	memcpy((void *)(tx+4),command,sizeof(command));*(uint32_t *)tx=sizeof(command);
	*(uint32_t *)(context+0x40)=sizeof(command);*(uint32_t *)(context+0x44)=259;*(uint32_t *)(context+0x4c)=2;
	const uint32_t initial_handlers[]={0xc638d254,0xc638d250,0xc638d240,0xc638d244,0xc638d24c,0xc638d248};
	for(unsigned int i=0;i<6;i++){irq_register(0xb1+i,word(initial_handlers[i]));}
	void (*wake)(uint32_t)=(void *)0xc638cf17,(*start)(uint32_t)=(void *)0xc638cd1f;
	alarm(device>=0 ? 5 : 15);
	if(cycle){
		uint32_t baseline=pmic_read(1,0x1e,15),selection=(baseline&6)>>1;
		if(selection!=1 && selection!=2){fatal("unexpected SIM voltage baseline");}
		uint32_t setter_address=word(context+0xb8);
		if(!firmware_code(setter_address)){fatal("invalid SIM voltage setter");}
		void (*voltage)(uint32_t,uint32_t)=(void *)(uintptr_t)setter_address;
		voltage(context,2);
		if(pmic_read(1,0x1e,15)&6){fatal("SIM voltage off verification");}
		delay(20000);voltage(context,selection==1 ? 1 : 0);
		if((pmic_read(1,0x1e,15)&6)!=(baseline&6)){fatal("SIM voltage restore verification");}
		delay(20000);fprintf(stderr,"SIM voltage selection restored: %04x\n",baseline&6);
	}
	
	if(reset){
		const unsigned char atr[]={0x3b,0x05,'H','E','L','L','O'};
		if(device==-2){memcpy(peer.rx,atr,sizeof(atr));peer.rx_length=sizeof(atr);}
		void (*cold_reset)(uint32_t)=(void *)0xc638d06b;cold_reset(context);
	}else{wake(context);delay(15000);start(context);}
	hardware_snapshot("started");
	wait_transaction(context,reset);
	alarm(0);
	uint32_t count=word(rx);
	if(word(context+0x30) || count<2 || count>262){hardware_snapshot("incomplete");isi_sim_report();fprintf(stderr,"SIM transaction incomplete: state=%u bytes=%u\n",word(context+0x30),count);return 1;}
	printf("SIM %s %s:",device>=0 ? "live" : "selftest",reset ? "ATR" : "STATUS");
	for(unsigned int i=0;i<count;i++){printf(" %02x",*(unsigned char *)(rx+4+i));}puts("");
	if(reset && device==-2){
		if(peer.tx_count || count!=peer.rx_length || memcmp((void *)(rx+4),peer.rx,count) || completion_event!=1 || fsm_event!=11 || timer_fires!=2){return 1;}
		for(unsigned int i=0;i<32;i++){if(timers[i].deadline){return 1;}}
		puts("SIM reset selftest: two periodic callbacks, ATR completion and timer cancellation passed");
		if(cycle){
			if(peer.voltage_count!=2 || peer.voltage_values[0]!=0 || peer.voltage_values[1]!=2 || peer.pmic[0][0x1e]!=2){return 1;}
			puts("SIM cycle selftest: original voltage setter off/restore sequence passed");
		}
	}else if(!reset && device==-2 && (peer.tx_count!=5 || memcmp(peer.tx,command,5) || count!=2 || memcmp((void *)(rx+4),peer.rx,2))){return 1;}
	if(reset && device>=0 && (count>33 || (*(unsigned char *)(rx+4)!=0x3b && *(unsigned char *)(rx+4)!=0x3f) || completion_event!=1)){return 1;}
	if(device==-2 && !reset && apdu_selftest(context,tx,rx)){return 1;}
	const char *list=getenv("SIM_APDU");
	if(!reset && device>=0 && list && apdus(context,tx,rx,list)){return 1;}
	if(device>=0){close(device);}
	device=-1;return 0;
}