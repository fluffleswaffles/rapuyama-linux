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
#ifdef RB_E5
#include <iscapi.h>
#endif

#ifdef RB_E5
// the 9.3 efsrv has no RFs::Close (ordinal 390 > its 351 exports): close the session by its base
#define FSCLOSE() gFs.RSessionBase::Close()
#else
#define FSCLOSE() gFs.Close()
#endif

_LIT(KLog, "E:\\boot.log");
#ifdef RB_IMAGE_BOOT
// e6bfix: the uncompressed kernel (build-vc/arch/arm/boot/Image), it runs in
// place inside the chunk, see AllocBoot in runblob_ldd.cpp
_LIT(KZImage, "E:\\Image");
#else
_LIT(KZImage, "E:\\zImage");
#endif
#ifdef RB_E5
_LIT(KDtb, "E:\\e5.dtb");
#else
_LIT(KDtb, "E:\\e6.dtb");
#endif

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

// a real dialog on the E5 too: User::InfoPrint shows nothing on the retail
// E5 firmware (phone-tested 2026-11-02), RNotifier is exported by its euser
static TInt Ask(const TDesC& aLine1, const TDesC& aLine2, const TDesC& aBut1, const TDesC& aBut2){
	RNotifier n;
	TInt r=n.Connect();
	TInt button=-1;
	if(r==KErrNone){
		TRequestStatus s;
		n.Notify(aLine1, aLine2, aBut1, aBut2, button, s);
		User::WaitForRequest(s);
		n.Close();
		r=s.Int();
	}
#ifdef RB_E5
	if(gLog.SubSessionHandle()){Lf(_L8("notifier=%d button=%d\n"), r, button);}
#endif
	return (r==KErrNone) ? button : r;
}

static void Fail(const TDesC& aWhat, TInt aErr){
	TBuf<32> e;
	e.Format(_L("error %d"), aErr);
	Ask(aWhat, e, _L("OK"), KNullDesC);
}

#ifdef RB_E5
// wlan files for linux (init.sh copies them into the rootfs and binds
// wl1251): the wl1251 firmware + default nvs are Nokia's and can't ship, so
// they come out of this phone's own rom (wlanhwinit.dll), and the phone's
// wlan MAC comes from the modem (the nvs only has TI's placeholder). Done once:
// a file already on the card is left alone. Nothing here stops the boot, a
// failure is logged and linux runs without wlan / with the placeholder MAC
_LIT(KWlanDir, "E:\\linux\\");
_LIT(KWlanFw, "E:\\linux\\wl1251-fw.bin");
_LIT(KWlanNvs, "E:\\linux\\wl1251-nvs.bin");
_LIT(KWlanMac, "E:\\linux\\e5-wlan-mac");
_LIT(KWlanTmp, "E:\\linux\\wlan.tmp");
_LIT(KWlanDll, "Z:\\sys\\bin\\wlanhwinit.dll");
// offsets from the dll's code start (rom of firmware 101.003, wlanhwinit uid3
// 0x10207267). any other rom: the crc check refuses, no guessing
const TUint32 KWlanUid3=0x10207267;
const TInt KFwOff=0x268b, KFwLen=200460; // linux wl1251 format, "Rev 4.0.4.12.12"
const TInt KNvsOff=0x2216, KNvsLen=729;  // + 3 zero bytes = the 732 B nvs file
const TUint32 KFwCrc=0x38a5a30du, KNvsCrc=0x006428bfu; // zlib crc32
const TInt KIscWait=3000000;

static TUint32 Crc32(const TDesC8& aData){
	TUint32 c=0xffffffffu;
	for(TInt i=0;i<aData.Length();++i){
		c^=aData[i];
		for(TInt k=0;k<8;++k){c=(c>>1)^(0xedb88320u&(0u-(c&1)));}
	}
	return ~c;
}

static TBool Exists(const TDesC& aName){TEntry e; return gFs.Entry(aName, e)==KErrNone;}

