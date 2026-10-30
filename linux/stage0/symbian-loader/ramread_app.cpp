// reads the ramoops kernel-log region back after a warm watchdog reset.
// linux mirrors all printk into phys 0x1f000000 (reserved-memory ramoops, see
// linux/boot/e6.dts); the soc watchdog reset is warm so the ram survives, and
// this dumps 1 mb from there to E:\klog.bin. run it right after a linux boot
// attempt rebooted the phone. parse on the pc with tools/parse_klog.py
#include <e32std.h>
#include <f32file.h>
#include "runblob.h"

_LIT(KLog, "E:\\ramread.log");
_LIT(KBin, "E:\\klog.bin");
#ifndef RB_LOGPHYS
#define RB_LOGPHYS 0x1f000000u
#endif
const TUint32 KPhys=RB_LOGPHYS;
const TUint32 KLen=0x100000u; // 1 mb
const TUint32 KStep=0x8000u;  // 32 kb per call

static void L(RFs& fs, const TDesC8& m){
	RFile f; TInt r=f.Open(fs, KLog, EFileWrite);
	if(r==KErrNotFound){r=f.Create(fs, KLog, EFileWrite);}
	if(r!=KErrNone){return;}
	TInt p=0; f.Seek(ESeekEnd, p); f.Write(m); f.Flush(); f.Close();
}

GLDEF_C TInt E32Main(){
	RFs fs;
	if(fs.Connect()!=KErrNone){return KErrGeneral;}
	TBuf8<96> b;
	b.Copy(_L8("ramread\n")); L(fs, b);

	User::FreeLogicalDevice(KRunBlobName);
	TInt rLoad=User::LoadLogicalDevice(KRunBlobLdd);
	b.Format(_L8("Load=%d\n"), rLoad); L(fs, b);

	if(rLoad==KErrNone || rLoad==KErrAlreadyExists){
		RRunBlob dev;
		if(dev.Open()==KErrNone){
			RFile out;
			if(out.Replace(fs, KBin, EFileWrite)==KErrNone){
				TUint8* mem=(TUint8*)User::Alloc(KStep);
				if(mem){
					TInt ok=0;
					for(TUint32 off=0; off<KLen; off+=KStep){
						TPtr8 buf(mem, 0, KStep);
						TInt r=dev.ReadPhys(KPhys+off, buf);
						if(r!=KErrNone){b.Format(_L8("ReadPhys@%x=%d\n"), KPhys+off, r); L(fs, b); break;}
						out.Write(buf); ok++;
					}
					b.Format(_L8("wrote %d blocks\n"), ok); L(fs, b);
					User::Free(mem);
				}
				out.Flush(); out.Close();
			}
			dev.Close();
		}
	}
#ifdef RB_E5
	fs.RSessionBase::Close(); // RFs::Close is not exported by the 9.3 efsrv
#else
	fs.Close();
#endif
	return KErrNone;
}