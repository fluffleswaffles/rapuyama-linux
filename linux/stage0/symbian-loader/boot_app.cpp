// streams a linux zImage + dtb from E: into contiguous phys ram and hands off
// to the kernel (one-way). needs E:\zImage and E:\e6.dtb on the phone
//
// chunk layout: zImage @ +0x00008000 (TEXT_OFFSET), dtb @ +0x02000000.
// The E6 kernel has fixed PHYS_OFFSET=0x10000000; the DTB describes the
// same RAM range. The legacy sentinel patch in the driver does not match it.
// after Boot() the kernel takes over and this never returns (battery pull to
// recover). until there are rapuyama drivers + watchdog handling this wont
// boot visibly, its just the loader ready for when the adapter shows up
#include <e32std.h>
#include <f32file.h>
#include "runblob.h"
#ifdef RB_IMAGE_BOOT
#include <e32svr.h> // UserSvr::HalFunction
#include <u32hal.h> // ram zone hal
#include "factory_identity.h"
#include "boot_alloc.h"
#endif

_LIT(KLog, "E:\\boot.log");
#ifdef RB_IMAGE_BOOT
// e6bfix: the uncompressed kernel (build-vc/arch/arm/boot/Image), it runs in
// place inside the chunk, see AllocBoot in runblob_ldd.cpp
_LIT(KZImage, "E:\\Image");
#else
_LIT(KZImage, "E:\\zImage");
#endif
_LIT(KDtb, "E:\\e6.dtb");

const TUint32 KZOff=0x00008000; // TEXT_OFFSET
#ifdef RB_IMAGE_BOOT
// the Image runs in place up to ~chunk+0x1690000 (_end), the dtb right after
// the kernel keeps it reserved. 32 mb so the chunk can sit high enough
const TUint32 KDtbOff=0x01f00000;
const TUint32 KChunkSz=0x02000000;
#else
const TUint32 KDtbOff=0x02000000; // 32 mb into the chunk
const TUint32 KChunkSz=0x03000000; // 48 mb
#endif
const TInt KBuf=32768;

static RFs gFs;
static RFile gLog;
static void L(const TDesC8& s){TInt p=0; gLog.Seek(ESeekEnd, p); gLog.Write(s); gLog.Flush();}
static void Lf(TRefByValue<const TDesC8> f, ...){
	VA_LIST l;
	VA_START(l, f);
	TBuf8<128> b;
	b.FormatList(f, l);
	VA_END(l);
	L(b);
}

static TBool gHeadless=EFalse;

static TInt Ask(const TDesC& aLine1, const TDesC& aLine2, const TDesC& aBut1, const TDesC& aBut2, TInt aHeadless){
	if(gHeadless){
		TBuf8<64> l1, l2, a;
		l1.Copy(aLine1.Left(64));
		l2.Copy(aLine2.Left(64));
		a.Copy((aHeadless ? aBut2 : aBut1).Left(64));
		Lf(_L8("headless: %S / %S -> %S\n"), &l1, &l2, &a);
		return aHeadless;
	}
	RNotifier n;
	if(n.Connect()!=KErrNone){return KErrGeneral;}
	TInt button=-1;
	TRequestStatus s;
	n.Notify(aLine1, aLine2, aBut1, aBut2, button, s);
	User::WaitForRequest(s);
	n.Close();
	return (s.Int()==KErrNone) ? button : s.Int();
}
#ifdef RB_IMAGE_BOOT
static void FactoryIdentityDiag(TInt resource,TInt received,TInt matched,TInt result){
	Lf(_L8("identity INFO resource=%02x bytes=%d matched=%d result=%d\n"),resource,received,matched,result);
}
#endif

static void Fail(const TDesC& aWhat, TInt aErr){
	TBuf<32> e;
	e.Format(_L("error %d"), aErr);
	Ask(aWhat, e, _L("OK"), KNullDesC, 0);
}

