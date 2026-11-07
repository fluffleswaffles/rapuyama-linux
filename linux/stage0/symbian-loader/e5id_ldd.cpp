// E5-only cpu id reader. answers EControlRunBlob with the same word layout as
// the runblob blob (tools/parse_sysinfo.py) but only reads registers that are
// always valid in supervisor mode on ARMv6: MIDR, CTR, TLBTR, SCTLR, ACTLR.
// no exec memory, no RamDefrag, nothing the 9.3 ekern lacks.
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include "runblob.h"

class DE5IdChan : public DLogicalChannelBase{
	public:
		virtual ~DE5IdChan(){}
	protected:
		virtual TInt DoCreate(TInt, const TDesC8*, const TVersion&){return KErrNone;}
		virtual TInt Request(TInt aFunction, TAny* a1, TAny* a2);
};

TInt DE5IdChan::Request(TInt aFunction, TAny* a1, TAny*){
	if(aFunction!=RRunBlob::EControlRunBlob){return KErrNotSupported;}
	TUint32 w[32];
	for(TInt i=0; i<32; ++i){w[i]=0;}
	w[0]=0xe5000001u; // sig
	TUint32 v;
	asm volatile("mrs %0,cpsr":"=r"(v)); w[3]=v;
	asm volatile("mrc p15,0,%0,c0,c0,0":"=r"(v)); w[4]=v; // MIDR
	asm volatile("mrc p15,0,%0,c0,c0,1":"=r"(v)); w[5]=v; // CTR
	asm volatile("mrc p15,0,%0,c0,c0,3":"=r"(v)); w[6]=v; // TLBTR
	asm volatile("mrc p15,0,%0,c1,c0,0":"=r"(v)); w[19]=v; // SCTLR
	asm volatile("mrc p15,0,%0,c1,c0,1":"=r"(v)); w[20]=v; // ACTLR
	TPtrC8 src((const TUint8*)w, sizeof(w));
	return Kern::ThreadDesWrite(&Kern::CurrentThread(), a1, src, 0);
}

class DE5IdFactory : public DLogicalDevice{
	public:
		DE5IdFactory(){iVersion=TVersion(1, 0, 0); iParseMask=0;}
		virtual TInt Install(){return SetName(&KRunBlobName);}
		virtual void GetCaps(TDes8& aDes) const{
			TVersion v=iVersion;
			Kern::InfoCopy(aDes, (const TUint8*)&v, sizeof(v));
		}
		virtual TInt Create(DLogicalChannelBase*& aChannel){
			aChannel=new DE5IdChan;
			return aChannel ? KErrNone : KErrNoMemory;
		}
};

DECLARE_STANDARD_LDD(){
	return new DE5IdFactory;
}
