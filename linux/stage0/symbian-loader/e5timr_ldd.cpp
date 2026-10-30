// E5-only tick block probe. EControlTimedDiff (same runblob.h interface as the
// E6 one) but guarded: the window must lie inside ONE page that Symbian maps to
// phys 0x20033000 or 0x20040000 (checked with LinearToPhysical, so a wrong
// address is refused instead of aborting the cpu), <= 64 words. takes three
// snapshots ~0 / ~2 / ~100 ms apart so a counting register shows its rate:
//   out: [0] sig 0xe5000004 [1] va [2] count [3..5] NKern::TickCount A,B,C
//        [6..] A words, then B words, then C words
// plain reads only. NOTE: a register that clears on read would be disturbed
// (0x200401c4, which the tick isr reads, is harmless to read).
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <platform.h>
#include "runblob.h"

class DE5TimrChan : public DLogicalChannelBase{
	public:
		virtual ~DE5TimrChan(){}
	protected:
		virtual TInt DoCreate(TInt, const TDesC8*, const TVersion&){return KErrNone;}
		virtual TInt Request(TInt aFunction, TAny* a1, TAny* a2);
};

TInt DE5TimrChan::Request(TInt aFunction, TAny* a1, TAny* a2){
	if(aFunction!=RRunBlob::EControlTimedDiff){return KErrNotSupported;}
	TFbFill p; // iPhys = virt, iBytes = word count
	TInt rr=Kern::ThreadRawRead(&Kern::CurrentThread(), a1, &p, sizeof(p));
	if(rr!=KErrNone){return rr;}
	TUint count=p.iBytes;
	if(count==0 || count>64){return KErrArgument;}
	TUint32 va=p.iPhys;
	if(va&3 || ((va&0xfff)+count*4)>0x1000){return KErrArgument;}
	TPhysAddr pa=Epoc::LinearToPhysical(va&~0xfffu);
	if(pa!=0x20033000u && pa!=0x20040000u){return KErrNotSupported;}
	TUint32* o=(TUint32*)Kern::Alloc((6+count*3)*4);
	if(!o){return KErrNoMemory;}
	volatile TUint32* mm=(volatile TUint32*)va;
	o[0]=0xe5000004u; o[1]=va; o[2]=count;
	o[3]=NKern::TickCount();
	for(TUint i=0; i<count; ++i){o[6+i]=mm[i];}
	NKern::Sleep(2);
	o[4]=NKern::TickCount();
	for(TUint i=0; i<count; ++i){o[6+count+i]=mm[i];}
	NKern::Sleep(98);
	o[5]=NKern::TickCount();
	for(TUint i=0; i<count; ++i){o[6+2*count+i]=mm[i];}
	TPtrC8 src((const TUint8*)o, (TInt)((6+count*3)*4));
	TInt r=Kern::ThreadDesWrite(&Kern::CurrentThread(), a2, src, 0);
	Kern::Free(o);
	return r;
}

class DE5TimrFactory : public DLogicalDevice{
	public:
		DE5TimrFactory(){iVersion=TVersion(1, 0, 0); iParseMask=0;}
		virtual TInt Install(){return SetName(&KRunBlobName);}
		virtual void GetCaps(TDes8& aDes) const{
			TVersion v=iVersion;
			Kern::InfoCopy(aDes, (const TUint8*)&v, sizeof(v));
		}
		virtual TInt Create(DLogicalChannelBase*& aChannel){
			aChannel=new DE5TimrChan;
			return aChannel ? KErrNone : KErrNoMemory;
		}
};

DECLARE_STANDARD_LDD(){
	return new DE5TimrFactory;
}
