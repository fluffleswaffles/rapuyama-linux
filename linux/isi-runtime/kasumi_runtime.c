// SPDX-License-Identifier: GPL-2.0-only
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#ifdef __arm__
#include <ucontext.h>
#endif
#include "address_space.h"
#include "thumb_redirect.h"
#include "kasumi_runtime.h"
#include "trace_runtime.h"

static int diagnostics;
static unsigned diagnostic_records,plain_records,srb_records,hfn_scans;
static uint32_t last_hfn[32];

void isi_kasumi_diagnostics(int enabled){
	diagnostics=enabled;
}

// count and the plain bytes right before the ciphered span (rlc am header) show hfn/sn drift
static void trace_f8(unsigned batch_count,unsigned item,unsigned bearer,unsigned direction,
		     unsigned segments,unsigned bits,uint32_t count,uint32_t header){
	if(!diagnostics || diagnostic_records>=60000){return;}
	uint32_t metadata[]={++diagnostic_records,batch_count,item,bearer,direction,segments,bits,count,header};
	isi_trace_record(6,NULL,metadata,sizeof(metadata));
	// firmware may get here from a signal
	char line[128];
	int n=snprintf(line,sizeof(line),"F8 sequence=%u bearer=%u direction=%u segments=%u bits=%u count=%08x header=%05x\n",
		metadata[0],bearer,direction,segments,bits,count,header);
	if(n>0 && write(2,line,n)<0){diagnostics=0;}
	if(metadata[0]%32==1){
		struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
		n=snprintf(line,sizeof(line),"F8T sequence=%u t=%ld.%03ld\n",metadata[0],(long)ts.tv_sec,ts.tv_nsec/1000000);
		if(n>0 && write(2,line,n)<0){diagnostics=0;}
	}
}

#define WATCHES 16
static int watch_enabled;
static struct{uint32_t start,end,count;unsigned bearer,sequence;}watches[WATCHES];
static struct{uint32_t page;int armed;}watch_pages[WATCHES*2];
static unsigned watch_next,watch_page_count,watch_records,watch_gaps,watch_faults,watch_emulated;