// via a temp file + rename: a pulled card / battery never leaves half a file
// under the real name (linux would take it, the next run would skip it)
static TInt WriteFile(const TDesC& aName, const TDesC8& aData, const TDesC8& aTail){
	RFile f;
	TInt r=f.Replace(gFs, KWlanTmp, EFileWrite);
	if(r!=KErrNone){return r;}
	r=f.Write(aData);
	if(r==KErrNone && aTail.Length()){r=f.Write(aTail);}
	if(r==KErrNone){r=f.Flush();}
	f.Close();
	if(r==KErrNone){r=gFs.Rename(KWlanTmp, aName);}
	if(r!=KErrNone){gFs.Delete(KWlanTmp);}
	return r;
}

static void WlanRomFiles(){
	// TRomImageHeader: [2] uid3, [5] iCodeAddress, [7] iCodeSize. the rom is
	// readable from user side (the wlan stack uses these blobs in place too)
	const TUint32* h=(const TUint32*)gFs.IsFileInRom(KWlanDll);
	if(!h){L(_L8("wlan: no wlanhwinit.dll in the rom\n")); return;}
	if(h[2]!=KWlanUid3 || h[7]<(TUint32)(KFwOff+KFwLen)){
		Lf(_L8("wlan: unknown wlanhwinit.dll uid3=%08x size=%x\n"), h[2], h[7]);
		return;
	}
	const TUint8* code=(const TUint8*)h[5];
	TPtrC8 fw(code+KFwOff, KFwLen), nvs(code+KNvsOff, KNvsLen);
	TUint32 cf=Crc32(fw), cn=Crc32(nvs);
	if(cf!=KFwCrc || cn!=KNvsCrc){
		Lf(_L8("wlan: unknown rom blobs fw crc=%08x nvs crc=%08x\n"), cf, cn);
		return;
	}
	TBuf8<3> pad; pad.FillZ(3);
	Lf(_L8("wlan: fw write=%d\n"), WriteFile(KWlanFw, fw, KNullDesC8));
	Lf(_L8("wlan: nvs write=%d\n"), WriteFile(KWlanNvs, nvs, pad));
}

// the request wlanhwinit.dll's ConstructL sends (rom 0x8092ca82), but every
// wait with a timeout (ROM wlanhwinit waits forever). reference + phone
// result: e5wmac_app.cpp (probe run 2). channel 0x20 = EIscNokiaStartup
static TInt IscWait(TRequestStatus& aStatus, TRequestStatus& aDeadline){
	User::WaitForRequest(aStatus, aDeadline);
	return aStatus==KRequestPending ? KErrTimedOut : aStatus.Int();
}

static TInt WlanMacRead(RTimer& aTimer, TDes8& aReply, TUint8* aMac){
	// ROM RIscApi: OpenCancel panics while an open is pending, Close panics if
	// the driver refuses => a failed open is left alone (the process ends
	// with the handoff anyway). heap objects, so a late completion hits live
	// memory; no operator new in this link: User::Alloc + placement new
	TAny* m1=User::Alloc(sizeof(RIscApi));
	TAny* m2=User::Alloc(sizeof(TRequestStatus));
	if(!m1 || !m2){return KErrNoMemory;}
	RIscApi& isc=*new(m1) RIscApi;
	TRequestStatus& st=*new(m2) TRequestStatus;
	TRequestStatus deadline;
	isc.Open(0x20, st);
	aTimer.After(deadline, KIscWait);
	TInt r=IscWait(st, deadline);
	aTimer.Cancel(); User::WaitForRequest(deadline);
	Lf(_L8("wlan: isc open=%d\n"), r);
	if(r!=KErrNone){return r;}

	TBuf8<12> q; q.FillZ(12);
	q[3]=0x1b; q[4]=6; q[8]=0x51; q[9]=0x23; // PN_PHONE_INFO, len LE = 12-6
	r=isc.Send(q);
	if(r==KErrNone){
		aTimer.After(deadline, KIscWait);
		r=KErrTimedOut;
		for(TInt packet=0;packet<16;++packet){ // skip other resources' packets
			TRequestStatus rs; TUint16 needed=0;
			aReply.Zero();
			isc.Receive(rs, aReply, needed);
			r=IscWait(rs, deadline);
			if(r==KErrTimedOut){isc.ReceiveCancel(); User::WaitForRequest(rs); break;}
			if(r!=KErrNone){break;}
			if(aReply.Length()>=10 && aReply[3]==q[3] && aReply[8]==q[8]){break;}
			r=KErrCorrupt;
		}
		aTimer.Cancel(); User::WaitForRequest(deadline);
	}
	isc.Close();
	User::Free(m1); User::Free(m2);
	Lf(_L8("wlan: mac request=%d len=%d\n"), r, aReply.Length());
	if(r!=KErrNone){return r;}
	// reply: [9] 0x24, [10] status 0, 6 bytes at [11] in reverse order
	if(aReply.Length()<17 || aReply[9]!=0x24 || aReply[10]!=0){return KErrNotSupported;}
	for(TInt i=0;i<6;++i){aMac[i]=aReply[16-i];}
	return KErrNone;
}

