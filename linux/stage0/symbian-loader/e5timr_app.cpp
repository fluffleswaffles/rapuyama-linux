// E5-only: reads the two tick register windows one at a time and saves each to
// disk before the next (E:\e5timA.bin = 0x20033600.., E:\e5timB.bin =
// 0x20040180..), log in E:\e5timr.log. see e5timr_ldd.cpp
#include <e32std.h>
#include <f32file.h>
#include "runblob.h"

_LIT(KLog, "E:\\e5timr.log");
_LIT(KBinA, "E:\\e5timA.bin");
_LIT(KBinB, "E:\\e5timB.bin");
const TUint32 KCount=32;
const TInt KBytes=(6+KCount*3)*4;

static void W(RFs& fs, const TDesC& aName, const TDesC8& aData){
	RFile f;
	if(f.Replace(fs, aName, EFileWrite)==KErrNone){f.Write(aData); f.Close();}
}

GLDEF_C TInt E32Main(){
	RFs fs;
	if(fs.Connect()!=KErrNone){return KErrGeneral;}
	TBuf8<160> log;
	log.Append(_L8("e5timr\n"));
	User::FreeLogicalDevice(KRunBlobName);
	TInt rLoad=User::LoadLogicalDevice(KRunBlobLdd);
	log.AppendFormat(_L8("Load=%d\n"), rLoad);
	W(fs, KLog, log);
	TInt rA=KErrNotReady, rB=KErrNotReady;
	if(rLoad==KErrNone || rLoad==KErrAlreadyExists){
		RRunBlob dev;
		if(dev.Open()==KErrNone){
			TUint8* mem=(TUint8*)User::Alloc(KBytes);
			if(mem){
				TPtr8 buf(mem, 0, KBytes);
				rA=dev.TimedDiff(0xc6033600u, KCount, 0, buf);
				if(rA==KErrNone){W(fs, KBinA, buf);}
				log.AppendFormat(_L8("A(0x20033600)=%d len=%d\n"), rA, buf.Length());
				W(fs, KLog, log); // saved before the second window
				if(rA==KErrNone){
					buf.Zero();
					rB=dev.TimedDiff(0xc6040180u, KCount, 0, buf);
					if(rB==KErrNone){W(fs, KBinB, buf);}
					log.AppendFormat(_L8("B(0x20040180)=%d len=%d\n"), rB, buf.Length());
					W(fs, KLog, log);
				}
				User::Free(mem);
			}
			dev.Close();
		}
	}
	TBuf<48> note;
	note.Format(_L("e5timr L=%d A=%d B=%d"), rLoad, rA, rB);
	User::InfoPrint(note);
	fs.RSessionBase::Close(); // RFs::Close is not exported by the 9.3 efsrv
	return rA|rB;
}