void isi_kasumi_watch(int enabled){
	watch_enabled=enabled;
}
static void watch_line(const char *line,int n){
	if(n>0 && n<256 && write(2,line,n)<0){watch_enabled=0;}
}
static void watch_protect(unsigned i,int armed){
	if(watch_pages[i].armed==armed){return;}
	if(mprotect((void *)(uintptr_t)watch_pages[i].page,4096,armed ? PROT_READ : PROT_READ|PROT_WRITE)){
		watch_enabled=0;
		mprotect((void *)(uintptr_t)watch_pages[i].page,4096,PROT_READ|PROT_WRITE);
		watch_line("F8WSTOP mprotect failed\n",24);
		return;
	}
	watch_pages[i].armed=armed;
}
static void watch_disarm(void){
	for(unsigned i=0;i<watch_page_count;i++){watch_protect(i,0);}
}
void isi_kasumi_watch_rearm(void){
	if(!watch_enabled){return;}
	for(unsigned i=0;i<watch_page_count;i++){watch_protect(i,1);}
}
// only called disarmed
static void watch_add(uint32_t start,uint32_t end,uint32_t count,unsigned bearer){
	unsigned slot=watch_next;
	for(unsigned i=0;i<WATCHES;i++){
		if(watches[i].start==start && watches[i].end){slot=i;break;}
	}
	if(slot==watch_next){watch_next=(watch_next+1)%WATCHES;}
	watches[slot].start=start;watches[slot].end=end;watches[slot].count=count;
	watches[slot].bearer=bearer;watches[slot].sequence=diagnostic_records;
	watch_page_count=0;
	for(unsigned i=0;i<WATCHES;i++){
		if(!watches[i].end){continue;}
		for(uint32_t page=watches[i].start&~4095u;page<watches[i].end;page+=4096){
			unsigned known=0;
			for(unsigned j=0;j<watch_page_count;j++){known|=watch_pages[j].page==page;}
			if(!known){watch_pages[watch_page_count].page=page;watch_pages[watch_page_count++].armed=0;}
		}
	}
}
#ifdef __arm__
static int arm_condition(uint32_t cond,uint32_t cpsr){
	unsigned n=cpsr>>31&1,z=cpsr>>30&1,c=cpsr>>29&1,v=cpsr>>28&1,r;
	switch(cond>>1){
	case 0:r=z;break;case 1:r=c;break;case 2:r=n;break;case 3:r=v;break;
	case 4:r=c && !z;break;case 5:r=n==v;break;case 6:r=!z && n==v;break;default:return 1;
	}
	return cond&1 ? !r : r;
}
// decodes the store at pc
static int watch_decode(uint32_t *r,uint32_t cpsr,uint32_t *address,unsigned char *bytes,unsigned *length,
			int *writeback,uint32_t *writeback_value,unsigned *size){
	uint32_t values[16];unsigned words=0,width=0;
	*writeback=-1;
	if(cpsr&0x20){
		uint16_t op=*(uint16_t *)(uintptr_t)r[15];
		unsigned rt=op&7,rn=op>>3&7,rm=op>>6&7,imm=op>>6&31;
		*size=2;
		if((op&0xf800)==0x6000){*address=r[rn]+imm*4;width=4;}
		else if((op&0xf800)==0x7000){*address=r[rn]+imm;width=1;}
		else if((op&0xf800)==0x8000){*address=r[rn]+imm*2;width=2;}
		else if((op&0xfe00)==0x5000){*address=r[rn]+r[rm];width=4;}
		else if((op&0xfe00)==0x5200){*address=r[rn]+r[rm];width=2;}
		else if((op&0xfe00)==0x5400){*address=r[rn]+r[rm];width=1;}
		else if((op&0xf800)==0x9000){rt=op>>8&7;*address=r[13]+(op&0xff)*4;width=4;}
		else if((op&0xf800)==0xc000 || (op&0xfe00)==0xb400){
			unsigned list=op&0xff,base=(op&0xf800)==0xc000 ? op>>8&7 : 13;
			if((op&0xfe00)==0xb400 && op&0x100){list|=1u<<14;}
			for(unsigned i=0;i<15;i++){if(list>>i&1){values[words++]=r[i];}}
			if(!words){return -1;}
			*address=base==13 ? r[13]-words*4 : r[base];
			*writeback=base;*writeback_value=base==13 ? r[13]-words*4 : r[base]+words*4;
		}else{return -1;}
		if(width){values[0]=r[rt];}
	}else{
		uint32_t insn=*(uint32_t *)(uintptr_t)r[15];
		unsigned rn=insn>>16&15,rd=insn>>12&15,p=insn>>24&1,u=insn>>23&1,w=insn>>21&1;
		*size=4;
		if(insn>>28==15){return -1;}
		if(!arm_condition(insn>>28,cpsr)){return 0;}
		if(rn==15){return -1;}
		uint32_t offset;
		if((insn&0x0c100000)==0x04000000){
			if(insn&0x02000000){
				if(insn&0x70){return -1;}	// register offset, lsl only
				offset=r[insn&15]<<(insn>>7&31);
			}else{offset=insn&0xfff;}
			if(rd==15 || (!p && w)){return -1;}
			width=insn&0x00400000 ? 1 : 4;
		}else if((insn&0x0e1000f0)==0x000000b0 || (insn&0x0e1000f0)==0x000000f0){
			offset=insn&0x00400000 ? (insn>>4&0xf0)|(insn&15) : r[insn&15];
			if((!p && w) || ((insn&0xf0)==0xf0 && (rd&1 || rd==14))){return -1;}
			width=(insn&0xf0)==0xb0 ? 2 : 8;
		}else if((insn&0x0e100000)==0x08000000){
			unsigned list=insn&0xffff;
			if(insn&0x00400000 || list&0x8000 || !list){return -1;}
			for(unsigned i=0;i<15;i++){if(list>>i&1){values[words++]=r[i];}}
			uint32_t low=u ? r[rn]+(p ? 4 : 0) : r[rn]-words*4+(p ? 0 : 4);
			*address=low;
			if(w){*writeback=rn;*writeback_value=u ? r[rn]+words*4 : r[rn]-words*4;}
			goto store;
		}else{return -1;}
		uint32_t moved=u ? r[rn]+offset : r[rn]-offset;
		*address=p ? moved : r[rn];
		if(!p || w){*writeback=rn;*writeback_value=moved;}
		values[0]=r[rd];
		if(width==8){values[1]=r[rd+1];}
	}
store:
	if(width){
		if(width==8){words=2;}
		else{memcpy(bytes,values,width);*length=width;return 1;}
	}
	memcpy(bytes,values,words*4);*length=words*4;
	return 1;
}
#endif
int isi_kasumi_watch_fault(uint32_t fault,void *context){
#ifdef __arm__
	unsigned page;
	for(page=0;page<watch_page_count;page++){
		if(watch_pages[page].armed && watch_pages[page].page==(fault&~4095u)){break;}
	}
	if(!watch_enabled || page==watch_page_count){return 0;}
	ucontext_t *uc=context;
	_Static_assert(offsetof(ucontext_t,uc_mcontext.arm_pc)-offsetof(ucontext_t,uc_mcontext.arm_r0)==15*4,"ARM signal register layout");
	uint32_t *r=(uint32_t *)&uc->uc_mcontext.arm_r0,cpsr=uc->uc_mcontext.arm_cpsr,pc=r[15];
	uint32_t address=0,writeback_value=0;
	unsigned char bytes[64];unsigned length=0,size=0;int writeback;
	char line[256];
	// a fault storm means a hot page: give the coverage up rather than the modem
	if(++watch_faults>2000000){
		watch_disarm();watch_enabled=0;
		watch_line(line,snprintf(line,sizeof(line),"F8WSTOP faults=%u emulated=%u gaps=%u\n",watch_faults,watch_emulated,watch_gaps));
		return 1;
	}
	int decoded=watch_decode(r,cpsr,&address,bytes,&length,&writeback,&writeback_value,&size);
	if(decoded<0 || (decoded && (fault-address>=length || (address&~4095u)!=((address+length-1)&~4095u)))){
		watch_protect(page,0);
		if(++watch_gaps<=256){
			uint32_t insn=cpsr&0x20 ? *(uint16_t *)(uintptr_t)pc : *(uint32_t *)(uintptr_t)pc;
			watch_line(line,snprintf(line,sizeof(line),"F8WGAP pc=%08x lr=%08x thumb=%u insn=%08x address=%08x faults=%u emulated=%u after_f8=%u\n",
				pc,r[14],!!(cpsr&0x20),insn,fault,watch_faults,watch_emulated,diagnostic_records));
		}
		return 1;
	}
	if(decoded){
		for(unsigned i=0;i<WATCHES && watch_records<4096;i++){
			if(!watches[i].end || address>=watches[i].end || address+length<=watches[i].start){continue;}
			uint32_t from=address>watches[i].start ? address : watches[i].start;
			uint32_t to=address+length<watches[i].end ? address+length : watches[i].end;
			int n=snprintf(line,sizeof(line),"F8W sequence=%u after_f8=%u count=%08x bearer=%u offset=%d bytes=%u pc=%08x lr=%08x thumb=%u old=",
				watches[i].sequence,diagnostic_records,watches[i].count,watches[i].bearer,
				(int)(from-watches[i].start)-2,to-from,pc,r[14],!!(cpsr&0x20));
			for(uint32_t a=from;a<to && a<from+16 && n>0 && n<(int)sizeof(line)-3;a++){n+=snprintf(line+n,sizeof(line)-n,"%02x",*(unsigned char *)(uintptr_t)a);}
			n+=snprintf(line+n,sizeof(line)-n," new=");
			for(uint32_t a=from;a<to && a<from+16 && n>0 && n<(int)sizeof(line)-3;a++){n+=snprintf(line+n,sizeof(line)-n,"%02x",bytes[a-address]);}
			if(n>0 && n<(int)sizeof(line)-1){line[n++]='\n';watch_records++;watch_line(line,n);}
		}
		if(mprotect((void *)(uintptr_t)watch_pages[page].page,4096,PROT_READ|PROT_WRITE)){
			watch_pages[page].armed=1;watch_protect(page,0);return 1;
		}
		memcpy((void *)(uintptr_t)address,bytes,length);
		if(mprotect((void *)(uintptr_t)watch_pages[page].page,4096,PROT_READ)){watch_pages[page].armed=0;}
		watch_emulated++;
	}
	if(writeback>=0){r[writeback]=writeback_value;}
	r[15]=pc+size;
	return 1;
#else
	(void)fault;(void)context;return 0;
#endif
}