#ifdef RB_IMAGE_BOOT
static TInt CheckBootFile(const TDesC& aName, TInt aMax){
	RFile file;
	TInt r=file.Open(gFs, aName, EFileRead);
	if(r!=KErrNone){return r;}
	TInt size=0;
	r=file.Size(size);
	file.Close();
	if(r!=KErrNone){return r;}
	if(size<=0){return KErrCorrupt;}
	return size<=aMax ? KErrNone : KErrTooBig;
}
#endif

#ifdef RB_IMAGE_BOOT

static void BootZones(RRunBlob& aDev){
	TUint count=0;
	TInt r=UserSvr::HalFunction(EHalGroupRam, ERamHalGetZoneCount, &count, NULL);
	Lf(_L8("ram zones: %d (%d)\n"), count, r);
	if(r!=KErrNone){return;}
	TBootZones zones;
	zones.iCount=0;
	for(TUint i=0; i<count && i<64; ++i){
		SRamZoneConfig c;
		SRamZoneUtilisation u;
		if(UserSvr::HalFunction(EHalGroupRam, ERamHalGetZoneConfig, (TAny*)i, &c)!=KErrNone){continue;}
		TInt ru=UserSvr::HalFunction(EHalGroupRam, ERamHalGetZoneUtilisation, (TAny*)i, &u);
		Lf(_L8("  zone id %u %08x-%08x pref %u flags %x"), c.iZoneId, c.iPhysBase, c.iPhysEnd, c.iPref, c.iFlags);
		if(ru==KErrNone){
			Lf(_L8(" pages %u free %u fixed %u movable %u discardable %u unknown %u other %u\n"),
				u.iPhysPages, u.iFreePages, u.iAllocFixed, u.iAllocMovable, u.iAllocDiscardable, u.iAllocUnknown, u.iAllocOther);
		}
		else{L(_L8("\n"));}
		if(c.iPhysEnd>=KBootLo && c.iPhysBase<KBootHi+0x2000000u && zones.iCount<(TUint32)KBootZonesMax){
			TBootZone& z=zones.iZone[zones.iCount++];
			z.iId=c.iZoneId; z.iBase=c.iPhysBase; z.iEnd=c.iPhysEnd;
		}
	}
	TPtrC8 p((const TUint8*)&zones, sizeof(zones));
	Lf(_L8("ram zones near the boot window: %u, BootZones=%d\n"), zones.iCount, aDev.BootZones(p));
}

static TUint32 BootAllocReport(RRunBlob& aDev){
	static TBootAllocLog log;
	TPtr8 p((TUint8*)&log, 0, sizeof(log));
	TInt r=aDev.BootAllocLog(p);
	if(r!=KErrNone || p.Length()!=(TInt)sizeof(log)){Lf(_L8("alloc log unavailable: %d\n"), r); return 0xffffffffu;}
	static const char* const KVerdict[]={"taken", "too high", "too low", "past ramcon", "no block", "dropped", "compact"};
	TUint32 n=log.iCount<(TUint32)KBootLogMax ? log.iCount : (TUint32)KBootLogMax;
	for(TUint32 i=0; i<n; ++i){
		const TBootAllocEntry& e=log.iEntry[i];
		TPtrC8 v((const TUint8*)(e.iVerdict<=EBootCompact ? KVerdict[e.iVerdict] : "?"));
		if(e.iVerdict==EBootCompact){Lf(_L8("  round %u compact zone %u at %08x: %d\n"), e.iRound, e.iSize, e.iPhys, e.iResult);}
		else{Lf(_L8("  round %u %3u mb: %08x %S (%d)\n"), e.iRound, e.iSize>>20, e.iPhys, &v, e.iResult);}
	}
	if(log.iCount>n){Lf(_L8("  ... %u more attempts not logged\n"), log.iCount-n);}
	Lf(_L8("lowest block offered: %08x (window %08x-%08x)\n"), log.iLowest, KBootLo, KBootHi);
	return log.iLowest;
}
#endif