static void WlanMacFile(){
	RTimer timer;
	TUint8* buf=(TUint8*)User::Alloc(1024);
	if(!buf || timer.CreateLocal()!=KErrNone){User::Free(buf); L(_L8("wlan: no timer/heap\n")); return;}
	TPtr8 reply(buf, 0, 1024);
	TUint8 m[6];
	TInt r=WlanMacRead(timer, reply, m);
	timer.Close();
	User::Free(buf);
	if(r!=KErrNone){Lf(_L8("wlan: no mac (%d), linux keeps the default\n"), r); return;}
	// unicast, not all zero (a global Nokia one is 30:38:55:..)
	if((m[0]&1) || (m[0]|m[1]|m[2]|m[3]|m[4]|m[5])==0){
		Lf(_L8("wlan: bad mac %02x:%02x:%02x:%02x:%02x:%02x\n"), m[0],m[1],m[2],m[3],m[4],m[5]);
		return;
	}
	TBuf8<20> t;
	t.Format(_L8("%02x:%02x:%02x:%02x:%02x:%02x\n"), m[0],m[1],m[2],m[3],m[4],m[5]);
	Lf(_L8("wlan: mac %S"), &t);
	Lf(_L8("wlan: mac write=%d\n"), WriteFile(KWlanMac, t, KNullDesC8));
}

static void WlanFiles(){
	TBool fw=Exists(KWlanFw) && Exists(KWlanNvs), mac=Exists(KWlanMac);
	if(fw && mac){return;}
	TInt r=gFs.MkDirAll(KWlanDir);
	if(r!=KErrNone && r!=KErrAlreadyExists){Lf(_L8("wlan: mkdir E:\\linux=%d\n"), r); return;}
	if(!fw){WlanRomFiles();}
	if(!mac){WlanMacFile();}
}
#endif

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

