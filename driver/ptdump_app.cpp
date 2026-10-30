// minimal user-side client. loads the ldd, pulls cp15 regs and raw L1 table.
// writes it to *drive*\ptdump.bin and a more or less readable .log. mostly
// tries E: and only then C:
#include <e32std.h>
#include <f32file.h>
#include "ptdump.h"

_LIT(KLddFile, "ptdump.ldd");

// candidates (E: first bc it's readable by my pc)
_LIT(KBinE, "E:\\ptdump.bin");  _LIT(KLogE, "E:\\ptdump.log");
_LIT(KBinF, "F:\\ptdump.bin");  _LIT(KLogF, "F:\\ptdump.log");
_LIT(KBinC, "C:\\ptdump.bin");  _LIT(KLogC, "C:\\ptdump.log");

static TInt WriteFile(const TDesC& aName, const TDesC8& aData){
		RFs fs;
		TInt r=fs.Connect();
		if(r!=KErrNone){return r;}
		RFile f;
		r=f.Replace(fs, aName, EFileWrite);
		if(r==KErrNone){r=f.Write(aData);f.Close();}
		fs.RSessionBase::Close(); // RFs::Close is not exported by the 9.3 efsrv
		return r;
	}

GLDEF_C TInt E32Main(){
	// short ascii log
	TBuf8<256> log;
	log.Append(_L8("ptdump\n"));

	// this supposed to work but it doesnt. should unload the cached driver in the kernel
	// but it DOESNT WORK FOR SOME REASON :cry:
	TInt rFree=User::FreeLogicalDevice(KPtDumpName);
	log.AppendFormat(_L8("FreeLogicalDevice=%d\n"), rFree);

	TInt rLoad=User::LoadLogicalDevice(KLddFile);
	log.AppendFormat(_L8("LoadLogicalDevice=%d\n"), rLoad);

	TInt rOpen=KErrNotReady, rRead=KErrNotReady, len=0;
	TUint8* mem=NULL;
	if(rLoad==KErrNone || rLoad == KErrAlreadyExists){
		RPtDump dev;
		rOpen=dev.Open();
		log.AppendFormat(_L8("Open=%d\n"), rOpen);
		if(rOpen==KErrNone){
			mem=(TUint8*)User::Alloc(KPtBufSize);
			if(mem){
				TPtr8 buf(mem, 0, KPtBufSize);
				rRead=dev.ReadL1(buf);
				len=buf.Length();
				log.AppendFormat(_L8("ReadL1=%d len=%d\n"), rRead, len);
				if(rRead==KErrNone && len>=24){
					const TUint32* h=(const TUint32*)buf.Ptr();
					log.AppendFormat(_L8("TTBR0=%08x TTBR1=%08x\n"), h[0], h[1]);
					log.AppendFormat(_L8("TTBCR=%08x SCTLR=%08x\n"), h[2], h[3]);
					log.AppendFormat(_L8("nRuns=%d nProbes=%d\n"), h[4], h[5]);
					log.AppendFormat(_L8("scanVirt=%08x scanPhys=%08x\n"), h[6], h[7]);
					log.AppendFormat(_L8("pagesProbed=%d primecell=%d faulted=%d\n"), h[8], h[9], h[10]);
					log.AppendFormat(_L8("len=%d\n"), len);
					}
				if(rRead==KErrNone && len>24){
						TInt rb=WriteFile(KBinE, buf);
						if(rb!=KErrNone){rb = WriteFile(KBinF, buf);}
						if(rb != KErrNone){rb = WriteFile(KBinC, buf);}
						log.AppendFormat(_L8("writeBin=%d\n"), rb);
					}
			}
			else{log.Append(_L8("Alloc failed\n"));}
			dev.Close();
		}
	}

	TInt rl=WriteFile(KLogE, log);
	if(rl!=KErrNone){rl = WriteFile(KLogF, log);}
	if(rl!=KErrNone){WriteFile(KLogC, log);}

	TBuf<64> note;
	note.Format(_L("ptdump L=%d O=%d R=%d n=%d"), rLoad, rOpen, rRead, len);
	User::InfoPrint(note);

	if(mem){User::Free(mem);}
	return rRead;
}