// streams a file into the boot chunk at chunk offset aBase, returns size or <0
static TInt Stream(RRunBlob& aDev, const TDesC& aName, TUint32 aBase, TUint8* aBuf){
	RFile f;
	TInt r=f.Open(gFs, aName, EFileRead);
	if(r!=KErrNone){return r;}
	TInt total=0;
	f.Size(total);
	TUint32 off=0;
	for(;;){
		TPtr8 p(aBuf, 0, KBuf);
		r=f.Read(p);
		if(r!=KErrNone || p.Length()==0){break;}
		r=aDev.LoadChunk(aBase+off, aBuf, (TUint32)p.Length());
		if(r!=KErrNone){f.Close(); return r;}
		TInt was=total ? (TInt)((TInt64)off*10/total) : 0;
		off+=p.Length();
		TInt now=total ? (TInt)((TInt64)off*10/total) : 0;
		if(total>(1<<20) && now!=was){ // big files only, every 10 %
			TBuf<48> t;
			t.Format(_L("loading %S %d %%"), &aName, now*10);
			User::InfoPrint(t);
		}
	}
	f.Close();
	return (r==KErrNone) ? total : r;
}

#ifdef RB_CALIB
// phase calibration, read only (EControlSnoopPoll, no smc, no hooks).
// phase 1: watch securitykext's request descriptor for <window> s and log every
// request with its 32 kHz fast counter -> the period. phase 2: wait for the
// next request, sleep <delay> s, hand off. the ldd puts the counter of that
// request + the counter at the handoff into the isah block, the kernel flight
// recorder keeps logging the counter, tools/parse_calib.py lines them up.
// E:\calib.txt (optional): "<window> <delay>" in seconds, default "330 0"
_LIT(KCalibCfg, "E:\\calib.txt");

static TInt SecsSince(const TTime& aT){
	TTime n;
	n.HomeTime();
	TTimeIntervalSeconds s;
	n.SecondsFrom(aT, s);
	return s.Int();
}

static void ReadCalibCfg(TInt& aWindow, TInt& aDelay){
	aWindow=0; // the ldd reads the secure timer itself, snooping is optional
	aDelay=0;
	RFile f;
	if(f.Open(gFs, KCalibCfg, EFileRead)!=KErrNone){return;}
	TBuf8<64> b;
	f.Read(b);
	f.Close();
	TLex8 lx(b);
	TInt v;
	if(lx.Val(v)==KErrNone && v>0){aWindow=v;}
	lx.SkipSpace();
	if(lx.Val(v)==KErrNone && v>=0){aDelay=v;}
}

// one poll burst. ETrue on a NEW request: the descriptor stays non-zero for a
// moment, so a sighting less than 1 s after the previous one is the same request
static TBool CalibPoll(RRunBlob& aDev, TUint32* aSnap, TUint32& aLastFc, TBool& aHave){
	TPtr8 p((TUint8*)aSnap, 0, 40*4);
	if(aDev.SnoopPoll(p, 0)!=1){return EFalse;}
	TUint32 fc=aSnap[38];
	TBool isNew=!aHave || fc-aLastFc>=32768u;
	aHave=ETrue;
	aLastFc=fc;
	return isNew;
}

static void CalibLog(TInt aN, const TUint32* aSnap, TUint32& aPrevFc, TBool& aHavePrev){
	TUint32 fc=aSnap[38];
	TUint32 dtMs=aHavePrev ? (TUint32)(((TUint64)(fc-aPrevFc)*1000)>>15) : 0;
	Lf(_L8("calib: req %d fc=%08x dt=%u ms cmd=%08x PAid=%08x\n"), aN, fc, dtMs, aSnap[0], aSnap[1]);
	Lf(_L8("  reqphys=%08x reqbuf %08x %08x %08x %08x\n"), aSnap[3], aSnap[22], aSnap[23], aSnap[24], aSnap[25]);
	aPrevFc=fc;
	aHavePrev=ETrue;
}

