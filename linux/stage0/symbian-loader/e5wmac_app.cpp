// E5-only probe (release step 3, run 2): reads the wlan MAC (and the wlan
// perm record) from the modem ourselves, with the exact ISI requests that
// wlanhwinit.dll's CWlanHwInit ConstructL sends (ROM 0x8092ca82 / 0x8092c89a),
// but with a 3 s timeout on every wait. Run 1 (e5wlan, NewL) stopped inside
// NewL: its RIscApi waits have no timeout, so a hang and a panic look the same.
//   MAC:  resource 0x1b (PN_PHONE_INFO) msg 0x23 -> resp 0x24, [10] status 0,
//         6 bytes at [11], stored REVERSED by wlanhwinit (= TMacAddr order)
//   perm: resource 0x23 (PN_PERMANENT_DATA) msg 0x04 PERM_PM_RECORD_READ_REQ
//         group 0x0139, index 0, offset 0, 0xad bytes -> resp 0x05, [10] 0,
//         [11] 1, [12] 0, u32 [20] 0xad, data at [24]
// wlanhwinit opens ISC channel 0x20 (EIscNokiaStartup); the E6 IMEI code uses
// 0x18 (EIscNokiaTest2) while Symbian runs. We try 0x20, then 0x18.
// Writes E:\e5wmac.log (flushed per line) and E:\e5wmac-perm.bin (raw reply).
// The MAC is not sensitive (agreed); nothing else is logged by value.
#include <e32std.h>
#include <f32file.h>
#include <iscapi.h>

_LIT(KLog, "E:\\e5wmac.log");
_LIT(KPerm, "E:\\e5wmac-perm.bin");
const TInt KWait = 3000000;

static RFs gFs;
static RFile gLog;
static RTimer gTimer;
static TDes8* gR; // heap (E32Main): no reliance on global ctors
#define gReply (*gR)

static void Lf(TRefByValue<const TDesC8> f, ...){
	VA_LIST l;
	VA_START(l, f);
	TBuf8<200> b;
	b.FormatList(f, l);
	VA_END(l);
	TInt p=0; gLog.Seek(ESeekEnd, p); gLog.Write(b); gLog.Flush();
}
static void Hex(const TDesC8& aWhat, const TDesC8& aData, TInt aMax){
	TBuf8<200> b; b.Append(aWhat);
	TInt n=aData.Length()<aMax ? aData.Length() : aMax;
	for(TInt i=0;i<n;++i){b.AppendFormat(_L8(" %02x"), aData[i]);}
	b.Append('\n');
	TInt p=0; gLog.Seek(ESeekEnd, p); gLog.Write(b); gLog.Flush();
}
// wait for aStatus or the deadline; KErrTimedOut if it never completed
static TInt Wait(TRequestStatus& aStatus, TRequestStatus& aDeadline){
	User::WaitForRequest(aStatus, aDeadline);
	return aStatus==KRequestPending ? KErrTimedOut : aStatus.Int();
}

// send aReq, collect the reply with the same resource + transaction id.
// gReply holds it on KErrNone.
static TInt Transact(RIscApi& aIsc, const TDesC8& aReq){
	TInt r=aIsc.Send(aReq);
	Lf(_L8("  Send=%d\n"), r);
	if(r!=KErrNone){return r;}
	TRequestStatus deadline; gTimer.After(deadline, KWait);
	r=KErrTimedOut;
	for(TInt packet=0;packet<16;++packet){
		TRequestStatus st; TUint16 needed=0;
		gReply.Zero();
		aIsc.Receive(st, gReply, needed);
		r=Wait(st, deadline);
		if(r==KErrTimedOut){aIsc.ReceiveCancel(); User::WaitForRequest(st); Lf(_L8("  Receive timed out\n")); break;}
		Lf(_L8("  Receive=%d len=%d needed=%d\n"), r, gReply.Length(), needed);
		if(r!=KErrNone){break;}
		Hex(_L8("  rx"), gReply, 28);
		if(gReply.Length()>=10 && gReply[3]==aReq[3] && gReply[8]==aReq[8]){r=KErrNone; break;}
		r=KErrCorrupt; // someone else's packet, keep reading until the deadline
	}
	gTimer.Cancel(); User::WaitForRequest(deadline);
	return r;
}

