// E5-only RAM map reader. returns, in one buffer:
//   0x000: sig 0xe5000002, superpage addr, 0xc601b820 marker words (8 words)
//   0x020: 0x100 bytes of the bootstrap bank list (phys 0x30003820, kernel va c601b820)
//   0x120: 0x200 bytes of the kernel superpage
// plain memory only (the 0x30000000 area is shared RAM the bootstrap writes);
// SafeRead so a bad address returns an error instead of aborting.
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include "runblob.h"

const TUint32 KBankListVa=0xc601b820u; // phys 0x30003820 (ptdump: 0x30000000 -> c6018000, 4 pages)
const TInt KBankBytes=0x100;
const TInt KSpBytes=0x200;
const TInt KOut=0x20+KBankBytes+KSpBytes;

class DE5RmapChan : public DLogicalChannelBase{
	public:
		virtual ~DE5RmapChan(){}
	protected:
		virtual TInt DoCreate(TInt, const TDesC8*, const TVersion&){return KErrNone;}
		virtual TInt Request(TInt aFunction, TAny* a1, TAny*);
};

TInt DE5RmapChan::Request(TInt aFunction, TAny* a1, TAny*){
	if(aFunction!=RRunBlob::EControlRunBlob){return KErrNotSupported;}
	TUint8* b=(TUint8*)Kern::Alloc(KOut);
	if(!b){return KErrNoMemory;}
	memclr(b, KOut);
	TUint32* h=(TUint32*)b;
	h[0]=0xe5000002u;
	h[1]=(TUint32)&Kern::SuperPage();
	h[2]=KBankListVa;
	h[3]=(TUint32)Kern::SafeRead((const TAny*)KBankListVa, b+0x20, KBankBytes); // 0 = ok
	h[4]=(TUint32)Kern::SafeRead((const TAny*)&Kern::SuperPage(), b+0x20+KBankBytes, KSpBytes);
	TPtrC8 src(b, KOut);
	TInt r=Kern::ThreadDesWrite(&Kern::CurrentThread(), a1, src, 0);
	Kern::Free(b);
	return r;
}

class DE5RmapFactory : public DLogicalDevice{
	public:
		DE5RmapFactory(){iVersion=TVersion(1, 0, 0); iParseMask=0;}
		virtual TInt Install(){return SetName(&KRunBlobName);}
		virtual void GetCaps(TDes8& aDes) const{
			TVersion v=iVersion;
			Kern::InfoCopy(aDes, (const TUint8*)&v, sizeof(v));
		}
		virtual TInt Create(DLogicalChannelBase*& aChannel){
			aChannel=new DE5RmapChan;
			return aChannel ? KErrNone : KErrNoMemory;
		}
};

DECLARE_STANDARD_LDD(){
	return new DE5RmapFactory;
}
