// E5-only, read-only: where do the ISASW SBIF RAM drives live in physical RAM?
// Linux overwrites their L2 table (phys 0x11609400 at the handoff), so read it
// while Symbian runs. Request(EControlRunBlob, buf, index):
//   index 0 -> 0x1000 bytes:
//     0x000 sig 0xe5000003, superpage, page dir va, L1[cb5], L1[cb6], read results
//     0x100 L2 table of va cb500000 (0x400)   0x500 L2 table of va cb600000 (0x400)
//     0x900 ISASW RAM drive table (va c6823f3c, 0x20)
//   index 1..16 -> 0x10000 bytes of the drive data from va cb600000 + (index-1)*0x10000
// SafeRead everywhere, so a bad address returns an error instead of aborting.
#include <kernel/kernel.h>
#include <kernel/kern_priv.h>
#include <platform.h>
#include "runblob.h"

const TUint32 KVa5=0xcb500000u, KVa6=0xcb600000u, KDrvTable=0xc6823f3cu;
const TInt KHead=0x1000, KChunk=0x10000;

class DE5L2Chan : public DLogicalChannelBase{
	public:
		virtual ~DE5L2Chan(){}
	protected:
		virtual TInt DoCreate(TInt, const TDesC8*, const TVersion&){return KErrNone;}
		virtual TInt Request(TInt aFunction, TAny* a1, TAny* a2);
};

// copies the 1 KB coarse table behind L1 descriptor aL1 into aOut, returns 0 or an error
static TInt ReadL2(TUint32 aL1, TUint8* aOut){
	if((aL1&3)!=1){return -100-(TInt)(aL1&3);}
	TPhysAddr pa=aL1&~0x3ffu;
	DPlatChunkHw* c=NULL;
	TInt r=DPlatChunkHw::New(c, pa&~0xfffu, 0x1000, EMapAttrSupRo|EMapAttrFullyBlocking);
	if(r!=KErrNone){return r;}
	TInt s=Kern::SafeRead((const TAny*)(c->LinearAddress()+(pa&0xfffu)), aOut, 0x400) ? KErrAbort : KErrNone;
	c->Close(NULL);
	return s;
}

TInt DE5L2Chan::Request(TInt aFunction, TAny* a1, TAny* a2){
	if(aFunction!=RRunBlob::EControlRunBlob){return KErrNotSupported;}
	TInt index=(TInt)a2;
	if(index<0 || index>16){return KErrArgument;}
	TInt size=index ? KChunk : KHead;
	TUint8* b=(TUint8*)Kern::Alloc(size);
	if(!b){return KErrNoMemory;}
	memclr(b, size);
	TUint32* h=(TUint32*)b;
	if(index==0){
		TUint32 sp=(TUint32)&Kern::SuperPage(), pd=0;
		h[0]=0xe5000003u;
		h[1]=sp;
		h[2]=(TUint32)Kern::SafeRead((const TAny*)(sp+0xc), &pd, 4);
		h[3]=pd;
		h[4]=(TUint32)Kern::SafeRead((const TAny*)(pd+(KVa5>>20)*4), &h[5], 4);	// h[5]=L1[cb5]
		h[6]=(TUint32)Kern::SafeRead((const TAny*)(pd+(KVa6>>20)*4), &h[7], 4);	// h[7]=L1[cb6]
		h[8]=(TUint32)ReadL2(h[5], b+0x100);
		h[9]=(TUint32)ReadL2(h[7], b+0x500);
		h[10]=(TUint32)Kern::SafeRead((const TAny*)KDrvTable, b+0x900, 0x20);
	}else{
		TInt s=Kern::SafeRead((const TAny*)(KVa6+(index-1)*KChunk), b, KChunk) ? KErrAbort : KErrNone;
		if(s){memclr(b, size); h[0]=0xbad00000u|(TUint32)(s&0xffff);}
	}
	TPtrC8 src(b, size);
	TInt r=Kern::ThreadDesWrite(&Kern::CurrentThread(), a1, src, 0);
	Kern::Free(b);
	return r;
}

class DE5L2Factory : public DLogicalDevice{
	public:
		DE5L2Factory(){iVersion=TVersion(1, 0, 0); iParseMask=0;}
		virtual TInt Install(){return SetName(&KRunBlobName);}
		virtual void GetCaps(TDes8& aDes) const{
			TVersion v=iVersion;
			Kern::InfoCopy(aDes, (const TUint8*)&v, sizeof(v));
		}
		virtual TInt Create(DLogicalChannelBase*& aChannel){
			aChannel=new DE5L2Chan;
			return aChannel ? KErrNone : KErrNoMemory;
		}
};

DECLARE_STANDARD_LDD(){
	return new DE5L2Factory;
}
