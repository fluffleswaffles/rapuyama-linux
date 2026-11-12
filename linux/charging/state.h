// SPDX-License-Identifier: GPL-2.0-only
#ifndef RAPUYAMA_CHARGE_STATE_H
#define RAPUYAMA_CHARGE_STATE_H
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
enum{CHARGE_DISCHARGING,CHARGE_CHARGING,CHARGE_FULL,CHARGE_INHIBITED,CHARGE_ERROR,CHARGE_STOPPED};
struct charge_state{
 int64_t stamp_ms,charge_uas;
 int pct,source,mode,net_ma,voltage_uv,temp_dc,usb_budget_ua,capacity_mah,error;
};
static inline int charge_state_parse(const char *p,int64_t now,struct charge_state *s){
 int64_t v[12];
 for(unsigned i=0;i<12;i++){
  if(i && !isspace((unsigned char)*p))return 0;
  char *end;errno=0;v[i]=strtoll(p,&end,10);
  if(p==end || errno)return 0;
  p=end;
 }
 while(isspace((unsigned char)*p))p++;
 if(*p || v[0]!=1 || v[1]<0 || now<v[1] || now-v[1]>5000 || v[2]<0 || v[2]>100 ||
    v[3]<0 || v[3]>2 || v[4]<0 || v[4]>CHARGE_STOPPED || v[5]<-3000 || v[5]>3000 ||
    v[6]<2500000 || v[6]>4250000 || v[7]<-600 || v[7]>1500 || v[9]<0 || v[9]>500000 ||
    v[10]<500 || v[10]>2500 || v[8]<0 || v[8]>v[10]*3600000 || v[11]<0 || v[11]>4095)return 0;
 *s=(struct charge_state){.stamp_ms=v[1],.pct=v[2],.source=v[3],.mode=v[4],.net_ma=v[5],
  .voltage_uv=v[6],.temp_dc=v[7],.charge_uas=v[8],.usb_budget_ua=v[9],.capacity_mah=v[10],.error=v[11]};
 return 1;
}
#endif