// first ciphered bytes of each uplink data-bearer pdu by SN
static struct{unsigned char plain[4],ciphered[4],valid;}uplink_pdus[4096];
int isi_kasumi_uplink_match(unsigned sn,const unsigned char bytes[4]){
	if(sn>=4096 || !uplink_pdus[sn].valid){return '-';}
	if(!memcmp(bytes,uplink_pdus[sn].ciphered,4)){return 'c';}
	if(!memcmp(bytes,uplink_pdus[sn].plain,4)){return 'p';}
	return '?';
}
static void invalid(void){fprintf(stderr,"invalid KASUMI f8 batch\n");_Exit(1);}
static int private_range(uint32_t address,uint32_t size){
	return (address>=0xc6800000 && size<=0x44d000 && address-0xc6800000<=0x44d000-size) ||
		(address>=0xc6700000 && size<=0x8000 && address-0xc6700000<=0x8000-size);
}

static void f8_batch(uint32_t *batch,const unsigned char *key,uint32_t bearer,uint32_t direction){
	void (*schedule)(const unsigned char *)=(void *)0xc62f291f;
	void (*cipher)(unsigned char *)=(void *)0xc62f28ab;
	if(!batch){return;}
	uint32_t address=(uintptr_t)batch;
	if(!private_range(address,4) || (address&3) || bearer>31 || direction>1){invalid();}
	static unsigned batches;
	if(!batches++){fprintf(stderr,"software UMTS f8 active\n");}
	unsigned count=batch[0];
	if(count>64 || !private_range(address,4+count*16)){invalid();}
	if(!count){return;}
	if(watch_enabled){watch_disarm();}
	if(!private_range((uintptr_t)key,16)){invalid();}
	static uint16_t last_key[32][2];
	if(diagnostics){
		uint32_t h=2166136261u;
		for(unsigned i=0;i<16;i++){h=(h^key[i])*16777619u;}
		uint16_t print=(uint16_t)(h^h>>16)|1;
		if(last_key[bearer][direction]!=print){
			last_key[bearer][direction]=print;
			char line[96];
			int n=snprintf(line,sizeof(line),"F8KEY sequence=%u bearer=%u direction=%u key=%04x\n",diagnostic_records,bearer,direction,print);
			if(n>0 && write(2,line,n)<0){diagnostics=0;}
		}
	}
	for(unsigned item=0;item<count;item++){
		uint32_t *record=batch+1+item*4;
		unsigned segments=record[1],total=0;
		if(segments>64 || !private_range((uintptr_t)record,8+segments*8)){invalid();}
		for(unsigned i=0;i<segments;i++){
			unsigned bits=record[3+i*2];
			if(bits>524288-total || (bits && !private_range(record[2+i*2],(bits+7)/8))){invalid();}
			total+=bits;
		}
		uint32_t header=0;
		if(segments && private_range(record[2]-2,2)){
			const unsigned char *before=(void *)(uintptr_t)(record[2]-2);
			header=0x10000|before[0]<<8|before[1];
		}
		trace_f8(count,item,bearer,direction,segments,total,record[0],header);
		// first use of a new user-plane hfn
		if(diagnostics && bearer>=4 && hfn_scans<8 && record[0]>>12>=0x40 && record[0]>>12!=last_hfn[bearer&31]){
			uint32_t hfn=record[0]>>12;
			last_hfn[bearer&31]=hfn;hfn_scans++;
			char line[200];
			int n=snprintf(line,sizeof(line),"F8HFN sequence=%u bearer=%u direction=%u hfn=%05x at",diagnostic_records,bearer,direction,hfn);
			unsigned found=0;
			for(uint32_t a=0xc6800000;a<0xc6800000+0x44d000 && found<12 && n>0 && n<(int)sizeof(line)-16;a+=4){
				uint32_t v=*(const uint32_t *)(uintptr_t)a;
				if(v==hfn || v==((hfn-1)&0xfffff) || v==hfn<<12 || v==((hfn-1)&0xfffff)<<12){
					n+=snprintf(line+n,sizeof(line)-n," %08x=%x",a,v);found++;
				}
			}
			if(n>0 && n<(int)sizeof(line)-1){line[n++]='\n';if(write(2,line,n)<0){diagnostics=0;}}
		}
		if(diagnostics && direction==0 && bearer<=3 && segments==1 && srb_records<4096){
			const unsigned char *p=(void *)(uintptr_t)record[2];
			unsigned bytes=(record[3]+7)/8;
			char line[200];
			int n=snprintf(line,sizeof(line),"F8SRB sequence=%u bearer=%u count=%08x header=%05x bits=%u data=",
				diagnostic_records,bearer,record[0],header,record[3]);
			for(unsigned i=0;i<bytes && i<48 && n>0 && n<(int)sizeof(line)-3;i++){n+=snprintf(line+n,sizeof(line)-n,"%02x",p[i]);}
			if(n>0 && n<(int)sizeof(line)-1){line[n++]='\n';srb_records++;if(write(2,line,n)<0){diagnostics=0;}}
		}
		// uplink user-plane only
		if(diagnostics && direction==0 && bearer>=4 && segments && record[3]>=32 && plain_records<1024){
			const unsigned char *first=(void *)(uintptr_t)record[2];
			unsigned last_bits=record[3+(segments-1)*2];
			const unsigned char *last=(void *)(uintptr_t)record[2+(segments-1)*2];
			if(last_bits>=16){
				char line[96];
				int n=snprintf(line,sizeof(line),"F8PT sequence=%u head=%02x%02x%02x%02x tail=%02x%02x\n",
					diagnostic_records,first[0],first[1],first[2],first[3],
					last[last_bits/8-2],last[last_bits/8-1]);
				plain_records++;
				if(n>0 && write(2,line,n)<0){diagnostics=0;}
			}
		}
		// uplink data pdus that end in padding loop forever on bad sessions, and their padding
		// differs on every retransmission. compare the padding region (never sdu bytes) of a
		// retransmission with the plaintext and the ciphertext left there last time
		static struct{uint32_t count;unsigned bearer,padding;unsigned char plain[64],ciphered[64];}sent[64];
		static unsigned sent_next,rtx_records;
		int track=-1;
		if(diagnostics && direction==0 && bearer>=4 && segments==1 && record[3]>=24 && record[3]<=512 && (header&3)==1){
			const unsigned char *p=(void *)(uintptr_t)record[2];
			unsigned bytes=record[3]/8,i=0,used=0,padding=0;
			while(i<bytes){
				unsigned li=p[i]>>1,e=p[i]&1;i++;
				if(li==0x7f || li==0x7e){padding=1;break;}
				used+=li;
				if(!e){break;}
			}
			if(padding && i+used<bytes){
				padding=i+used;
				for(unsigned s=0;s<64;s++){
					if(sent[s].count!=record[0] || sent[s].bearer!=bearer || sent[s].padding!=padding || rtx_records>=512){continue;}
					unsigned same_head=!memcmp(p,sent[s].plain,padding),as_plain=0,as_ciphered=0;
					for(unsigned k=padding;k<bytes;k++){as_plain+=p[k]==sent[s].plain[k];as_ciphered+=p[k]==sent[s].ciphered[k];}
					char line[240];
					int n=snprintf(line,sizeof(line),"F8RTX sequence=%u bearer=%u count=%08x padding_at=%u head_same=%u pad_bytes=%u as_plain=%u as_ciphered=%u now=",
						diagnostic_records,bearer,record[0],padding,same_head,bytes-padding,as_plain,as_ciphered);
					for(unsigned k=padding;k<bytes && k<padding+12 && n>0 && n<(int)sizeof(line)-3;k++){n+=snprintf(line+n,sizeof(line)-n,"%02x",p[k]);}
					n+=snprintf(line+n,sizeof(line)-n," plain=");
					for(unsigned k=padding;k<bytes && k<padding+12 && n>0 && n<(int)sizeof(line)-3;k++){n+=snprintf(line+n,sizeof(line)-n,"%02x",sent[s].plain[k]);}
					if(n>0 && n<(int)sizeof(line)-1){line[n++]='\n';rtx_records++;if(write(2,line,n)<0){diagnostics=0;}}
					break;
				}
				track=sent_next;sent_next=(sent_next+1)%64;
				sent[track].count=record[0];sent[track].bearer=bearer;sent[track].padding=padding;
				memcpy(sent[track].plain,p,bytes<64 ? bytes : 64);
			}
		}
		unsigned table_sn=4096;
		if(diagnostics && direction==0 && bearer>=4 && segments==1 && header&0x10000 && header&0x8000 && record[3]>=32){
			table_sn=(header&0xffff)>>3&0xfff;
			memcpy(uplink_pdus[table_sn].plain,(void *)(uintptr_t)record[2],4);
		}
		unsigned char modified[16],a[8]={0},stream[8]={0};
		for(unsigned i=0;i<16;i++){modified[i]=key[i]^0x55;}
		for(unsigned i=0;i<4;i++){a[i]=record[0]>>(24-i*8);}
		a[4]=(bearer<<3)|(direction<<2);
		schedule(modified);cipher(a);schedule(key);
		unsigned position=64,block=0;
		for(unsigned i=0;i<segments;i++){
			unsigned char *data=(void *)(uintptr_t)record[2+i*2];
			for(unsigned bit=0;bit<record[3+i*2];bit++){
				if(position==64){
					for(unsigned j=0;j<8;j++){stream[j]^=a[j];}
					stream[6]^=block>>8;stream[7]^=block;
					cipher(stream);position=0;block++;
				}
				unsigned value=(stream[position/8]>>(7-position%8))&1;
				data[bit/8]^=value<<(7-bit%8);position++;
			}
		}
		if(track>=0){
			unsigned bytes=record[3]/8;
			memcpy(sent[track].ciphered,(void *)(uintptr_t)record[2],bytes<64 ? bytes : 64);
		}
		if(table_sn<4096){
			memcpy(uplink_pdus[table_sn].ciphered,(void *)(uintptr_t)record[2],4);
			uplink_pdus[table_sn].valid=1;
		}
		if(watch_enabled && direction==0 && bearer>=4 && segments==1 && header && record[3]>=16){
			watch_add(record[2]-2,record[2]+(record[3]+7)/8,record[0],bearer);
		}
		// downlink rrc on srb0-3 after deciphering
		if(diagnostics && direction==1 && bearer<=3 && segments && srb_records<4096){
			const unsigned char *p=(void *)(uintptr_t)record[2];
			unsigned bytes=(record[3]+7)/8;
			char line[240];
			int n=snprintf(line,sizeof(line),"F8DL sequence=%u bearer=%u count=%08x header=%05x bits=%u segments=%u data=",
				diagnostic_records,bearer,record[0],header,record[3],segments);
			for(unsigned i=0;i<bytes && i<96 && n>0 && n<(int)sizeof(line)-3;i++){n+=snprintf(line+n,sizeof(line)-n,"%02x",p[i]);}
			if(n>0 && n<(int)sizeof(line)-1){line[n++]='\n';srb_records++;if(write(2,line,n)<0){diagnostics=0;}}
		}
	}
	isi_kasumi_watch_rearm();
}