static void Calibrate(RRunBlob& aDev){
	TInt window=0, delay=0;
	ReadCalibCfg(window, delay);
	Lf(_L8("calib: window %d s, delay after the request %d s\n"), window, delay);
	if(window==0){return;}
	TUint32 snap[40];
	TUint32 lastFc=0, prevFc=0;
	TBool have=EFalse, havePrev=EFalse;
	TInt n=0;
	TTime t;
	t.HomeTime();
	while(SecsSince(t)<window){
		if(CalibPoll(aDev, snap, lastFc, have)){CalibLog(++n, snap, prevFc, havePrev);}
	}
	Lf(_L8("calib: %d requests in the window\n"), n);
	t.HomeTime();
	TBool got=EFalse;
	while(!got && SecsSince(t)<window){
		if(CalibPoll(aDev, snap, lastFc, have)){CalibLog(++n, snap, prevFc, havePrev); got=ETrue;}
	}
	if(!got){
		L(_L8("calib: no request in phase 2, booting anyway (phase unknown)\n"));
		return;
	}
	if(delay>0){
		Lf(_L8("calib: sleeping %d s before the handoff\n"), delay);
		User::After(TTimeIntervalMicroSeconds32(delay*1000000));
	}
}
#endif

const TUint KSidInstaller=0x1020473f;

