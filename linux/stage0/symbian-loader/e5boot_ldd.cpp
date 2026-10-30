// E5-00 (Symbian 9.3 / EKA2 kernel) booter + ram log reader. the e6bfix path of
// runblob_ldd.cpp cut down to what the 9.3 ekern can do. used by both apps:
//   LDD_SRC=e5boot_ldd.cpp RB_DEFS="-DRB_IMAGE_BOOT -DRB_E5" ... boot_app.cpp  (e5boot)
//   LDD_SRC=e5boot_ldd.cpp RB_DEFS="-DRB_E5" ... ramread_app.cpp               (e5logs)
// controls: AllocBoot, LoadChunk, Boot, ReadPhys. nothing else from runblob.
//
// differences to runblob_ldd.cpp (all checked against the E5 ROM export tables):
// - no TRamDefragRequest (ekern ordinals 880-885 are empty on 9.3). AllocBoot
//   retries the AllocPhysicalRam ladder with sleeps in between, then falls back
//   to ClaimPhysicalRam on a window that is free right now
// - the headers come from driver/stage-e5 (the ^3 DObject virtuals removed)
// - Boot() does NO hardware prep. the E6 PrepareHw writes clock gates, the bt
//   uart and both watchdogs through 0xc6000000+ addresses that are only known
//   from the E6. on the E5 Symbian kicked the soc counter (~13 s) and the pmic
//   (48 s) every 3 s, so >=10 s are left at the handoff, linux's wdt driver has
//   to take over in that time. no ISASW handoff block either (E6 modem sw)
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <kernel/cache.h>
#include <platform.h> // DPlatChunkHw, Epoc::AllocPhysicalRam, EMapAttr*
#include "runblob.h"
#include "tramp_boot.hdr" // KTrampBoot[], KTrampBootLen (kernel handoff)

typedef void (*TBootFn)(TUint32 aEntry, TUint32 aMach, TUint32 aDtb, TUint32 aWdog);

// ram log: linux mirrors printk here (earlycon=ramcon,<phys>). must stay out of
// the boot chunk and below the superpage at 0x1fd00000. the e5rom probe's free
// map decides the final value, build with -DRB_LOGPHYS=0x........ to change it
#ifndef RB_LOGPHYS
#define RB_LOGPHYS 0x1f000000u
#endif

// cp15 helpers (arm, supervisor), same as runblob_ldd.cpp
static TUint32 Ttbr0(){TUint32 v; asm volatile("mrc p15,0,%0,c2,c0,0":"=r"(v)); return v;}
static TUint32 Ttbcr(){TUint32 v; asm volatile("mrc p15,0,%0,c2,c0,2":"=r"(v)); return v;}
static void CleanDCacheMVA(TLinAddr a){asm volatile("mcr p15,0,%0,c7,c10,1"::"r"(a));}
static void Dsb(){asm volatile("mcr p15,0,%0,c7,c10,4"::"r"(0));}
static void InvTlbAll(){asm volatile("mcr p15,0,%0,c8,c7,0"::"r"(0));}
static void Isb(){asm volatile("mcr p15,0,%0,c7,c5,4"::"r"(0));}

// identity-maps a 1 mb section covering phys aP as normal executable in TTBR0.
// returns the L1 chunk + index + old descriptor, or NULL
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

class DE5BootChan : public DLogicalChannelBase{
	public:
		DE5BootChan() : iBootChunk(NULL), iBootPhys(0), iBootVirt(0), iBootSize(0), iBootAlloc(0){}
		// a booter that quits after AllocBoot must give the block back
		virtual ~DE5BootChan(){ReleaseBoot();}
		void ReleaseBoot(){
			if(iBootChunk){iBootChunk->Close(NULL); iBootChunk=NULL;}
			if(iBootAlloc){Epoc::FreePhysicalRam(iBootPhys, iBootAlloc); iBootAlloc=0;}
			iBootSize=0;
		}
		DPlatChunkHw* iBootChunk;
		TPhysAddr iBootPhys;
		TLinAddr iBootVirt;
		TUint32 iBootSize; // mapped
		TInt iBootAlloc;   // allocated/claimed, >= iBootSize
	protected:
		virtual TInt DoCreate(TInt, const TDesC8*, const TVersion&){return KErrNone;}
		virtual TInt Request(TInt aFunction, TAny* a1, TAny* a2);
		TInt AllocBoot(TAny* a1, TAny* a2);
		TInt ReadPhys(TUint32 aPhys, TAny* aDes);
};

