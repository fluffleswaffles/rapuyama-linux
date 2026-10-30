// E5-only: loads the e5rmap ldd, writes the bootstrap bank list + superpage to
// E:\e5rmap.bin plus a short E:\e5rmap.log
#include <e32std.h>
#include <f32file.h>
#include "runblob.h"

_LIT(KLog, "E:\\e5rmap.log");
_LIT(KBin, "E:\\e5rmap.bin");

static void W(RFs& fs, const TDesC& aName, const TDesC8& aData){
	RFile f;
	if(f.Replace(fs, aName, EFileWrite)==KErrNone){f.Write(aData); f.Close();}
}

GLDEF_C TInt E32Main(){
	RFs fs;
	if(fs.Connect()!=KErrNone){return KErrGeneral;}
	TBuf8<160> log;
	log.Append(_L8("e5rmap\n"));
	User::FreeLogicalDevice(KRunBlobName);
	TInt rLoad=User::LoadLogicalDevice(KRunBlobLdd);
	log.AppendFormat(_L8("Load=%d\n"), rLoad);
	TInt rOpen=KErrNotReady, rRun=KErrNotReady;
	if(rLoad==KErrNone || rLoad==KErrAlreadyExists){
		RRunBlob dev;
		rOpen=dev.Open();
		log.AppendFormat(_L8("Open=%d\n"), rOpen);
		if(rOpen==KErrNone){
			TUint8* mem=(TUint8*)User::Alloc(0x320);
			if(mem){
				TPtr8 buf(mem, 0, 0x320);
				rRun=dev.Run(buf);
				log.AppendFormat(_L8("Run=%d len=%d\n"), rRun, buf.Length());
				if(rRun==KErrNone && buf.Length()>=0x20){
					const TUint32* h=(const TUint32*)buf.Ptr();
					log.AppendFormat(_L8("sp=%08x bankSR=%d spSR=%d\n"), h[1], h[3], h[4]);
					W(fs, KBin, buf);
				}
				User::Free(mem);
			}
			dev.Close();
		}
	}
	W(fs, KLog, log);
	TBuf<48> note;
	note.Format(_L("e5rmap L=%d O=%d R=%d"), rLoad, rOpen, rRun);
	User::InfoPrint(note);
	fs.RSessionBase::Close(); // RFs::Close is not exported by the 9.3 efsrv
	return rRun;
}
