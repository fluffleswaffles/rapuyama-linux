// runs a RAW arm blob from actually EXECUTABLE kernel memory
//
// first try jumped into Kern::Alloc memory which is execute-never (XN) ->
// prefetch abort. fix: alloc phys ram and map it with EMapAttrSupRwx
// (supervisor read/write/EXECUTE, XN clear) via DPlatChunkHw, copy the blob
// there and jump. mapping is uncached (EMapAttrFullyBlocking) so the I side sees
// our writes without cache games. this is the real primitive for loading code
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <kernel/cache.h>
#include <platform.h> // DPlatChunkHw, Epoc::AllocPhysicalRam, EMapAttr*
#include "runblob.h"
#include "blob.hdr" // KStage0Blob[], KStage0BlobLen
#include "stub2.hdr" // KStub2[], KStub2Len (mmu-off trampoline stub)
#include "tramp3.hdr" // KTramp3[], KTramp3Len (full handoff trampoline)
#include "fakekern.hdr" // KFakeKern[], KFakeKernLen (fake kernel)
#include "takeover.hdr" // KTakeover[], KTakeoverLen (one-way takeover)
#include "tramp_boot.hdr" // KTrampBoot[], KTrampBootLen (zImage handoff)
#include "kickloop.hdr" // KKickLoop[], KKickLoopLen (watchdog test)
#include "securetrace.hdr" // KSecureTrace[], KSecureTraceLen (counter-gap trace)
#include "smcprobe.hdr" // KSmcProbe[], KSmcProbeLen (secure-call feasibility probe)
#include "hook.hdr" // KHook[], KHookLen (transport-fn capture trampoline)
#include "hook8494.hdr" // KHook8494[], KHook8494Len (cbus-write logger trampoline)
#include "hookbeac8.hdr" // KHookBeac8[], KHookBeac8Len (modem TX send logger trampoline)

// securitykext monitor-call transport pointer = channel object 0xc800d1c4 + 0x14
// (confirmed live by e6secs). hooking it captures every secure request.
static const TUint32 KTransportPtr=0xc800d1d8u;
static DPlatChunkHw* gHookChunk=NULL; // persistent exec page holding the hook
static TUint32* gHookRing=NULL; // capture ring (kernel heap)
static TUint32 gHookOrig=0; // saved original transport ptr (0x908e0001)
static TLinAddr gHookLin=0; // hook code global va

// cbus-write hook: patch karap FUN_800b8494 (0x800b8494) to log every pmic write
static const TUint32 KCbusFn=0x800b8494u;
static DPlatChunkHw* gCbusChunk=NULL; // exec page for the trampoline
static TUint32* gCbusRing=NULL; // log ring
static TUint32 gCbusOrig0=0, gCbusOrig1=0; // saved original 2 instructions
static TLinAddr gCbusLin=0;

// modem TX send hook: patch ASIC send FUN_800beac8 (0x800beac8) to log every
// message Symbian writes to the modem TX FIFO (phys 0x40100000) = the exact EM
// (resource 0xA9) sleep/awake bytes we need to replay from Linux.
static const TUint32 KBeac8Fn=0x800beac8u;
static DPlatChunkHw* gBeac8Chunk=NULL;
static TUint32* gBeac8Ring=NULL;
static TUint32 gBeac8Orig0=0, gBeac8Orig1=0;
static TLinAddr gBeac8Lin=0;

// phase calibration (boot_app RB_CALIB)
static TUint32 gCalibSnap[40];
static TUint32 gCalibFc=0;
static TInt gCalibKick=1; // result of the request sent at the handoff, 1 = not sent
static TUint32 gCalibKickFc=0;
static TUint32 gCalibStartTick=0; // symbian tick at AllocBoot = the booter's start
static TUint32 gCalibCount=0;
static TUint32 FastCounter(){
	volatile TUint32* fc=(volatile TUint32*)0xc6033350u; // phys 0x20033350
	fc[1]=0xc6033354u; // latch (same literal ekern writes)
	return fc[0];
}
const TUint32 KCalibMagic=0x424c4143; // 'CALB'
const TInt KCalibWord=0x100; // free words between 0xf8 and the L1 copy at 0x400
// securitykext's 300 s NTimer (FUN_80140d84): object pointer at c800d1c0, the
// timer at +0x10, its trigger tick at timer+0x20, state byte at timer+0xa
const TUint32 KSecKextObj=0xc800d1c0u;
// layout (words from 0x100): 0 magic, 1 version, 2 fc at handoff, 3 fc of the
// last catch, 4 catch count, 5 secure timer state (0xffffffff = no object),
// 6 its trigger tick, 7 tick count at handoff, 8 tick period us, 9 result of
// the request sent at the handoff (1 = none), 10 fc right after it,
// 11 symbian tick when the booter called AllocBoot,
// 0x10..0x37 the 40-word snapshot of that catch
static void WriteCalib(volatile TUint32* h){
	volatile TUint32* c=h+KCalibWord;
	c[0]=0;
	c[1]=4;
	c[2]=FastCounter();
	c[3]=gCalibFc;
	c[4]=gCalibCount;
	TUint32 obj=*(volatile TUint32*)KSecKextObj;
	c[5]=0xffffffffu;
	c[6]=0;
	if(obj){
		c[5]=*(volatile TUint8*)(obj+0x10+0xa);
		c[6]=*(volatile TUint32*)(obj+0x10+0x20);
	}
	c[7]=NKern::TickCount();
	c[8]=NKern::TickPeriod();
	c[9]=(TUint32)gCalibKick;
	c[10]=gCalibKickFc;
	c[11]=gCalibStartTick;
	for(TInt i=0; i<40; ++i){c[0x10+i]=gCalibSnap[i];}
	c[0]=KCalibMagic; // last
}
// WatchService: sleep until just after the inferred 210 s phase
// the long wait itself with a countdown and calls this a few seconds before
const TUint32 KSvcPeriod=210000, KSvcPhase=900, KSvcAfter=300;

#ifdef RB_TIMER_CAPTURE
#include "timer_capture.h"
#endif

static TInt WatchService(TAny* user){
	TUint32 now=NKern::TickCount();
	TUint32 target=((now+KSvcPhase)/KSvcPeriod+1)*KSvcPeriod-KSvcPhase;
#ifdef RB_TIMER_CAPTURE
	return CaptureTimers(user, target);
#else
	(void)user;
	for(;;){
		TInt d=(TInt)(target+KSvcAfter-NKern::TickCount());
		if(d<=0){return KErrNone;}
		NKern::Sleep(d>1000 ? 1000 : d);
	}
#endif
}

typedef TInt (*TStubFn)(TUint32 aMarkerPhys, TUint32 aVal);
typedef void (*TVoidFn)(void);
typedef void (*TBootFn)(TUint32 aEntry, TUint32 aMach, TUint32 aDtb, TUint32 aWdog);
typedef TInt (*TTrampFn)(TUint32 aEntryPhys, TUint32 aMarkerPhys, TUint32 aDtb, TUint32 aMach);

typedef TInt (*TBlobFn)(TUint32* aOut, TInt aWords, TUint32 aCookie);

// cp15 helpers (arm, supervisor)
static TUint32 Ttbr0(){TUint32 v; asm volatile("mrc p15,0,%0,c2,c0,0":"=r"(v)); return v;}
static TUint32 Ttbcr(){TUint32 v; asm volatile("mrc p15,0,%0,c2,c0,2":"=r"(v)); return v;}
static void CleanDCacheMVA(TLinAddr a){asm volatile("mcr p15,0,%0,c7,c10,1"::"r"(a));}
static void Dsb(){asm volatile("mcr p15,0,%0,c7,c10,4"::"r"(0));}
static void InvTlbAll(){asm volatile("mcr p15,0,%0,c8,c7,0"::"r"(0));}
static void Isb(){asm volatile("mcr p15,0,%0,c7,c5,4"::"r"(0));}
// va -> pa with the current tables (privileged read), returns the PAR
static TUint32 Va2Pa(TUint32 aVa){
	TUint32 p;
	asm volatile("mcr p15,0,%1,c7,c8,0\n\tmcr p15,0,%2,c7,c5,4\n\tmrc p15,0,%0,c7,c4,0":"=r"(p):"r"(aVa), "r"(0));
	return p;
}

