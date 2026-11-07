// E5-only probe (release step 3): asks Symbian's own wlanhwinit.dll for the
// wl1251 data the way the wlan stack does: CWlanHwInit::NewL (perm data from
// the modem over ISC), GetMacAddress, GetHwInitData. Writes E:\e5wlan.log
// (each step flushed, so a panic shows how far it got) and the buffers:
// E:\e5wlan-init.bin (init block) + E:\e5wlan-fw.bin (nvs + fw buffer).
// Build with EXE_CAPS=all-tcb: wlanhwinit.dll has all caps but TCB, and a
// process can only load dlls with at least its own caps.
#include <e32std.h>
#include <f32file.h>

_LIT(KLog, "E:\\e5wlan.log");
_LIT(KInit, "E:\\e5wlan-init.bin");
_LIT(KFw, "E:\\e5wlan-fw.bin");
_LIT(KDll, "wlanhwinit.dll");
_LIT(KRomDll, "Z:\\sys\\bin\\wlanhwinit.dll");

// export ordinals (eabi/wlanhwinitU.DEF, same 14 exports in the E5 rom)
const TInt KOrdGetHwInitData=1, KOrdGetMacAddress=3, KOrdNewL=6, KOrdDelete=7;
struct TMac{TUint8 b[6];};
class CWlanHwInit;
typedef CWlanHwInit* (*TNewL)();
typedef TInt (*TGetMac)(CWlanHwInit*, TMac&);
typedef void (*TGetInit)(CWlanHwInit*, const TUint8**, TUint&, const TUint8**, TUint&);
typedef void (*TDelete)(CWlanHwInit*);

static RFs gFs;
static RFile gLog;
static void Lf(TRefByValue<const TDesC8> f, ...){
	VA_LIST l;
	VA_START(l, f);
	TBuf8<160> b;
	b.FormatList(f, l);
	VA_END(l);
	TInt p=0; gLog.Seek(ESeekEnd, p); gLog.Write(b); gLog.Flush();
}
static TInt W(const TDesC& aName, const TUint8* aData, TUint aLen){
	RFile f;
	TInt r=f.Replace(gFs, aName, EFileWrite);
	if(r==KErrNone){r=f.Write(TPtrC8(aData, aLen)); f.Close();}
	return r;
}

GLDEF_C TInt E32Main(){
	if(gFs.Connect()!=KErrNone){return KErrGeneral;}
	if(gLog.Replace(gFs, KLog, EFileWrite)!=KErrNone){gFs.RSessionBase::Close(); return KErrGeneral;}
	Lf(_L8("e5wlan\n"));
	Lf(_L8("rom file=%08x\n"), (TUint)gFs.IsFileInRom(KRomDll));
	CTrapCleanup* tc=CTrapCleanup::New(); // NewL pushes on the cleanup stack
	Lf(_L8("cleanup=%08x\n"), (TUint)tc);

	RLibrary lib;
	TInt r=lib.Load(KDll);
	Lf(_L8("Load=%d\n"), r);
	if(r==KErrNone){
		TNewL newl=(TNewL)lib.Lookup(KOrdNewL);
		TGetMac getmac=(TGetMac)lib.Lookup(KOrdGetMacAddress);
		TGetInit getinit=(TGetInit)lib.Lookup(KOrdGetHwInitData);
		TDelete del=(TDelete)lib.Lookup(KOrdDelete);
		Lf(_L8("NewL=%08x mac=%08x init=%08x del=%08x\n"), (TUint)newl, (TUint)getmac, (TUint)getinit, (TUint)del);
		Lf(_L8("calling NewL (a leave here = panic, log stops)\n"));
		CWlanHwInit* hw=newl(); // talks to the modem (perm data)
		Lf(_L8("hw=%08x\n"), (TUint)hw);
		if(hw){
			TMac mac; Mem::FillZ(&mac, sizeof(mac));
			r=getmac(hw, mac);
			Lf(_L8("GetMacAddress=%d %02x:%02x:%02x:%02x:%02x:%02x\n"), r,
				mac.b[0], mac.b[1], mac.b[2], mac.b[3], mac.b[4], mac.b[5]);
			const TUint8* init=0; const TUint8* fw=0; TUint initLen=0, fwLen=0;
			getinit(hw, &init, initLen, &fw, fwLen);
			Lf(_L8("GetHwInitData init=%08x len=%d fw=%08x len=%d\n"), (TUint)init, initLen, (TUint)fw, fwLen);
			if(fw && fwLen>=8){
				const TUint32* h=(const TUint32*)fw;
				Lf(_L8("fw buf words %08x %08x\n"), h[0], h[1]);
			}
			if(init && initLen){Lf(_L8("write init=%d\n"), W(KInit, init, initLen));}
			if(fw && fwLen){Lf(_L8("write fw=%d\n"), W(KFw, fw, fwLen));}
			del(hw);
			Lf(_L8("deleted\n"));
		}
		lib.Close();
	}
	Lf(_L8("done\n"));
	delete tc;
	gLog.Close();
	gFs.RSessionBase::Close();
	return KErrNone;
}