// f9 entry c62f221c, same abi as the original
static unsigned f9_records;
static void f9_logged(void *message,unsigned bits,void *key,uint32_t count,uint32_t fresh,
		     void *mac,unsigned direction){
	void (*software)(void *,uint32_t,uint32_t,unsigned,void *,unsigned,void *)=(void *)0xc62f2a69;
	software(key,count,fresh,direction,message,bits,mac);
	if(diagnostics && f9_records<512){
		const unsigned char *m=mac;
		char line[256];
		int n=snprintf(line,sizeof(line),"F9 sequence=%u direction=%u bits=%u count=%08x fresh=%08x mac=%02x%02x%02x%02x after_f8=%u",
			++f9_records,direction,bits,count,fresh,m[0],m[1],m[2],m[3],diagnostic_records);
		if(direction==0 && private_range((uintptr_t)message,(bits+7)/8)){
			const unsigned char *p=message;
			n+=snprintf(line+n,sizeof(line)-n," data=");
			for(unsigned i=0;i<(bits+7)/8 && i<48 && n>0 && n<(int)sizeof(line)-3;i++){n+=snprintf(line+n,sizeof(line)-n,"%02x",p[i]);}
		}
		if(n>0 && n<(int)sizeof(line)-1){line[n++]='\n';if(write(2,line,n)<0){diagnostics=0;}}
	}
}