// isasw handoff: the modem sw runs on our cpu through the karap irq hooks
// (c8004068), linux has to host it. the hook addresses + the mmu setup they
// run under only exist in live symbian, so the booter saves them right before
// the jump. phys 0x1f0f1000 +0x5000, inside the ramcon no-map reservation:
// ramcon ends at 0x1f0f0008, the flight recorder starts at 0x1f0f8000
const TUint32 KIsaHandoffPhys=0x1f0f1000;
const TInt KIsaHandoffSz=0x5000;
const TUint32 KIsaMagic=0x48415349; // 'ISAH'
const TUint32 KKarapIsaCb=0xc8004068; // karap hook table, 6 words
const TUint32 KSisaObj=0xc800a6e0; // sisa_wrapper object, +8 = isasw object
static void SafeCopy(volatile TUint32* aDst, TUint32 aSrc, TInt aBytes){
	TUint32 tmp[64];
	for(TInt o=0; o<aBytes; o+=sizeof(tmp)){
		TInt n=aBytes-o; if(n>(TInt)sizeof(tmp)){n=sizeof(tmp);}
		if(Kern::SafeRead((const TAny*)(aSrc+o), tmp, n)){for(TInt i=0; i<n/4; ++i){tmp[i]=0xdeadbeef;}}
		for(TInt i=0; i<n/4; ++i){aDst[o/4+i]=tmp[i];}
	}
}
// layout (words): 0 magic, 1 version, 2 ttbr0, 3 ttbr1, 4 ttbcr, 5 dacr,
// 6 contextidr, 7 sctlr, 8 prrr, 9 nmrr, 0x10.. cb[0..5], 0x18 isasw obj ptr,
// 0x20..0x5f isasw obj (0x100 b), 0x60..0x9f sisa obj c800a6e0 (0x100 b),
// 0xa0 superpage, 0xa1 imachinedata, 0xb0..0xef imachinedata (0x100 b),
// 0xf0..0xf7 pa of cb[0..5] + obj, 0x400.. (byte 0x1000) the 16 kb kernel L1
static void SaveIsaHandoff(volatile TUint32* h){
	TUint32 v;
	asm volatile("mrc p15,0,%0,c2,c0,0":"=r"(v)); h[2]=v;
	asm volatile("mrc p15,0,%0,c2,c0,1":"=r"(v)); h[3]=v;
	asm volatile("mrc p15,0,%0,c2,c0,2":"=r"(v)); h[4]=v;
	asm volatile("mrc p15,0,%0,c3,c0,0":"=r"(v)); h[5]=v;
	asm volatile("mrc p15,0,%0,c13,c0,1":"=r"(v)); h[6]=v;
	asm volatile("mrc p15,0,%0,c1,c0,0":"=r"(v)); h[7]=v;
	asm volatile("mrc p15,0,%0,c10,c2,0":"=r"(v)); h[8]=v;
	asm volatile("mrc p15,0,%0,c10,c2,1":"=r"(v)); h[9]=v;
	SafeCopy(h+0x10, KKarapIsaCb, 0x18);
	SafeCopy(h+0x18, KSisaObj+8, 4);
	SafeCopy(h+0x20, h[0x18], 0x100);
	SafeCopy(h+0x60, KSisaObj, 0x100);
	TUint32 sp=(TUint32)&Kern::SuperPage();
	h[0xa0]=sp;
	SafeCopy(h+0xa1, sp+0x2c, 4);
	SafeCopy(h+0xb0, h[0xa1], 0x100);
	for(TInt i=0; i<6; ++i){h[0xf0+i]=Va2Pa(h[0x10+i]);}
	h[0xf6]=Va2Pa(h[0x18]);
	TUint32 pd=0;
	SafeCopy(&pd, sp+0xc, 4); // iPageDir
	SafeCopy(h+0x400, pd, 0x4000);
	h[1]=1;
	h[0]=KIsaMagic; // last, so a half-written block never looks valid
}

// identity-maps a 1 mb section covering phys aP as normal executable in TTBR0.
// returns the L1 chunk (close it later) + index + old descriptor, or NULL
static DPlatChunkHw* IdMapExec(TPhysAddr aP, TUint32& aIdx, TUint32& aOld){
	TUint32 n=Ttbcr()&0x7;
	TUint32 l1size=0x4000u>>n;
	TPhysAddr l1phys=Ttbr0()&~(l1size-1);
	aIdx=(aP>>20);
	DPlatChunkHw* cl=NULL;
	if(DPlatChunkHw::New(cl, l1phys, (TInt)l1size, EMapAttrSupRw|EMapAttrCachedMax)!=KErrNone){return NULL;}
	volatile TUint32* l1=(volatile TUint32*)cl->LinearAddress();
	aOld=l1[aIdx];
	l1[aIdx]=(aP&0xFFF00000u)|(1u<<10)|0x8u|0x4u|0x2u; // normal exec
	CleanDCacheMVA((TLinAddr)&l1[aIdx]);
	Dsb(); InvTlbAll(); Isb();
	return cl;
}

// ONE-WAY: copy a raw blob into its own identity-mapped executable section
// and jump. the blob kills the mmu itself. never returns on success
static TInt OneWay(const TUint8* aBlob, TUint aLen){
	const TInt KSec=0x100000;
	TPhysAddr p=0;
	TInt r=Epoc::AllocPhysicalRam(KSec, p, 20);
	if(r!=KErrNone){return r;}
	DPlatChunkHw* cw=NULL;
	r=DPlatChunkHw::New(cw, p, KSec, EMapAttrSupRw|EMapAttrFullyBlocking);
	if(r!=KErrNone){Epoc::FreePhysicalRam(p, KSec); return r;}
	volatile TUint8* wb=(volatile TUint8*)cw->LinearAddress();
	for(TUint i=0; i<aLen; ++i){wb[i]=aBlob[i];}

	TUint32 n=Ttbcr()&0x7;
	TUint32 l1size=0x4000u>>n;
	TPhysAddr l1phys=Ttbr0()&~(l1size-1);
	TUint32 idx=(p>>20);
	DPlatChunkHw* cl=NULL;
	r=DPlatChunkHw::New(cl, l1phys, (TInt)l1size, EMapAttrSupRw|EMapAttrCachedMax);
	if(r!=KErrNone){cw->Close(NULL); Epoc::FreePhysicalRam(p, KSec); return r;}
	volatile TUint32* l1=(volatile TUint32*)cl->LinearAddress();
	if(l1[idx]!=0){cl->Close(NULL); cw->Close(NULL); Epoc::FreePhysicalRam(p, KSec); return KErrInUse;}
	l1[idx]=(p&0xFFF00000u)|(1u<<10)|0x8u|0x4u|0x2u; // normal exec
	CleanDCacheMVA((TLinAddr)&l1[idx]);
	Dsb(); InvTlbAll(); Isb();
	Cache::IMB_Range((TLinAddr)p, aLen);

	TVoidFn fn=(TVoidFn)p;
	fn(); // never returns
	return KErrNone; // unreachable
}

// handoff prep, reversed from the firmware (docs/driver-re.md):
// clock gates for the bt uart (earlycon) and i2c on, and a fresh kick of both
// watchdogs so linux starts with the full 48 s. call with irqs off
static void PrepareHw(){
	volatile TUint32* r=(volatile TUint32*)0xc6000000u;
	// clock gates (power_resources table 0x800cbee8, polarity 1 -> write R+4):
	r[0x330d4/4]=0x1000; // EHwClkBB38M4AnaClk1 (bt uart) = gate 0x22, 0x200330d0 bit 12
	r[0x330d4/4]=0x40; // EHwClkI2C1 (i2c0.dll) = gate 0x7, 0x200330d0 bit 6
	r[0x330b4/4]=0x10; // + gate 0x12, 0x200330b0 bit 4
	r[0x25a04/4]=1; // bt uart active
	r[0x336c4/4]=0x3fffffffu; // soc watchdog counter
	// pmic watchdog over cbus: dev 1 reg 0x17 = 48 s (NEVER 0, that powers off)
	volatile TUint32* cb=(volatile TUint32*)0xc6030700u;
	// offsets from karap 0x800b8494 (base ptr 0x20030704 there): en 04 busy 08
	// wr mode 3c bits 2c data 20 wr cmd 1c
	cb[0x04/4]=1;
	cb[0x3c/4]=1;
	cb[0x2c/4]=15;
	cb[0x20/4]=0x30;
	cb[0x1c/4]=(1<<6)|0x17;
	for(TInt n=0x10000; n && cb[0x08/4]; --n){}
	cb[0x04/4]=0;
}

