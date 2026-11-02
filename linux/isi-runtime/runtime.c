// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <grp.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include "sim_runtime.h"
#include "kasumi_runtime.h"
#include "trace_runtime.h"
#include "os_timer_runtime.h"
#include "identity_runtime.h"
#include "fault_diagnostics.h"
static int os_timer_test,os_timer_live;

/* linux/rapuyama_mdi.h */
struct rapuyama_mdi_status{
	uint32_t version,ap,dsp,rx_interrupts,tx_interrupts,event_interrupts;
};
#define RAPUYAMA_MDI_STATUS _IOR('M',0,struct rapuyama_mdi_status)
#define RAPUYAMA_MDI_BUFFERS _IOW('M',1,uint32_t)

static void sandbox(void){
	struct rlimit memory={48*1024*1024,48*1024*1024},core={0,0};
	if(setrlimit(RLIMIT_AS,&memory) || setrlimit(RLIMIT_CORE,&core)){perror("rlimit");exit(1);}
	if(geteuid()==0 && (setgroups(0,NULL) || setgid(65534) || setuid(65534))){perror("drop privileges");exit(1);}
	if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)){perror("no_new_privs");exit(1);}
#define ALLOW(number) BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,number,0,1),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)
	struct sock_filter filter[]={
		BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch)),
		BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,AUDIT_ARCH_ARM,1,0),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
		BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
		ALLOW(SYS_read),ALLOW(SYS_write),ALLOW(SYS_writev),ALLOW(SYS_close),ALLOW(SYS_exit),ALLOW(SYS_exit_group),ALLOW(SYS_rt_sigreturn),ALLOW(SYS_restart_syscall),ALLOW(SYS_setitimer),ALLOW(SYS_ioctl),ALLOW(SYS_poll),ALLOW(SYS_ppoll),
#ifdef SYS_clock_gettime
		ALLOW(SYS_clock_gettime),
#endif
#ifdef SYS_clock_gettime32
		ALLOW(SYS_clock_gettime32),
#endif
#ifdef SYS_clock_gettime64
		ALLOW(SYS_clock_gettime64),
#endif
		BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
	};
	struct sock_fprog program={.len=sizeof(filter)/sizeof(filter[0]),.filter=filter};
	if(prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&program)){perror("seccomp");exit(1);}
#undef ALLOW
}

