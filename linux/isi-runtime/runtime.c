// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
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
#include "devlock_runtime.h"
#include "simlock_runtime.h"
#include "gprs_diag.h"
#include "fault_diagnostics.h"
#include "address_space.h"
#include "dsp_payload_runtime.h"
#include "harq_verify_runtime.h"
#include "harq_state_runtime.h"
#include "harq_pdu_runtime.h"
static int harq_firmware_read(void *opaque,uint32_t address,void *out,size_t size);
#include "harq_pack_hooks.h"
#include "../kernel/linux/include/uapi/linux/rapuyama_mdi.h"
static int os_timer_test,os_timer_live,secure_required;
static int gprs_diag_enabled,pdu_watch_enabled;
static struct isi_harq_verify_state harq_verify;
static int harq_state_enabled;
static int harq_pdu_enabled;
static int harq_pack_enabled;
static struct isi_harq_feedback harq_feedback[256];

static int gprs_diag_configure(void){
	const char *verify=getenv("ISI_HARQ_VERIFY");
	if(verify && *verify && strcmp(verify,"0") && strcmp(verify,"1")){fprintf(stderr,"ISI_HARQ_VERIFY must be 0 or 1\n");return -1;}
	harq_verify.enabled=verify && !strcmp(verify,"1");
	const char *state=getenv("ISI_HARQ_STATE");
	if(state && *state && strcmp(state,"0") && strcmp(state,"1")){fprintf(stderr,"ISI_HARQ_STATE must be 0 or 1\n");return -1;}
	harq_state_enabled=state && !strcmp(state,"1");
	if(harq_state_enabled && !harq_verify.enabled){fprintf(stderr,"ISI_HARQ_STATE requires ISI_HARQ_VERIFY=1\n");return -1;}
	const char *pdu=getenv("ISI_HARQ_PDU");
	if(pdu && *pdu && strcmp(pdu,"0") && strcmp(pdu,"1")){fprintf(stderr,"ISI_HARQ_PDU must be 0 or 1\n");return -1;}
	harq_pdu_enabled=pdu && !strcmp(pdu,"1");
	if(harq_pdu_enabled && !harq_state_enabled){fprintf(stderr,"ISI_HARQ_PDU requires ISI_HARQ_STATE=1\n");return -1;}
	const char *pack=getenv("ISI_HARQ_PACK");
	if(pack && *pack && strcmp(pack,"0") && strcmp(pack,"1")){fprintf(stderr,"ISI_HARQ_PACK must be 0 or 1\n");return -1;}
	harq_pack_enabled=pack && !strcmp(pack,"1");
	if(harq_pack_enabled && !harq_pdu_enabled){fprintf(stderr,"ISI_HARQ_PACK requires ISI_HARQ_PDU=1\n");return -1;}
	const char *value=getenv("ISI_GPRS_DIAG");
	if(value && *value && strcmp(value,"0") && strcmp(value,"1")){fprintf(stderr,"ISI_GPRS_DIAG must be 0 or 1\n");return -1;}
	gprs_diag_enabled=value && !strcmp(value,"1");
	const char *watch=getenv("ISI_PDU_WATCH");
	if(watch && *watch && strcmp(watch,"0") && strcmp(watch,"1")){fprintf(stderr,"ISI_PDU_WATCH must be 0 or 1\n");return -1;}
	pdu_watch_enabled=watch && !strcmp(watch,"1");
	if(pdu_watch_enabled && !gprs_diag_enabled){fprintf(stderr,"ISI_PDU_WATCH requires ISI_GPRS_DIAG=1\n");return -1;}
	return 0;
}

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
		// ISI_PDU_WATCH: mprotect, read-only or read-write only DO NOT EXECUTE
		BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,pdu_watch_enabled ? SYS_mprotect : ~0u,0,5),
		BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,args[2])),
		BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,PROT_READ,1,0),
		BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,PROT_READ|PROT_WRITE,0,1),
		BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
		BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
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
	if(gprs_diag_enabled && isi_gprs_diag_fault(sig,context)){return;}
	if(sig==SIGILL && isi_kasumi_fault(context)){return;}
	if(sig==SIGSEGV && info->si_code==SEGV_ACCERR && isi_kasumi_watch_fault((uintptr_t)info->si_addr,context)){return;}
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
static void failure_handlers(void){
	stack_t alternate={.ss_sp=signal_stack,.ss_size=sizeof(signal_stack)};
	if(sigaltstack(&alternate,NULL)){perror("signal stack");exit(1);}
	struct sigaction sa={.sa_sigaction=failure,.sa_flags=SA_SIGINFO|SA_ONSTACK};sigemptyset(&sa.sa_mask);
	if(sigaction(SIGSEGV,&sa,NULL) || sigaction(SIGBUS,&sa,NULL)){perror("fault handler");exit(1);}
	if(isi_sim_active() && sigaction(SIGALRM,&sa,NULL)){perror("timer handler");exit(1);}
	if(gprs_diag_enabled){sigfillset(&sa.sa_mask);}
	if(sigaction(SIGILL,&sa,NULL)){perror("illegal-instruction handler");exit(1);}
}
static void region(uint32_t address,size_t length){
	isi_window_release(address,length);
	if(mmap((void *)address,length,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==MAP_FAILED){
		int error=errno;
		fprintf(stderr,"mmap %08x+%zx: %s\n",address,length,strerror(error));
		// show what is in the way, only possible before the sandbox
		FILE *maps=fopen("/proc/self/maps","r");
		if(maps){char line[160];while(fgets(line,sizeof(line),maps)){fputs(line,stderr);}fclose(maps);}
		exit(1);
	}
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
	if(harq_pack_enabled && isi_harq_pack_hooks_install()){fprintf(stderr,"unsupported HARQ packing/allocation capture sites\n");exit(2);}
	if(os_timer_test || os_timer_live){isi_os_timer_patches();}
	if(isi_gprs_diag_attach(gprs_diag_enabled)){fprintf(stderr,"unsupported GPDS diagnostic sites\n");exit(2);}
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
	if(executable){if(address==0xc6200000){traps();if(isi_sim_active()){isi_sim_patches();}if(isi_identity_attach(secure_required)){fprintf(stderr,"unsupported identity reader\n");exit(2);}if(isi_devlock_attach(secure_required)){fprintf(stderr,"unsupported device-lock reader\n");exit(2);}if(isi_simlock_attach(secure_required)){fprintf(stderr,"unsupported SIM-lock checker\n");exit(2);}}__builtin___clear_cache((char *)address,(char *)(address+size));if(mprotect((void *)address,size,PROT_READ|PROT_EXEC)){perror("mprotect");exit(1);}}
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
static uint32_t published_sequence;
static int harq_firmware_read(void *opaque,uint32_t address,void *out,size_t size){
	(void)opaque;
	if(!size || !((address>=0xc6200000 && address<0xc660c000 && size<=0xc660c000-address) ||
		(address>=0xc6800000 && address<0xc6c4d000 && size<=0xc6c4d000-address))){return -1;}
	memcpy(out,(void *)(uintptr_t)address,size);
	return 0;
}
static void capture_harq_feedback(const uint32_t descriptor[4]){
	if(!harq_state_enabled || harq_verify.records>=ISI_HARQ_VERIFY_LIMIT){return;}
	uint64_t physical=(uint64_t)descriptor[3]+0x10000000;
	uint32_t length=__builtin_bswap16(descriptor[1]&0xffff)+10;
	if((descriptor[2]>>24&0x1e)!=0x16 || length!=64 ||
		physical<0x10140000 || physical+length>0x101ff000){return;}
	const uint8_t *raw=(void *)(uintptr_t)(0x90c00000+physical-0x10140000);
	struct isi_harq_feedback feedback;
	if(isi_harq_feedback_decode(raw,length,&feedback)){
		harq_feedback[(feedback.tag0*5+feedback.tag1)&255]=feedback;
	}
}
static void report_harq_state(const uint32_t verification[ISI_HARQ_VERIFY_WORDS],
			     const struct isi_harq_snapshot *state,uint32_t attempt){
	fprintf(stderr,"HARQ_STATE sequence=%u publication=%u tag0=%u tag1=%u bits=%u wire_attempt=%u feedback_match=%u input14=%u input16=%u process_matches=%u process=%u pending=%u state_attempt=%u selection_match=%u flow_mask=%u table_count=%u errors=%u publish_status=%u\n",
		verification[0],verification[1],verification[2],verification[3],verification[4],
		attempt,state->feedback_match,state->word14,state->word16,
		state->process_matches,state->process_index,state->pending,state->state_attempt,
		state->current_selection_matches,state->flow_mask,state->table_count,
		state->errors,verification[8]);
	for(unsigned i=0;i<8;i++){
		if(!(state->flow_mask&(1u<<i))){continue;}
		const struct isi_harq_flow_state *f=&state->flow[i];
		fprintf(stderr,"HARQ_FLOW sequence=%u slot=%u grant=%u bit14=%u raw_index=%u selected_index=%u stored=%u table=%u\n",
			verification[0],i,f->grant,f->flag14,f->raw_index,f->selected_index,f->stored_value,f->table_value);
	}
}
static void report_harq_verify(const uint32_t metadata[ISI_HARQ_VERIFY_WORDS]){
	isi_trace_record(7,NULL,metadata,ISI_HARQ_VERIFY_WORDS*sizeof(*metadata));
	fprintf(stderr,"HARQ_VERIFY sequence=%u publication=%u tag0=%u tag1=%u bits=%u bytes=%u different=%u read_errno=%u publish_status=%u\n",
		metadata[0],metadata[1],metadata[2],metadata[3],metadata[4],metadata[5],metadata[6],metadata[7],metadata[8]);
}
static void report_harq_pdu(const uint32_t verification[ISI_HARQ_VERIFY_WORDS],
			   const struct isi_harq_pdu_snapshot *pdu){
	fprintf(stderr,"HARQ_PDU sequence=%u publication=%u tag0=%u tag1=%u bits=%u wire_index=%u state_index=%u state_bits=%u table_count=%u table_bits=%u process_matches=%u selection_match=%u index_match=%u state_length_match=%u table_length_match=%u table_capacity_ok=%u table_padding_bits=%u errors=%u publish_status=%u\n",
		verification[0],verification[1],verification[2],verification[3],verification[4],
		pdu->wire_index,pdu->state_index,pdu->state_bits,pdu->table_count,
		pdu->table_bits,pdu->process_matches,pdu->selection_match,pdu->index_match,
		pdu->state_length_match,pdu->table_length_match,pdu->table_capacity_ok,
		pdu->table_padding_bits,pdu->errors,verification[8]);
}
static void report_harq_pack(const uint32_t v[ISI_HARQ_VERIFY_WORDS],
			    const struct isi_harq_packing *p,unsigned matched){
	fprintf(stderr,"HARQ_COVERAGE sequence=%u pack_calls=%u invalid_state_calls=%u limited_calls=%u captures=%u allocation_valid=%u generation=%u\n",
		v[0],harq_pack_capture.pack_calls,harq_pack_capture.invalid_state_calls,
		harq_pack_capture.limited_calls,harq_pack_capture.serial,
		harq_pack_capture.allocation_valid,harq_pack_capture.generation);
	fprintf(stderr,"HARQ_PACK sequence=%u publication=%u tag0=%u tag1=%u bits=%u matched=%u capture=%u generation=%u wire_index=%u attempt=%u si7=%u si8=%u entries=%u capacity_bits=%u allocation_bytes=%u packed_bits=%u si_bits=%u terminator_bits=%u legacy_bytes=%u complete_bytes=%u errors=%u publish_status=%u\n",
		v[0],v[1],v[2],v[3],v[4],matched,p->serial,p->generation,p->index,p->attempt,
		p->si7,p->si8,p->entries,p->capacity_bits,p->allocation_bytes,
		p->span.packed_bits,p->span.si_bits,p->span.terminator_bits,
		p->span.legacy_bytes,p->span.complete_bytes,p->errors,v[8]);
}
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
	// wcdma configuration (object 3a) both ways for the whole session
	const uint8_t *p=payload;
	unsigned at=kind==1 ? 7 : 3,object=kind==1 ? 10 : 7,message=kind==1 ? 11 : 9;
	static unsigned config_records;
	if(gprs_diag_enabled && p && size>=at+8 && p[at]==0xed && p[object]==0x3a && config_records<20000){
		struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
		char line[200];
		int n=snprintf(line,sizeof(line),"CFG t=%ld.%03ld %s %02x/%02x",(long)ts.tv_sec,ts.tv_nsec/1000000,
			kind==1 ? "dsp" : "ap",p[object],p[message]);
		for(unsigned i=message+1;i<size && i<message+41 && n<(int)sizeof(line)-4;i++){
			n+=snprintf(line+n,sizeof(line)-n,"%s%02x",i==message+1 ? " " : "",p[i]);
		}
		line[n++]='\n';config_records++;
		if(write(2,line,n)<0){gprs_diag_enabled=0;}
	}
}
// uplink rlc headers as they leave for the dsp
static unsigned uplink_records;
static uint32_t uplink_bits(const uint8_t *p,uint32_t pos,unsigned n){
	uint32_t v=0;
	for(unsigned i=0;i<n;i++){v=v<<1|(p[(pos+i)/8]>>(7-(pos+i)%8)&1);}
	return v;
}
// sn (=c), its plaintext (=p), something else (=?) or unknown (=-)
static int uplink_rlc(char *line,int n,int size,uint32_t header,const unsigned char *after){
	if(header>>15){
		unsigned sn=header>>3&0xfff;
		return n+snprintf(line+n,size-n," %u%s%s=%c",sn,header&4 ? "p" : "",(header&3)==1 ? "h" : "",
			after ? isi_kasumi_uplink_match(sn,after) : '-');
	}
	return n+snprintf(line+n,size-n," C%u",header>>12&7);
}
static void log_uplink(const uint8_t *packet,uint32_t length,const uint8_t *payload,uint32_t bits){
	if(!gprs_diag_enabled || uplink_records>=8000){return;}
	char line[640];int n;
	if(payload){
		unsigned groups=0,count[4]={0};uint32_t ddi[4]={0},need=0;
		for(unsigned k=1;k<=4 && !groups;k++){
			need=12*k;
			for(unsigned i=0;i<k;i++){ddi[i]=uplink_bits(payload,12*i,6);count[i]=uplink_bits(payload,12*i+6,6);need+=6+count[i]*336;}
			if(need==bits || need+6==bits){groups=k;}
		}
		n=snprintf(line,sizeof(line),"ULHS sequence=%u bits=%u attempt=%u",uplink_records+1,bits,packet[19]);
		if(!groups){
			n+=snprintf(line+n,sizeof(line)-n," raw=");
			for(unsigned i=0;i<12 && i*8<bits;i++){n+=snprintf(line+n,sizeof(line)-n,"%02x",payload[i]);}
		}
		uint32_t pos=12*groups;
		for(unsigned g=0;g<groups && n<(int)sizeof(line)-64;g++){
			n+=snprintf(line+n,sizeof(line)-n," ddi=%u n=%u tsn=%u :",ddi[g],count[g],uplink_bits(payload,pos,6));
			pos+=6;
			for(unsigned i=0;i<count[g] && n<(int)sizeof(line)-20;i++,pos+=336){
				unsigned char after[4];
				for(unsigned k=0;k<4;k++){after[k]=uplink_bits(payload,pos+16+8*k,8);}
				n=uplink_rlc(line,n,sizeof(line),uplink_bits(payload,pos,16),after);
			}
		}
	}else{
		const uint8_t *q=packet+10;uint32_t left=length-10,i;
		for(i=0;i+4<left;i++){
			uint32_t b=q[i]<<8|q[i+1];
			if(b>=200 && !q[i+2] && q[i+3]*8u>=b && q[i+3]*8u-b<16 && i+4+q[i+3]<=left){break;}
		}
		if(i+4>=left){return;}
		const uint8_t *block=q+i+4;
		n=snprintf(line,sizeof(line),"ULRACH sequence=%u bits=%u mac=%02x%02x%02x :",uplink_records+1,q[i]<<8|q[i+1],block[0],block[1],block[2]);
		n=uplink_rlc(line,n,sizeof(line),(uint32_t)block[3]<<8|block[4],block+5);
	}
	if(n>0 && n<(int)sizeof(line)-1){line[n++]='\n';uplink_records++;if(write(2,line,n)<0){gprs_diag_enabled=0;}}
}

#define DSP_WINDOW 0xbf000u
static unsigned char dsp_last[DSP_WINDOW];
static unsigned dsp_scan_records,dsp_scan_chunk,dsp_scan_passes;
static void dsp_scan(void){
	if(!gprs_diag_enabled || dsp_scan_records>=3000){return;}
	static unsigned char now[8192];
	uint32_t offset=dsp_scan_chunk*8192,size=DSP_WINDOW-offset<8192 ? DSP_WINDOW-offset : 8192;
	struct rapuyama_mdi_buffer buffer={.offset=offset,.size=size,.buffer=(uintptr_t)now};
	if(ioctl(mdi,RAPUYAMA_MDI_DSP_GET,&buffer)){fprintf(stderr,"DSPSTALE read failed: %s\n",strerror(errno));dsp_scan_records=3000;return;}
	const unsigned char *shadow=(void *)(uintptr_t)(0x90c00000+offset);
	for(uint32_t page=0;dsp_scan_passes && page<size;page+=4096){
		unsigned changed=0,differ=0,first=4096;
		for(unsigned i=page;i<page+4096 && i<size;i++){
			changed+=now[i]!=dsp_last[offset+i];
			if(now[i]!=shadow[i]){if(first==4096){first=i-page;}differ++;}
		}
		if(!changed || !differ){continue;}
		struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
		char line[200];
		int n=snprintf(line,sizeof(line),"DSPSTALE t=%ld.%02ld pass=%u dsp=%06x changed=%u differ=%u first=%03x dsp_head=",
			(long)ts.tv_sec,ts.tv_nsec/10000000,dsp_scan_passes,0x140000+offset+page,changed,differ,first);
		for(unsigned i=page+first;i<page+first+16 && i<size && n<(int)sizeof(line)-3;i++){n+=snprintf(line+n,sizeof(line)-n,"%02x",now[i]);}
		n+=snprintf(line+n,sizeof(line)-n," shadow=");
		for(unsigned i=page+first;i<page+first+16 && i<size && n<(int)sizeof(line)-3;i++){n+=snprintf(line+n,sizeof(line)-n,"%02x",shadow[i]);}
		if(n>0 && n<(int)sizeof(line)-1){line[n++]='\n';dsp_scan_records++;if(write(2,line,n)<0){gprs_diag_enabled=0;}}
	}
	memcpy(dsp_last+offset,now,size);
	if(++dsp_scan_chunk*8192>=DSP_WINDOW){dsp_scan_chunk=0;dsp_scan_passes++;}
}
static int mdi_forward(void){
	volatile uint32_t *ap=(void *)0xc601bce0;
	while(forwarded!=(*ap&31)){
		volatile uint32_t *slot=ap+1+(forwarded&15)*4;
		uint32_t descriptor[4]={slot[0],slot[1],slot[2],slot[3]};
		uint32_t verification[ISI_HARQ_VERIFY_WORDS]={0};
		int verified=0;
		struct isi_harq_snapshot snapshot;
		struct isi_harq_pdu_snapshot pdu_snapshot;
		struct isi_harq_packing pack_snapshot={0};
		unsigned pack_matched=0;
		unsigned have_snapshot=0,wire_attempt=0;

		if(descriptor[2]>>24==6 && descriptor[3]+0x10000000-0x1f8b0000-0xf8000<0x6000){fprintf(stderr,"mdi publish into the PA_SWD stack blocked: %08x %08x %08x %08x\n",descriptor[0],descriptor[1],descriptor[2],descriptor[3]);return 1;}

		if(descriptor[2]==0x19020007 && (!descriptor[0] || !descriptor[1] || !descriptor[3])){fprintf(stderr,"mdi dealloc with an empty tag blocked: %08x %08x %08x %08x\n",descriptor[0],descriptor[1],descriptor[2],descriptor[3]);return 1;}
		if(descriptor[2]>>24==6){
			uint64_t physical=(uint64_t)descriptor[3]+0x10000000;
			uint32_t length=__builtin_bswap16(descriptor[1]&0xffff)+6;
			if(physical<0x1f8b0000 || physical+length>0x1fcfd000){fprintf(stderr,"mdi payload range invalid\n");return 1;}
			const uint8_t *packet=(void *)(uintptr_t)(0xc6800000+physical-0x1f8b0000);
			uint32_t offset,size;
			int payload=isi_dsp_harq_payload(packet,length,&offset,&size);
			if(payload<0){fprintf(stderr,"invalid HSUPA HARQ payload\n");return 1;}
			if(length>=30 && packet[3]==0xed && packet[7]==0x3b && packet[9]==0x0d){log_uplink(packet,length,NULL,0);}
			if(payload){
				struct rapuyama_mdi_buffer buffer={
					.offset=offset,.size=size,.buffer=UINT64_C(0x90c00000)+offset,
				};
				if(ioctl(mdi,RAPUYAMA_MDI_DSP_PUT,&buffer)){fprintf(stderr,"HSUPA HARQ copy failed: %s; updated MDI driver required\n",strerror(errno));return 1;}
				log_uplink(packet,length,(const uint8_t *)(uintptr_t)(UINT32_C(0x90c00000)+offset),(uint32_t)packet[22]<<8|packet[23]);
				verified=isi_harq_verify(&harq_verify,mdi,offset,
					(void *)(uintptr_t)(UINT32_C(0x90c00000)+offset),size,verification);
				if(verified){
					verification[1]=published_sequence+1;
					verification[2]=(uint32_t)packet[10]<<8|packet[11];
					verification[3]=(uint32_t)packet[12]<<8|packet[13];
					verification[4]=(uint32_t)packet[22]<<8|packet[23];
					if(verified<0){
						report_harq_verify(verification);
						fprintf(stderr,"HARQ verification failed before publication; no retry\n");
						return 1;
					}
					if(harq_state_enabled){
						const struct isi_harq_feedback *feedback=&harq_feedback[
							(verification[2]*5+verification[3])&255];
						isi_harq_snapshot_read(packet,feedback,harq_firmware_read,NULL,&snapshot);
						wire_attempt=packet[19];have_snapshot=1;
						if(harq_pdu_enabled){
							isi_harq_pdu_snapshot_read(packet,harq_firmware_read,NULL,&pdu_snapshot);
						}
						if(harq_pack_enabled){
							const struct isi_harq_packing *packing=isi_harq_capture_match(&harq_pack_capture,packet);
							if(packing){pack_snapshot=*packing;pack_matched=1;}
						}
					}
				}
			}
		}
		ssize_t n=write(mdi,descriptor,sizeof(descriptor));
		if(verified){
			verification[8]=n==sizeof(descriptor) ? 1 : n<0 && errno==EAGAIN ? 2 : 3;
			int error=errno;
			report_harq_verify(verification);
			if(have_snapshot){report_harq_state(verification,&snapshot,wire_attempt);}
			if(have_snapshot && harq_pdu_enabled){report_harq_pdu(verification,&pdu_snapshot);}
			if(have_snapshot && harq_pack_enabled){report_harq_pack(verification,&pack_snapshot,pack_matched);}
			if(pack_matched && n==sizeof(descriptor)){
				for(unsigned i=0;i<8;i++)
					if(harq_pack_capture.packing[i].serial==pack_snapshot.serial)
						harq_pack_capture.packing[i].valid=0;
			}
			errno=error;
		}
		if(n<0 && errno==EAGAIN){return 0;}
		if(n!=sizeof(descriptor)){fprintf(stderr,"mdi publish failed: %d\n",errno);return 1;}
		trace_descriptor(2,descriptor);
		published_sequence++;
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
// received transport blocks (PN_WCDMA 3c/00) sit in a DSP-written ring 
static unsigned downlink_syncs,downlink_invalid;
static struct{uint16_t key;uint32_t n;}dl_kinds[24];
static uint32_t dl_other,dl_outside,dl_entries,dl_low=~0u,dl_high;
static time_t dl_since;
static void dl_stat(const uint8_t *packet,int count,const uint32_t *offsets){
	struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
	if(!dl_since){dl_since=ts.tv_sec;}
	if(packet){
		uint16_t key=packet[7]==0xed?packet[10]<<8|packet[11]:0xff00|packet[7];
		unsigned i=0;
		while(i<24 && dl_kinds[i].n && dl_kinds[i].key!=key){i++;}
		if(i<24){dl_kinds[i].key=key;dl_kinds[i].n++;}else{dl_other++;}
		for(int j=0;j<count;j++){
			dl_entries++;
			if(offsets[j]<dl_low){dl_low=offsets[j];}
			if(offsets[j]>dl_high){dl_high=offsets[j];}
		}
	}
	if(ts.tv_sec-dl_since<10){return;}
	char line[640];
	int n=snprintf(line,sizeof(line),"DLSTAT t=%ld entries=%u range=%05x-%05x outside=%u other=%u :",(long)ts.tv_sec,
		dl_entries,dl_entries?dl_low:0,dl_entries?dl_high:0,dl_outside,dl_other);
	for(unsigned i=0;i<24 && dl_kinds[i].n && n<(int)sizeof(line)-16;i++){
		n+=snprintf(line+n,sizeof(line)-n," %04x=%u",dl_kinds[i].key,dl_kinds[i].n);
	}
	if(n<(int)sizeof(line)-1){line[n++]='\n';}
	if(write(2,line,n)<0){gprs_diag_enabled=0;}
	memset(dl_kinds,0,sizeof(dl_kinds));
	dl_other=dl_outside=dl_entries=dl_high=0;dl_low=~0u;dl_since=ts.tv_sec;
}
static int mdi_downlink(const uint32_t descriptor[4]){
	if((descriptor[2]>>24&0x1e)!=0x16){
		if(gprs_diag_enabled){dl_other++;dl_stat(NULL,0,NULL);}
		return 0;
	}
	uint64_t physical=(uint64_t)descriptor[3]+0x10000000;
	uint32_t length=__builtin_bswap16(descriptor[1]&0xffff)+10;
	if(physical<0x10140000 || physical+length>0x101ff000){
		if(gprs_diag_enabled){dl_outside++;dl_stat(NULL,0,NULL);}
		return 0;
	}
	const uint8_t *packet=(void *)(uintptr_t)(0x90c00000+physical-0x10140000);
	uint32_t offsets[8],sizes[8];
	int count=isi_dsp_dl_buffers(packet,length,offsets,sizes,8);
	if(gprs_diag_enabled && length>=12){dl_stat(packet,count>0?count:0,offsets);}
	if(count<0){
		if(!downlink_invalid++){fprintf(stderr,"invalid downlink buffer reference: %02x%02x%02x%02x\n",packet[16],packet[17],packet[18],packet[19]);}
		return 0;
	}
	for(int i=0;i<count;i++){
		struct rapuyama_mdi_buffer buffer={.offset=offsets[i],.size=sizes[i],.buffer=UINT64_C(0x90c00000)+offsets[i]};
		if(ioctl(mdi,RAPUYAMA_MDI_DSP_GET,&buffer)){fprintf(stderr,"downlink buffer copy failed: %s; updated MDI driver required\n",strerror(errno));return 1;}
		if(!downlink_syncs++){fprintf(stderr,"downlink transport block copy active\n");}
		static unsigned block_records;
		if(gprs_diag_enabled && block_records<40000){
			const uint8_t *b=(void *)(uintptr_t)(0x90c00000+offsets[i]);
			struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
			char line[96];
			int n=snprintf(line,sizeof(line),"DLB t=%ld.%03ld id=%02x%02x off=%05x head=%02x%02x%02x%02x%02x%02x%02x%02x\n",
				(long)ts.tv_sec,ts.tv_nsec/1000000,packet[18+6*i],packet[19+6*i],offsets[i],
				b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7]);
			block_records++;
			if(n>0 && write(2,line,n)<0){gprs_diag_enabled=0;}
		}
	}
	return 0;
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
		capture_harq_feedback(descriptor);
		trace_descriptor(1,descriptor);
		if(mdi_downlink(descriptor)){return 1;}
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
		if(gprs_diag_enabled && (reply[3]==0x31 || reply[3]==0xd9)){isi_trace_record(5,NULL,reply,size);}
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
		isi_kasumi_watch_rearm();
		if(isi_sim_embedded() || os_timer_live || sbif_attached){firmware_services();}
		if(mdi_pump()){return 1;}
		dsp_scan();
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
		if(gprs_diag_enabled){isi_gprs_diag_report();}
		alarm(0);
	}
	struct rapuyama_mdi_status status;
	if(ioctl(mdi,RAPUYAMA_MDI_STATUS,&status)){return 1;}
	printf("mdi status ap=%08x dsp=%08x rx=%u tx=%u event=%u\n",status.ap,status.dsp,status.rx_interrupts,status.tx_interrupts,status.event_interrupts);
	return 0;
}

int main(int argc,char **argv){
	if(isi_window_reserve()){perror("firmware address window");return 1;}
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
	if(argc==2 && !strcmp(argv[1],"--pdu-watch-selftest")){
		// under the same seccomp filter and fault handler the live runtime uses
		failure_handlers();pdu_watch_enabled=1;sandbox();
		return isi_kasumi_watch_selftest();
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
	if(gprs_diag_configure()){return 2;}
	isi_kasumi_diagnostics(gprs_diag_enabled);
	isi_kasumi_watch(pdu_watch_enabled);
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
	failure_handlers();alarm(15);
	secure_required=link && isi_sim_embedded();
	image(argv[1],0xc6200000,0x40c000,1);image(argv[1],0xc6800000,0x44d000,0);image(argv[1],0xc6700000,0x8000,0);
	region(0xc6033000,0x1000);isi_sim_clock_init();region(0xc6060000,0x1000);region(0xc6f00000,0x200000);
	// c62dc880 (from an event handler) writes c6033088 and polls bit 4 of c603264c for up to 256 reads
	region(0xc6032000,0x1000);*(volatile uint32_t *)0xc603264c=0x10;
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
