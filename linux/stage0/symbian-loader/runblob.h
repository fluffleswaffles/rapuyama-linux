// shared stuff between the runblob loader (kernel) and its user client
#ifndef RUNBLOB_H
#define RUNBLOB_H

#include <e32cmn.h>
#ifndef __KERNEL_MODE__
#include <e32std.h>
#endif

// device name + ldd file. build_app.sh gives every app its own (-DRB_NAME=
// '"e6boot"'), so the apps install side by side instead of replacing each other
#ifndef RB_NAME
#define RB_NAME "runblob"
#endif
#define RB_LIT(n, s) _LIT(n, s)
RB_LIT(KRunBlobName, RB_NAME);
RB_LIT(KRunBlobLdd, RB_NAME ".ldd");

// returned buffer = up to 32 words the blob wrote (sig/sum + cp15 cpu id)
const TInt KRunBlobOutBytes=32*4;

// params for EControlFbFill: fill iBytes of phys ram at iPhys with iColor
// (to find the framebuffer by watching the screen)
struct TFbFill{
	TUint32 iPhys;
	TUint32 iBytes;
	TUint32 iColor;
};

// timer probe output: [0]magic [1..4] before (tick,fc,mst cnt,mst reload)
// [5..36] 16x (fc, mst cnt) 100us apart [37..40] after 1 s sleep (tick,fc,mst cnt,mst reload)
const TInt KTimerProbeWords=41;
const TUint32 KTimerProbeMagic=0x7e570001u;
const TInt KRegProbeMax=64;

// booter params
// copy iLen bytes from user iSrc into boot chunk + iOffset
struct TLoadChunk{
	TUint32 iOffset;
	TUint32 iSrc;
	TUint32 iLen;
};
// offsets inside the boot chunk + boot args
struct TBootParams{
	TUint32 iZImageOff; // zImage entry offset (TEXT_OFFSET, 0x8000)
	TUint32 iDtbOff; // dtb offset in the chunk
	TUint32 iMach; // machine type (r1, ~0 for dt)
	TUint32 iWdog; // watchdog reg phys (0 = none)
};

class RRunBlob : public RBusLogicalChannel{
	public:
		enum TControl{EControlRunBlob, EControlIdentityTest, EControlTrampTest,
			EControlFullTramp, EControlFbFill, EControlTakeover,
			EControlAllocBoot, EControlLoadChunk, EControlBoot,
			EControlTimedDiff, EControlTimerProbe, EControlRegProbe, EControlRegWrite, EControlKickLoop, EControlReadPhys,
			EControlVirtInfo, EControlReadVirt, EControlSecureTrace, EControlSmcProbe,
			EControlSnoopPoll, EControlHookInstall, EControlHookRead, EControlHookRemove,
			EControlHookCbus, EControlHookCbusRead, EControlHookCbusRemove,
			EControlModemReset, EControlPeek,
			EControlHookBeac8, EControlHookBeac8Read, EControlHookBeac8Remove,
			EControlWatchService, EControlSaveIsah};
	#ifndef __KERNEL_MODE__
		TInt Open(){return DoCreate(KRunBlobName, TVersion(1,0,0), KNullUnit, NULL, NULL);}
		TInt Run(TDes8& aBuf){return DoControl(EControlRunBlob, (TAny*)&aBuf);}

		// builds an armv6 identity section mapping (virt==phys) in the page table and
		// checks it, WITHOUT turning the mmu off. aBuf gets 8 words:
		// [0]=status(0 ok, 1 va already mapped so aborted, <0 err) [1]=phys [2]=L1 idx
		// [3]=descriptor written [4]=old descriptor
		// [5]=read via identity va (expect DEADBEEF) [6]=read-back reverse [7]=0
		TInt IdentityTest(TDes8& aBuf){return DoControl(EControlIdentityTest, (TAny*)&aBuf);}

		// trampoline stage 2: identity-map an executable page, jump to a stub that
		// turns off irq+mmu+caches, writes a marker to PHYS ram, turns the mmu back
		// on and returns. aBuf gets 8 words:
		// [0]=status(0 ok, <0 err) [1]=phys [2]=marker written [3]=marker read back
		// [4]=descriptor [5]=0 [6]=0 [7]=0
		TInt TrampTest(TDes8& aBuf){return DoControl(EControlTrampTest, (TAny*)&aBuf);}

