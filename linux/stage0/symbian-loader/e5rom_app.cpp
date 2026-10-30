// E5-only: loads the e5rom ldd, writes the rom phys runs + free ram map to
// E:\e5rom.bin plus a short E:\e5rom.log
#include <e32std.h>
#include <f32file.h>
#include "runblob.h"

_LIT(KLog, "E:\\e5rom.log");
_LIT(KBin, "E:\\e5rom.bin");

static void W(RFs& fs, const TDesC& aName, const TDesC8& aData){
	RFile f;
	if(f.Replace(fs, aName, EFileWrite)==KErrNone){f.Write(aData); f.Close();}
}

GLDEF_C TInt E32Main(){
	RFs fs;
	if(fs.Connect()!=KErrNone){return KErrGeneral;}
	TBuf8<160> log;
	log.Append(_L8("e5rom\n"));
	User::FreeLogicalDevice(KRunBlobName);
	TInt rLoad=User::LoadLogicalDevice(KRunBlobLdd);
	log.AppendFormat(_L8("Load=%d\n"), rLoad);
	TInt rOpen=KErrNotReady, rRun=KErrNotReady;
	if(rLoad==KErrNone || rLoad==KErrAlreadyExists){
		RRunBlob dev;
		rOpen=dev.Open();
		log.AppendFormat(_L8("Open=%d\n"), rOpen);
		if(rOpen==KErrNone){
			TUint8* mem=(TUint8*)User::Alloc(0x600);
			if(mem){
				TPtr8 buf(mem, 0, 0x600);
				rRun=dev.Run(buf);
				log.AppendFormat(_L8("Run=%d len=%d\n"), rRun, buf.Length());
				if(rRun==KErrNone && buf.Length()>=0x20){
					const TUint32* h=(const TUint32*)buf.Ptr();
					log.AppendFormat(_L8("runs=%d ctlLo=%d ctlHi=%d\n"), h[1], (TInt)h[5], (TInt)h[6]);
					W(fs, KBin, buf);
				}
				User::Free(mem);
			}
			dev.Close();
		}
	}
	W(fs, KLog, log);
	TBuf<48> note;
	note.Format(_L("e5rom L=%d O=%d R=%d"), rLoad, rOpen, rRun);
	User::InfoPrint(note);
	fs.RSessionBase::Close(); // RFs::Close is not exported by the 9.3 efsrv
	return rRun;
}