GLDEF_C TInt E32Main(){
	if(gFs.Connect()!=KErrNone){return KErrGeneral;}
	if(gLog.Replace(gFs, KLog, EFileWrite)!=KErrNone){
#ifdef RB_E5
		// no card in the phone (E: is the card on the E5) or it is read-only
		Ask(_L("No memory card (E:)."), _L("Linux boots from E:\\Image + E:\\e5.dtb"), _L("OK"), KNullDesC);
#endif
		FSCLOSE(); return KErrGeneral;
	}
	L(_L8("boot: started\n"));

#ifdef RB_IMAGE_BOOT
	TInt check=CheckBootFile(KZImage, KDtbOff-KZOff);
	if(check==KErrNone){check=CheckBootFile(KDtb, KChunkSz-KDtbOff);}
	if(check!=KErrNone){
		Lf(_L8("boot files invalid: %d\n"), check);
#ifdef RB_E5
		// say which file and what is wrong instead of a bare error number
		_LIT(KMissing, "missing"); _LIT(KTooBig, "too big");
		_LIT(KEmpty, "is empty"); _LIT(KUnreadable, "unreadable");
		const TDesC* file=(CheckBootFile(KZImage, KDtbOff-KZOff)!=KErrNone) ? &KZImage() : &KDtb();
		const TDesC* why=(check==KErrNotFound || check==KErrPathNotFound) ? &KMissing() :
			check==KErrTooBig ? &KTooBig() : check==KErrCorrupt ? &KEmpty() : &KUnreadable();
		TBuf<48> what;
		what.Format(_L("%S %S."), file, why);
		Ask(what, _L("Copy the release files to the card."), _L("OK"), KNullDesC);
#else
		Fail(_L("check E:\\Image and E:\\e6.dtb"), check);
#endif
		gLog.Close(); FSCLOSE();
		return check;
	}
#endif
#ifdef RB_E5
	WlanFiles();
#endif

	User::FreeLogicalDevice(KRunBlobName);
	TInt rLoad=User::LoadLogicalDevice(KRunBlobLdd);
	Lf(_L8("LoadLogicalDevice=%d\n"), rLoad);
	if(rLoad!=KErrNone && rLoad!=KErrAlreadyExists){Fail(_L("can't load the ldd"), rLoad); gLog.Close(); FSCLOSE(); return rLoad;}

	RRunBlob dev;
	TInt r=dev.Open();
	if(r!=KErrNone){L(_L8("open failed\n")); Fail(_L("can't open the ldd"), r); gLog.Close(); FSCLOSE(); return r;}

	TUint8* buf=(TUint8*)User::Alloc(KBuf);
	if(!buf){L(_L8("no mem\n")); Fail(_L("no heap"), KErrNoMemory); dev.Close(); gLog.Close(); FSCLOSE(); return KErrNoMemory;}

	TUint32 phys=0;
	TInt tries=0;
	for(;;){
		r=dev.AllocBoot(KChunkSz, phys);
		Lf(_L8("AllocBoot=%d phys=%08x\n"), r, phys);
		if(r==KErrNone){break;}
#ifdef RB_E5
		if(++tries>=8){ // Ask() above never says Cancel, so bound it here
			User::Free(buf); dev.Close(); gLog.Close(); FSCLOSE();
			return r;
		}
#else
		(void)tries;
#endif
		TBuf<48> m;
		m.Format(_L("no free ram block (%d)"), r);
		if(Ask(m, _L("close apps and retry?"), _L("Retry"), _L("Cancel"))!=0){
			User::Free(buf); dev.Close(); gLog.Close(); FSCLOSE();
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
		dev.Close(); gLog.Close(); FSCLOSE();
		return KErrNotFound;
	}

#ifdef RB_CALIB
	Calibrate(dev);
#endif
	TBuf<48> m;
	m.Format(_L("ram %08x, kernel %d kb"), phys, zsz>>10);
#if defined(RB_E5) || defined(RB_AUTOBOOT)
	// no Boot/Cancel dialog (the E5 has no RNotifier here), so no reason to
	// wait: 1 s so the message is seen (was a silent 5 s)
#ifdef RB_E5
	User::InfoPrint(_L("Starting Linux..."));
	User::After(1000000);
#else
	User::After(5000000);
#endif
	if(0){
#else
	if(Ask(m, _L("boot linux?"), _L("Boot"), _L("Cancel"))!=0){
#endif
		L(_L8("cancelled\n"));
		dev.Close(); gLog.Close(); FSCLOSE();
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
			dev.Close(); gLog.Close(); FSCLOSE();
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
			dev.Close(); gLog.Close(); FSCLOSE();
			return rs;
		}
#else
		TBuf8<4> none;
		TInt rs=dev.WatchService(none);
		Lf(_L8("WatchService=%d\n"), rs);
#endif
	}
#endif
#ifdef RB_E5_ISAH
	// E5 handoff block for the PA_SWD bridge (phase A: only saved, linux does
	// not use it yet). failure is logged, the boot goes on
	{
		TBuf8<44> ib;
		TInt ir=dev.SaveIsah(ib);
		const TUint32* iw=(const TUint32*)ib.Ptr();
		if(ir==KErrNone && ib.Length()==44){
			Lf(_L8("isah=0 ttbr0=%08x ttbr1=%08x ttbcr=%08x\n"), iw[0], iw[1], iw[2]);
			for(TInt i=0; i<8; ++i){Lf(_L8("isah l1[%d]=%08x\n"), i, iw[3+i]);}
		}else{
			Lf(_L8("isah=%d len=%d\n"), ir, ib.Length());
		}
	}
#endif
#ifdef RB_E5
	// symbian uptime at the handoff: the linux log has its own uptime, the sum
	// puts the reset on symbian's clock (needed to tell a fixed deadline from
	// a period, see docs/modem-reset-findings.md for the E6 method)
	Lf(_L8("handoff symbian tick=%u ms\n"), User::NTickCount());
#endif
	L(_L8("about-to-Boot (one-way)\n"));
	gLog.Close(); FSCLOSE();
	dev.Boot(KZOff, KDtbOff, 0xFFFFFFFFu, 0); // never returns
	return KErrNone;
}