		// FULL trampoline (but reversible): load a fake "kernel" at base+0x8000,
		// identity-map it, kill the mmu, hand off with r0=0/r1=machtype/r2=dtb, the
		// payload echoes those regs back. aBuf gets 8 words:
		// [0]=status [1]=phys [2]=echo r0(=0) [3]=echo r1(machtype) [4]=echo r2(dtb)
		// [5]=ran signature(=5a000000) [6]=0 [7]=0
		TInt FullTramp(TDes8& aBuf){return DoControl(EControlFullTramp, (TAny*)&aBuf);}

		// fill aBytes of phys ram at aPhys with aColor (to find the live
		// framebuffer, watch the screen for the flash)
		TInt FbFill(TUint32 aPhys, TUint32 aBytes, TUint32 aColor){
			TFbFill f;
			f.iPhys=aPhys; f.iBytes=aBytes; f.iColor=aColor;
			return DoControl(EControlFbFill, (TAny*)&f);
		}

		// ONE-WAY: kills symbian (mmu off) and drives the display from bare metal.
		// never returns, battery pull to recover. no buffer
		TInt Takeover(){return DoControl(EControlTakeover, NULL);}

		// ONE-WAY watchdog test: kills symbian (mmu off) and kicks both watchdogs
		// forever from bare metal. never returns
		TInt KickLoop(){return DoControl(EControlKickLoop, NULL);}

		// ONE-WAY trace: watchdog-fed bare-metal loop with counter-gap log at
		// phys 0x1f080000. Retrieve the 1 MiB RAM log with ramread after reset.
		TInt SecureTrace(){return DoControl(EControlSecureTrace, NULL);}

		// ONE-WAY secure-call feasibility probe: kill symbian, feed the watchdogs,
		// issue one smc #0 and log to phys 0x1f080000 whether the monitor returned.
		// read back with e6logs, parse with tools/parse_smcprobe.py
		TInt SmcProbe(){return DoControl(EControlSmcProbe, NULL);}

		// tight-poll securitykext's request descriptor to catch the transient
		// secure request. aBuf gets 40 words (desc + shared header + reqbuf) when a
		// request is caught. returns 1 = caught, 0 = idle this burst. read-only
		TInt SnoopPoll(TDes8& aBuf, TUint32 aIters){return DoControl(EControlSnoopPoll, (TAny*)&aBuf, (TAny*)aIters);}

		// redirect securitykext's transport fn to a logging trampoline so EVERY
		// secure request is captured into a ram ring (no transient race). Read
		// pulls the 0x1000-byte ring (ring[0]=count, entries of 7 words at +0x20).
		TInt HookInstall(){return DoControl(EControlHookInstall, NULL);}
		TInt HookRead(TDes8& aBuf){return DoControl(EControlHookRead, (TAny*)&aBuf);}
		TInt HookRemove(){return DoControl(EControlHookRemove, NULL);}

		// call Symbian's own SisaResetGenerate (0x801039e0) to reset/quiesce the
		// modem via the firmware's real sequence, while Symbian is still alive.
		// aType must have bit 0x80 set (0x80|resettype). returns 0.
		TInt ModemReset(TUint32 aType){return DoControl(EControlModemReset, (TAny*)aType);}

		// read ONE 32-bit mmio register at phys aPhys through a fresh UNCACHED
		// (device) mapping, so polling sees live changes (ReadPhys is cached =
		// stale for mmio). for watching the modem/ISA keepalive register.
		TInt Peek(TUint32 aPhys, TUint32* aOut){return DoControl(EControlPeek, (TAny*)aPhys, (TAny*)aOut);}

		// hook the ASIC modem TX send FUN_800beac8 (0x800beac8): log {descriptor,
		// channel, word0} of every message Symbian writes to the modem TX FIFO.
		// catches the exact EM (resource 0xA9) sleep/awake bytes to replay in Linux.
		TInt HookBeac8(){return DoControl(EControlHookBeac8, NULL);}
		TInt HookBeac8Read(TDes8& aBuf){return DoControl(EControlHookBeac8Read, (TAny*)&aBuf);}
		TInt HookBeac8Remove(){return DoControl(EControlHookBeac8Remove, NULL);}

		// sleep until just before symbian's next ~210 s reset service (tick
		// 210000*k - 900 ms), record every thread's cpu time each 200 ms
		TInt WatchService(TDes8& aBuf){return DoControl(EControlWatchService, (TAny*)&aBuf);}

