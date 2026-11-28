// e6bfix boot block policy: which physical block of symbian's ram linux boots in
#ifndef BOOT_ALLOC_H
#define BOOT_ALLOC_H

const TUint32 KBootLo=0x11600000u; // below: ram the modem code runs in
const TUint32 KBootHi=0x1a000000u; // highest block start (gfx dma reach)
const TUint32 KBootEnd=0x1f000000u; // ramcon reservation
const TUint32 KBootAlign=21; // 2 mb
const TInt KBootRounds=3; // 0 plain, 1 defrag, 2 empty the zones in the window
const TInt KBootHold=16;
const TInt KBootLogMax=96;

// biggest first: then nearly all of linux's ram is ram symbian gave away
static const TUint32 KBootSizes[]={0xa000000u, 0x9000000u, 0x8000000u, 0x7000000u, 0x6000000u, 0x5000000u, 0x4000000u, 0x3000000u, 0x2000000u};

enum TBootVerdict{EBootTaken, EBootTooHigh, EBootTooLow, EBootPastEnd, EBootNoBlock, EBootDropped, EBootCompact};

// one line of E:\boot.log per allocation attempt (and per compaction step)
struct TBootAllocEntry{
	TUint32 iRound;
	TUint32 iSize; // EBootCompact: the step's own value (zone id or 0)
	TUint32 iPhys;
	TInt32 iResult; // allocator / compaction result
	TUint32 iVerdict;
};

struct TBootAllocLog{
	TUint32 iCount; // entries, can exceed KBootLogMax (then the rest is lost)
	TUint32 iLowest; // lowest block symbian offered, 0xffffffff = none
	TBootAllocEntry iEntry[KBootLogMax];
};

struct TBootAllocOps{
	TAny* iCtx;
	TInt (*iAlloc)(TAny* aCtx, TUint32 aSize, TUint32& aPhys); // aligned to 1<<KBootAlign
	void (*iFree)(TAny* aCtx, TUint32 aPhys, TUint32 aSize);
	// before round aRound (1..KBootRounds-1): let symbian compact. logs its own steps
	void (*iCompact)(TAny* aCtx, TInt aRound, TBootAllocLog& aLog);
};

static inline void BootLog(TBootAllocLog& aLog, TUint32 aRound, TUint32 aSize, TUint32 aPhys, TInt aResult, TBootVerdict aVerdict){
	if(aLog.iCount<(TUint32)KBootLogMax){
		TBootAllocEntry& e=aLog.iEntry[aLog.iCount];
		e.iRound=aRound; e.iSize=aSize; e.iPhys=aPhys; e.iResult=aResult; e.iVerdict=aVerdict;
	}
	aLog.iCount++;
}

static inline TBootVerdict BootCheck(TUint32 aPhys, TUint32 aSize){
	if(aPhys<KBootLo){return EBootTooLow;}
	if(aPhys>KBootHi){return EBootTooHigh;}
	if(aPhys+aSize>KBootEnd || aPhys+aSize<aPhys){return EBootPastEnd;}
	return EBootTaken;
}

// KErrNone: aPhys/aSize is ours (the caller frees it). else everything was given back.
// aMaxSize: skip the sizes above it. e6bfix's hold (the cfw boot menu) only keeps
// 32 mb while symbian boots on; linux takes the rest back with e6.lowmem=1 anyway
static TInt BootAllocPick(const TBootAllocOps& aOps, TUint32& aPhys, TUint32& aSize, TBootAllocLog& aLog, TUint32 aMaxSize=0xffffffffu){
	aLog.iCount=0;
	aLog.iLowest=0xffffffffu;
	for(TInt round=0; round<KBootRounds; ++round){
		if(round>0){aOps.iCompact(aOps.iCtx, round, aLog);}
		TUint32 held[KBootHold], heldSize[KBootHold];
		TInt nheld=0, r=KErrNoMemory;
		for(TUint k=0; r!=KErrNone && k<sizeof(KBootSizes)/sizeof(KBootSizes[0]); ++k){
			TUint32 size=KBootSizes[k];
			if(size>aMaxSize){continue;}
			// the same size again while symbian still has blocks of it: they're all
			// held, so it has to offer the next one
			for(;;){
				TUint32 p=0;
				TInt a=aOps.iAlloc(aOps.iCtx, size, p);
				if(a!=KErrNone){BootLog(aLog, round, size, 0, a, EBootNoBlock); break;}
				if(p<aLog.iLowest){aLog.iLowest=p;}
				TBootVerdict v=BootCheck(p, size);
				if(v==EBootTaken){
					BootLog(aLog, round, size, p, KErrNone, v);
					aPhys=p; aSize=size; r=KErrNone;
					break;
				}
				if(nheld==KBootHold){
					aOps.iFree(aOps.iCtx, p, size);
					BootLog(aLog, round, size, p, KErrNone, EBootDropped);
					break;
				}
				BootLog(aLog, round, size, p, KErrNone, v);
				held[nheld]=p; heldSize[nheld++]=size;
			}
		}
		for(TInt i=0; i<nheld; ++i){aOps.iFree(aOps.iCtx, held[i], heldSize[i]);}
		if(r==KErrNone){return KErrNone;}
	}
	return KErrNoMemory;
}

#endif // BOOT_ALLOC_H
