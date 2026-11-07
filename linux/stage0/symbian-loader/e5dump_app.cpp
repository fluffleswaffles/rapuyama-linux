// E5-only: copies fixed RAM regions to E: with the ReadPhys control of
// e5boot_ldd.cpp (plain RAM reads, nothing else). the regions are the ones
// symbian maps next to its mmio in e5rom (c6300000, c6800000, c6b00000,
// c60f0000) plus the in-use hole below the superpage, to look for the modem
// software (ISASW) and the E5's periodic supervisor service offline.
// E:\e5d_<phys>.bin per region, progress in E:\e5dump.log (flushed per region)
#include <e32std.h>
#include <f32file.h>
#include "runblob.h"

struct TReg{TUint32 iPhys; TUint32 iLen;};
static const TReg KRegs[]={
	{0x10480000u, 0x003ae000u}, // c6300000 x942 pages
	{0x1fd50000u, 0x00280000u}, // c6800000 x640 pages
	{0x1ffe0000u, 0x00010000u}, // c6b00000 x16 pages
	{0x100ec000u, 0x00002000u}, // c60f0000 x2 pages
	{0x1fb60000u, 0x00180000u}, // in use at e5rom time, unidentified
};
const TUint32 KStep=0x8000u;

static void L(RFs& fs, const TDesC8& m){
	RFile f; TInt r=f.Open(fs, _L("E:\\e5dump.log"), EFileWrite);
	if(r==KErrNotFound){r=f.Create(fs, _L("E:\\e5dump.log"), EFileWrite);}
	if(r!=KErrNone){return;}
	TInt p=0; f.Seek(ESeekEnd, p); f.Write(m); f.Flush(); f.Close();
}

GLDEF_C TInt E32Main(){
	RFs fs;
	if(fs.Connect()!=KErrNone){return KErrGeneral;}
	TBuf8<96> b;
	b.Copy(_L8("e5dump\n")); L(fs, b);
	User::FreeLogicalDevice(KRunBlobName);
	TInt rLoad=User::LoadLogicalDevice(KRunBlobLdd);
	b.Format(_L8("Load=%d\n"), rLoad); L(fs, b);
	if(rLoad==KErrNone || rLoad==KErrAlreadyExists){
		RRunBlob dev;
		if(dev.Open()==KErrNone){
			TUint8* mem=(TUint8*)User::Alloc(KStep);
			if(mem){
				for(TUint k=0; k<sizeof(KRegs)/sizeof(KRegs[0]); ++k){
					TBuf<48> name;
					name.Format(_L("E:\\e5d_%08x.bin"), KRegs[k].iPhys);
					RFile out;
					if(out.Replace(fs, name, EFileWrite)!=KErrNone){b.Format(_L8("%08x: can't create\n"), KRegs[k].iPhys); L(fs, b); continue;}
					TUint32 done=0; TInt r=KErrNone;
					while(done<KRegs[k].iLen){
						TUint32 n=KRegs[k].iLen-done; if(n>KStep){n=KStep;}
						TPtr8 buf(mem, 0, (TInt)n);
						r=dev.ReadPhys(KRegs[k].iPhys+done, buf);
						if(r!=KErrNone){break;}
						out.Write(buf);
						done+=n;
					}
					out.Flush(); out.Close();
					b.Format(_L8("%08x len %x done %x r=%d\n"), KRegs[k].iPhys, KRegs[k].iLen, done, r); L(fs, b);
				}
				User::Free(mem);
			}
			dev.Close();
		}
	}
	User::InfoPrint(_L("e5dump done"));
	fs.RSessionBase::Close(); // RFs::Close is not exported by the 9.3 efsrv
	return KErrNone;
}
