// SPDX-License-Identifier: GPL-2.0-only
#define main controller_main
#include "../charge.c"
#undef main
#include <assert.h>
int main(void){
 int ma;assert(!ibat_ma(400,4,9989,&ma) && ma==79);
 assert(!ibat_ma((unsigned)(uint16_t)-400,4,9989,&ma) && ma==-79);
 assert(ibat_ma(32767,4,9989,&ma) && ibat_ma(1,0,9989,&ma));
 assert(source_for(8,500000)==2 && source_for(0,500000)==1 && !source_for(0,100000));
 struct calibration c={9989,1521,4180};assert(target_for(&c,139)==4100 && target_for(&c,140)==4180);
 struct gauge g={.charge_uas=1000000};gauge_sample(&g,100,1000,1521);gauge_sample(&g,100,3000,1521);assert(g.charge_uas==1200000);
 gauge_sample(&g,-100,5000,1521);assert(g.charge_uas==1000000);
 gauge_sample(&g,-3000,7000,1521);assert(!g.charge_uas);
 g=(struct gauge){.have_filter=1,.filter=50*8};
 full_update(&g,2,1,4180,4160000,1,0,1000,1521);assert(!g.term_since && !g.full);
 full_update(&g,2,1,4180,4160000,0,0,2000,1521);assert(!g.term_since && !g.full);
 full_update(&g,2,1,4180,4160000,0,0,62000,1521);assert(!g.full);
 full_update(&g,2,0,4180,4170000,0,1,63000,1521);assert(!g.full);
 full_update(&g,2,0,4180,4170000,0,1,69000,1521);assert(g.full && g.charge_uas==(int64_t)1521*3600000);
 full_update(&g,2,0,4180,4070000,0,0,100000,1521);assert(g.full);
 full_update(&g,2,0,4180,4070000,0,0,369000,1521);assert(!g.full);
 g=(struct gauge){.have_filter=1,.filter=50*8};
 full_update(&g,2,1,4180,4160000,0,0,1000,1521);assert(!g.full && !g.term_since);
 full_update(&g,2,1,4180,4170000,1,0,2000,1521);assert(!g.term_since);
 full_update(&g,2,1,4180,4170000,0,0,3000,1521);assert(g.term_since==3000 && !g.full);
 full_update(&g,2,1,4180,4170000,0,0,302999,1521);assert(!g.full);
 full_update(&g,2,1,4180,4170000,0,0,303000,1521);assert(g.full);
 g=(struct gauge){.have_filter=1,.filter=71*8};
 full_update(&g,2,1,4180,4170000,0,0,500000,1521);assert(!g.full && !g.term_since);
 g.filter=-8;full_update(&g,2,1,4180,4170000,0,0,500001,1521);assert(!g.term_since);
 g=(struct gauge){.charge_uas=(int64_t)1521*3600000*90/100-1000};assert(gauge_pct(&g,1521)==90);
 g.charge_uas=(int64_t)1521*3600000;assert(gauge_pct(&g,1521)==99);g.full=1;assert(gauge_pct(&g,1521)==100);
 g.charge_uas=(int64_t)1521*3600000*98/100;assert(gauge_pct(&g,1521)==98);
 g=(struct gauge){.full=1,.full_since=1000,.charge_uas=(int64_t)1521*3600000*95/100};
 full_update(&g,2,0,4180,4170000,0,0,300999,1521);assert(g.full);
 full_update(&g,2,0,4180,4170000,0,0,301000,1521);assert(!g.full);
 struct charge_state s;
 const char *valid="1 1000 90 2 1 70 4037000 338 4928040000 500000 1521 0";
 assert(charge_state_parse(valid,2000,&s) && s.pct==90 && s.net_ma==70);
 assert(!charge_state_parse(valid,6001,&s) && !charge_state_parse(valid,999,&s));
 assert(!charge_state_parse("1 1000 101 2 1 70 4037000 338 0 500000 1521 0",2000,&s));
 assert(!charge_state_parse("1 1000 90 2 1 70 4037000 338 0 500000 1521 0 junk",2000,&s));
 puts("PASS current sign/calibration, source priority, real current integration, DPM termination and recharge, state bounds/freshness");
 return 0;
}