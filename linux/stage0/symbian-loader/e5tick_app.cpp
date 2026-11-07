// E5-only: loads the e5tick ldd, writes the dense counter samples to
// E:\e5tick.bin plus a short E:\e5tick.log
#include <e32std.h>
#include <f32file.h>
#include "runblob.h"

_LIT(KLog, "E:\\e5tick.log");
_LIT(KBin, "E:\\e5tick.bin");

static void W(RFs& fs, const TDesC& aName, const TDesC8& aData){
	RFile f;
	if(f.Replace(fs, aName, EFileWrite)==KErrNone){f.Write(aData); f.Close();}
}

GLDEF_C TInt E32Main(){
	RFs fs;
	if(fs.Connect()!=KErrNone){return KErrGeneral;}
	TBuf8<160> log;
	log.Append(_L8("e5tick\n"));
	User::FreeLogicalDevice(KRunBlobName);
	TInt rLoad=User::LoadLogicalDevice(KRunBlobLdd);
	log.AppendFormat(_L8("Load=%d\n"), rLoad);
	TInt rOpen=KErrNotReady, rRun=KErrNotReady;
	if(rLoad==KErrNone || rLoad==KErrAlreadyExists){
		RRunBlob dev;
		rOpen=dev.Open();
		log.AppendFormat(_L8("Open=%d\n"), rOpen);
		if(rOpen==KErrNone){
			TUint8* mem=(TUint8*)User::Alloc(0x4010);
			if(mem){
				TPtr8 buf(mem, 0, 0x4010);
				rRun=dev.Run(buf);
				log.AppendFormat(_L8("Run=%d len=%d\n"), rRun, buf.Length());
				if(rRun==KErrNone && buf.Length()>=0xc){
					const TUint32* h=(const TUint32*)buf.Ptr();
					log.AppendFormat(_L8("n=%d\n"), h[1]);
					W(fs, KBin, buf);
				}
				User::Free(mem);
			}
			dev.Close();
		}
	}
	W(fs, KLog, log);
	TBuf<48> note;
	note.Format(_L("e5tick L=%d O=%d R=%d"), rLoad, rOpen, rRun);
	User::InfoPrint(note);
	fs.RSessionBase::Close(); // RFs::Close is not exported by the 9.3 efsrv
	return rRun;
}
