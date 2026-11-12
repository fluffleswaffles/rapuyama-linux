// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "../kernel/linux/include/uapi/linux/rapuyama_energy.h"
#include "state.h"
#ifndef CHARGE_RUN_DIR
#define CHARGE_RUN_DIR "/run/rapuyama-charge"
#endif
struct calibration{int gain,capacity,target;};
struct gauge{int64_t charge_uas,last_ms,term_since,done_since,full_since;int full,filter,have_filter;};
static volatile sig_atomic_t stopping;
static int broken;
static void stop(int sig){(void)sig;stopping=1;}
static int64_t now_ms(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return -1;return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static int read_value(const char *path,int *value){
 FILE *f=fopen(path,"r");if(!f)return -1;
 int ok=fscanf(f,"%d",value)==1;fclose(f);if(!ok)errno=EIO;return ok?0:-1;
}
static int load_calibration(const char *path,struct calibration *c){
 FILE *f=fopen(path,"r");char tag[16],extra;int ok=0;
 if(!f)return -1;
 if(fscanf(f,"%15s %d %d %d %c",tag,&c->gain,&c->capacity,&c->target,&extra)==4 &&
    !strcmp(tag,"RCHG1") && c->gain>=5000 && c->gain<=20000 && c->capacity>=500 && c->capacity<=2500 &&
    c->target>=3900 && c->target<=4200 && c->target%20==0)ok=1;
 fclose(f);if(!ok)errno=EINVAL;return ok?0:-1;
}
static int ibat_ma(unsigned raw,unsigned period,int gain,int *ma){
 if(raw>65535 || !period || period>255 || raw==32767 || raw==32768)return -1;
 int q=(int)(int16_t)raw/(int)period;
 /* Original factory scaler, with the physical sign normalized to charge-positive. */
 int primitive=(int)(q*0.8030941611842105);
 *ma=(int)(primitive*(double)gain/10000.0);
 return *ma>=-3000 && *ma<=3000?0:-1;
}
static int source_for(unsigned usb_flags,int budget){return usb_flags&8?2:budget>=500000?1:0;}
static int target_for(const struct calibration *c,int temp){return temp<140 && c->target>4100?4100:c->target;}
static int energy_io(int fd,unsigned chip,unsigned reg,unsigned *value,int write){
 struct rapuyama_energy_io q={chip,reg,*value,write};
 if(ioctl(fd,RAPUYAMA_ENERGY_IO,&q)){
  if(errno==EAGAIN)return 1;
  if(reg==5 && write && errno==EPERM)return 1;
  broken=1;return -1;
 }
 if(q.address!=chip || q.reg!=reg || q.write!=(unsigned)write || q.value>255){errno=EPROTO;broken=1;return -1;}
 *value=q.value;return 0;
}
static int reg_read(int fd,unsigned chip,unsigned reg,unsigned *value){*value=0;return energy_io(fd,chip,reg,value,0);}
static int reg_write(int fd,unsigned chip,unsigned reg,unsigned value){return energy_io(fd,chip,reg,&value,1);}
static int pmic(int fd,unsigned field,unsigned *value,int write){
 struct rapuyama_energy_pmic q={field,*value,write,0};
 if(ioctl(fd,RAPUYAMA_ENERGY_PMIC,&q)){broken=1;return -1;}
 if(q.field!=field || q.write!=(unsigned)write || q.reserved || q.value>65535){errno=EPROTO;broken=1;return -1;}
 *value=q.value;return 0;
}
static int pmic_write(int fd,unsigned field,unsigned value){return pmic(fd,field,&value,1);}
static int all_off(int fd){
 int a=reg_write(fd,0x6a,1,6),b=reg_write(fd,0x6b,1,6);
 return a<0 || b<0?-1:0;
}
static int configure(int fd,int source,int target){
 if(all_off(fd))return -1;
 if(!source)return 0;
 unsigned chip=source==2?0x6a:0x6b,special;
 if(reg_write(fd,chip,2,((target-3500)/20)<<2)<0 || reg_write(fd,chip,4,1)<0 || reg_read(fd,chip,5,&special)<0)return -1;
 return reg_write(fd,chip,5,special&~0x20);
}
static int publish(const struct charge_state *s){
 char path[128];snprintf(path,sizeof(path),CHARGE_RUN_DIR "/state.%ld",(long)getpid());
 FILE *f=fopen(path,"w");if(!f)return -1;
 int n=fprintf(f,"1 %lld %d %d %d %d %d %d %lld %d %d %d",(long long)s->stamp_ms,s->pct,s->source,s->mode,s->net_ma,
  s->voltage_uv,s->temp_dc,(long long)s->charge_uas,s->usb_budget_ua,s->capacity_mah,s->error);
 int error=n<0;if(fclose(f))error=1;
 if(!error && rename(path,CHARGE_RUN_DIR "/state")==0)return 0;
 unlink(path);return -1;
}
static void gauge_sample(struct gauge *g,int ma,int64_t now,int capacity){
 int64_t maximum=(int64_t)capacity*3600000,dt=g->last_ms?now-g->last_ms:0;
 if(dt>0 && dt<=5000)g->charge_uas+=(int64_t)ma*dt;
 if(g->charge_uas<0)g->charge_uas=0;
 if(g->charge_uas>maximum)g->charge_uas=maximum;
 g->last_ms=now;
 if(!g->have_filter){g->filter=ma*8;g->have_filter=1;}else g->filter+=ma-g->filter/8;
}
static int gauge_pct(const struct gauge *g,int capacity){
 int64_t maximum=(int64_t)capacity*3600000;
 int pct=(int)((g->charge_uas*100+maximum/2)/maximum);
 return !g->full && pct>99?99:pct;
}
static void full_update(struct gauge *g,int source,int active,int target,int voltage,int dpm,int hw_full,int64_t now,int capacity){
 if(!source){g->full=0;g->term_since=0;g->done_since=0;return;}
 if(g->full){
  if(now-g->full_since>=300000 && (voltage<=(target-100)*1000 || g->charge_uas<=(int64_t)capacity*3600000*95/100)){g->full=0;g->term_since=0;}
  return;
 }
 int finish=0;
 if(hw_full && voltage>=(target-100)*1000){
  if(!g->done_since)g->done_since=now;
  finish=now-g->done_since>=6000;
 }else g->done_since=0;
 if(active && g->have_filter && !dpm && voltage>=(target-10)*1000 && g->filter/8>=0 && g->filter/8<=70){
  if(!g->term_since)g->term_since=now;
  if(now-g->term_since>=300000)finish=1;
 }else g->term_since=0;
 if(finish){g->full=1;g->full_since=now;g->charge_uas=(int64_t)capacity*3600000;}
}
int main(int argc,char **argv){
 if(argc!=2){fprintf(stderr,"usage: rapuyama-charge calibration-file\n");return 1;}
 struct calibration cal;
 if(load_calibration(argv[1],&cal)){perror("charging calibration");return 1;}
 if(mkdir(CHARGE_RUN_DIR,0755) && errno!=EEXIST){perror("charging state directory");return 1;}
 struct stat directory;
 if(lstat(CHARGE_RUN_DIR,&directory) || !S_ISDIR(directory.st_mode) || directory.st_uid!=geteuid() || directory.st_mode&0022){fprintf(stderr,"unsafe charging state directory\n");return 1;}
 struct gauge gauge={0};struct charge_state state={.capacity_mah=cal.capacity,.mode=CHARGE_INHIBITED};
 int pct=0;if(read_value("/sys/class/power_supply/battery/capacity",&pct) || pct<0 || pct>100){perror("battery capacity");return 1;}
 gauge.charge_uas=(int64_t)cal.capacity*3600000*pct/100;
 FILE *old=fopen(CHARGE_RUN_DIR "/state","r");
 if(old){char buf[256];struct charge_state previous;
  if(fgets(buf,sizeof(buf),old) && charge_state_parse(buf,now_ms(),&previous) && previous.capacity_mah==cal.capacity)gauge.charge_uas=previous.charge_uas;
  fclose(old);
 }
 signal(SIGTERM,stop);signal(SIGINT,stop);setvbuf(stdout,NULL,_IOLBF,0);
 int fd=open("/dev/rapuyama-energy",O_RDWR|O_CLOEXEC);if(fd<0){perror("charging open");return 1;}
 struct rapuyama_energy_status lease={.version=RAPUYAMA_ENERGY_VERSION};
 if(ioctl(fd,RAPUYAMA_ENERGY_ACQUIRE,&lease)){perror("charging acquire");close(fd);return 1;}
 int error=0,configured=-1,configured_target=0,previous_mode=-1;int64_t last_current=now_ms(),last_log=0;
 if(lease.version!=RAPUYAMA_ENERGY_VERSION || !(lease.flags&RAPUYAMA_ENERGY_OWNED) || lease.flags&RAPUYAMA_ENERGY_REVOKED || lease.reserved){errno=EPROTO;broken=1;goto failure;}
 if(pmic_write(fd,0x2c0100,0) || pmic_write(fd,0x2c00ff,4) || pmic_write(fd,0x210040,1) || pmic_write(fd,0x2c0100,1))goto failure;
 while(!stopping){
  int64_t now=now_ms();if(now<0)goto failure;
  if(ioctl(fd,RAPUYAMA_ENERGY_HEARTBEAT,0)){broken=1;goto failure;}
  state.stamp_ms=now;
  if(read_value("/sys/class/power_supply/battery/voltage_now",&state.voltage_uv) || read_value("/sys/class/power_supply/battery/temp",&state.temp_dc) ||
     read_value("/sys/class/power_supply/usb/current_max",&state.usb_budget_ua))goto failure;
  if(state.voltage_uv<2500000 || state.voltage_uv>4250000 || state.temp_dc<-600 || state.temp_dc>1500 || state.usb_budget_ua<0 || state.usb_budget_ua>500000){errno=ERANGE;goto failure;}
  unsigned usb_flags=0;
  if(reg_read(fd,0x6b,5,&usb_flags)<0)goto failure;
  if(now-last_current>=2000){
   unsigned raw=0,period=0,enabled=0;int ma;
   if(pmic(fd,0x2dffff,&raw,0) || pmic(fd,0x2c00ff,&period,0) || pmic(fd,0x2c0100,&enabled,0))goto failure;
   if(enabled!=1 || period!=4 || ibat_ma(raw,period,cal.gain,&ma)){errno=ERANGE;goto failure;}
   gauge_sample(&gauge,ma,now,cal.capacity);last_current=now;
  }
  if(now-last_current>5000){errno=ETIMEDOUT;goto failure;}
  int source=source_for(usb_flags,state.usb_budget_ua),target=target_for(&cal,state.temp_dc);
  state.source=source;
  if(configured!=source || configured_target!=target){
   int result=configure(fd,source,target);if(result<0)goto failure;
   configured=result? -1:source;configured_target=target;gauge.term_since=0;gauge.done_since=0;gauge.have_filter=0;
  }
  int active=0,hw_full=0,dpm=0;
  unsigned selected=source==2?0x6a:0x6b;
  if(source && configured==source && !gauge.full && gauge.have_filter){
   int result=reg_write(fd,selected,1,0x48);if(result<0)goto failure;
   unsigned status=0,control=0,voltage=0,special=0;
   if(reg_read(fd,selected,0,&status)<0 || reg_read(fd,selected,1,&control)<0 || reg_read(fd,selected,2,&voltage)<0 || reg_read(fd,selected,5,&special)<0)goto failure;
   if(voltage!=(unsigned)(((target-3500)/20)<<2)){errno=EPROTO;goto failure;}
   active=!result && !(control&6) && ((status>>4)&3)==1;
   hw_full=!result && !(control&6) && ((status>>4)&3)==2;
   dpm=!!(special&16);
  }
  int was_full=gauge.full;
  full_update(&gauge,source,active,target,state.voltage_uv,dpm,hw_full,now,cal.capacity);
  if(gauge.full && !was_full){if(all_off(fd))goto failure;active=0;}
  for(unsigned chip=0x6a;chip<=0x6b;chip++)if(reg_write(fd,chip,0,0xc0)<0)goto failure;
  state.net_ma=gauge.have_filter?gauge.filter/8:0;
  state.pct=gauge_pct(&gauge,cal.capacity);
  state.mode=gauge.full?(state.pct==100?CHARGE_FULL:CHARGE_DISCHARGING):active?(state.net_ma<=0?CHARGE_DISCHARGING:CHARGE_CHARGING):source?CHARGE_INHIBITED:CHARGE_DISCHARGING;
  state.charge_uas=gauge.charge_uas;
  if(publish(&state))goto failure;
  if(state.mode!=previous_mode || now-last_log>=60000){printf("t=%lld source=%d mode=%d net_ma=%d voltage_uv=%d temp_dc=%d pct=%d dpm=%d\n",(long long)now,source,state.mode,state.net_ma,state.voltage_uv,state.temp_dc,state.pct,dpm);previous_mode=state.mode;last_log=now;}
  usleep(2000000);
 }
 goto cleanup;
 failure:
 error=errno?errno:EIO;fprintf(stderr,"charging stopped: %s (transport=%d)\n",strerror(error),broken);
 cleanup:
 if(!broken && ioctl(fd,RAPUYAMA_ENERGY_RELEASE,0)){error=errno?errno:EIO;broken=1;}
 close(fd);
 state.stamp_ms=now_ms();state.mode=error?CHARGE_ERROR:CHARGE_STOPPED;state.error=error;state.charge_uas=gauge.charge_uas;
 if(state.voltage_uv>=2500000 && state.voltage_uv<=4250000)publish(&state);
 return error?1:0;
}