class DRunBlobChan : public DLogicalChannelBase{
	public:
		DRunBlobChan() : iBootChunk(NULL), iBootPhys(0), iBootVirt(0), iBootSize(0), iBootAlloc(0){}
		// a booter that quits after AllocBoot must give the block back, else
		// it stays held until reboot and every next try finds less ram
		virtual ~DRunBlobChan(){
			if(iBootChunk){iBootChunk->Close(NULL);}
			if(iBootAlloc){Epoc::FreePhysicalRam(iBootPhys, iBootAlloc);}
		}
		// boot chunk state, kept between AllocBoot/LoadChunk/Boot calls
		DPlatChunkHw* iBootChunk;
		TPhysAddr iBootPhys;
		TLinAddr iBootVirt;
		TUint32 iBootSize; // mapped
		TUint32 iBootAlloc; // allocated, >= iBootSize (e6bfix holds more)
	protected:
		virtual TInt DoCreate(TInt, const TDesC8*, const TVersion&){return KErrNone;}
		virtual TInt Request(TInt aFunction, TAny* a1, TAny* a2);
};

TInt DRunBlobChan::Request(TInt aFunction, TAny* a1, TAny* a2){
	// trampoline stage 1: build + check an identity section map
	if(aFunction==RRunBlob::EControlIdentityTest){
		const TInt KSec=0x100000; // 1 mb section
		TUint32 res[8];
		for(TInt z=0; z<8; ++z){res[z]=0;}
		DPlatChunkHw* cw=NULL;
		DPlatChunkHw* cl=NULL;
		TPhysAddr p=0;
		TBool haveP=EFalse;

		do{
			TInt r=Epoc::AllocPhysicalRam(KSec, p, 20); // 1 mb aligned
			if(r!=KErrNone){res[0]=(TUint32)r; break;}
			haveP=ETrue;
			res[1]=p;

			// put a marker in phys[0] through an UNCACHED writable alias
			r=DPlatChunkHw::New(cw, p, KSec, EMapAttrSupRw|EMapAttrFullyBlocking);
			if(r!=KErrNone){res[0]=(TUint32)r; break;}
			volatile TUint32* w=(volatile TUint32*)cw->LinearAddress();
			w[0]=0xDEADBEEFu;

			// L1 entry for va == p (identity). N (TTBCR) sets the TTBR0 table size
			TUint32 n=Ttbcr()&0x7;
			TUint32 l1size=0x4000u>>n; // 16kb >> N
			TPhysAddr l1phys=Ttbr0()&~(l1size-1);
			TUint32 idx=(p>>20); // section index
			res[2]=idx;

			r=DPlatChunkHw::New(cl, l1phys, (TInt)l1size, EMapAttrSupRw|EMapAttrCachedMax);
			if(r!=KErrNone){res[0]=(TUint32)r; break;}
			volatile TUint32* l1=(volatile TUint32*)cl->LinearAddress();

			TUint32 oldDesc=l1[idx];
			res[4]=oldDesc;
			if(oldDesc!=0){res[0]=1; break;} // va already mapped -> bail out safely

			// armv6 section descriptor, AFE format (SCTLR.AFE=1 on this chip):
			// bits[1:0]=10 section, AF=bit10=1, AP2=bit15=0 AP1=bit11=0 -> PL1 RW,
			// C=B=0 uncached (same as the uncached marker write), XN=0, domain 0
			TUint32 desc=(p&0xFFF00000u)|(1u<<10)|0x2u;
			res[3]=desc;
			l1[idx]=desc;
			CleanDCacheMVA((TLinAddr)&l1[idx]);
			Dsb(); InvTlbAll(); Isb();

			volatile TUint32* idva=(volatile TUint32*)p; // the identity va
			res[5]=idva[0]; // expect 0xDEADBEEF
			idva[0]=0x600DF00Du; // write via identity va...
			Dsb();
			res[6]=w[0]; // ...read back via the alias (expect 600DF00D)

			// restore
			l1[idx]=oldDesc;
			CleanDCacheMVA((TLinAddr)&l1[idx]);
			Dsb(); InvTlbAll(); Isb();
		}while(0);

		if(cl){cl->Close(NULL);}
		if(cw){cw->Close(NULL);}
		if(haveP){Epoc::FreePhysicalRam(p, KSec);}

		TPtrC8 src((const TUint8*)res, sizeof(res));
		return Kern::ThreadDesWrite(&Kern::CurrentThread(), a1, src, 0);
	}

	// trampoline stage 2: reversible mmu-off test
	if(aFunction==RRunBlob::EControlTrampTest){
		const TInt KSec=0x100000;
		TUint32 res[8];
		for(TInt z=0; z<8; ++z){res[z]=0;}
		DPlatChunkHw* cw=NULL;
		DPlatChunkHw* cl=NULL;
		TPhysAddr p=0;
		TBool haveP=EFalse;

		do{
			TInt r=Epoc::AllocPhysicalRam(KSec, p, 20);
			if(r!=KErrNone){res[0]=(TUint32)r; break;}
			haveP=ETrue; res[1]=p;

			// writable alias: copy the stub to phys[0], clear the marker at phys+0x1000
			r=DPlatChunkHw::New(cw, p, KSec, EMapAttrSupRw|EMapAttrFullyBlocking);
			if(r!=KErrNone){res[0]=(TUint32)r; break;}
			volatile TUint8* wb=(volatile TUint8*)cw->LinearAddress();
			for(TUint i=0; i<KStub2Len; ++i){wb[i]=KStub2[i];}
			*(volatile TUint32*)(cw->LinearAddress()+0x1000)=0;

			// identity-map section P as NORMAL (executable, SO memory cant run code)
			TUint32 n=Ttbcr()&0x7;
			TUint32 l1size=0x4000u>>n;
			TPhysAddr l1phys=Ttbr0()&~(l1size-1);
			TUint32 idx=(p>>20);
			r=DPlatChunkHw::New(cl, l1phys, (TInt)l1size, EMapAttrSupRw|EMapAttrCachedMax);
			if(r!=KErrNone){res[0]=(TUint32)r; break;}
			volatile TUint32* l1=(volatile TUint32*)cl->LinearAddress();
			TUint32 oldDesc=l1[idx];
			res[4]=oldDesc;
			if(oldDesc!=0){res[0]=1; break;}

			// section, AF=1, C=1,B=1 (normal cacheable with TRE on), XN=0, PL1 RW
			TUint32 desc=(p&0xFFF00000u)|(1u<<10)|0x8u|0x4u|0x2u; // 0x40E
			l1[idx]=desc;
			CleanDCacheMVA((TLinAddr)&l1[idx]);
			Dsb(); InvTlbAll(); Isb();
			Cache::IMB_Range((TLinAddr)p, KStub2Len); // so the I side sees the stub

			// grab PRRR/NMRR for debugging (memory type remap regs)
			asm volatile("mrc p15,0,%0,c10,c2,0":"=r"(res[5])); // PRRR
			asm volatile("mrc p15,0,%0,c10,c2,1":"=r"(res[6])); // NMRR

			// JUMP: run the stub from the identity va, it drops the mmu and brings it back
			TStubFn fn=(TStubFn)p;
			fn(p+0x1000, 0x600DCAFEu);
			res[2]=0x600DCAFEu;
			res[3]=*(volatile TUint32*)(cw->LinearAddress()+0x1000); // read the marker back

			l1[idx]=oldDesc;
			CleanDCacheMVA((TLinAddr)&l1[idx]);
			Dsb(); InvTlbAll(); Isb();
			res[0]=0;
		}while(0);

		if(cl){cl->Close(NULL);}
		if(cw){cw->Close(NULL);}
		if(haveP){Epoc::FreePhysicalRam(p, KSec);}

		TPtrC8 src((const TUint8*)res, sizeof(res));
		return Kern::ThreadDesWrite(&Kern::CurrentThread(), a1, src, 0);
	}

	// FULL trampoline test: load payload @+0x8000, hand off, echo regs back
	if(aFunction==RRunBlob::EControlFullTramp){
		const TInt KSec=0x100000;
		const TUint KOff=0x8000; // payload (kernel) offset, like TEXT_OFFSET
		const TUint KMk=0x4000; // marker offset
		const TUint32 KMach=0xE6E6E6E6u, KDtb=0xD7B00000u;
		TUint32 res[8];
		for(TInt z=0; z<8; ++z){res[z]=0;}
		DPlatChunkHw* cw=NULL;
		DPlatChunkHw* cl=NULL;
		TPhysAddr p=0;
		TBool haveP=EFalse;

		do{
			TInt r=Epoc::AllocPhysicalRam(KSec, p, 20);
			if(r!=KErrNone){res[0]=(TUint32)r; break;}
			haveP=ETrue; res[1]=p;

			r=DPlatChunkHw::New(cw, p, KSec, EMapAttrSupRw|EMapAttrFullyBlocking);
			if(r!=KErrNone){res[0]=(TUint32)r; break;}
			TLinAddr wl=cw->LinearAddress();
			volatile TUint8* wb=(volatile TUint8*)wl;
			for(TUint i=0; i<KTramp3Len; ++i){wb[i]=KTramp3[i];} // tramp @ 0
			for(TUint i=0; i<KFakeKernLen; ++i){wb[KOff+i]=KFakeKern[i];} // payload @ +0x8000
			for(TInt i=0; i<16; ++i){*(volatile TUint8*)(wl+KMk+i)=0;} // clear the marker

			TUint32 n=Ttbcr()&0x7;
			TUint32 l1size=0x4000u>>n;
			TPhysAddr l1phys=Ttbr0()&~(l1size-1);
			TUint32 idx=(p>>20);
			r=DPlatChunkHw::New(cl, l1phys, (TInt)l1size, EMapAttrSupRw|EMapAttrCachedMax);
			if(r!=KErrNone){res[0]=(TUint32)r; break;}
			volatile TUint32* l1=(volatile TUint32*)cl->LinearAddress();
			TUint32 oldDesc=l1[idx];
			if(oldDesc!=0){res[0]=1; break;}
			l1[idx]=(p&0xFFF00000u)|(1u<<10)|0x8u|0x4u|0x2u; // normal exec (0x40E)
			CleanDCacheMVA((TLinAddr)&l1[idx]);
			Dsb(); InvTlbAll(); Isb();
			Cache::IMB_Range((TLinAddr)p, KOff+KFakeKernLen);

			// hand off: tramp_entry(payloadPhys, markerPhys, dtb, machtype)
			TTrampFn fn=(TTrampFn)p;
			fn(p+KOff, p+KMk, KDtb, KMach);

			// read the regs the payload echoed back (mmu is back on by now)
			volatile TUint32* mk=(volatile TUint32*)(wl+KMk);
			res[2]=mk[0]; res[3]=mk[1]; res[4]=mk[2]; res[5]=mk[3];

			l1[idx]=oldDesc;
			CleanDCacheMVA((TLinAddr)&l1[idx]);
			Dsb(); InvTlbAll(); Isb();
			res[0]=0;
		}while(0);

		if(cl){cl->Close(NULL);}
		if(cw){cw->Close(NULL);}
		if(haveP){Epoc::FreePhysicalRam(p, KSec);}

		TPtrC8 src((const TUint8*)res, sizeof(res));
		return Kern::ThreadDesWrite(&Kern::CurrentThread(), a1, src, 0);
	}

	// EControlFbFill: write a colour through an EXISTING kernel va (fb hunt).
	// iPhys here is actually the kernel VIRT address that already maps the
	// target ram (from the unfiltered 0xc6 sweep). we do NOT make a new
	// DPlatChunkHw mapping (that crashed when pointed at in-use system ram).
	// write through the live mapping, then clean the D cache so the display
	// controller dma (reads phys ram) sees our pixels
	if(aFunction==RRunBlob::EControlFbFill){
		TFbFill f;
		TInt rr=Kern::ThreadRawRead(&Kern::CurrentThread(), a1, &f, sizeof(f));
		if(rr!=KErrNone){return rr;}
		if(f.iBytes==0 || f.iBytes>0x100000){f.iBytes=0x80000;} // default 512 kb
		volatile TUint32* v=(volatile TUint32*)f.iPhys; // live kernel va
		TUint n=f.iBytes/4;
		for(TUint i=0; i<n; ++i){v[i]=f.iColor;}
		for(TUint a=0; a<f.iBytes; a+=32){CleanDCacheMVA((TLinAddr)f.iPhys+a);} // clean D cache lines
		Dsb();
		return KErrNone;
	}

	// EControlTakeover: ONE-WAY. kill symbian, drive the display
	if(aFunction==RRunBlob::EControlTakeover){return OneWay(KTakeover, KTakeoverLen);}
	// EControlKickLoop: ONE-WAY. kill symbian, kick the watchdogs forever
	if(aFunction==RRunBlob::EControlKickLoop){return OneWay(KKickLoop, KKickLoopLen);}
	// EControlSecureTrace: ONE-WAY. watchdog-fed loop with fast-counter log
	if(aFunction==RRunBlob::EControlSecureTrace){return OneWay(KSecureTrace, KSecureTraceLen);}
	// EControlSmcProbe: ONE-WAY. feed watchdogs, do one smc #0, log if it returned
	if(aFunction==RRunBlob::EControlSmcProbe){return OneWay(KSmcProbe, KSmcProbeLen);}

	// booter: AllocBoot / LoadChunk / Boot
	if(aFunction==RRunBlob::EControlAllocBoot){
		gCalibStartTick=NKern::TickCount();
		TUint32 size=0;
		TInt rr=Kern::ThreadRawRead(&Kern::CurrentThread(), a1, &size, 4);
		if(rr!=KErrNone){return rr;}
		if(iBootChunk){
			iBootChunk->Close(NULL);
			iBootChunk=NULL;
			Epoc::FreePhysicalRam(iBootPhys, iBootAlloc);
			iBootSize=0;
			iBootAlloc=0;
		}
		size=(size+0xFFFFFu)&~0xFFFFFu; // round up to 1 mb
#ifdef RB_IMAGE_BOOT
		// e6bfix: the uncompressed Image runs right where it sits (chunk+0x8000,
		// PHYS_OFFSET = chunk, ARM_PATCH_PHYS_VIRT), so no decompressor writes
		// into ram symbian never gave us (0x11600000.. was its live memory, any
		// dma it left running landed in the kernel = the random XZ corrupt /
		// bad dtb deaths 18.10.2026). linux can't use ram below the chunk and
		// needs ~60 mb from its start (qemu, cma=0): base <= 0x1a000000.
		// phone runs: ClaimPhysicalRam pages can't be mapped (-14), a 32 mb
		// alloc lands too high (-4). so ask for the BIGGEST block symbian can
		// give (it has to compact and hand out low ram): then nearly all of
		// linux's ram is ram symbian gave away. out-of-range blocks are held
		// so the next try lands elsewhere. the requested size is ignored
		static const TUint32 KSizes[]={0xa000000u, 0x9000000u, 0x8000000u, 0x7000000u, 0x6000000u, 0x5000000u, 0x4000000u, 0x3000000u, 0x2000000u};
		const TUint32 KLo=0x11600000u, KHi=0x1a000000u;
		TInt r=KErrNoMemory;
		// symbian's free ram is fragmented by movable pages and the paging cache,
		// so between rounds let the kernel compact it (moves movable pages,
		// drops discardable ones)
		for(TInt round=0; r!=KErrNone && round<3; ++round){
			if(round>0){
				TRamDefragRequest d;
				d.DefragRam(TRamDefragRequest::KInheritPriority);
			}
			TPhysAddr held[4];
			TUint32 heldsz[4];
			TInt nheld=0;
			for(TUint k=0; r!=KErrNone && k<sizeof(KSizes)/sizeof(KSizes[0]); ++k){
				TPhysAddr p=0;
				if(Epoc::AllocPhysicalRam((TInt)KSizes[k], p, 21)!=KErrNone){continue;}
				if(p>=KLo && p<=KHi && p+KSizes[k]<=0x1f000000u){iBootPhys=p; size=KSizes[k]; r=KErrNone;}
				else if(nheld<4){held[nheld]=p; heldsz[nheld++]=KSizes[k];}
				else{Epoc::FreePhysicalRam(p, (TInt)KSizes[k]);}
			}
			for(TInt i=0; i<nheld; ++i){Epoc::FreePhysicalRam(held[i], (TInt)heldsz[i]);}
		}
		if(r!=KErrNone){return KErrNoMemory;}
		// all of it stays allocated (no symbian buffers in linux's ram), only
		// the first 32 mb (Image + dtb) get mapped
		TUint32 alloc=size;
		if(size>0x2000000u){size=0x2000000u;}
#else
		TInt r=Epoc::AllocPhysicalRam((TInt)size, iBootPhys, 20);
		if(r!=KErrNone){return r;}
		TUint32 alloc=size;
#endif
		r=DPlatChunkHw::New(iBootChunk, iBootPhys, (TInt)size, EMapAttrSupRw|EMapAttrCachedMax);
		if(r!=KErrNone){Epoc::FreePhysicalRam(iBootPhys, (TInt)alloc); iBootPhys=0; return r;}
		iBootVirt=iBootChunk->LinearAddress();
		iBootSize=size;
		iBootAlloc=alloc;
		TUint32 out=(TUint32)iBootPhys;
		return Kern::ThreadRawWrite(&Kern::CurrentThread(), a2, &out, 4);
	}

	if(aFunction==RRunBlob::EControlLoadChunk){
		if(!iBootChunk){return KErrNotReady;}
		TLoadChunk c;
		TInt rr=Kern::ThreadRawRead(&Kern::CurrentThread(), a1, &c, sizeof(c));
		if(rr!=KErrNone){return rr;}
		if(c.iOffset>iBootSize || c.iLen>iBootSize-c.iOffset){return KErrArgument;}
		return Kern::ThreadRawRead(&Kern::CurrentThread(), (const TAny*)c.iSrc, (TUint8*)(iBootVirt+c.iOffset), (TInt)c.iLen);
	}

	if(aFunction==RRunBlob::EControlBoot){
		if(!iBootChunk){return KErrNotReady;}
		TBootParams b;
		TInt rr=Kern::ThreadRawRead(&Kern::CurrentThread(), a1, &b, sizeof(b));
		if(rr!=KErrNone){return rr;}

		// patch the dtb /memory base sentinel (BE 1b ad b0 02) -> boot chunk phys
		TUint8* dtb=(TUint8*)(iBootVirt+b.iDtbOff);
		TUint32 scan=iBootSize-b.iDtbOff;
		if(scan>0x10000u){scan=0x10000u;}
		for(TUint32 i=0; i+4<=scan; ++i){
			if(dtb[i]==0x1b && dtb[i+1]==0xad && dtb[i+2]==0xb0 && dtb[i+3]==0x02){
				TUint32 be=(TUint32)iBootPhys;
				dtb[i]=(TUint8)(be>>24); dtb[i+1]=(TUint8)(be>>16);
				dtb[i+2]=(TUint8)(be>>8); dtb[i+3]=(TUint8)be;
				break;
			}
		}

		// make the whole boot chunk coherent for the mmu-off kernel
		for(TUint32 off=0; off<iBootSize; off+=32){CleanDCacheMVA(iBootVirt+off);}
		Dsb();

		// put tramp_boot in its own identity-mapped executable page and jump
		const TInt KSec=0x100000;
		TPhysAddr sp=0;
		if(Epoc::AllocPhysicalRam(KSec, sp, 20)!=KErrNone){return KErrNoMemory;}
		DPlatChunkHw* sc=NULL;
		if(DPlatChunkHw::New(sc, sp, KSec, EMapAttrSupRw|EMapAttrFullyBlocking)!=KErrNone){
			Epoc::FreePhysicalRam(sp, KSec);
			return KErrNoMemory;
		}
		volatile TUint8* sb=(volatile TUint8*)sc->LinearAddress();
		for(TUint i=0; i<KTrampBootLen; ++i){sb[i]=KTrampBoot[i];}
		TUint32 idx=0, old=0;
		DPlatChunkHw* cl=IdMapExec(sp, idx, old);
		if(!cl){sc->Close(NULL); Epoc::FreePhysicalRam(sp, KSec); return KErrGeneral;}
		Cache::IMB_Range((TLinAddr)sp, KTrampBootLen);

		// isasw handoff page, uncached so the mmu-off kernel sees it. mapped now,
		// filled with irqs off (symbian may own that ram until it dies)
		DPlatChunkHw* hc=NULL;
		if(DPlatChunkHw::New(hc, KIsaHandoffPhys, KIsaHandoffSz, EMapAttrSupRw|EMapAttrFullyBlocking)!=KErrNone){hc=NULL;}

#ifdef RB_CALIB_KICK
		// send securitykext's periodic request now
		if(*(volatile TUint32*)KSecKextObj){ // FUN_80140ea4 faults without it
			TUint32 req[3]={0, 0, 9};
			typedef TInt (*TSecReq)(TUint32*);
			gCalibKick=((TSecReq)0x80140ea4u)(req);
			gCalibKickFc=FastCounter();
		}
#endif

		// last things while the c6 window is still mapped. irqs off so no symbian
		// code runs after this (the bt stack would gate the uart again)
		NKern::DisableAllInterrupts();
		if(hc){
			SaveIsaHandoff((volatile TUint32*)hc->LinearAddress());
			WriteCalib((volatile TUint32*)hc->LinearAddress());
			Dsb();
		}
		PrepareHw();

		TBootFn fn=(TBootFn)sp;
		fn((TUint32)iBootPhys+b.iZImageOff, b.iMach, (TUint32)iBootPhys+b.iDtbOff, b.iWdog); // never returns
		return KErrNone; // unreachable
	}

	// EControlTimedDiff: find free-running counters (timer/watchdog)
	if(aFunction==RRunBlob::EControlTimedDiff){
		TFbFill p; // reused: iPhys=virt iBytes=count iColor=delay
		TInt rr=Kern::ThreadRawRead(&Kern::CurrentThread(), a1, &p, sizeof(p));
		if(rr!=KErrNone){return rr;}
		TUint count=p.iBytes;
		if(count==0 || count>1024){count=256;}
		TUint32* ab=(TUint32*)Kern::Alloc(count*8);
		if(!ab){return KErrNoMemory;}
		volatile TUint32* mm=(volatile TUint32*)p.iPhys;
		for(TUint i=0; i<count; ++i){ab[i]=mm[i];} // snapshot A
		volatile TUint32 spin=0;
		for(TUint32 k=0; k<p.iColor; ++k){spin+=k;} // delay
		for(TUint i=0; i<count; ++i){ab[count+i]=mm[i];} // snapshot B
		TPtrC8 src((const TUint8*)ab, (TInt)(count*8));
		TInt r=Kern::ThreadDesWrite(&Kern::CurrentThread(), a2, src, 0);
		Kern::Free(ab);
		return r;
	}

	// EControlTimerProbe: sanity check of the reversed timers, read only
	// (+ the fast counter latch, which ekern writes all the time anyway)
	if(aFunction==RRunBlob::EControlTimerProbe){
		volatile TUint32* fc=(volatile TUint32*)0xc6033350u; // phys 0x20033350
		volatile TUint32* mst=(volatile TUint32*)0xc6033780u; // phys 0x20033780
		TUint32 o[KTimerProbeWords];
		o[0]=KTimerProbeMagic;
		TInt irq=NKern::DisableAllInterrupts();
		o[1]=NKern::TickCount();
		fc[1]=0xc6033354u; o[2]=fc[0];
		o[3]=mst[0x24/4]; o[4]=mst[0x28/4];
		NKern::RestoreInterrupts(irq);
		for(TInt i=0; i<16; ++i){
			fc[1]=0xc6033354u; o[5+i*2]=fc[0];
			o[6+i*2]=mst[0x24/4];
			Kern::NanoWait(100000); // 100 us
		}
		NKern::Sleep(NKern::TimerTicks(1000));
		irq=NKern::DisableAllInterrupts();
		o[37]=NKern::TickCount();
		fc[1]=0xc6033354u; o[38]=fc[0];
		o[39]=mst[0x24/4]; o[40]=mst[0x28/4];
		NKern::RestoreInterrupts(irq);
		TPtrC8 src((const TUint8*)o, (TInt)sizeof(o));
		return Kern::ThreadDesWrite(&Kern::CurrentThread(), a1, src, 0);
	}

	// EControlRegProbe: read a list of regs in the c6 window, one word each.
	// the caller picks regs with no read side effects (no fifos / clear on read)
	if(aFunction==RRunBlob::EControlRegProbe){
		TUint32 in[KRegProbeMax];
		TPtr8 ip((TUint8*)in, 0, sizeof(in));
		TInt rr=Kern::ThreadDesRead(&Kern::CurrentThread(), a1, ip, 0);
		if(rr!=KErrNone){return rr;}
		TInt n=ip.Length()/4;
		TUint32 o[KRegProbeMax];
		for(TInt i=0; i<n; ++i){
			TUint32 v=in[i];
			if(v>=0xc6000000u && v<0xc7000000u && !(v&3)){o[i]=*(volatile TUint32*)v;}
			else{o[i]=0xdeadc6c6u;}
		}
		TPtrC8 src((const TUint8*)o, n*4);
		return Kern::ThreadDesWrite(&Kern::CurrentThread(), a2, src, 0);
	}

	// EControlRegWrite: one write in the c6 window (clock gates etc)
	if(aFunction==RRunBlob::EControlRegWrite){
		TUint32 v=(TUint32)a1;
		if(v<0xc6000000u || v>=0xc7000000u || (v&3)){return KErrArgument;}
		*(volatile TUint32*)v=(TUint32)a2;
		return KErrNone;
	}

	// EControlSnoopPoll: tight-poll securitykext's request descriptor at virt
	// 0xc670c450 (= phys 0x1ffec450, in the mapped 0x1ffe0000 ram window at
	// c6700000) and snapshot it the instant cmd (word0) is non-zero, to catch the
	// transient secure request. a1 = out buffer (40 words), a2 = iterations per
	// burst (0 = default). returns 1 if a request was caught this burst, else 0.
	// read-only, no interrupt mask, so symbian keeps running between bursts
	if(aFunction==RRunBlob::EControlSnoopPoll){
		volatile TUint32* d=(volatile TUint32*)0xc670c450u; // descriptor
		TUint32 iters=(TUint32)a2; if(iters==0){iters=0x800000u;}
		TUint32 cmd=0;
		for(TUint32 i=0; i<iters; ++i){cmd=d[0]; if(cmd!=0){break;}}
		if(cmd==0){return 0;}
		TUint32 fcCatch=FastCounter(); // first, as close to the catch as possible
		TUint32 out[40];
		for(TInt k=0; k<40; ++k){out[k]=0;}
		for(TInt k=0; k<6; ++k){out[k]=d[k];} // descriptor: cmd,PAid,w2,reqphys,respphys,flag
		volatile TUint32* s=(volatile TUint32*)0xc670c400u;
		for(TInt k=0; k<16; ++k){out[6+k]=s[k];} // shared +0x400 header
		TUint32 reqp=out[3]; // reqbuf phys; read it if it's in the mapped shm window
		if(reqp>=0x1ffe0000u && reqp<0x1fff0000u){
			volatile TUint32* rb=(volatile TUint32*)(0xc6700000u+(reqp-0x1ffe0000u));
			for(TInt k=0; k<16; ++k){out[22+k]=rb[k];} // reqbuf 0x40 bytes
		}
		out[38]=fcCatch; // words 38/39 were unused, snooppoll_app ignores them
		out[39]=++gCalibCount;
		gCalibFc=fcCatch;
		for(TInt k=0; k<40; ++k){gCalibSnap[k]=out[k];}
		TPtrC8 src((const TUint8*)out, sizeof(out));
		Kern::ThreadDesWrite(&Kern::CurrentThread(), a1, src, 0);
		return 1;
	}

	// EControlHookInstall: redirect securitykext's transport ptr (0xc800d1d8) to a
	// hook trampoline in an exec page, so every secure request is logged to a ram
	// ring. the hook reads the descriptor and bx's to the real stub (0x908e0001).
	// runs in securitykext's thread with mmu on, so the exec page must be a GLOBAL
	// kernel va (EMapAttrSupRwx, like RunBlob) - identity mappings wouldn't be
	// visible in that context
	if(aFunction==RRunBlob::EControlHookInstall){
		if(gHookChunk){return KErrInUse;}
		gHookRing=(TUint32*)Kern::Alloc(0x1000);
		if(!gHookRing){return KErrNoMemory;}
		for(TInt i=0; i<0x400; ++i){gHookRing[i]=0;} // ring[0]=index, entries at +0x20
		TPhysAddr phys=0;
		TInt r=Epoc::AllocPhysicalRam(0x1000, phys);
		if(r!=KErrNone){Kern::Free(gHookRing); gHookRing=NULL; return r;}
		r=DPlatChunkHw::New(gHookChunk, phys, 0x1000, EMapAttrSupRwx|EMapAttrFullyBlocking);
		if(r!=KErrNone){Epoc::FreePhysicalRam(phys, 0x1000); Kern::Free(gHookRing); gHookRing=NULL; gHookChunk=NULL; return r;}
		gHookLin=gHookChunk->LinearAddress();
		TUint8* code=(TUint8*)gHookLin;
		for(TUint i=0; i<KHookLen; ++i){code[i]=KHook[i];}
		gHookOrig=*(volatile TUint32*)KTransportPtr; // save the real transport (0x908e0001)
		*(volatile TUint32*)(gHookLin+KHookLen-8)=(TUint32)gHookRing; // patch ringva
		*(volatile TUint32*)(gHookLin+KHookLen-4)=gHookOrig; // patch origstub
		Cache::IMB_Range(gHookLin, KHookLen);
		*(volatile TUint32*)KTransportPtr=(TUint32)gHookLin; // arm the hook (arm, even)
		Dsb();
		return KErrNone;
	}

	// EControlHookRead: copy the 0x1000-byte capture ring to a1
	if(aFunction==RRunBlob::EControlHookRead){
		if(!gHookRing){return KErrNotReady;}
		TPtrC8 src((const TUint8*)gHookRing, 0x1000);
		return Kern::ThreadDesWrite(&Kern::CurrentThread(), a1, src, 0);
	}

	// EControlHookRemove: restore the real transport ptr. we deliberately do NOT
	// free the exec page/ring (an in-flight hooked call could still be running
	// them); the small leak goes away on unload/reboot
	if(aFunction==RRunBlob::EControlHookRemove){
		if(!gHookChunk){return KErrNotReady;}
		*(volatile TUint32*)KTransportPtr=gHookOrig;
		Dsb();
		gHookChunk=NULL; // keep the mapping alive, just forget it
		return KErrNone;
	}

	// EControlHookCbus: patch karap's cbus-write fn 0x800b8494 so every PMIC write
	// is logged to a ram ring. overwrites its first 2 instrs with ldr pc,[pc,#-4]
	// + trampoline addr; the trampoline (hook8494.S) logs {dev,reg,val}, runs the
	// 2 relocated instrs, returns to 0x800b849c. this is the definitive catch for
	// a hidden periodic pet done over cbus. patches live kernel rom (risky)
	if(aFunction==RRunBlob::EControlHookCbus){
		if(gCbusChunk){return KErrInUse;}
		gCbusRing=(TUint32*)Kern::Alloc(0x1000);
		if(!gCbusRing){return KErrNoMemory;}
		for(TInt i=0; i<0x400; ++i){gCbusRing[i]=0;}
		TPhysAddr ep=0;
		TInt r=Epoc::AllocPhysicalRam(0x1000, ep);
		if(r!=KErrNone){Kern::Free(gCbusRing); gCbusRing=NULL; return r;}
		r=DPlatChunkHw::New(gCbusChunk, ep, 0x1000, EMapAttrSupRwx|EMapAttrFullyBlocking);
		if(r!=KErrNone){Epoc::FreePhysicalRam(ep, 0x1000); Kern::Free(gCbusRing); gCbusRing=NULL; gCbusChunk=NULL; return r;}
		gCbusLin=gCbusChunk->LinearAddress();
		TUint8* code=(TUint8*)gCbusLin;
		for(TUint i=0; i<KHook8494Len; ++i){code[i]=KHook8494[i];}
		*(volatile TUint32*)(gCbusLin+KHook8494Len-8)=(TUint32)gCbusRing; // patch ringptr
		Cache::IMB_Range(gCbusLin, KHook8494Len);
		// map the rom page writable and patch 0x800b8494
		TPhysAddr rp=Epoc::LinearToPhysical(KCbusFn);
		if(rp==KPhysAddrInvalid){
			gCbusChunk->Close(NULL); gCbusChunk=NULL; Epoc::FreePhysicalRam(ep, 0x1000);
			Kern::Free(gCbusRing); gCbusRing=NULL; return KErrNotFound;
		}
		DPlatChunkHw* romw=NULL;
		r=DPlatChunkHw::New(romw, rp&~0xfffu, 0x1000, EMapAttrSupRw|EMapAttrFullyBlocking);
		if(r!=KErrNone){
			gCbusChunk->Close(NULL); gCbusChunk=NULL; Epoc::FreePhysicalRam(ep, 0x1000);
			Kern::Free(gCbusRing); gCbusRing=NULL; return r;
		}
		volatile TUint32* w=(volatile TUint32*)(romw->LinearAddress()+(KCbusFn&0xfffu));
		gCbusOrig0=w[0]; gCbusOrig1=w[1];
		w[1]=(TUint32)gCbusLin; Dsb(); // target first, so the ldr-pc always sees it
		w[0]=0xe51ff004u; Dsb(); // ldr pc,[pc,#-4]
		TUint32 chk=w[0];
		if(chk!=0xe51ff004u){ w[0]=gCbusOrig0; w[1]=gCbusOrig1; Dsb(); } // rom not writable: undo
		romw->Close(NULL);
		Cache::IMB_Range(KCbusFn, 8); Dsb(); Isb();
		if(chk!=0xe51ff004u){
			gCbusChunk->Close(NULL); gCbusChunk=NULL; Epoc::FreePhysicalRam(ep, 0x1000);
			Kern::Free(gCbusRing); gCbusRing=NULL; return KErrAccessDenied;
		}
		return KErrNone;
	}

	// EControlHookCbusRead: copy the 0x1000-byte cbus log ring to a1
	if(aFunction==RRunBlob::EControlHookCbusRead){
		if(!gCbusRing){return KErrNotReady;}
		TPtrC8 src((const TUint8*)gCbusRing, 0x1000);
		return Kern::ThreadDesWrite(&Kern::CurrentThread(), a1, src, 0);
	}

	// EControlHookCbusRemove: restore the original 2 instructions at 0x800b8494
	if(aFunction==RRunBlob::EControlHookCbusRemove){
		if(!gCbusChunk){return KErrNotReady;}
		TPhysAddr rp=Epoc::LinearToPhysical(KCbusFn);
		if(rp!=KPhysAddrInvalid){
			DPlatChunkHw* romw=NULL;
			if(DPlatChunkHw::New(romw, rp&~0xfffu, 0x1000, EMapAttrSupRw|EMapAttrFullyBlocking)==KErrNone){
				volatile TUint32* w=(volatile TUint32*)(romw->LinearAddress()+(KCbusFn&0xfffu));
				w[1]=gCbusOrig1; Dsb(); // restore 2nd instr first
				w[0]=gCbusOrig0; Dsb(); // then disarm the ldr-pc
				romw->Close(NULL);
				Cache::IMB_Range(KCbusFn, 8); Dsb(); Isb();
			}
		}
		gCbusChunk=NULL; // keep the exec page (an in-flight call may still run it)
		return KErrNone;
	}

	// EControlHookBeac8: patch ASIC send FUN_800beac8 (0x800beac8) so every modem
	// TX FIFO send is logged {descptr, channel, descword0} to a ram ring. same
	// mechanism as the cbus hook. this is the GROUND TRUTH for what Symbian sends
	// the modem over the ASIC byte-FIFO (0x40100000) - settles FIFO vs mailbox.
	if(aFunction==RRunBlob::EControlHookBeac8){
		if(gBeac8Chunk){return KErrInUse;}
		gBeac8Ring=(TUint32*)Kern::Alloc(0x1000);
		if(!gBeac8Ring){return KErrNoMemory;}
		for(TInt i=0; i<0x400; ++i){gBeac8Ring[i]=0;}
		TPhysAddr ep=0;
		TInt r=Epoc::AllocPhysicalRam(0x1000, ep);
		if(r!=KErrNone){Kern::Free(gBeac8Ring); gBeac8Ring=NULL; return r;}
		r=DPlatChunkHw::New(gBeac8Chunk, ep, 0x1000, EMapAttrSupRwx|EMapAttrFullyBlocking);
		if(r!=KErrNone){Epoc::FreePhysicalRam(ep, 0x1000); Kern::Free(gBeac8Ring); gBeac8Ring=NULL; gBeac8Chunk=NULL; return r;}
		gBeac8Lin=gBeac8Chunk->LinearAddress();
		TUint8* code=(TUint8*)gBeac8Lin;
		for(TUint i=0; i<KHookBeac8Len; ++i){code[i]=KHookBeac8[i];}
		*(volatile TUint32*)(gBeac8Lin+KHookBeac8Len-8)=(TUint32)gBeac8Ring; // patch ringptr
		Cache::IMB_Range(gBeac8Lin, KHookBeac8Len);
		TPhysAddr rp=Epoc::LinearToPhysical(KBeac8Fn);
		if(rp==KPhysAddrInvalid){
			gBeac8Chunk->Close(NULL); gBeac8Chunk=NULL; Epoc::FreePhysicalRam(ep, 0x1000);
			Kern::Free(gBeac8Ring); gBeac8Ring=NULL; return KErrNotFound;
		}
		DPlatChunkHw* romw=NULL;
		r=DPlatChunkHw::New(romw, rp&~0xfffu, 0x1000, EMapAttrSupRw|EMapAttrFullyBlocking);
		if(r!=KErrNone){
			gBeac8Chunk->Close(NULL); gBeac8Chunk=NULL; Epoc::FreePhysicalRam(ep, 0x1000);
			Kern::Free(gBeac8Ring); gBeac8Ring=NULL; return r;
		}
		volatile TUint32* w=(volatile TUint32*)(romw->LinearAddress()+(KBeac8Fn&0xfffu));
		gBeac8Orig0=w[0]; gBeac8Orig1=w[1];
		w[1]=(TUint32)gBeac8Lin; Dsb(); // target first, so the ldr-pc always sees it
		w[0]=0xe51ff004u; Dsb(); // ldr pc,[pc,#-4]
		TUint32 chk=w[0];
		if(chk!=0xe51ff004u){ w[0]=gBeac8Orig0; w[1]=gBeac8Orig1; Dsb(); } // rom not writable: undo
		romw->Close(NULL);
		Cache::IMB_Range(KBeac8Fn, 8); Dsb(); Isb();
		if(chk!=0xe51ff004u){
			gBeac8Chunk->Close(NULL); gBeac8Chunk=NULL; Epoc::FreePhysicalRam(ep, 0x1000);
			Kern::Free(gBeac8Ring); gBeac8Ring=NULL; return KErrAccessDenied;
		}
		return KErrNone;
	}

	// EControlHookBeac8Read: copy the 0x1000-byte modem-TX log ring to a1
	if(aFunction==RRunBlob::EControlHookBeac8Read){
		if(!gBeac8Ring){return KErrNotReady;}
		TPtrC8 src((const TUint8*)gBeac8Ring, 0x1000);
		return Kern::ThreadDesWrite(&Kern::CurrentThread(), a1, src, 0);
	}

	// EControlHookBeac8Remove: restore the original 2 instructions at 0x800beac8
	if(aFunction==RRunBlob::EControlHookBeac8Remove){
		if(!gBeac8Chunk){return KErrNotReady;}
		TPhysAddr rp=Epoc::LinearToPhysical(KBeac8Fn);
		if(rp!=KPhysAddrInvalid){
			DPlatChunkHw* romw=NULL;
			if(DPlatChunkHw::New(romw, rp&~0xfffu, 0x1000, EMapAttrSupRw|EMapAttrFullyBlocking)==KErrNone){
				volatile TUint32* w=(volatile TUint32*)(romw->LinearAddress()+(KBeac8Fn&0xfffu));
				w[1]=gBeac8Orig1; Dsb(); // restore 2nd instr first
				w[0]=gBeac8Orig0; Dsb(); // then disarm the ldr-pc
				romw->Close(NULL);
				Cache::IMB_Range(KBeac8Fn, 8); Dsb(); Isb();
			}
		}
		gBeac8Chunk=NULL; // keep the exec page (an in-flight call may still run it)
		return KErrNone;
	}

	// EControlModemReset: call Symbian's SisaResetGenerate(0x801039e0) with a1 as
	// the reset type (0x80|type). runs the firmware's real modem-reset sequence
	// while Symbian is alive; it queues a DFC, so the caller should sleep after.
	if(aFunction==RRunBlob::EControlModemReset){
		typedef void (*TSisaReset)(TUint32);
		TSisaReset fn=(TSisaReset)0x801039e0u;
		fn((TUint32)a1);
		return KErrNone;
	}

	// EControlReadPhys: map physical RAM at a1 (page aligned) and copy it to the
	// user descriptor a2. used to read back the ramoops kernel log after a warm
	// watchdog reset (phys 0x1f000000), so a hung linux leaves a readable log
	if(aFunction==RRunBlob::EControlReadPhys){
		TUint32 phys=(TUint32)a1;
		TInt maxlen=Kern::ThreadGetDesMaxLength(&Kern::CurrentThread(), a2);
		if(maxlen<=0){return KErrArgument;}
		TUint32 len=(TUint32)maxlen;
		TUint32 pageoff=phys&0xfff;
		TUint32 base=phys&~0xfffu;
		TUint32 msize=(pageoff+len+0xfff)&~0xfffu;
		DPlatChunkHw* c=NULL;
		TInt r=DPlatChunkHw::New(c, base, (TInt)msize, EMapAttrSupRw|EMapAttrCachedMax);
		if(r!=KErrNone){return r;}
		const TUint8* src=(const TUint8*)(c->LinearAddress()+pageoff);
		// copy through a kernel staging buffer in chunks
		TUint8* stage=(TUint8*)Kern::Alloc(0x1000);
		if(!stage){c->Close(NULL); return KErrNoMemory;}
		TUint32 done=0;
		while(done<len){
			TUint32 n=len-done; if(n>0x1000){n=0x1000;}
			for(TUint32 i=0; i<n; ++i){stage[i]=src[done+i];}
			TPtrC8 p8(stage, (TInt)n);
			r=Kern::ThreadDesWrite(&Kern::CurrentThread(), a2, p8, (TInt)done);
			if(r!=KErrNone){break;}
			done+=n;
		}
		Kern::Free(stage);
		c->Close(NULL);
		return r;
	}

	// EControlPeek: read ONE 32-bit mmio word at phys a1 through a fresh UNCACHED
	// (EMapAttrFullyBlocking = device/strongly-ordered) mapping, so each poll sees
	// the live register (cached ReadPhys would return a stale first value for
	// mmio). write the word to *a2. for watching the modem/ISA keepalive register.
	if(aFunction==RRunBlob::EControlWatchService){return WatchService(a1);}

	if(aFunction==RRunBlob::EControlPeek){
		TUint32 phys=(TUint32)a1;
		TUint32 base=phys&~0xfffu;
		TUint32 off=phys&0xfff;
		DPlatChunkHw* c=NULL;
		TInt r=DPlatChunkHw::New(c, base, 0x1000, EMapAttrSupRw|EMapAttrFullyBlocking);
		if(r!=KErrNone){return r;}
		volatile TUint32* reg=(volatile TUint32*)(c->LinearAddress()+off);
		TUint32 val=*reg;
		c->Close(NULL);
		return Kern::ThreadRawWrite(&Kern::CurrentThread(), a2, &val, sizeof(val));
	}

	// EControlVirtInfo: superpage address + phys of a kernel virtual address
	if(aFunction==RRunBlob::EControlVirtInfo){
		TUint32 out[2];
		out[0]=(TUint32)&Kern::SuperPage();
		out[1]=(TUint32)Epoc::LinearToPhysical((TLinAddr)a1);
		return Kern::ThreadRawWrite(&Kern::CurrentThread(), a2, out, sizeof(out));
	}

	// EControlReadVirt: copy kernel virtual memory (ram only) to the user
	// descriptor a2. Kern::SafeRead per page so an unmapped page just comes
	// back as 0xee. the c6 window is mmio except the 0x1ffe0000 ram block at
	// c6700000, reading mmio there can kill the phone, so it's refused
	if(aFunction==RRunBlob::EControlReadVirt){
		TUint32 va=(TUint32)a1;
		TInt maxlen=Kern::ThreadGetDesMaxLength(&Kern::CurrentThread(), a2);
		if(maxlen<=0){return KErrArgument;}
		TUint32 len=(TUint32)maxlen;
		if(va+len<va){return KErrArgument;}
		if(va<0xc7000000u && va+len>0xc6000000u &&
		   !(va>=0xc6700000u && va+len<=0xc6710000u)){return KErrAccessDenied;}
		TUint8* stage=(TUint8*)Kern::Alloc(0x1000);
		if(!stage){return KErrNoMemory;}
		TInt bad=0;
		TInt r=KErrNone;
		TUint32 done=0;
		while(done<len){
			TUint32 n=0x1000-((va+done)&0xfff);
			if(n>len-done){n=len-done;}
			if(Kern::SafeRead((const TAny*)(va+done), stage, (TInt)n)!=0){
				for(TUint32 i=0; i<n; ++i){stage[i]=0xee;}
				bad++;
			}
			TPtrC8 p8(stage, (TInt)n);
			r=Kern::ThreadDesWrite(&Kern::CurrentThread(), a2, p8, (TInt)done);
			if(r!=KErrNone){break;}
			done+=n;
		}
		Kern::Free(stage);
		return r!=KErrNone ? r : bad;
	}

	if(aFunction!=RRunBlob::EControlRunBlob){return KErrNotSupported;}

	const TInt KPageSz=0x1000;
	const TInt KOutWords=32;

	// 1. grab one physically contiguous ram page
	TPhysAddr phys=0;
	TInt r=Epoc::AllocPhysicalRam(KPageSz, phys);
	if(r!=KErrNone){return r;}

	// 2. map it executable (supervisor rwx, uncached)
	DPlatChunkHw* chunk=NULL;
	r=DPlatChunkHw::New(chunk, phys, KPageSz, EMapAttrSupRwx|EMapAttrFullyBlocking);
	if(r!=KErrNone){Epoc::FreePhysicalRam(phys, KPageSz); return r;}
	TLinAddr lin=chunk->LinearAddress();

	// 3. copy the raw blob in
	TUint8* code=(TUint8*)lin;
	for(TUint i=0; i<KStage0BlobLen; ++i){code[i]=KStage0Blob[i];}
	Cache::IMB_Range(lin, KStage0BlobLen);

	// 4. THE JUMP: run the loaded raw machine code
	TUint32 out[KOutWords];
	for(TInt j=0; j<KOutWords; ++j){out[j]=0;}
	TBlobFn fn=(TBlobFn)lin;
	TInt wrote=fn(out, KOutWords, 0x5A5AA5A5u);
	(void)wrote;

	// 5. give the result words back to user space
	TPtrC8 src((const TUint8*)out, KOutWords*4);
	r=Kern::ThreadDesWrite(&Kern::CurrentThread(), a1, src, 0);

	// 6. cleanup (DPlatChunkHw is a DObject -> Close(NULL), not Kern::ChunkClose)
	chunk->Close(NULL);
	Epoc::FreePhysicalRam(phys, KPageSz);
	return r;
}

class DRunBlobFactory : public DLogicalDevice{
	public:
		DRunBlobFactory(){iVersion=TVersion(1,0,0); iParseMask=0;}
		virtual TInt Install(){return SetName(&KRunBlobName);}
		virtual void GetCaps(TDes8& aDes) const{TVersion v=iVersion; Kern::InfoCopy(aDes, (const TUint8*)&v, sizeof(v));}
		virtual TInt Create(DLogicalChannelBase*& aChannel){aChannel=new DRunBlobChan; return aChannel ? KErrNone : KErrNoMemory;}
};

DECLARE_STANDARD_LDD(){
	return new DRunBlobFactory;
}