static unsigned char signal_stack[32768] __attribute__((aligned(16)));
static void failure(int sig,siginfo_t *info,void *context){
	if(sig==SIGSEGV && isi_sim_fault(info,context)){return;}
	ucontext_t *uc=context;
	if(isi_sim_active()){isi_sim_report();}
	fprintf(stderr,"signal %d at %p, pc=%08lx sp=%08lx\n",sig,info->si_addr,uc->uc_mcontext.arm_pc,uc->uc_mcontext.arm_sp);
	fprintf(stderr,"registers: r0=%08lx r1=%08lx r2=%08lx r3=%08lx lr=%08lx\n",uc->uc_mcontext.arm_r0,uc->uc_mcontext.arm_r1,uc->uc_mcontext.arm_r2,uc->uc_mcontext.arm_r3,uc->uc_mcontext.arm_lr);
	struct isi_secure_fault secure;
	if(isi_secure_fault_decode(uc->uc_mcontext.arm_pc,uc->uc_mcontext.arm_sp,uc->uc_mcontext.arm_r6,&secure)){
		fprintf(stderr,"secure address fault: caller=%08x module=%s%04x\n",secure.caller,secure.has_module ? "" : "unknown/",secure.module);
	}
	_exit(128+sig);
}
static void region(uint32_t address,size_t length){
	if(mmap((void *)address,length,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==MAP_FAILED){perror("mmap");exit(1);}
}
static void firmware_assert(uint32_t expression,uint32_t file,uint32_t line){
	fprintf(stderr,"firmware assertion: expr=%08x file=%08x line=%u\n",expression,file,line);
	if(expression>=0xc6200000 && expression<0xc660bf00 && file>=0xc6200000 && file<0xc660bf00){fprintf(stderr,"%.192s: %u: %.192s\n",(char *)file,line,(char *)expression);}
	_exit(120);
}
static void firmware_reset(uint32_t reason){
	fprintf(stderr,"firmware reset requested: %08x\n",reason);
	_exit(121);
}
static void traps(void){
	const uint32_t assertion[]={0xe51ff004,(uint32_t)(uintptr_t)firmware_assert};
	const uint16_t reset[]={0x4b00,0x4718};
	uint32_t handler=(uint32_t)(uintptr_t)firmware_reset;
	memcpy((void *)0xc65a7070,assertion,sizeof(assertion));
	memcpy((void *)0xc62da64c,reset,sizeof(reset));
	memcpy((void *)0xc62da650,&handler,sizeof(handler));
	isi_kasumi_patches();
	if(os_timer_test || os_timer_live){isi_os_timer_patches();}
}
static void image(const char *dir,uint32_t address,size_t size,int executable){
	char path[512];snprintf(path,sizeof(path),"%s/%08x.bin",dir,address);
	int fd=open(path,O_RDONLY);if(fd<0){perror(path);exit(1);}
	struct stat metadata;
	if(fstat(fd,&metadata) || metadata.st_size!=(off_t)size){fprintf(stderr,"invalid image size: %s\n",path);exit(1);}
	region(address,size);
	size_t off=0;ssize_t n;
	while((n=read(fd,(void *)(address+off),size-off))>0){off+=n;}
	if(n<0){perror("read");exit(1);}
	if(off!=size){fprintf(stderr,"incomplete image: %s\n",path);exit(1);}
	close(fd);
	if(executable){if(address==0xc6200000){traps();if(isi_sim_active()){isi_sim_patches();}if(isi_identity_attach()){fprintf(stderr,"unsupported identity reader\n");exit(2);}}__builtin___clear_cache((char *)address,(char *)(address+size));if(mprotect((void *)address,size,PROT_READ|PROT_EXEC)){perror("mprotect");exit(1);}}
}

static void ramdrives_private(void){
	const uint32_t expected[]={0x90d00000,0x200,0x7e000,0,0x90d7e000,0x200,0x7e000,1};
	if(memcmp((void *)0xc680aa7c,expected,sizeof(expected))){fprintf(stderr,"unsupported SBIF RAM drives\n");exit(1);}
	const size_t size=0xfc000;
	void *copy=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
	if(copy==MAP_FAILED){perror("RAM drive copy");exit(1);}
	memcpy(copy,(void *)0x90d00000,size);
	if(munmap((void *)0x90d00000,size)){perror("RAM drive unmap");exit(1);}
	region(0x90d00000,size);
	memcpy((void *)0x90d00000,copy,size);
	if(munmap(copy,size)){perror("RAM drive copy unmap");exit(1);}
	puts("SBIF RAM drives9/10: private snapshot, physical backing remains read-only");
}

#include "sbif_runtime.h"

static const unsigned char *ramdrive_backing;
static void ramdrive_test_setup(void){
	const size_t size=0xfc000;
	int fd=memfd_create("ramdrive-test",MFD_CLOEXEC);
	if(fd<0 || ftruncate(fd,size)){perror("RAM drive test backing");exit(1);}
	void *fill=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
	if(fill==MAP_FAILED){perror("RAM drive test fill");exit(1);}
	memset(fill,0x5a,size);
	if(munmap(fill,size)){perror("RAM drive test fill unmap");exit(1);}
	ramdrive_backing=mmap(NULL,size,PROT_READ,MAP_PRIVATE,fd,0);
	if(ramdrive_backing==MAP_FAILED || mmap((void *)0x90d00000,size,PROT_READ,MAP_PRIVATE|MAP_FIXED_NOREPLACE,fd,0)==MAP_FAILED){perror("RAM drive test map");exit(1);}
	close(fd);ramdrives_private();
}

static int ramdrive_selftest(void){
	sbif_attach();
	unsigned char sent[512],reply[512];
	sbif_test_source=sent;
	void (*write_sector)(unsigned,unsigned,unsigned,void *)=(void *)0xc623aca3;
	void (*read_sector)(unsigned,unsigned,unsigned,void *)=(void *)0xc623ac69;
	for(unsigned drive=9;drive<=10;drive++){
		for(unsigned i=0;i<sizeof(sent);i++){sent[i]=i+drive;}
		write_sector(drive,2,1,sent);sbif_service();read_sector(drive,2,1,reply);
		if(memcmp(sent,reply,sizeof(sent))){fprintf(stderr,"RAM drive sector readback failed\n");return 1;}
	}
	for(unsigned i=0;i<0xfc000;i++){
		unsigned drive=i<0x7e000 ? 9 : 10,offset=i%0x7e000;
		unsigned char expected=offset>=1024 && offset<1536 ? offset-1024+drive : 0x5a;
		if(ramdrive_backing[i]!=0x5a || ((unsigned char *)0x90d00000)[i]!=expected || sbif_store[i]!=expected){fprintf(stderr,"RAM drive backing, consumer or neighbour changed\n");return 1;}
	}

	for(unsigned sector=4;sector<68;sector++){
		for(unsigned drive=9;drive<=10;drive++){
			for(unsigned i=0;i<sizeof(sent);i++){sent[i]=i+drive+sector;}
			write_sector(drive,sector,1,sent);sbif_service();read_sector(drive,sector,1,reply);
			if(memcmp(sent,reply,512) || memcmp(sbif_store+(drive-9)*0x7e000+sector*512,sent,512)){return 1;}
		}
	}
	uint32_t control=*(const uint32_t *)0xc623ae8c,queue=*(const uint32_t *)(uintptr_t)(control+8);
	if(sbif_writes<130 || *(const uint32_t *)(uintptr_t)queue){return 1;}
	for(unsigned i=0;i<0xfc000;i++){if(ramdrive_backing[i]!=0x5a){return 1;}}
	sbif_test_source=NULL;
	puts("PASS: original SBIF130 writes/readbacks and consumer drain on both private drives; queue empty, physical backing unchanged");return 0;
}

static void dsp_windows(void){
	const uint32_t windows[][3]={{0x90900000,0x101ff000,0x261000},{0x90d00000,0x11400000,0xfc000},{0x90e00000,0x115a0000,0x52000}};
	int fd=open("/dev/mem",O_RDONLY|O_SYNC|O_CLOEXEC);
	if(fd<0){perror("DSP memory");exit(1);}

	for(unsigned i=0;i<sizeof(windows)/sizeof(windows[0]);i++){
		if(mmap((void *)windows[i][0],windows[i][2],PROT_READ,MAP_PRIVATE|MAP_FIXED_NOREPLACE,fd,windows[i][1])==MAP_FAILED){perror("DSP window");exit(1);}
	}
	close(fd);
	ramdrives_private();
}

static int kasumi_selftest(void){
	/* ETSI TS135203 5.3  */
	unsigned char key[]={0x2b,0xd6,0x45,0x9f,0x82,0xc5,0xb3,0,0x95,0x2c,0x49,0x10,0x48,0x81,0xff,0x48};
	unsigned char message[]={0x6b,0x22,0x77,0x37,0x29,0x6f,0x39,0x3c,0x80,0x79,0x35,0x3e,0xdc,0x87,0xe2,0xe8,5,0xd2,0xec,0x49,0xa4,0xf2,0xd8,0xe0};
	unsigned char result[8]={0,0,0,0,0xa5,0xa5,0xa5,0xa5};
	const unsigned char expected[]={0xf6,0x3b,0xd7,0x2c,0xa5,0xa5,0xa5,0xa5};
	void (*integrity)(void *,unsigned,void *,uint32_t,uint32_t,void *,unsigned)=(void *)0xc62f221d;
	integrity(message,189,key,0x38a6f056,0x05d2ec49,result,0);
	if(memcmp(result,expected,sizeof(result))){fprintf(stderr,"KASUMI software f9 mismatch\n");return 1;}
	puts("original software KASUMI f9: ETSI189-bit MAC and output bounds passed");return 0;
}

static int call_selftest(void){
	unsigned char message[]={0x15,0,0x6c,0xc9,0,0,0,0xa1,0x42,1,0,2,7,4,1,0,3,20,1,0,0,7,0,'5',0,'5',0,'5',0,'0',0,'1',0,'0',0,'0'};
	message[4]=sizeof(message)-6;
	void *(*allocate)(unsigned)=(void *)0xc62c8027;
	void (*send_packet)(void *)=(void *)0xc6252ba7,(*schedule)(void)=(void *)0xc62bc1c7;
	unsigned char *(*receive)(void)=(void *)0xc62be89d;
	void (*release)(void *)=(void *)0xc62c8579;
	*(volatile uint32_t *)0xc601bce0=0;*(volatile uint32_t *)0xc601bdf0=0;
	void *packet=allocate(sizeof(message));if(!packet){return 1;}
	memcpy(packet,message,sizeof(message));send_packet(packet);
	int accepted=0;
	for(unsigned int i=0;i<100;i++){
		schedule();
		unsigned char *queue=(void *)0xc681c11c;
		while(queue[16]!=queue[17]){
			unsigned char *reply=receive();
			if(reply[3]==0xc9 && reply[8]==0x42 && reply[9]==2 && reply[10]==1 && reply[11]==0){accepted=1;}
			release(reply);
		}
	}
	if(!accepted){fprintf(stderr,"call selftest: no call-create acceptance\n");return 1;}
	puts("native call selftest: original server accepted call1 without entering Symbian; simulated peripherals only");return 0;
}

static int dsp_selftest(void){
	volatile uint32_t *ap=(void *)0xc601bce0,*dsp=(void *)0xc601bdf0;
	unsigned char *buffer=(void *)0x90c01000;
	const unsigned char message[]={0x11,0x22,0x33,0x44,0x12,0x6c,0x7c,0x15,0,10,0xa1,0x1e,0x33,0xf0,0x13,18,2,0,0,0};
	const unsigned char expected[]={0x15,0x6c,0x7c,0x15,10,0,0xa1,0x1e,0x33,0xf0,0x13,18,2,0,0,0};
	void (*interrupt)(void)=(void *)0xc62cb6db;
	void (*schedule)(void)=(void *)0xc62bc1c7;
	unsigned char *(*receive)(void)=(void *)0xc62be89d;
	void (*release)(void *)=(void *)0xc62c8579;
	*ap=0;*dsp=0;
	for(unsigned transaction=0;transaction<100;transaction++){
		unsigned char *current=buffer+transaction*256;
		uint32_t tag=0x11223344+transaction;
		memcpy(current,message,sizeof(message));
		current[0]=tag>>24;current[1]=tag>>16;current[2]=tag>>8;current[3]=tag;current[12]=transaction;
		unsigned producer=transaction&31;
		*dsp=((transaction+1)&31)|((*ap&31)<<16);
		volatile uint32_t *slot=dsp+1+(producer&15)*4;
		slot[0]=tag;slot[1]=0x1ea10a00;slot[2]=0x1600f000|transaction;slot[3]=0x141000+transaction*256;
		interrupt();schedule();
		unsigned char *queue=(void *)0xc681c11c;
		if(queue[16]==queue[17] || queue[28]!=1){fprintf(stderr,"DSP reply queue invalid\n");return 1;}
		unsigned char *reply=receive();
		unsigned char answer[sizeof(expected)];memcpy(answer,expected,sizeof(answer));answer[8]=transaction;
		if((uintptr_t)reply<0xc6800000 || (uintptr_t)reply+sizeof(answer)>0xc6c4d000 || memcmp(reply,answer,sizeof(answer))){fprintf(stderr,"DSP packet mismatch\n");return 1;}
		release(reply);
		if(queue[28]){fprintf(stderr,"DSP queue not empty\n");return 1;}
	}
	puts("native ARM firmware: 100 synthetic DSP deliveries, wraps and frees passed");return 0;
}

static int dsp_tx_selftest(void){
	volatile uint32_t *ap=(void *)0xc601bce0,*dsp=(void *)0xc601bdf0;
	void *(*allocate)(unsigned)=(void *)0xc62c8027;
	void (*send_packet)(void *)=(void *)0xc6252ba7;
	void (*interrupt)(void)=(void *)0xc62cb6db;
	void (*schedule)(void)=(void *)0xc62bc1c7;
	unsigned char *first=NULL;
	*ap=0;*dsp=0;
	for(unsigned transaction=0;transaction<100;transaction++){
		unsigned char message[]={0x15,0x7c,0x6c,0x15,10,0,0,0xa1,transaction,0xf0,0x12,0,0,0,0,0};
		unsigned producer=*ap&31;
		void *p=allocate(sizeof(message));if(!p){fprintf(stderr,"allocator exhausted\n");return 1;}
		memcpy(p,message,sizeof(message));send_packet(p);
		if((*ap&31)!=((producer+1)&31)){fprintf(stderr,"no MDI publish\n");return 1;}
		volatile uint32_t *sent=ap+1+(producer&15)*4;
		/* type 06 descriptors carry AP physical minus 0x10000000 */
		unsigned char *packet=(void *)(sent[3]+0xb6f50000);
		if(sent[2]>>24!=6 || (uintptr_t)packet<0xc6800000 || (uintptr_t)packet+sizeof(message)>0xc6c4d000){fprintf(stderr,"invalid MDI descriptor\n");return 1;}
		message[0]=0x12;message[4]=0;message[5]=10;
		if(memcmp(packet,message,sizeof(message))){fprintf(stderr,"MDI packet mismatch\n");return 1;}
		unsigned consumer=*dsp&31;
		*dsp=((consumer+1)&31)|((*ap&31)<<16);
		volatile uint32_t *slot=dsp+1+(consumer&15)*4;
		slot[0]=sent[3];slot[1]=0;slot[2]=0x19020001;slot[3]=0;
		interrupt();schedule();
		/* a freed packet is handed out again by the next allocation */
		if(!first){first=packet;}else if(packet!=first){fprintf(stderr,"AP packet not freed\n");return 1;}
	}
	puts("native ARM firmware: 100 DSP publishes and acknowledgement frees passed");return 0;
}

static int mdi=-1;
static unsigned forwarded;
static int mdi_verbose;
static uint32_t seen_tx,seen_event;
static void trace_descriptor(unsigned kind,const uint32_t descriptor[4]){
	uint32_t type=descriptor[2]>>24,size=0;const void *payload=NULL;
	uint64_t physical=(uint64_t)descriptor[3]+0x10000000;
	if(kind==1 && (type&0x1e)==0x16){
		size=__builtin_bswap16(descriptor[1]&0xffff)+10;
		if(physical<0x10140000 || physical+size>0x101ff000){return;}
		payload=(void *)(uintptr_t)(0x90c00000+physical-0x10140000);
	}else if(kind==2 && type==6){
		size=__builtin_bswap16(descriptor[1]&0xffff)+6;
		if(physical<0x1f8b0000 || physical+size>0x1fcfd000){return;}
		payload=(void *)(uintptr_t)(0xc6800000+physical-0x1f8b0000);
	}
	isi_trace_record(kind,descriptor,payload,size);
}
static int mdi_forward(void){
	volatile uint32_t *ap=(void *)0xc601bce0;
	while(forwarded!=(*ap&31)){
		volatile uint32_t *slot=ap+1+(forwarded&15)*4;
		uint32_t descriptor[4]={slot[0],slot[1],slot[2],slot[3]};

		if(descriptor[2]>>24==6 && descriptor[3]+0x10000000-0x1f8b0000-0xf8000<0x6000){fprintf(stderr,"mdi publish into the PA_SWD stack blocked: %08x %08x %08x %08x\n",descriptor[0],descriptor[1],descriptor[2],descriptor[3]);return 1;}

		if(descriptor[2]==0x19020007 && (!descriptor[0] || !descriptor[1] || !descriptor[3])){fprintf(stderr,"mdi dealloc with an empty tag blocked: %08x %08x %08x %08x\n",descriptor[0],descriptor[1],descriptor[2],descriptor[3]);return 1;}
		ssize_t n=write(mdi,descriptor,sizeof(descriptor));
		if(n<0 && errno==EAGAIN){return 0;}
		if(n!=sizeof(descriptor)){fprintf(stderr,"mdi publish failed: %d\n",errno);return 1;}
		trace_descriptor(2,descriptor);
		if(mdi_verbose){printf("mdi tx %08x %08x %08x %08x\n",descriptor[0],descriptor[1],descriptor[2],descriptor[3]);}
		forwarded=(forwarded+1)&31;
	}
	return 0;
}
static void firmware_services(void){
	sbif_service();
	if(isi_sim_embedded()){isi_sim_service();}
	if(os_timer_live){isi_os_timer_service();}
	((void (*)(void))0xc62bc1c7)();
}
static int mdi_pump(void){
	volatile uint32_t *dsp=(void *)0xc601bdf0;
	void (*interrupt)(void)=(void *)0xc62cb6db,(*consumed)(void)=(void *)0xc62cb7cf,(*event)(void)=(void *)0xc62cb561;
	void (*schedule)(void)=(void *)0xc62bc1c7;
	struct rapuyama_mdi_status status;
	uint32_t descriptor[4];
	ssize_t n;
	if(ioctl(mdi,RAPUYAMA_MDI_STATUS,&status)){fprintf(stderr,"mdi status failed: %d\n",errno);return 1;}

	*dsp=(*dsp&0xffff)|(status.dsp&0xffff0000);
	if(status.tx_interrupts!=seen_tx){seen_tx=status.tx_interrupts;consumed();schedule();}
	if(status.event_interrupts!=seen_event){seen_event=status.event_interrupts;printf("mdi event %u\n",seen_event);event();schedule();}
	while((n=read(mdi,descriptor,sizeof(descriptor)))==sizeof(descriptor)){
		trace_descriptor(1,descriptor);
		if(mdi_verbose || descriptor[0]==0x55555555){printf("mdi rx %08x %08x %08x %08x\n",descriptor[0],descriptor[1],descriptor[2],descriptor[3]);}
		unsigned producer=*dsp&31;
		volatile uint32_t *slot=dsp+1+(producer&15)*4;
		slot[0]=descriptor[0];slot[1]=descriptor[1];slot[2]=descriptor[2];slot[3]=descriptor[3];
		*dsp=(*dsp&0xffff0000)|((producer+1)&31);
		interrupt();schedule();
		if(isi_sim_embedded() || os_timer_live || sbif_attached){firmware_services();}
		if(mdi_forward()){return 1;}
	}
	if(n>=0 || errno!=EAGAIN){fprintf(stderr,"mdi receive failed: %d\n",errno);return 1;}
	return mdi_forward();
}

static int drain(int backend){
	void *(*receive_packet)(void)=(void *)0xc62be89d;
	void (*free_packet)(void *)=(void *)0xc62c8579;
	for(unsigned i=0;i<64;i++){
		unsigned char *q=(void *)0xc681c11c;
		if(q[16]==q[17]){return 0;}
		unsigned char *reply=receive_packet();
		if((uintptr_t)reply<0xc6800000 || (uintptr_t)reply>0xc6c4cff0){return 1;}
		unsigned size=((unsigned)reply[4]|(unsigned)reply[5]<<8)+6;
		if(size<10 || size>65541 || (uintptr_t)reply+size>0xc6c4d000){return 1;}
		if(backend>=0){if(write(backend,reply,size)!=(ssize_t)size){return 1;}}
		else{printf("host rx");for(unsigned j=0;j<size && j<64;j++){printf(" %02x",reply[j]);}puts("");}
		free_packet(reply);
	}
	return 0;
}

static int live(int backend,int probe_dev,int probe_res){
	void *(*allocate)(unsigned)=(void *)0xc62c8027;
	void (*send_packet)(void *)=(void *)0xc6252ba7;
	static unsigned char frame[65541];
	if(probe_dev>=0){
		unsigned char message[]={0x15,probe_dev,0x6c,probe_res,6,0,0,0xa1,1,0xf0,0x12,0};
		void *p=allocate(sizeof(message));if(!p){return 1;}
		memcpy(p,message,sizeof(message));alarm(2);send_packet(p);alarm(0);
	}
	for(unsigned round=0;probe_dev<0 || round<100;round++){
		struct pollfd wait[3]={{.fd=mdi,.events=POLLIN|POLLPRI},{.fd=backend,.events=POLLIN},{.fd=isi_sim_fd(),.events=POLLIN}};
		if(poll(wait,isi_sim_embedded() ? 3 : backend>=0 ? 2 : 1,isi_sim_embedded() ? isi_sim_timeout() : 100)<0 && errno!=EINTR){return 1;}
		alarm(2);
		if(isi_sim_embedded() || os_timer_live || sbif_attached){firmware_services();}
		if(mdi_pump()){return 1;}
		if(backend>=0 && (wait[1].revents&POLLIN)){
			ssize_t n=read(backend,frame,sizeof(frame));
			if(n<0 && errno!=EAGAIN && errno!=EINTR){return 1;}
			if(!n){return 0;}
			if(n>0){
				if(n<10 || ((unsigned)frame[4]|(unsigned)frame[5]<<8)+6!=(unsigned)n){return 1;}
				isi_trace_record(3,NULL,frame,n);
				void *p=allocate(n);if(!p){return 1;}
				memcpy(p,frame,n);send_packet(p);
				if(mdi_pump()){return 1;}
			}
		}
		if(drain(backend)){return 1;}
		alarm(0);
	}
	struct rapuyama_mdi_status status;
	if(ioctl(mdi,RAPUYAMA_MDI_STATUS,&status)){return 1;}
	printf("mdi status ap=%08x dsp=%08x rx=%u tx=%u event=%u\n",status.ap,status.dsp,status.rx_interrupts,status.tx_interrupts,status.event_interrupts);
	return 0;
}

int main(int argc,char **argv){
	if(argc==2 && !strcmp(argv[1],"--fault-diagnostic-selftest")){
		region(0xc65b8000,0x1000);region(0xc6800000,0x1000);
		const unsigned char title[]={0xab,0x0f,0xbd,0x96,0xaf,0x2a,0xc2,0x71,0xd2,0x62,0x64,0xaf,0,0,0x1b,0};
		memcpy((void *)0xc65b8400,title,sizeof(title));
		uint32_t *stack=(void *)0xc6800100;stack[3]=0xc6261a51;stack[7]=0xc62628bd;
		struct isi_secure_fault decoded;
		if(!isi_secure_fault_decode(0xc62ef9fc,(uintptr_t)stack,0xc65b8400,&decoded) || decoded.caller!=stack[7] || !decoded.has_module || decoded.module!=0x1b){return 1;}
		if(isi_secure_fault_decode(0xc62ef9fc,0,0,&decoded)){return 1;}
		puts("PASS bounded secure-fault caller/module diagnostics; synthetic private memory only");return 0;
	}
	os_timer_test=argc==3 && !strcmp(argv[2],"--os-timer-selftest");
	if(argc==3 && !strcmp(argv[2],"--sms-timeout-selftest")){os_timer_test=2;}
	if(argc==3 && !strcmp(argv[2],"--os-timer-live-selftest")){os_timer_test=3;}
	int ramdrive_test=argc==3 && !strcmp(argv[2],"--ramdrive-selftest");
	int identity_test=argc==3 && !strcmp(argv[2],"--identity-selftest");
	int sim_backend_test=argc==3 && !strcmp(argv[2],"--sim-backend-selftest");
	const char *sim_backend=getenv("ISI_SIM_DEVICE");
	int call_test=argc==3 && !strcmp(argv[2],"--call-selftest");
	int sim_cycle_test=argc==3 && !strcmp(argv[2],"--sim-cycle-selftest");
	int sim_cycle_live=argc==4 && !strcmp(argv[2],"--sim-cycle");
	int sim_cycle=sim_cycle_test || sim_cycle_live;
	int sim_reset_test=argc==3 && (!strcmp(argv[2],"--sim-reset-selftest") || sim_cycle_test);
	int sim_reset_live=argc==4 && (!strcmp(argv[2],"--sim-reset") || sim_cycle_live);
	int sim_reset=sim_reset_test || sim_reset_live;
	int sim_test=argc==3 && !strcmp(argv[2],"--sim-selftest"),sim_live=argc==4 && (!strcmp(argv[2],"--sim-probe") || sim_reset_live);
	int synthetic=argc==3 && !strcmp(argv[2],"--dsp-selftest"),transmit=argc==3 && !strcmp(argv[2],"--dsp-tx-selftest");
	if(argc!=2 && !identity_test && !ramdrive_test && !os_timer_test && !sim_backend_test && !call_test && !synthetic && !transmit && !sim_test && !sim_live && !sim_reset && !(argc>=4 && !strcmp(argv[2],"--mdi")) && !(argc==4 && !strcmp(argv[2],"--backend"))){fprintf(stderr,"usage: rapuyama-isi-runtime <private-images-dir> [--backend <packet-device> | --dsp-selftest | --dsp-tx-selftest]\n");return 2;}
	setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
	if(isi_identity_load(getenv("ISI_DEVICE_IMEI_FILE"))){fprintf(stderr,"invalid factory identity file\n");return 2;}
	if((sim_test || sim_live || sim_reset) && isi_sim_open((sim_test || sim_reset_test) ? NULL : argv[3],0)){perror("SIM bridge");return 1;}
	int backend=-1,probe_dev=-1,probe_res=0;
	int link=argc>=4 && !strcmp(argv[2],"--mdi");
	const char *os_clock=getenv("ISI_OS_TIMERS");
	os_timer_live=link && os_clock && !strcmp(os_clock,"1");
	if(link){
		const char *verbose=getenv("ISI_MDI_VERBOSE");mdi_verbose=verbose && !strcmp(verbose,"1");
		if(isi_trace_open(getenv("ISI_TRACE"))){perror("ISI trace");return 1;}
		if(argc==6 && !strcmp(argv[4],"--backend")){backend=open(argv[5],O_RDWR|O_NONBLOCK|O_CLOEXEC);if(backend<0){perror(argv[5]);return 1;}}
		else if(argc==6 && !strcmp(argv[4],"--probe") && sscanf(argv[5],"%x:%x",&probe_dev,&probe_res)==2 && probe_dev>0 && probe_dev<256 && probe_res>=0 && probe_res<256){}
		else if(argc!=4){fprintf(stderr,"usage: rapuyama-isi-runtime <private-images-dir> --mdi <mdi-device> [--backend <packet-device> | --probe <dev>:<resource>]\n");return 2;}
		mdi=open(argv[3],O_RDWR|O_NONBLOCK|O_CLOEXEC);if(mdi<0){perror(argv[3]);return 1;}
	}
	else if(argc==4 && !sim_live){backend=open(argv[3],O_RDWR|O_CLOEXEC);if(backend<0){perror(argv[3]);return 1;}}
	if(sim_backend_test || (link && sim_backend && *sim_backend)){if(isi_sim_open(sim_backend_test ? NULL : sim_backend,1)){perror("SIM backend");return 1;}}
	if(os_timer_live && !isi_sim_embedded()){fprintf(stderr,"OS live timers require the embedded SIM backend\n");return 2;}
	sigset_t empty;sigemptyset(&empty);if(sigprocmask(SIG_SETMASK,&empty,NULL)){perror("signal mask");return 1;}
	struct sigaction defaults={.sa_handler=SIG_DFL};sigemptyset(&defaults.sa_mask);sigaction(SIGALRM,&defaults,NULL);sigaction(SIGTERM,&defaults,NULL);
	stack_t alternate={.ss_sp=signal_stack,.ss_size=sizeof(signal_stack)};
	if(sigaltstack(&alternate,NULL)){perror("signal stack");return 1;}
	struct sigaction sa={.sa_sigaction=failure,.sa_flags=SA_SIGINFO|SA_ONSTACK};sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV,&sa,NULL);sigaction(SIGILL,&sa,NULL);sigaction(SIGBUS,&sa,NULL);if(isi_sim_active()){sigaction(SIGALRM,&sa,NULL);}alarm(15);
	image(argv[1],0xc6200000,0x40c000,1);image(argv[1],0xc6800000,0x44d000,0);image(argv[1],0xc6700000,0x8000,0);
	region(0xc6033000,0x1000);region(0xc6060000,0x1000);region(0xc6f00000,0x200000);
	if(isi_sim_active()){image(argv[1],0xc60f0000,0x1000,1);if(isi_sim_map()){perror("SIM register page");return 1;}}
	if(os_timer_test || synthetic || transmit || call_test || sim_backend_test || link){region(0xc6018000,0x4000);region(0xc6040000,0x1000);region(0x90c00000,0xbf000);}
	if(identity_test){region(0x30000000,0x1000);}
	void *(*allocate)(unsigned)=(void *)0xc62c8027;
	void (*send_packet)(void *)=(void *)0xc6252ba7;
	void *(*receive_packet)(void)=(void *)0xc62be89d;
	void (*free_packet)(void *)=(void *)0xc62c8579;
	if(link){
		dsp_windows();
		struct rapuyama_mdi_status status;
		uint32_t buffers=1;
		if(ioctl(mdi,RAPUYAMA_MDI_BUFFERS,&buffers) || ioctl(mdi,RAPUYAMA_MDI_STATUS,&status) || status.version!=2){perror("mdi setup");return 1;}
		*(volatile uint32_t *)0xc601bce0=status.ap;*(volatile uint32_t *)0xc601bdf0=(status.dsp&0xffff0000)|(status.ap>>16);
		forwarded=status.ap&31;seen_tx=status.tx_interrupts;seen_event=status.event_interrupts;
		uint32_t initial[]={status.ap,status.dsp,status.tx_interrupts,status.event_interrupts};
		isi_trace_record(4,initial,NULL,0);
		printf("mdi status ap=%08x dsp=%08x\n",status.ap,status.dsp);
	}
	if(ramdrive_test){ramdrive_test_setup();}
	if(isi_sim_embedded()){isi_sim_attach();}
	if(os_timer_live){isi_os_timer_attach();}
	if(link){sbif_attach();}
	sandbox();
	if(identity_test){return isi_identity_selftest();}
	if(ramdrive_test){return ramdrive_selftest();}
	if(os_timer_test){return os_timer_test==3 ? isi_os_timer_live_selftest() : os_timer_test==2 ? isi_os_sms_timeout_selftest() : isi_os_timer_selftest();}
	if(sim_backend_test){if(isi_kasumi_selftest()){return 1;}if(kasumi_selftest()){return 1;}if(call_selftest()){return 1;}return isi_sim_backend_selftest();}
	if(isi_sim_active() && !isi_sim_embedded()){return isi_sim_run(sim_reset,sim_cycle);}
	if(link){return live(backend,probe_dev,probe_res);}
	if(call_test){return call_selftest();}
	if(synthetic){return dsp_selftest();}
	if(transmit){return dsp_tx_selftest();}
	if(backend>=0){
		static unsigned char frame[65541];
		for(;;){
			alarm(0);
			ssize_t n=read(backend,frame,sizeof(frame));
			if(n<0 && errno==EINTR){continue;}
			if(n<=0){return n<0 ? 1 : 0;}
			if(n<10 || ((unsigned)frame[4]|(unsigned)frame[5]<<8)+6!=(unsigned)n){return 1;}
			alarm(2);
			void *p=allocate(n);if(!p){return 1;}
			memcpy(p,frame,n);send_packet(p);
			for(unsigned i=0;i<64;i++){
				unsigned char *q=(void *)0xc681c11c;
				if(q[16]==q[17]){break;}
				unsigned char *reply=receive_packet();
				if((uintptr_t)reply<0xc6800000 || (uintptr_t)reply>0xc6c4cff0){return 1;}
				unsigned size=((unsigned)reply[4]|(unsigned)reply[5]<<8)+6;
				if(size<10 || size>sizeof(frame) || (uintptr_t)reply+size>0xc6c4d000){return 1;}
				if(write(backend,reply,size)!=(ssize_t)size){return 1;}
				free_packet(reply);
			}
		}
	}
	for(unsigned transaction=1;transaction<=100;transaction++){
		unsigned char message[]={0x15,0,0x6c,0x15,6,0,0,0xa1,transaction,0xf0,0x12,0};
		void *p=allocate(sizeof(message));if(!p){fprintf(stderr,"allocator exhausted\n");return 1;}
		memcpy(p,message,sizeof(message));send_packet(p);
		unsigned char *q=(void *)0xc681c11c;
		if(q[16]==q[17] || q[28]!=1){fprintf(stderr,"reply queue invalid: %u/%u/%u\n",q[16],q[17],q[28]);return 1;}
		unsigned char *reply=receive_packet();
		if(reply[8]!=(transaction&255) || reply[9]!=0xf0 || reply[10]!=0x13 || reply[11]!=18 || reply[12]!=2){fprintf(stderr,"invalid response\n");return 1;}
		free_packet(reply);
		if(q[28]){fprintf(stderr,"queue not empty\n");return 1;}
	}
	puts("native ARM firmware: 100 version request/reply/free cycles passed");return 0;
}