// same ladder and window as e6bfix (the E5 kernel gets the same in-place Image
// layout): biggest block first, base in [KLo, KHi], end <= the ram log
static const TUint32 KSizes[]={0xa000000u, 0x9000000u, 0x8000000u, 0x7000000u, 0x6000000u, 0x5000000u, 0x4000000u, 0x3000000u, 0x2000000u};
static const TUint32 KLo=0x11600000u, KHi=0x1a000000u;
static const TInt KNSizes=(TInt)(sizeof(KSizes)/sizeof(KSizes[0]));

TInt DE5BootChan::AllocBoot(TAny* a1, TAny* a2){
	TUint32 req=0;
	TInt rr=Kern::ThreadRawRead(&Kern::CurrentThread(), a1, &req, 4);
	if(rr!=KErrNone){return rr;}
	ReleaseBoot();
	TUint32 size=0;
	TInt r=KErrNoMemory;
#ifdef RB_IMAGE_BOOT
	// 1. ladder. out-of-window blocks are held so the next size lands elsewhere
	for(TInt round=0; r!=KErrNone && round<3; ++round){
		if(round>0){NKern::Sleep(500);} // let symbian drop what it can, no defrag on 9.3
		TPhysAddr held[4];
		TUint32 heldsz[4];
		TInt nheld=0;
		for(TInt k=0; r!=KErrNone && k<KNSizes; ++k){
			TPhysAddr p=0;
			if(Epoc::AllocPhysicalRam((TInt)KSizes[k], p, 21)!=KErrNone){continue;}
			if(p>=KLo && p<=KHi && p+KSizes[k]<=RB_LOGPHYS){iBootPhys=p; size=KSizes[k]; r=KErrNone;}
			else if(nheld<4){held[nheld]=p; heldsz[nheld++]=KSizes[k];}
			else{Epoc::FreePhysicalRam(p, (TInt)KSizes[k]);}
		}
		for(TInt i=0; i<nheld; ++i){Epoc::FreePhysicalRam(held[i], (TInt)heldsz[i]);}
	}
	// 2. claim a window that is free right now, highest base first. Claim only
	// succeeds on pages the ram allocator has free, so nothing live is taken
	for(TInt k=0; r!=KErrNone && k<KNSizes; ++k){
		for(TUint32 p=KHi; r!=KErrNone && p>=KLo; p-=0x100000u){
			if(p+KSizes[k]>RB_LOGPHYS){continue;}
			if(Epoc::ClaimPhysicalRam(p, (TInt)KSizes[k])==KErrNone){iBootPhys=p; size=KSizes[k]; r=KErrNone;}
		}
	}
	if(r!=KErrNone){return KErrNoMemory;}
	// everything stays allocated (no symbian buffers in linux's ram), only the
	// first 32 mb (Image + dtb) get mapped
	TInt alloc=(TInt)size;
	if(size>0x2000000u){size=0x2000000u;}
#else
	size=(req+0xFFFFFu)&~0xFFFFFu;
	r=Epoc::AllocPhysicalRam((TInt)size, iBootPhys, 20);
	if(r!=KErrNone){return r;}
	TInt alloc=(TInt)size;
#endif
	r=DPlatChunkHw::New(iBootChunk, iBootPhys, (TInt)size, EMapAttrSupRw|EMapAttrCachedMax);
	if(r!=KErrNone){
		Epoc::FreePhysicalRam(iBootPhys, alloc);
		iBootPhys=0;
		return r;
	}
	iBootVirt=iBootChunk->LinearAddress();
	iBootSize=size;
	iBootAlloc=alloc;
	TUint32 out=(TUint32)iBootPhys; // the app passes a TUint32
	return Kern::ThreadRawWrite(&Kern::CurrentThread(), a2, &out, 4);
}