static unsigned f9_compares;
static int f9_compare(uint32_t received,uint32_t computed){
	int match=received==computed;
	if(diagnostics && f9_compares<512){
		char line[96];
		int n=snprintf(line,sizeof(line),"F9CMP sequence=%u after_f9=%u received=%08x computed=%08x match=%d\n",
			++f9_compares,f9_records,received,computed,match);
		if(n>0 && write(2,line,n)<0){diagnostics=0;}
	}
	return match;
}

typedef uint32_t (*isi_call4)(uint32_t,uint32_t,uint32_t,uint32_t);
static isi_call4 start_update_original,bearer_init_original;
static unsigned start_records;
static void start_log(const char *what,uint32_t ctx,uint32_t before,uint32_t caller,uint32_t a2,uint32_t a3){
	if(!diagnostics || start_records>=512){return;}
	char line[160];
	int n=snprintf(line,sizeof(line),"START %s sequence=%u after_f8=%u ctx=%08x cs=%05x ps=%05x before=%05x domain=%u caller=%08x a2=%x a3=%x\n",
		what,++start_records,diagnostic_records,ctx,*(uint32_t *)(uintptr_t)(ctx+0xf4),
		*(uint32_t *)(uintptr_t)(ctx+0xf8),before,*(uint8_t *)(uintptr_t)(ctx+0x1ab),caller,a2,a3);
	if(n>0 && write(2,line,n)<0){diagnostics=0;}
}

static uint32_t start_update_logged(uint32_t ctx,uint32_t a1,uint32_t a2,uint32_t a3){
	uint32_t caller=(uint32_t)(uintptr_t)__builtin_return_address(0);
	uint32_t before=*(uint32_t *)(uintptr_t)(ctx+0xf8);
	uint32_t r=start_update_original(ctx,a1,a2,a3);
	start_log("update_ps",ctx,before,caller,a2,a3);
	return r;
}
static uint32_t bearer_init_logged(uint32_t ctx,uint32_t a1,uint32_t a2,uint32_t a3){
	start_log("bearer_init",ctx,*(uint32_t *)(uintptr_t)(ctx+0xf8),(uint32_t)(uintptr_t)__builtin_return_address(0),a2,a3);
	return bearer_init_original(ctx,a1,a2,a3);
}
static isi_call4 trampoline(unsigned char *page,uint32_t entry,unsigned copied){
	memcpy(page,(void *)(uintptr_t)entry,copied);
	isi_thumb_redirect(page+copied,entry+copied+1);
	return (isi_call4)(uintptr_t)((uintptr_t)page+1);
}
static void start_hooks(void){
	static const unsigned char update_head[]={0x18,0xb5,0x04,0x46,0x00,0x20,0x00,0x90,0xe0,0x1d,0xf9,0x30};
	static const unsigned char init_head[]={0xf0,0xb5,0x04,0x46,0xff,0x30,0xa1,0x30,0x21,0x46,0xc6,0x7a,0xff,0x31};
	if(memcmp((void *)0xc64e6a18,update_head,sizeof(update_head)) || memcmp((void *)0xc64e7e12,init_head,sizeof(init_head))){
		fprintf(stderr,"unsupported START bookkeeping sites\n");_Exit(1);
	}
	unsigned char *page=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
	if(page==MAP_FAILED){fprintf(stderr,"START trampoline page\n");_Exit(1);}
	start_update_original=trampoline(page,0xc64e6a18,sizeof(update_head));
	bearer_init_original=trampoline(page+64,0xc64e7e12,sizeof(init_head));
	__builtin___clear_cache((char *)page,(char *)page+128);
	if(mprotect(page,4096,PROT_READ|PROT_EXEC)){fprintf(stderr,"START trampoline protect\n");_Exit(1);}
	isi_thumb_redirect((void *)0xc64e6a18,(uintptr_t)start_update_logged);
	isi_thumb_redirect((void *)0xc64e7e12,(uintptr_t)bearer_init_logged);
}

