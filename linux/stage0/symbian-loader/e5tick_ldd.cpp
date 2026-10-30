// E5-only dense tick counter sampler (EControlTimerProbe).
// samples back to back, irqs off: fc latch+value (0x20033350/54, 32768 Hz), A+0x20 (0x20033620), B+0x24 and B+0x28 (0x200401a4/a8).
// out: [0] sig 0xe5000005 [1] n [2] tick count [3..] n * {fc, a20, b24, b28}
// (old text follows)
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

class DE5TickChan : public DLogicalChannelBase{
	public:
		virtual ~DE5TickChan(){}
	protected:
		virtual TInt DoCreate(TInt, const TDesC8*, const TVersion&){return KErrNone;}
		virtual TInt Request(TInt aFunction, TAny* a1, TAny*);
};

const TInt KN=1024;

TInt DE5TickChan::Request(TInt aFunction, TAny* a1, TAny*){
	if(aFunction!=RRunBlob::EControlRunBlob){return KErrNotSupported;}
	// all three pages must be mapped where the ROM says, else refuse
	if(Epoc::LinearToPhysical(0xc6033000u)!=0x20033000u || Epoc::LinearToPhysical(0xc6040000u)!=0x20040000u){return KErrNotSupported;}
	TUint32* o=(TUint32*)Kern::Alloc((3+KN*4)*4);
	if(!o){return KErrNoMemory;}
	volatile TUint32* fc=(volatile TUint32*)0xc6033350u;
	volatile TUint32* a=(volatile TUint32*)0xc6033620u;
	volatile TUint32* b=(volatile TUint32*)0xc60401a4u;
	o[0]=0xe5000005u; o[1]=KN; o[2]=NKern::TickCount();
	TInt irq=NKern::DisableAllInterrupts();
	for(TInt i=0; i<KN; ++i){
		fc[1]=0xc6033354u; // latch, as ekern does
		o[3+i*4]=fc[0];
		o[4+i*4]=a[0];
		o[5+i*4]=b[0];
		o[6+i*4]=b[1];
	}
	NKern::RestoreInterrupts(irq);
	TPtrC8 src((const TUint8*)o, (3+KN*4)*4);
	TInt r=Kern::ThreadDesWrite(&Kern::CurrentThread(), a1, src, 0);
	Kern::Free(o);
	return r;
}

class DE5TickFactory : public DLogicalDevice{
	public:
		DE5TickFactory(){iVersion=TVersion(1, 0, 0); iParseMask=0;}
		virtual TInt Install(){return SetName(&KRunBlobName);}
		virtual void GetCaps(TDes8& aDes) const{
			TVersion v=iVersion;
			Kern::InfoCopy(aDes, (const TUint8*)&v, sizeof(v));
		}
		virtual TInt Create(DLogicalChannelBase*& aChannel){
			aChannel=new DE5TickChan;
			return aChannel ? KErrNone : KErrNoMemory;
		}
};

DECLARE_STANDARD_LDD(){
	return new DE5TickFactory;
}