// maps aPhys.. (page aligned down) and copies the user descriptor's max length
// out of it. plain map first, if the mapper refuses a page the ram allocator
// owns, claim it for the copy (works when symbian holds it free, which is the
// whole point of picking a free log address)
TInt DE5BootChan::ReadPhys(TUint32 aPhys, TAny* aDes){
	TInt maxlen=Kern::ThreadGetDesMaxLength(&Kern::CurrentThread(), aDes);
	if(maxlen<=0){return KErrArgument;}
	TUint32 len=(TUint32)maxlen;
	TUint32 pageoff=aPhys&0xfff;
	TUint32 base=aPhys&~0xfffu;
	TUint32 msize=(pageoff+len+0xfff)&~0xfffu;
	TBool claimed=EFalse;
	DPlatChunkHw* c=NULL;
	TInt r=DPlatChunkHw::New(c, base, (TInt)msize, EMapAttrSupRw|EMapAttrCachedMax);
	if(r!=KErrNone){
		if(Epoc::ClaimPhysicalRam(base, (TInt)msize)!=KErrNone){return r;}
		claimed=ETrue;
		r=DPlatChunkHw::New(c, base, (TInt)msize, EMapAttrSupRw|EMapAttrCachedMax);
		if(r!=KErrNone){Epoc::FreePhysicalRam(base, (TInt)msize); return r;}
	}
	const TUint8* src=(const TUint8*)(c->LinearAddress()+pageoff);
	TUint8* stage=(TUint8*)Kern::Alloc(0x1000);
	if(!stage){
		c->Close(NULL);
		if(claimed){Epoc::FreePhysicalRam(base, (TInt)msize);}
		return KErrNoMemory;
	}
	TUint32 done=0;
	r=KErrNone;
	while(done<len){
		TUint32 n=len-done; if(n>0x1000){n=0x1000;}
		for(TUint32 i=0; i<n; ++i){stage[i]=src[done+i];}
		TPtrC8 p8(stage, (TInt)n);
		r=Kern::ThreadDesWrite(&Kern::CurrentThread(), aDes, p8, (TInt)done);
		if(r!=KErrNone){break;}
		done+=n;
	}
	Kern::Free(stage);
	c->Close(NULL);
	if(claimed){Epoc::FreePhysicalRam(base, (TInt)msize);}
	return r;
}

TInt DE5BootChan::Request(TInt aFunction, TAny* a1, TAny* a2){
	if(aFunction==RRunBlob::EControlAllocBoot){return AllocBoot(a1, a2);}

	if(aFunction==RRunBlob::EControlReadPhys){return ReadPhys((TUint32)a1, a2);}

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

		NKern::DisableAllInterrupts(); // no symbian code runs after this
		TBootFn fn=(TBootFn)sp;
		fn((TUint32)iBootPhys+b.iZImageOff, b.iMach, (TUint32)iBootPhys+b.iDtbOff, b.iWdog); // never returns
		return KErrNone; // unreachable
	}
	return KErrNotSupported;
}

class DE5BootFactory : public DLogicalDevice{
	public:
		DE5BootFactory(){iVersion=TVersion(1, 0, 0); iParseMask=0;}
		virtual TInt Install(){return SetName(&KRunBlobName);}
		virtual void GetCaps(TDes8& aDes) const{
			TVersion v=iVersion;
			Kern::InfoCopy(aDes, (const TUint8*)&v, sizeof(v));
		}
		virtual TInt Create(DLogicalChannelBase*& aChannel){
			aChannel=new DE5BootChan;
			return aChannel ? KErrNone : KErrNoMemory;
		}
};

DECLARE_STANDARD_LDD(){
	return new DE5BootFactory;
}