#define START_TRAP_SITE 0xc65578ba
static uint32_t reported_ps;
static int reported_ps_valid;
static unsigned seed_records;
int isi_kasumi_fault(void *context){
#ifdef __arm__
	ucontext_t *uc=context;
	if(!(uc->uc_mcontext.arm_cpsr&0x20) || uc->uc_mcontext.arm_pc!=START_TRAP_SITE ||
	   *(volatile uint16_t *)START_TRAP_SITE!=0xdeee){return 0;}
	reported_ps=uc->uc_mcontext.arm_r1&0xfffff;reported_ps_valid=1;
	// overwritten by movs r0,#1 two later
	uc->uc_mcontext.arm_r1<<=4;
	uc->uc_mcontext.arm_pc+=2;
	if(diagnostics && start_records<512){
		char line[64];
		int n=snprintf(line,sizeof(line),"START report ps=%05x after_f8=%u\n",reported_ps,diagnostic_records);
		start_records++;
		if(n>0 && write(2,line,n)<0){diagnostics=0;}
	}
	return 1;
#else
	(void)context;return 0;
#endif
}
// original c64ea5b2: *dst=start; if(flag) tail-call c64ea558(index,start,direction)
static uint32_t bearer_seed(uint32_t *dst,uint32_t start,uint32_t index,uint32_t flag,uint32_t direction){
	uint32_t caller=(uint32_t)(uintptr_t)__builtin_return_address(0);
	uint32_t used=start;
	if(diagnostics && seed_records<256){
		char line[128];
		int n=snprintf(line,sizeof(line),"START seed caller=%08x start=%05x reported=%05x used=%05x index=%u flag=%u direction=%u after_f8=%u\n",
			caller,start,reported_ps_valid?reported_ps:0xfffff,used,index,flag,direction,diagnostic_records);
		seed_records++;
		if(n>0 && write(2,line,n)<0){diagnostics=0;}
	}
	*dst=used;
	if(flag){
		void (*apply)(uint32_t,uint32_t,uint32_t)=(void *)0xc64ea559;
		apply(index,used,direction);
	}
	return (uint32_t)(uintptr_t)dst;
}

static isi_call4 start_encode_original;
static uint32_t start_encode_logged(uint32_t out,uint32_t value,uint32_t a2,uint32_t a3){
	uint32_t caller=(uint32_t)(uintptr_t)__builtin_return_address(0);
	if(diagnostics && start_records<512){
		char line[96];
		int n=snprintf(line,sizeof(line),"START encode value=%05x caller=%08x after_f8=%u\n",value&0xfffff,caller,diagnostic_records);
		start_records++;
		if(n>0 && write(2,line,n)<0){diagnostics=0;}
	}
	return start_encode_original(out,value,a2,a3);
}
static void start_encode_hook(void){
	static const unsigned char head[]={0x13,0xb5,0x6b,0x46,0x01,0x99,0x09,0x03,0x0a,0x0e,0x1a,0x71};
	if(memcmp((void *)0xc64e680c,head,sizeof(head))){fprintf(stderr,"unsupported START encoder\n");_Exit(1);}
	unsigned char *page=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
	if(page==MAP_FAILED){fprintf(stderr,"START encoder trampoline\n");_Exit(1);}
	start_encode_original=trampoline(page,0xc64e680c,sizeof(head));
	__builtin___clear_cache((char *)page,(char *)page+64);
	if(mprotect(page,4096,PROT_READ|PROT_EXEC)){fprintf(stderr,"START encoder protect\n");_Exit(1);}
	isi_thumb_redirect((void *)0xc64e680c,(uintptr_t)start_encode_logged);
}
static void start_seed_fix(void){
	static const unsigned char seed_head[]={0x10,0xb4,0x14,0x46,0x01,0x9a,0x00,0x2b,0x01,0x60,0x02,0xd0,0x20,0x46};
	if(memcmp((void *)0xc64ea5b2,seed_head,sizeof(seed_head)) || *(uint16_t *)START_TRAP_SITE!=0x0109){
		fprintf(stderr,"unsupported START seed sites\n");_Exit(1);
	}
	isi_thumb_redirect((void *)0xc64ea5b2,(uintptr_t)bearer_seed);
	*(volatile uint16_t *)START_TRAP_SITE=0xdeee;
	__builtin___clear_cache((char *)START_TRAP_SITE,(char *)START_TRAP_SITE+2);
}

