// E5-only ROM phys base + free-RAM probe. no mmio, no mappings, just kernel
// api calls that exist in the 9.3 ekern (Epoc::LinearToPhysical ord 245,
// ClaimPhysicalRam 243, FreePhysicalRam 241). returns, in one buffer:
//   0x000: sig 0xe5000003, nRuns, claimBase, nBlocks, blockSize, ctlLo, ctlHi, nRuns2
//   0x020: nRuns * {virt, phys, pages} (max KMaxRuns), the logical ROM window
//          0x80000000..0x83200000 run-coded; pages with no mapping (paged-out
//          rom) end a run, so a gap is expected past the unpaged part
//   0x260: nBlocks bytes, one per blockSize of ram from claimBase:
//          0 = Claim ok (and freed again right away), else -code (1 = KErrNotFound ...)
//   then nRuns2 * {virt, phys, pages} (max KMaxRuns2), unfiltered sweep of the
//   kernel peripheral window 0xc6000000..0xc7000000 (ptdump's sweep hit its
//   200 entry cap before reaching it), at KRuns2Off = KMapOff + nBlocks rounded to 4
// Claim only succeeds on a FREE ram page, so a 0 byte = unused by symbian now.
// controls: ctlLo = Claim at 0x10000000 (kernel L1, must fail), ctlHi = Claim at
// 0x1fd00000 (superpage, must fail). if a control returns 0 the test is bogus.
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <platform.h>
#include "runblob.h"

const TLinAddr KRomVa=0x80000000u;
const TLinAddr KRomEnd=0x807e0000u; // unpaged rom only (0x7da000 + slack); the paged tail is scattered and blew the run cap
const TUint32 KStep=0x1000u;
const TInt KMaxRuns=48;
const TUint32 KClaimBase=0x1e000000u;
const TUint32 KClaimEnd=0x1fd00000u; // superpage and everything above stays alone
const TUint32 KBlk=0x10000u;         // 64 kb
const TInt KBlocks=(TInt)((KClaimEnd-KClaimBase)/KBlk); // 464
const TInt KRunsOff=0x20;
const TInt KMapOff=KRunsOff+KMaxRuns*12;
const TInt KRuns2Off=KMapOff+((KBlocks+3)&~3);
const TInt KMaxRuns2=32;
const TLinAddr KPerVa=0xc6000000u;
const TLinAddr KPerEnd=0xc7000000u;
const TInt KOut=KRuns2Off+KMaxRuns2*12;

class DE5RomChan : public DLogicalChannelBase{
	public:
		virtual ~DE5RomChan(){}
	protected:
		virtual TInt DoCreate(TInt, const TDesC8*, const TVersion&){return KErrNone;}
		virtual TInt Request(TInt aFunction, TAny* a1, TAny*);
};

// run-coded LinearToPhysical sweep of [aFrom,aTo), unmapped pages end a run
static TInt SweepRuns(TLinAddr aFrom, TLinAddr aTo, TUint32* aRuns, TInt aMax){
	const TUint32 KInvalid=0xFFFFFFFFu;
	TInt n=0;
	TLinAddr curV=0; TPhysAddr curP=KInvalid; TUint32 curN=0, expect=KInvalid;
	for(TLinAddr va=aFrom; va<aTo; va+=KStep){
		TPhysAddr pa=Epoc::LinearToPhysical(va);
		TBool mapped=(pa!=KInvalid);
		if(curN>0 && mapped && pa==expect){curN++;}
		else{
			if(curN>0 && n<aMax){aRuns[n*3+0]=(TUint32)curV; aRuns[n*3+1]=(TUint32)curP; aRuns[n*3+2]=curN; n++;}
			if(mapped){curV=va; curP=pa; curN=1;}
			else{curN=0;}
		}
		expect=mapped ? (pa+KStep) : KInvalid;
	}
	if(curN>0 && n<aMax){aRuns[n*3+0]=(TUint32)curV; aRuns[n*3+1]=(TUint32)curP; aRuns[n*3+2]=curN; n++;}
	return n;
}

// claim + immediate free; returns the Claim result
static TInt ClaimFree(TPhysAddr aP){
	TInt r=Epoc::ClaimPhysicalRam(aP, (TInt)KBlk);
	if(r==KErrNone){Epoc::FreePhysicalRam(aP, (TInt)KBlk);}
	return r;
}

TInt DE5RomChan::Request(TInt aFunction, TAny* a1, TAny*){
	if(aFunction!=RRunBlob::EControlRunBlob){return KErrNotSupported;}
	TUint8* b=(TUint8*)Kern::Alloc(KOut);
	if(!b){return KErrNoMemory;}
	memclr(b, KOut);
	TUint32* h=(TUint32*)b;
	TUint32* runs=(TUint32*)(b+KRunsOff);

	TInt nRuns=SweepRuns(KRomVa, KRomEnd, runs, KMaxRuns);
	TInt nRuns2=SweepRuns(KPerVa, KPerEnd, (TUint32*)(b+KRuns2Off), KMaxRuns2);

	h[0]=0xe5000003u;
	h[1]=(TUint32)nRuns;
	h[7]=(TUint32)nRuns2;
	h[2]=KClaimBase;
	h[3]=(TUint32)KBlocks;
	h[4]=KBlk;
	h[5]=(TUint32)ClaimFree(0x10000000u);
	h[6]=(TUint32)ClaimFree(KClaimEnd);
	for(TInt i=0; i<KBlocks; ++i){
		TInt r=ClaimFree(KClaimBase+(TUint32)i*KBlk);
		b[KMapOff+i]=(TUint8)(r==KErrNone ? 0 : (-r)&0xff);
	}

	TPtrC8 src(b, KOut);
	TInt r=Kern::ThreadDesWrite(&Kern::CurrentThread(), a1, src, 0);
	Kern::Free(b);
	return r;
}

class DE5RomFactory : public DLogicalDevice{
	public:
		DE5RomFactory(){iVersion=TVersion(1, 0, 0); iParseMask=0;}
		virtual TInt Install(){return SetName(&KRunBlobName);}
		virtual void GetCaps(TDes8& aDes) const{
			TVersion v=iVersion;
			Kern::InfoCopy(aDes, (const TUint8*)&v, sizeof(v));
		}
		virtual TInt Create(DLogicalChannelBase*& aChannel){
			aChannel=new DE5RomChan;
			return aChannel ? KErrNone : KErrNoMemory;
		}
};

DECLARE_STANDARD_LDD(){
	return new DE5RomFactory;
}