GLDEF_C TInt E32Main(){
	if(User::CreatorSecureId().iId==KSidInstaller){return KErrNone;}
	if(gFs.Connect()!=KErrNone){return KErrGeneral;}
	if(gLog.Replace(gFs, KLog, EFileWrite)!=KErrNone){gFs.Close(); return KErrGeneral;}
	TBuf<32> arg;
	if(User::CommandLineLength()<=arg.MaxLength()){User::CommandLine(arg);}
	gHeadless=(arg.Find(_L("headless"))>=0);
	L(gHeadless ? _L8("boot: started, headless\n") : _L8("boot: started\n"));

#ifdef RB_IMAGE_BOOT
	TInt check=CheckBootFile(KZImage, KDtbOff-KZOff);
	if(check==KErrNone){check=CheckBootFile(KDtb, KChunkSz-KDtbOff);}
	if(check!=KErrNone){
		Lf(_L8("boot files invalid: %d\n"), check);
		Fail(_L("check E:\\Image and E:\\e6.dtb"), check);
		gLog.Close(); gFs.Close();
		return check;
	}
#endif

	User::FreeLogicalDevice(KRunBlobName);
	TInt rLoad=User::LoadLogicalDevice(KRunBlobLdd);
	Lf(_L8("LoadLogicalDevice=%d\n"), rLoad);
	if(rLoad!=KErrNone && rLoad!=KErrAlreadyExists){Fail(_L("can't load the ldd"), rLoad); gLog.Close(); gFs.Close(); return rLoad;}

	RRunBlob dev;
	TInt r=dev.Open();
	if(r!=KErrNone){L(_L8("open failed\n")); Fail(_L("can't open the ldd"), r); gLog.Close(); gFs.Close(); return r;}
	#ifdef RB_IMAGE_BOOT
	TBuf8<15> factoryIdentity;
	r=ReadFactoryIdentity(factoryIdentity);
	if(r==KErrNone){r=dev.FactoryIdentity(factoryIdentity);}
	factoryIdentity.FillZ();
	if(r!=KErrNone){
		Lf(_L8("factory identity handoff unavailable: %d\n"),r);
		Fail(_L("factory identity unavailable"),r);dev.Close();gLog.Close();gFs.Close();return r;
	}
	L(_L8("factory identity ready; value private\n"));
	#endif

	TUint8* buf=(TUint8*)User::Alloc(KBuf);
	if(!buf){L(_L8("no mem\n")); Fail(_L("no heap"), KErrNoMemory); dev.Close(); gLog.Close(); gFs.Close(); return KErrNoMemory;}

	TUint32 phys=0;
	for(;;){
#ifdef RB_IMAGE_BOOT
		BootZones(dev);
#endif
		r=dev.AllocBoot(KChunkSz, phys);
		Lf(_L8("AllocBoot=%d phys=%08x\n"), r, phys);
#ifdef RB_IMAGE_BOOT
		TUint32 lowest=BootAllocReport(dev);
#endif
		if(r==KErrNone){break;}
		TBuf<48> m;
#ifdef RB_IMAGE_BOOT
		if(r==KErrNoMemory && lowest!=0xffffffffu){m.Format(_L("lowest ram block %08x"), lowest);}
		else{m.Format(_L("no free ram block (%d)"), r);}
#else
		m.Format(_L("no free ram block (%d)"), r);
#endif
		if(Ask(m, _L("close apps and retry?"), _L("Retry"), _L("Cancel"), 1)!=0){
			User::Free(buf); dev.Close(); gLog.Close(); gFs.Close();
			return r;
		}
	}

	TInt zsz=Stream(dev, KZImage, KZOff, buf);
	Lf(_L8("zImage stream=%d\n"), zsz);
	TInt dsz=Stream(dev, KDtb, KDtbOff, buf);
	Lf(_L8("dtb stream=%d\n"), dsz);
	User::Free(buf);

	if(zsz<=0 || dsz<=0){
		L(_L8("missing zImage/dtb on E: -- aborting\n"));
		Fail((zsz<=0) ? KZImage() : KDtb(), (zsz<=0) ? zsz : dsz);
		dev.Close(); gLog.Close(); gFs.Close();
		return KErrNotFound;
	}

#ifdef RB_CALIB
	Calibrate(dev);
#endif
	TBuf<48> m;
	m.Format(_L("ram %08x, kernel %d kb"), phys, zsz>>10);
	if(Ask(m, _L("boot linux?"), _L("Boot"), _L("Cancel"), 0)!=0){
		L(_L8("cancelled\n"));
		dev.Close(); gLog.Close(); gFs.Close();
		return KErrCancel;
	}
#ifdef RB_CALIB
	// boot right after the inferred 210 s phase. tthere should be a countdown
	// but it doesn't work and i don't wanna fix it
	{
		const TUint32 KPeriod=210000, KPhase=900;
		TUint32 now=User::NTickCount();
		TUint32 target=((now+KPhase)/KPeriod+1)*KPeriod-KPhase;
		for(;;){
			TInt left=(TInt)(target-User::NTickCount());
#ifdef RB_TIMER_CAPTURE
			if(left<=6000){break;}
#else
			if(left<=3000){break;}
#endif
			TBuf<48> t;
			t.Format(_L("linux in %d s"), left/1000);
			User::InfoPrint(t);
			User::After(2000000);
		}
		User::InfoPrint(_L("booting linux"));
#ifdef RB_TIMER_CAPTURE
		// allocate before sampling; write the file after the service window
		Lf(_L8("timer capture start tick=%u\n"), User::NTickCount());
		const TInt bytes=(8+36*(16+512*32))*4;
		RChunk capture;
		TInt rs=capture.CreateLocal(bytes, bytes);
		if(rs!=KErrNone){
			Fail(_L("timer capture allocation"), rs);
			dev.Close(); gLog.Close(); gFs.Close();
			return rs;
		}
		TPtr8 p(capture.Base(), 0, bytes);
		rs=dev.WatchService(p);
		Lf(_L8("WatchService=%d bytes=%d\n"), rs, p.Length());
		if(rs==KErrNone){
			RFile f;
			rs=f.Replace(gFs, _L("E:\\timers.bin"), EFileWrite);
			if(rs==KErrNone){rs=f.Write(p); f.Close();}
		}
		capture.Close();
		if(rs!=KErrNone){
			Fail(_L("timer capture"), rs);
			dev.Close(); gLog.Close(); gFs.Close();
			return rs;
		}
#else
		TBuf8<4> none;
		TInt rs=dev.WatchService(none);
		Lf(_L8("WatchService=%d\n"), rs);
#endif
	}
#endif
	L(_L8("about-to-Boot (one-way)\n"));
	gLog.Close(); gFs.Close();
	r=dev.Boot(KZOff, KDtbOff, 0xFFFFFFFFu, 0); // never returns on success
	Fail(_L("linux handoff failed"),r);dev.Close();return r;
}