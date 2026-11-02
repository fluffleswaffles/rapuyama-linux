// Read-only INFO request while Symbian still owns the modem. No value logging.
#ifndef E6_FACTORY_IDENTITY_H
#define E6_FACTORY_IDENTITY_H
#include <iscapi.h>
#include "../../kernel/linux/include/linux/rapuyama_identity.h"
static void FactoryIdentityDiag(TInt resource,TInt received,TInt matched,TInt result);

static TInt IdentityWait(TRequestStatus& aStatus, RTimer& aTimer){
	TRequestStatus timeout;aTimer.After(timeout,3000000);
	User::WaitForRequest(aStatus,timeout);
	TBool expired=aStatus==KRequestPending;
	aTimer.Cancel();User::WaitForRequest(timeout);
	return expired ? KErrTimedOut : aStatus.Int();
}
static TInt ReadFactoryIdentity(TDes8& aResult){
	aResult.Zero();RIscApi isc;RTimer timer;
	TInt r=timer.CreateLocal();if(r!=KErrNone){return r;}
	TRequestStatus status;
	isc.Open(0x18,status);r=IdentityWait(status,timer);
	if(r==KErrTimedOut){isc.OpenCancel();User::WaitForRequest(status);}
	if(r!=KErrNone){isc.Close();timer.Close();return r;}
	const TUint8 resources[]={0xc5,0x1b};
	for(TInt n=0;n<2;++n){
		TBuf8<12> request;request.FillZ(12);
		request[3]=resources[n];request[4]=6;request[8]=0x71+n;
		request[9]=0;request[10]=0x41;
		r=isc.Send(request);if(r!=KErrNone){continue;}

		TRequestStatus deadline;timer.After(deadline,3000000);
		for(TInt packet=0;packet<16;++packet){
			TBuf8<256> reply;TUint16 needed=0;
			isc.Receive(status,reply,needed);User::WaitForRequest(status,deadline);
			if(status==KRequestPending){isc.ReceiveCancel();User::WaitForRequest(status);r=KErrTimedOut;break;}
			r=status.Int();
			if(r!=KErrNone){FactoryIdentityDiag(resources[n],reply.Length(),0,r);break;}
			TBool matched=reply.Length()>=10 && reply[3]==resources[n] && reply[8]==request[8];
			if(!matched){reply.FillZ();r=KErrCorrupt;if(deadline!=KRequestPending){break;}continue;}
			TUint8 value[15];Mem::FillZ(value,15);
			TInt reason=e6_id_info_error(reply.Ptr(),reply.Length(),resources[n],request[8],value);
			if(reason==0){
				aResult.Append(value,15);Mem::FillZ(value,15);reply.FillZ();r=KErrNone;break;
			}
			FactoryIdentityDiag(resources[n],reply.Length(),1,reason);
			Mem::FillZ(value,15);reply.FillZ();r=KErrCorrupt;break;
		}
		timer.Cancel();User::WaitForRequest(deadline);
		FactoryIdentityDiag(resources[n],0,0,r);
		if(aResult.Length()==15){break;}
	}
	isc.Close();timer.Close();return aResult.Length()==15 ? KErrNone : r;
}
#endif
