// E5-only, read-only: loads the e5l2pt ldd, writes the cb5/cb6 L2 tables + the
// ISASW RAM drive table to E:\e5l2pt.bin and the drive data (1 MB from va
// cb600000) to E:\e5l2pt-drv.bin, plus E:\e5l2pt.log
#include <e32std.h>
#include <f32file.h>
#include "runblob.h"

_LIT(KLog, "E:\\e5l2pt.log");
_LIT(KBin, "E:\\e5l2pt.bin");
_LIT(KDrv, "E:\\e5l2pt-drv.bin");

class RE5L2 : public RRunBlob{
	public:
		TInt Get(TDes8& aBuf, TInt aIndex){return DoControl(EControlRunBlob, (TAny*)&aBuf, (TAny*)aIndex);}
};

GLDEF_C TInt E32Main(){
	RFs fs;
	if(fs.Connect()!=KErrNone){return KErrGeneral;}
	TBuf8<400> log;
	log.Append(_L8("e5l2pt\n"));
	User::FreeLogicalDevice(KRunBlobName);
	TInt rLoad=User::LoadLogicalDevice(KRunBlobLdd);
	log.AppendFormat(_L8("Load=%d\n"), rLoad);
	TInt rOpen=KErrNotReady, rHead=KErrNotReady, bad=0, got=0;
	if(rLoad==KErrNone || rLoad==KErrAlreadyExists){
		RE5L2 dev;
		rOpen=dev.Open();
		log.AppendFormat(_L8("Open=%d\n"), rOpen);
		TUint8* mem=(TUint8*)User::Alloc(0x10000);
		if(rOpen==KErrNone && mem){
			TPtr8 buf(mem, 0, 0x10000);
			rHead=dev.Get(buf, 0);
			log.AppendFormat(_L8("Head=%d len=%d\n"), rHead, buf.Length());
			if(rHead==KErrNone && buf.Length()>=0x1000){
				const TUint32* h=(const TUint32*)buf.Ptr();
				log.AppendFormat(_L8("sp=%08x pd=%08x(%d) L1cb5=%08x(%d) L1cb6=%08x(%d) L2cb5=%d L2cb6=%d tab=%d\n"),
					h[1], h[3], h[2], h[5], h[4], h[7], h[6], h[8], h[9], h[10]);
				RFile f;
				if(f.Replace(fs, KBin, EFileWrite)==KErrNone){f.Write(buf); f.Close();}
			}
			RFile d;
			if(d.Replace(fs, KDrv, EFileWrite)==KErrNone){
				for(TInt i=1; i<=16; ++i){
					buf.SetLength(0);
					TInt r=dev.Get(buf, i);
					if(r!=KErrNone || buf.Length()!=0x10000){++bad; continue;}
					if(*(const TUint32*)buf.Ptr()>>20==0xbad){++bad;}
					d.Write(buf); ++got;
				}
				d.Close();
			}
			log.AppendFormat(_L8("drive chunks ok=%d bad=%d\n"), got, bad);
		}
		if(mem){User::Free(mem);}
		dev.Close();
	}
	RFile f;
	if(f.Replace(fs, KLog, EFileWrite)==KErrNone){f.Write(log); f.Close();}
	TBuf<48> note;
	note.Format(_L("e5l2pt H=%d ok=%d bad=%d"), rHead, got, bad);
	User::InfoPrint(note);
	fs.RSessionBase::Close(); // RFs::Close is not exported by the 9.3 efsrv
	return rHead;
}