static TBool ReadMac(RIscApi& aIsc){
	TBuf8<12> q; q.FillZ(12);
	q[3]=0x1b; q[4]=6; q[8]=0x51; q[9]=0x23; // len LE = 12-6, [10..11] = 0
	Lf(_L8(" MAC request (res 1b msg 23)\n"));
	if(Transact(aIsc, q)!=KErrNone){return EFalse;}
	if(gReply.Length()<17 || gReply[9]!=0x24){Lf(_L8("  unexpected msg %02x\n"), gReply.Length()>9 ? gReply[9] : 0xff); return EFalse;}
	Lf(_L8("  status=%d\n"), gReply[10]);
	if(gReply[10]!=0){return EFalse;}
	const TUint8* m=gReply.Ptr()+11;
	Lf(_L8("  MAC raw     %02x:%02x:%02x:%02x:%02x:%02x\n"), m[0],m[1],m[2],m[3],m[4],m[5]);
	Lf(_L8("  MAC (wlanhwinit order, reversed) %02x:%02x:%02x:%02x:%02x:%02x\n"), m[5],m[4],m[3],m[2],m[1],m[0]);
	return ETrue;
}

static void ReadPerm(RIscApi& aIsc){
	TBuf8<24> q; q.FillZ(24);
	q[3]=0x23; q[4]=18; q[8]=0x52; q[9]=0x04;
	q[10]=0x01; q[11]=0x39; // group 0x0139 (BE), index 0, filler, offset 0
	q[23]=0xad;             // amount 0xad (BE u32 at 20)
	Lf(_L8(" perm request (res 23 msg 04 group 0139 len ad)\n"));
	if(Transact(aIsc, q)!=KErrNone){return;}
	if(gReply.Length()>=24){
		Lf(_L8("  msg=%02x status=%d [11]=%d [12]=%d amount=%02x%02x%02x%02x\n"), gReply[9], gReply[10], gReply[11], gReply[12],
			gReply[20], gReply[21], gReply[22], gReply[23]);
	}
	RFile f;
	TInt r=f.Replace(gFs, KPerm, EFileWrite);
	if(r==KErrNone){r=f.Write(gReply); f.Close();}
	Lf(_L8("  write perm reply (%d B)=%d\n"), gReply.Length(), r);
}

// ROM RIscApi (0x807e2068): OpenCancel panics IADUserApiPanic 2 while the
// channel number is unset (= a pending open), Close panics 4 if the driver
// refuses. So a failed/timed-out open is just left alone (the process exit
// cleans up); the objects are heap (never freed) so a late completion hits
// live memory. Heap, not static: the RIscApi ctor must run (channel = 0xffff).
static TBool TryChannel(TUint16 aCh){
	Lf(_L8("channel %02x\n"), aCh);
	// no operator new/delete in this link (no scppnwdl): User::Alloc + placement new
	TAny* m1=User::Alloc(sizeof(RIscApi));
	TAny* m2=User::Alloc(sizeof(TRequestStatus));
	if(!m1 || !m2){Lf(_L8(" no memory\n")); return EFalse;}
	RIscApi& isc=*new(m1) RIscApi;
	TRequestStatus& st=*new(m2) TRequestStatus;
	TRequestStatus deadline;
	isc.Open(aCh, st);
	gTimer.After(deadline, KWait);
	TInt r=Wait(st, deadline);
	gTimer.Cancel(); User::WaitForRequest(deadline);
	Lf(_L8(" Open=%d\n"), r);
	if(r!=KErrNone){return EFalse;}
	TBool ok=ReadMac(isc);
	if(ok){ReadPerm(isc);}
	isc.Close();
	Lf(_L8(" closed\n"));
	User::Free(m1); User::Free(m2); // RIscApi dtor = nothing after Close
	return ok;
}

GLDEF_C TInt E32Main(){
	if(gFs.Connect()!=KErrNone){return KErrGeneral;}
	if(gLog.Replace(gFs, KLog, EFileWrite)!=KErrNone){gFs.RSessionBase::Close(); return KErrGeneral;}
	Lf(_L8("e5wmac\n"));
	TInt r=gTimer.CreateLocal();
	HBufC8* rb=HBufC8::New(1024);
	Lf(_L8("timer=%d buf=%d\n"), r, rb ? 1 : 0);
	TPtr8 rp(0, 0);
	if(rb){rp.Set(rb->Des()); gR=&rp;}
	if(r==KErrNone && rb){
		const TUint16 ch[]={0x20, 0x18};
		for(TInt i=0;i<2;++i){if(TryChannel(ch[i])){break;}}
		gTimer.Close();
	}
	User::Free(rb);
	Lf(_L8("done\n"));
	gLog.Close();
	gFs.RSessionBase::Close();
	return KErrNone;
}