// which resets complete?
static unsigned rlc_records;
static void rlc_log(const char *what,uint32_t entity,uint32_t value,int expected){
	if(!diagnostics || rlc_records>=512){return;}
	char line[160];
	int n=snprintf(line,sizeof(line),"RLC %s entity=%08x id=%u value=%05x",what,entity,
		*(unsigned char *)(uintptr_t)(entity+0x2d4),value);
	if(expected>=0){n+=snprintf(line+n,sizeof(line)-n," sent_rsn=%d",expected);}
	n+=snprintf(line+n,sizeof(line)-n," after_f8=%u\n",diagnostic_records);
	rlc_records++;
	if(n>0 && n<(int)sizeof(line) && write(2,line,n)<0){diagnostics=0;}
}
static void rlc_hfni(void *dst,void *pdu){((void (*)(void *,void *))0xc64ca1f1)(dst,pdu);}
static void rlc_rsn(void *dst,void *pdu){((void (*)(void *,void *))0xc64ca1bb)(dst,pdu);}
static void rlc_reset_rsn(unsigned char *dst,void *pdu){
	rlc_rsn(dst,pdu);rlc_log("reset_rx_rsn",(uintptr_t)dst-0x2c7,*dst,-1);
}
static void rlc_reset_hfni(uint32_t *dst,void *pdu){
	rlc_hfni(dst,pdu);rlc_log("reset_rx_hfni",(uintptr_t)dst-0x12b4,*dst,-1);
}
static void rlc_ack_rsn(unsigned char *dst,void *pdu){
	rlc_rsn(dst,pdu);
	uint32_t entity=(uintptr_t)dst-0x2c6;
	rlc_log("ack_rx_rsn",entity,*dst,*(unsigned char *)(uintptr_t)(entity+0x3d0));
}
static void rlc_ack_hfni(uint32_t *dst,void *pdu){
	rlc_hfni(dst,pdu);rlc_log("ack_rx_hfni",(uintptr_t)dst-0x12b4,*dst,-1);
}
static void rlc_ack4_hfni(uint32_t *dst,void *pdu){
	rlc_hfni(dst,pdu);rlc_log("ack4_rx_hfni",(uintptr_t)dst-0x12b4,*dst,-1);
}
static void rlc_hooks(void){
	static const struct{uint32_t site;uint16_t first,second;void *target;}sites[]={
		{0xc653b75c,0xf78e,0xfd2d,rlc_reset_rsn},{0xc653b786,0xf78e,0xfd33,rlc_reset_hfni},
		{0xc653dd50,0xf78c,0xfa33,rlc_ack_rsn},{0xc653dd7c,0xf78c,0xfa38,rlc_ack_hfni},
		{0xc653dce2,0xf78c,0xfa85,rlc_ack4_hfni},
	};
	for(unsigned i=0;i<5;i++){
		const uint16_t *bl=(void *)(uintptr_t)sites[i].site;
		if(bl[0]!=sites[i].first || bl[1]!=sites[i].second){fprintf(stderr,"unsupported RLC reset sites\n");_Exit(1);}
	}
	isi_window_release(0xc6611000,4096);
	unsigned char *page=mmap((void *)0xc6611000,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
	if(page==MAP_FAILED){fprintf(stderr,"RLC veneer page\n");_Exit(1);}
	for(unsigned i=0;i<5;i++){isi_thumb_redirect(page+i*16,(uintptr_t)sites[i].target);}
	__builtin___clear_cache((char *)page,(char *)page+80);
	if(mprotect(page,4096,PROT_READ|PROT_EXEC)){fprintf(stderr,"RLC veneer protect\n");_Exit(1);}
	for(unsigned i=0;i<5;i++){
		int32_t offset=(int32_t)(0xc6611000+i*16-sites[i].site-4);
		uint16_t branch[2]={0xf000|((uint32_t)offset>>12&0x7ff),0xf800|((uint32_t)offset>>1&0x7ff)};
		memcpy((void *)(uintptr_t)sites[i].site,branch,sizeof(branch));
		__builtin___clear_cache((char *)(uintptr_t)sites[i].site,(char *)(uintptr_t)sites[i].site+4);
	}
}

void isi_kasumi_patches(void){
	if(*(uint16_t *)0xc62f222e!=0xd30a){fprintf(stderr,"unsupported KASUMI selector\n");_Exit(1);}
	*(uint16_t *)0xc62f222e=0x46c0;
	if(*(uint16_t *)0xc62f2290!=0xd308){fprintf(stderr,"unsupported GEA3 selector\n");_Exit(1);}
	*(uint16_t *)0xc62f2290=0x46c0;
	isi_thumb_redirect((void *)0xc62f262a,(uintptr_t)f8_batch);
	if(diagnostics){
		if(*(uint32_t *)0xc62f221c!=0xb083b5ff){fprintf(stderr,"unsupported KASUMI f9 entry\n");_Exit(1);}
		isi_thumb_redirect((void *)0xc62f221c,(uintptr_t)f9_logged);
		if(*(uint32_t *)0xc64efb48!=0xd1014288){fprintf(stderr,"unsupported MAC compare\n");_Exit(1);}
		isi_thumb_redirect((void *)0xc64efb48,(uintptr_t)f9_compare);
		start_hooks();
		start_seed_fix();
		start_encode_hook();
		rlc_hooks();
	}
}
int isi_kasumi_selftest(void){
	/* ETSI TS135203 4.5 fuck me if this eorks */
	/* if someone's reading this please call me a good boy */
	const unsigned char key[]={0x5a,0xcb,0x1d,0x64,0x4c,0x0d,0x51,0x20,0x4e,0xa5,0xf1,0x45,0x10,0x10,0xd8,0x52};
	const unsigned char plain[]={0xad,0x9c,0x44,0x1f,0x89,0x0b,0x38,0xc4,0x57,0xa4,0x9d,0x42,0x14,0x07,0xe8};
	const unsigned char expected[]={0x9b,0xc9,0x2c,0xa8,0x03,0xc6,0x7b,0x28,0xa1,0x1a,0x4b,0xee,0x5a,0x0c,0x25};
	void *(*allocate)(unsigned)=(void *)0xc62c8027;
	void (*release)(void *)=(void *)0xc62c8579;
	void (*integrity)(void *,void *,unsigned,unsigned)=(void *)0xc62f262b;
	unsigned char *storage=allocate(96);if(!storage){return 1;}
	uint32_t *batch=(void *)storage;
	unsigned char *testkey=storage+32,*data=storage+48;
	memcpy(testkey,key,16);memcpy(data,plain,sizeof(plain));data[15]=0xa5;
	batch[0]=1;batch[1]=0xfa556b26;batch[2]=1;batch[3]=(uintptr_t)data;batch[4]=120;
	integrity(batch,testkey,3,1);
	if(memcmp(data,expected,15) || data[15]!=0xa5){fprintf(stderr,"UMTS f8 software mismatch\n");return 1;}
	memcpy(data,plain,sizeof(plain));batch[2]=2;batch[4]=56;batch[5]=(uintptr_t)(data+7);batch[6]=64;
	integrity(batch,testkey,3,1);
	if(memcmp(data,expected,15) || data[15]!=0xa5){fprintf(stderr,"UMTS f8 fragments mismatch\n");return 1;}
	release(storage);
	// gea3: a short request used to go to the unmapped engine, a long one was always software
	void (*gea3)(void *,unsigned,const void *,unsigned,uint32_t,unsigned)=(void *)0xc62f2275;
	unsigned char *g=allocate(1024+64+8);if(!g){return 1;}
	unsigned char *gl=g,*gs=g+1024,*kc=g+1088;
	memset(g,0,1088);memcpy(kc,key,8);
	gea3(gl,1024,kc,64,0x8e9421a3,1);gea3(gs,64,kc,64,0x8e9421a3,1);
	unsigned nonzero=0;for(unsigned i=0;i<64;i++){nonzero|=gs[i];}
	if(!nonzero || memcmp(gs,gl,64)){fprintf(stderr,"GEA3 short software path mismatch\n");return 1;}
	release(g);
	puts("original software KASUMI blocks: ETSI120-bit UMTS f8, fragmented stream, bounds and short GEA3 software path passed");return 0;
}

#ifdef __arm__
__asm__(".text\n.align 2\n"
	".thumb\n.thumb_func\n.type watch_t_strb,%function\nwatch_t_strb: strb r1,[r0,#3]\nbx lr\n"
	".thumb_func\n.type watch_t_str_reg,%function\nwatch_t_str_reg: str r1,[r0,r2]\nbx lr\n"
	".thumb_func\n.type watch_t_stmia,%function\nwatch_t_stmia: stmia r0!,{r1,r2}\nbx lr\n"
	".arm\n.align 2\n.type watch_a_strh,%function\nwatch_a_strh: strh r1,[r0,#2]\nbx lr\n"
	".type watch_a_str_wb,%function\nwatch_a_str_wb: str r1,[r0,#4]!\nbx lr\n"
	".type watch_a_stmdb,%function\nwatch_a_stmdb: stmdb r0!,{r1,r2}\nbx lr\n"
	".type watch_a_strne,%function\nwatch_a_strne: cmp r2,#0\nstrne r1,[r0]\nbx lr\n"
	".type watch_a_strd,%function\nwatch_a_strd: strd r2,r3,[r0]\nbx lr\n"
	".type watch_a_gap,%function\nwatch_a_gap: str r1,[r0],-r2,asr #1\nbx lr\n");
void watch_t_strb(void *,uint32_t);
void watch_t_str_reg(void *,uint32_t,uint32_t);
void *watch_t_stmia(void *,uint32_t,uint32_t);
void watch_a_strh(void *,uint32_t);
void *watch_a_str_wb(void *,uint32_t);
void *watch_a_stmdb(void *,uint32_t,uint32_t);
void watch_a_strne(void *,uint32_t,uint32_t);
void watch_a_strd(void *,uint32_t,uint32_t,uint32_t);
void watch_a_gap(void *,uint32_t,uint32_t);
#endif
int isi_kasumi_watch_selftest(void){
#ifdef __arm__
	static unsigned char page[4096] __attribute__((aligned(4096)));
	unsigned char *pdu=page+0x100;	// header at -2, ciphered span 0..0x3f
	memset(page,0xee,4096);
	watch_enabled=1;watch_add((uintptr_t)pdu-2,(uintptr_t)pdu+64,0x1234,4);isi_kasumi_watch_rearm();
	unsigned records=watch_records;
	uint32_t u;
#define CHECK(c,what) do{if(!(c)){fprintf(stderr,"PDU watch selftest: %s\n",what);return 1;}}while(0)
	watch_t_strb(pdu,0x7f);CHECK(pdu[3]==0x7f && watch_records==records+1,"thumb strb");
	watch_t_str_reg(pdu,0x11223344,8);memcpy(&u,pdu+8,4);CHECK(u==0x11223344 && watch_records==records+2,"thumb str reg");
	CHECK(watch_t_stmia(pdu+16,1,2)==pdu+24,"thumb stmia writeback");
	memcpy(&u,pdu+20,4);CHECK(u==2 && watch_records==records+3,"thumb stmia");
	watch_a_strh(pdu-4,0xabcd);CHECK(pdu[-2]==0xcd && pdu[-1]==0xab && watch_records==records+4,"arm strh into header");
	CHECK(watch_a_str_wb(pdu+24,5)==pdu+28,"arm str writeback");
	memcpy(&u,pdu+28,4);CHECK(u==5,"arm str");
	CHECK(watch_a_stmdb(pdu+40,3,4)==pdu+32,"arm stmdb writeback");
	memcpy(&u,pdu+36,4);CHECK(u==4,"arm stmdb");
	watch_a_strne(pdu+44,9,0);CHECK(pdu[44]==0xee,"arm condition skipped");
	watch_a_strne(pdu+44,9,1);CHECK(pdu[44]==9,"arm condition taken");
	watch_a_strd(pdu+48,0,6,7);memcpy(&u,pdu+52,4);CHECK(u==7,"arm strd");
	records=watch_records;
	watch_a_strh(page+0x800,0x5555);CHECK(page[0x802]==0x55 && watch_records==records,"store outside the pdu");
	unsigned emulated=watch_emulated,gaps=watch_gaps;
	watch_a_gap(pdu+56,0x42,0);CHECK(pdu[56]==0x42 && watch_gaps==gaps+1 && !watch_pages[0].armed,"unemulated store leaves a gap");
	isi_kasumi_watch_rearm();CHECK(watch_pages[0].armed,"rearm");
	watch_t_strb(pdu,1);CHECK(pdu[3]==1 && watch_emulated==emulated+1,"thumb strb after rearm");
	watch_disarm();watch_enabled=0;
#undef CHECK
	puts("PASS PDU watch: thumb strb/str/stmia, arm strh/str!/stmdb/strne/strd emulated in an armed page, gap and rearm");
	return 0;
#else
	return 1;
#endif
}