		// E5 (e5boot_ldd RB_E5_ISAH): write the version 2 handoff block at phys
		// 0x1f0f1000 (cp15 regs, kernel L1, the L2 tables of the ISASW windows).
		// aBuf gets 11 words: ttbr0, ttbr1, ttbcr, the 8 L1 descriptors
		TInt SaveIsah(TDes8& aBuf){return DoControl(EControlSaveIsah, (TAny*)&aBuf);}

		// patch karap's cbus-write fn 0x800b8494 to log every PMIC/CBUS write
		// {dev,reg,val} into a ram ring (ring[0]=count, 16-byte slots at +4).
		// catches any hidden periodic pet done over cbus. returns 0 or <0.
		TInt HookCbus(){return DoControl(EControlHookCbus, NULL);}
		TInt HookCbusRead(TDes8& aBuf){return DoControl(EControlHookCbusRead, (TAny*)&aBuf);}
		TInt HookCbusRemove(){return DoControl(EControlHookCbusRemove, NULL);}

		// booter: alloc a contiguous boot chunk, stream zImage+dtb in, boot
		TInt AllocBoot(TUint32 aSize, TUint32& aPhysOut){
			return DoControl(EControlAllocBoot, (TAny*)&aSize, (TAny*)&aPhysOut);
		}
		TInt LoadChunk(TUint32 aOffset, const TUint8* aSrc, TUint32 aLen){
			TLoadChunk c;
			c.iOffset=aOffset; c.iSrc=(TUint32)aSrc; c.iLen=aLen;
			return DoControl(EControlLoadChunk, (TAny*)&c);
		}
		// never returns if it works
		TInt Boot(TUint32 aZImageOff, TUint32 aDtbOff, TUint32 aMach, TUint32 aWdog){
			TBootParams b;
			b.iZImageOff=aZImageOff; b.iDtbOff=aDtbOff;
			b.iMach=aMach; b.iWdog=aWdog;
			return DoControl(EControlBoot, (TAny*)&b);
		}

		// reads aCount words at virt aVirt, spins aDelay loops, reads again. aBuf gets
		// A then B (2*aCount words). regs that change A->B are free-running
		// counters -> system timer and watchdog. (safe, 0x30000000 reads fine)
		TInt TimedDiff(TUint32 aVirt, TUint32 aCount, TUint32 aDelay, TDes8& aBuf){
			TFbFill p;
			p.iPhys=aVirt; p.iBytes=aCount; p.iColor=aDelay;
			return DoControl(EControlTimedDiff, (TAny*)&p, (TAny*)&aBuf);
		}

		// checks the timers reversed from the firmware (docs/driver-re.md), only
		// touches regs symbian itself uses: fast counter 0x20033350 (latch 0x354)
		// and the ms timer 0x200337a4/0x7a8. aBuf gets KTimerProbeWords words
		TInt TimerProbe(TDes8& aBuf){return DoControl(EControlTimerProbe, (TAny*)&aBuf);}

		// reads one word from each virt address in aAddrs (KRegProbeMax max, only
		// the c6 peripheral window, others give 0xdeadc6c6). aBuf gets one word each
		TInt RegProbe(const TDesC8& aAddrs, TDes8& aBuf){return DoControl(EControlRegProbe, (TAny*)&aAddrs, (TAny*)&aBuf);}

		// writes aVal to one virt address in the c6 peripheral window (KErrArgument otherwise)
		TInt RegWrite(TUint32 aVirt, TUint32 aVal){return DoControl(EControlRegWrite, (TAny*)aVirt, (TAny*)aVal);}

		// maps physical RAM at aPhys (page aligned) and copies aBuf.MaxLength() bytes into aBuf
		TInt ReadPhys(TUint32 aPhys, TDes8& aBuf){return DoControl(EControlReadPhys, (TAny*)aPhys, (TAny*)&aBuf);}
		// aOut[0] = Kern::SuperPage(), aOut[1] = phys of aVirt (~0 = unmapped)
		TInt VirtInfo(TUint32 aVirt, TUint32* aOut){return DoControl(EControlVirtInfo, (TAny*)aVirt, (TAny*)aOut);}
		// copy aBuf.MaxLength() bytes of kernel virtual memory at aVirt. pages
		// that don't read come back as 0xee, return = how many pages failed
		TInt ReadVirt(TUint32 aVirt, TDes8& aBuf){return DoControl(EControlReadVirt, (TAny*)aVirt, (TAny*)&aBuf);}
	#endif
};

#endif // RUNBLOB_H