/* e5rt: the Nokia E5's own modem firmware (ISASW, fw 101.003) as an ordinary
 * ARM process, ported from upstream's E6 runtime (linux/isi-runtime) to the E5
 * addresses in docs/devices/e5-00/cellular-map.md. Development only: no SIM,
 * no OS timers, no radio yet. Needs the firmware VAs in user space: qemu-arm on
 * the PC or the VMSPLIT_3G25 kernel (build-e5.sh with E5_CELL=1) on the phone.
 * The images come from the SAME boot (e5mem module): code 10480000.bin, data
 * 1fd50000.bin, secure pool 1ffe0000.bin, top area 1fb60000.bin.
 *   e5rt <dir>                       100 MTC version request/reply cycles
 *   e5rt <dir> --dsp-selftest        100 synthetic DSP->AP deliveries (needs low-10000000.bin)
 *   e5rt <dir> --dsp-tx-selftest     100 AP->DSP publishes + acks
 *   e5rt <dir> --replay d0 d1 d2 d3  one DSP descriptor from the dump (needs low-11200000.bin)
 *   e5rt <dir> --mdi /dev/rapuyama-mdi [--probe dev:res] [--seconds n, 0 = forever]
 *                [--backend /dev/rapuyama-isi]   live: real MDI rings, Phonet backend
 *                (E5RT_SIM_DEVICE=/dev/rapuyama-sim: embedded SIM backend, PMIC/ADC/IRQs via the bridge;
 *                 E5RT_OS_CLOCK=1: firmware OS timers on the Linux clock, needs the SIM backend)
 *   e5rt <dir> --sim-backend-selftest   embedded SIM backend against the simulated card
 *   E5RT_IMEI_FILE=<file with the 15-digit IMEI>: the firmware's identity reader answers from it
 *   e5rt <dir> --identity-selftest      (needs E5RT_IMEI_FILE, use a test IMEI)
 *   e5rt <dir> --kasumi-selftest        software UMTS f8 (ETSI vector) instead of the crypto hardware
 *   e5rt <dir> --os-soak <seconds>      offline: simulated embedded SIM + OS timers on the real clock +
 *                                       scheduler, no MDI (needs low-11200000.bin for the RAM drives)
 *   e5rt <dir> --os-timer-selftest | --os-timer-live-selftest   upstream OS timer tests
 * One live start per Linux boot (the DSP ring state moves on). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>
#include "sim_runtime.h"
#include "os_timer_runtime.h"
#include "identity_runtime.h"
#include "kasumi_runtime.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define CODE 0xc6300000u	/* phys 0x10480000 */
#define CODE_SIZE 0x3ae000u
#define DATA 0xc6800000u	/* phys 0x1fd50000 */
#define DATA_SIZE 0x280000u
#define SECURE 0xc6b00000u	/* phys 0x1ffe0000 */
#define SECURE_SIZE 0x10000u

/* E5 firmware entry points (thumb, bit 0 set) */
#define FW_ALLOCATE 0xc63c646du
#define FW_SEND 0xc635b4e3u	/* frame from the host -> router task 0x21 */
static int quiet;	/* E5RT_QUIET=1: no per-frame MDI / host frame lines (each was dozens of unbuffered writes) */
#define FW_RECEIVE 0xc63bef51u
#define FW_RELEASE 0xc63c69bfu
#define FW_SCHEDULE 0xc63bca37u
#define FW_MDI_RX_IRQ 0xc63c8f6du	/* IRQ 0x13 handler */
#define FW_MDI_TX_IRQ 0xc63c9031u	/* IRQ 0x15: AP descriptors consumed */
#define FW_MDI_EVENT_IRQ 0xc63c8e0du	/* IRQ 0x17 */
#define SWD_STACK 0xd2000u		/* PA_SWD borrowed stack inside the ISASW data (sp c68d7e6c) */
#define MDI_AP 0xc601bce0u	/* AP->DSP ring header + 16 x 4-word slots (pa 0x30003ce0) */
#define MDI_DSP 0xc601bdf0u	/* DSP->AP ring (pa 0x30003df0) */
#define DSP_BUF 0xcb500000u	/* DSP shared buffers, pa 0x10140000 (dsp address = pa - 0x10000000) */
#define DATA_PA 0x1fd50000u
#define FW_ASSERT 0xc63d8864u	/* thumb handler (expr, file, line) */
#define FW_RESET 0xc63d7be4u	/* reset(reason) */
#define HOST_QUEUE 0xc6836838u	/* queue of task 0x2f (E6: 0xc681c11c, task 0x30) */
#define SCHED 0xc6835c3cu	/* +2 = current task, 0x2f when idle */
#define STACK_GUARDS 0xc6672564u	/* per task: address of its stack guard word, must hold 0xaaaaaaaa (task switch c6658af4) */
static int stack_guard_broken(unsigned t)
{
	uint32_t g = ((const uint32_t *)(uintptr_t)STACK_GUARDS)[t];
	return g >= DATA && g + 4 <= DATA + DATA_SIZE && *(volatile uint32_t *)(uintptr_t)g != 0xaaaaaaaau;
}

static void region(uint32_t address, size_t length)
{
	if (mmap((void *)(uintptr_t)address, length, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED) {
		fprintf(stderr, "map %08x+%zx: %s\n", address, length, strerror(errno));
		exit(1);
	}
}

static void image(const char *dir, const char *name, uint32_t address, size_t size)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	int fd = open(path, O_RDONLY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) || (size_t)st.st_size != size) {
		fprintf(stderr, "image %s: missing or not %zu bytes\n", path, size);
		exit(1);
	}
	region(address, size);
	size_t off = 0;
	ssize_t n;
	while (off < size && (n = read(fd, (void *)(uintptr_t)(address + off), size - off)) > 0)
		off += n;
	close(fd);
	if (off != size) {
		fprintf(stderr, "image %s: short read\n", path);
		exit(1);
	}
}

/* part of a dump file at an offset (no mmap: the region is already mapped) */
static void fill(const char *dir, const char *name, off_t offset, uint32_t address, size_t size)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	int fd = open(path, O_RDONLY);
	if (fd < 0 || pread(fd, (void *)(uintptr_t)address, size, offset) != (ssize_t)size) {
		fprintf(stderr, "fill %s+%lx: failed\n", path, (long)offset);
		exit(1);
	}
	close(fd);
}

static void firmware_assert(uint32_t expression, uint32_t file, uint32_t line)
{
	fprintf(stderr, "firmware assertion: expr=%08x file=%08x line=%u\n", expression, file, line);
	if (expression >= CODE && expression < CODE + CODE_SIZE && file >= CODE && file < CODE + CODE_SIZE)
		fprintf(stderr, "%.192s: %u: %.192s\n", (char *)(uintptr_t)file, line, (char *)(uintptr_t)expression);
	_exit(120);
}

uint32_t e5rt_reset_regs[4];	/* r4-r7 of the firmware caller (reset_entry) */
void e5rt_firmware_reset(uint32_t reason, uint32_t unused1, uint32_t unused2, uint32_t unused3, uint32_t caller);
/* the thumb trap enters here in ARM state: keep r4-r7, pass the firmware lr as 5th argument */
__attribute__((naked)) static void reset_entry(void)
{
	__asm__ volatile("ldr r3, =e5rt_reset_regs\n\tstm r3, {r4-r7}\n\tpush {lr}\n\tbl e5rt_firmware_reset\n\t.ltorg\n");
}
void e5rt_firmware_reset(uint32_t reason, uint32_t unused1, uint32_t unused2, uint32_t unused3, uint32_t caller)
{
	(void)unused1; (void)unused2; (void)unused3;
	fprintf(stderr, "firmware reset requested: %08x (called from %08x)\n", reason, caller);
	if (reason == 6)	/* task switch c6658af4: a task stack guard lost its 0xaaaaaaaa canary */
		for (unsigned t = 0; t < 48; t++) {
			if (stack_guard_broken(t)) {
				uint32_t g = ((const uint32_t *)(uintptr_t)STACK_GUARDS)[t];
				fprintf(stderr, "task %u (0x%02x) stack guard %08x overwritten; deepest frames (code-like words above the guard):\n", t, t, g);
				/* leftovers of the deepest frames: firmware return addresses (thumb, c63..c66 / c60f patch
				   window) and e5rt's own text (our hooks), offset from the guard */
				for (unsigned i = 0; i < 96 && g + 4 * i + 4 <= DATA + DATA_SIZE; i++) {
					uint32_t v = ((const uint32_t *)(uintptr_t)g)[i];
					extern char __executable_start[], etext[];
					int fw = (v >= 0xc6300000 && v < 0xc66ae000) || (v >= 0xc60f0000 && v < 0xc60f1000);
					int ours = v >= (uintptr_t)__executable_start && v < (uintptr_t)etext;
					if (fw || ours || i < 4)
						fprintf(stderr, "  +%03x %08x%s\n", 4 * i, v, ours ? " (e5rt)" : "");
				}
			}
		}
	for (unsigned r = 0; r < 4; r++) {	/* what the caller was working on (e.g. the parsed message) */
		uint32_t v = e5rt_reset_regs[r];
		fprintf(stderr, "r%u=%08x", r + 4, v);
		if (v >= DATA + 16 && v + 112 <= DATA + DATA_SIZE)
			for (unsigned i = 0; i < 112; i++)
				fprintf(stderr, "%s%02x", i % 16 ? " " : "\n  ", ((unsigned char *)(uintptr_t)(v - 16))[i]);
		fprintf(stderr, "\n");
	}
	_exit(121);
}

static void ring_panic(uint32_t text)
{
	volatile uint32_t *ap = (void *)(uintptr_t)0xc601bce0u, *dsp = (void *)(uintptr_t)0xc601bdf0u;
	fprintf(stderr, "MDI ring panic from %08lx: %.80s (ap hdr %08x, dsp hdr %08x)\n",
		(unsigned long)(uintptr_t)__builtin_return_address(0), (char *)(uintptr_t)text, ap[0], dsp[0]);
	_exit(123);
}

static void thumb16(uint32_t address, uint16_t value) { memcpy((void *)(uintptr_t)address, &value, 2); }
static void word(uint32_t address, uint32_t value) { memcpy((void *)(uintptr_t)address, &value, 4); }

/* thumb "ldr r3,[pc,#0]; bx r3" + handler word: the site must be 4-byte aligned */
static void trap(uint32_t site, void (*handler)())
{
	thumb16(site, 0x4b00);
	thumb16(site + 2, 0x4718);
	word(site + 4, (uint32_t)(uintptr_t)handler);
}

/* E5RT_MSG_TRACE=1: firmware inter-task messages (send to task c63bdbe4), armed by the first host
 * frame to GPDS (0x31). Per message: destination task, current task and the first 10 bytes (the
 * Phonet-like header). The hook keeps to a few words of stack (firmware task stacks are small);
 * printing happens in the serve loop. Trampoline = the 8 original bytes + jump back to the bl. */
#define FW_SEND_TASK 0xc63bdbe4u
static int msg_trace, msg_armed;
static struct { uint32_t ms; uint8_t to, from, n, head[64]; } msgs[2048];
static unsigned msgs_l1;	/* L1 transport-block messages captured at 64 bytes (first 400) */
static unsigned msgs_head, msgs_tail, msgs_printed, msgs_dropped;
static uint32_t (*send_task_original)(uint32_t, uint32_t);
static uint32_t (*const current_task)(void) = (void *)(uintptr_t)0xc63bdbc9u;
static uint64_t monotonic_ms(void);
static uint32_t send_task_hook(uint32_t task, uint32_t message)
{
	if (msg_armed) {
		if (msgs_head - msgs_tail < 2048) {
			typeof(msgs[0]) *r = &msgs[msgs_head % 2048];
			r->ms = (uint32_t)monotonic_ms();
			r->to = task;
			r->from = current_task();
			r->n = message >= DATA && message + 64 <= DATA + DATA_SIZE ? 10 : 0;
			if (r->n) {
				memcpy(r->head, (void *)(uintptr_t)message, 64);
				if (r->head[3] == 0x31 || r->head[3] == 0xc3)	/* GPDS: 24 bytes (no identities) */
					r->n = 24;
				/* DSP link transport-block indications d8/e8 and the uplink 61: 64 bytes, to find
				   DSP buffer addresses the copied-buffer driver mode does not refresh */
				else if (r->head[3] == 0xed && (r->head[9] == 0xd8 || r->head[9] == 0xe8 || r->head[9] == 0x61) && msgs_l1 < 400) {
					msgs_l1++;
					r->n = 64;
				}
			}
			msgs_head++;
		} else
			msgs_dropped++;
	}
	return send_task_original(task, message);
}
static void msg_trace_install(void)
{
	const unsigned char entry[] = {0xf0, 0xb5, 0x85, 0xb0, 0x04, 0x46, 0x0d, 0x46};
	if (memcmp((void *)(uintptr_t)FW_SEND_TASK, entry, sizeof(entry))) {
		fprintf(stderr, "message trace: unexpected send-to-task entry, not installed\n");
		return;
	}
	unsigned char *t = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (t == MAP_FAILED) {
		perror("message trace");
		return;
	}
	memcpy(t, entry, 8);
	const uint16_t back[] = {0x4f00, 0x4738};	/* ldr r7,[pc,#0]; bx r7 */
	memcpy(t + 8, back, 4);
	uint32_t resume = (FW_SEND_TASK + 8) | 1;
	memcpy(t + 12, &resume, 4);
	__builtin___clear_cache((char *)t, (char *)t + 16);
	send_task_original = (void *)((uintptr_t)t | 1);
	trap(FW_SEND_TASK, (void (*)())send_task_hook);
	msg_trace = getenv("E5RT_MSG_TRACE") && !strcmp(getenv("E5RT_MSG_TRACE"), "data") ? 2 : 1;
	fprintf(stderr, "message trace installed (armed by the first %s)\n", msg_trace == 2 ? "uplink user-data frame" : "GPDS request");
}
static void msg_trace_flush(void)
{
	while (msgs_tail != msgs_head) {
		typeof(msgs[0]) *r = &msgs[msgs_tail++ % 2048];
		if (msgs_printed++ >= 6000)
			continue;
		printf("msg %u %02x<%02x:", r->ms, r->to, r->from);
		for (unsigned i = 0; i < r->n; i++)
			printf(" %02x", r->head[i]);
		printf("\n");
	}
}

/* upstream prepare.py's code patches, at the E5 addresses (core-map.md) */
static void patches(void)
{
	static const uint32_t bx_lr[] = {
		0xc63d83cc, 0xc63d83da,		/* critical section enter/exit: INTC writes */
		0xc63b9d16,			/* c6033340 timer helper */
		0xc6658dcc,			/* veneer */
		0xc63ec10c, 0xc63ec13e, 0xc63ec17c, 0xc63ec1ba, 0xc63ec1f8,
	};
	for (unsigned i = 0; i < sizeof(bx_lr) / sizeof(bx_lr[0]); i++)
		thumb16(bx_lr[i], 0x4770);
	thumb16(0xc63ec494, 0x2000);	/* movs r0,#0; bx lr */
	thumb16(0xc63ec496, 0x4770);
	/* E6 had INTC wrappers, the E5 inlines them: NOP the two bl's */
	thumb16(0xc63b9bba, 0x46c0); thumb16(0xc63b9bbc, 0x46c0);
	thumb16(0xc63b9cfa, 0x46c0); thumb16(0xc63b9cfc, 0x46c0);
	word(0xc6658ce4, 0xe3a00013);	/* get CPSR -> SVC mode */
	word(0xc6658ce8, 0xe12fff1e);
	word(0xc6658da4, 0xe12fff1e);	/* cache barrier -> bx lr */
	trap(FW_ASSERT, (void (*)())firmware_assert);
	trap(FW_RESET, reset_entry);
	trap(0xc63c9d30, (void (*)())ring_panic);	/* MDI ring dump + reset 0xd (diagnostic) */
	/* DSP measurement frames (res 0x38 msg 0x15, from dev 0x7c) with a type-1 sub-block whose u16 at +6
	 * is 0 make the sub-block parser c635492c call reset 0xa after it has already handled the block
	 * (phone xo2/xo5: the DSP keeps the radio session it had under Symbian). Development patch: NOP the
	 * reset call, the parser continues with the next sub-block (no E6 twin, upstream never met it) */
	static const unsigned char reset_a[] = {0x0a, 0x20, 0x83, 0xf0, 0xee, 0xf8};	/* movs r0,#0xa; bl c63d7be4 at c6354a02 */
	if (!memcmp((void *)0xc6354a02u, reset_a, sizeof(reset_a))) {
		thumb16(0xc6354a04, 0x46c0);
		thumb16(0xc6354a06, 0x46c0);
	}
}

static char altstack[65536];
static void failure(int sig, siginfo_t *info, void *context)
{
	if (sig == SIGSEGV && isi_sim_fault(info, context))
		return;	/* a trapped SIM register access, forwarded to the bridge */
	ucontext_t *uc = context;
	char text[200];
	int n = snprintf(text, sizeof(text), "signal %d at %08lx pc %08lx lr %08lx sp %08lx\n", sig,
			 (unsigned long)(uintptr_t)info->si_addr, (unsigned long)uc->uc_mcontext.arm_pc,
			 (unsigned long)uc->uc_mcontext.arm_lr, (unsigned long)uc->uc_mcontext.arm_sp);
	write(2, text, n);
	uint32_t sp = uc->uc_mcontext.arm_sp;
	if (sp >= DATA && sp + 64 <= DATA + DATA_SIZE && !(sp & 3)) {	/* return addresses for the diagnosis */
		n = snprintf(text, sizeof(text), "stack:");
		for (unsigned i = 0; i < 16; i++)
			n += snprintf(text + n, sizeof(text) - n, " %08x", ((uint32_t *)(uintptr_t)sp)[i]);
		text[n++] = '\n';
		write(2, text, n);
	}
	_exit(122);
}

static void *(*allocate)(unsigned) = (void *)(uintptr_t)FW_ALLOCATE;
static void (*send)(void *) = (void *)(uintptr_t)FW_SEND;
static unsigned char *(*receive)(void) = (void *)(uintptr_t)FW_RECEIVE;
static void (*release)(void *) = (void *)(uintptr_t)FW_RELEASE;
static void (*schedule)(void) = (void *)(uintptr_t)FW_SCHEDULE;
static void (*mdi_rx_irq)(void) = (void *)(uintptr_t)FW_MDI_RX_IRQ;

/* upstream dsp_selftest: 100 synthetic DSP->AP messages through the rx ring */
static int dsp_selftest(void)
{
	volatile uint32_t *ap = (void *)(uintptr_t)MDI_AP, *dsp = (void *)(uintptr_t)MDI_DSP;
	unsigned char *buffer = (void *)(uintptr_t)(DSP_BUF + 0x1000);	/* dsp address 0x141000 */
	const unsigned char message[] = {0x11, 0x22, 0x33, 0x44, 0x12, 0x6c, 0x7c, 0x15, 0, 10, 0xa1, 0x1e, 0x33, 0xf0, 0x13, 18, 2, 0, 0, 0};
	const unsigned char expected[] = {0x15, 0x6c, 0x7c, 0x15, 10, 0, 0xa1, 0x1e, 0x33, 0xf0, 0x13, 18, 2, 0, 0, 0};
	volatile unsigned char *q = (void *)(uintptr_t)HOST_QUEUE;
	*ap = 0;
	*dsp = 0;
	for (unsigned t = 0; t < 100; t++) {
		unsigned char *current = buffer + t * 256;
		uint32_t tag = 0x11223344 + t;
		memcpy(current, message, sizeof(message));
		current[0] = tag >> 24; current[1] = tag >> 16; current[2] = tag >> 8; current[3] = tag;
		current[12] = t;
		unsigned producer = t & 31;
		*dsp = ((t + 1) & 31) | ((*ap & 31) << 16);
		volatile uint32_t *slot = dsp + 1 + (producer & 15) * 4;
		slot[0] = tag;
		slot[1] = 0x1ea10a00;
		slot[2] = 0x1600f000 | t;
		slot[3] = 0x141000 + t * 256;
		mdi_rx_irq();
		schedule();
		if (q[16] == q[17] || q[28] != 1) {
			fprintf(stderr, "dsp %u: host queue %u/%u/%u\n", t, q[16], q[17], q[28]);
			return 1;
		}
		unsigned char *reply = receive();
		unsigned char answer[sizeof(expected)];
		memcpy(answer, expected, sizeof(answer));
		answer[8] = t;
		if ((uintptr_t)reply < DATA || (uintptr_t)reply + sizeof(answer) > DATA + DATA_SIZE || memcmp(reply, answer, sizeof(answer))) {
			fprintf(stderr, "dsp %u: packet mismatch at %p:", t, (void *)reply);
			for (unsigned i = 0; i < 16; i++)
				fprintf(stderr, " %02x", reply[i]);
			fprintf(stderr, "\n");
			return 1;
		}
		release(reply);
		if (q[28]) {
			fprintf(stderr, "dsp %u: host queue not empty\n", t);
			return 1;
		}
	}
	puts("E5 native ARM firmware: 100 synthetic DSP deliveries, wraps and frees passed");
	return 0;
}

/* upstream dsp_tx_selftest: 100 host->DSP frames published on the tx ring, acked, freed */
static int dsp_tx_selftest(void)
{
	volatile uint32_t *ap = (void *)(uintptr_t)MDI_AP, *dsp = (void *)(uintptr_t)MDI_DSP;
	unsigned char *first = NULL;
	*ap = 0;
	*dsp = 0;
	for (unsigned t = 0; t < 100; t++) {
		unsigned char message[] = {0x15, 0x7c, 0x6c, 0x15, 10, 0, 0, 0xa1, t, 0xf0, 0x12, 0, 0, 0, 0, 0};
		unsigned producer = *ap & 31;
		void *p = allocate(sizeof(message));
		if (!p) {
			fprintf(stderr, "tx %u: allocator exhausted\n", t);
			return 1;
		}
		memcpy(p, message, sizeof(message));
		send(p);
		if ((*ap & 31) != ((producer + 1) & 31)) {
			fprintf(stderr, "tx %u: no MDI publish (ap %08x)\n", t, *ap);
			return 1;
		}
		volatile uint32_t *sent = ap + 1 + (producer & 15) * 4;
		/* type 06 descriptors carry AP physical minus 0x10000000 */
		unsigned char *packet = (void *)(uintptr_t)(sent[3] + 0x10000000 - DATA_PA + DATA);
		if (t == 0)
			printf("first tx descriptor %08x %08x %08x %08x\n", sent[0], sent[1], sent[2], sent[3]);
		if (sent[2] >> 24 != 6 || (uintptr_t)packet < DATA || (uintptr_t)packet + sizeof(message) > DATA + DATA_SIZE) {
			fprintf(stderr, "tx %u: invalid MDI descriptor %08x %08x %08x %08x\n", t, sent[0], sent[1], sent[2], sent[3]);
			return 1;
		}
		message[0] = 0x12;
		message[4] = 0;
		message[5] = 10;
		if (memcmp(packet, message, sizeof(message))) {
			fprintf(stderr, "tx %u: MDI packet mismatch\n", t);
			return 1;
		}
		unsigned consumer = *dsp & 31;
		*dsp = ((consumer + 1) & 31) | ((*ap & 31) << 16);
		volatile uint32_t *slot = dsp + 1 + (consumer & 15) * 4;
		slot[0] = sent[3];
		slot[1] = 0;
		slot[2] = 0x19020001;
		slot[3] = 0;
		mdi_rx_irq();
		schedule();
		/* a freed packet is handed out again by the next allocation */
		if (!first)
			first = packet;
		else if (packet != first) {
			fprintf(stderr, "tx %u: AP packet not freed\n", t);
			return 1;
		}
	}
	puts("E5 native ARM firmware: 100 DSP publishes and acknowledgement frees passed");
	return 0;
}

/* SBIF consumer (upstream sbif_runtime.h, E5 addresses): the firmware queues
 * sector writes for the host; api(13, callback) hands them over. They go to a
 * private volatile store, the physical RAM drives are never written. Without it
 * a firmware task that writes a sector waits forever. */
#define SBIF_API 0xc6346c37u		/* E6 c623ae0f: cmd 13 = drain to callback */
#define SBIF_CONTROL 0xc682baa4u	/* E6 c6813f48/58, same layout */
static unsigned char sbif_store[0xfc000];
static int sbif_attached;
static unsigned sbif_writes;
static _Noreturn void sbif_invalid(const char *reason)
{
	fprintf(stderr, "private SBIF: %s\n", reason);
	_exit(124);
}
static int sbif_guest(uintptr_t p, unsigned size)
{
	return p >= DATA && size <= DATA_SIZE && p <= DATA + DATA_SIZE - size;
}
static uint32_t sbif_write(uint32_t status, uint32_t sector, uint32_t size, uint32_t reserved, const void *data)
{
	if (status || reserved || !size || size % 512 || size > sizeof(sbif_store) || sector >= 2016 ||
	    size / 512 > 2016 - sector || !sbif_guest((uintptr_t)data, size))
		sbif_invalid("invalid sector callback");
	memcpy(sbif_store + sector * 512, data, size);
	sbif_writes += size / 512;
	printf("SBIF: %u sector(s) at %u -> private store (%u total)\n", size / 512, sector, sbif_writes);
	return 0;
}
static void sbif_validate_queue(void)
{
	uint32_t queue = *(const uint32_t *)(uintptr_t)(SBIF_CONTROL + 8);
	if (!queue)
		return;
	if (!sbif_guest(queue, 8))
		sbif_invalid("queue range");
	uint32_t node = *(const uint32_t *)(uintptr_t)queue;
	unsigned count = 0;
	while (node) {
		if (++count > 256 || !sbif_guest(node, 8))
			sbif_invalid("queue bound or cycle");
		const uint32_t *entry = (void *)(uintptr_t)node;
		if (!sbif_guest(entry[0], 515))
			sbif_invalid("queued sector range");
		const unsigned char *d = (void *)(uintptr_t)entry[0];
		unsigned sector = d[512] | (unsigned)d[513] << 8;
		if (d[514] > 1 || sector >= 1008)
			sbif_invalid("queued sector metadata");
		node = entry[1];
	}
}
static void sbif_service(void)
{
	if (!sbif_attached)
		return;
	sbif_validate_queue();
	uint32_t (*api)(uint32_t, uintptr_t) = (void *)(uintptr_t)SBIF_API;
	if (api(13, (uintptr_t)sbif_write))
		sbif_invalid("register/drain failed");
}
static void sbif_attach(void)
{
	memcpy(sbif_store, (void *)(uintptr_t)0xcb600000u, sizeof(sbif_store));
	sbif_attached = 1;
	sbif_service();
	puts("SBIF consumer: volatile private backing; physical disks unchanged");
}

/* ---- live mode: the kernel's MDI driver (drivers/soc/rapuyama/mdi.c, nokia,e5-mdi) ---- */
struct rapuyama_mdi_status {
	uint32_t version, ap, dsp, rx_interrupts, tx_interrupts, event_interrupts;
};
#define RAPUYAMA_MDI_STATUS _IOR('M', 0, struct rapuyama_mdi_status)
#define RAPUYAMA_MDI_BUFFERS _IOW('M', 1, uint32_t)
static int mdi = -1;
static unsigned forwarded;
static uint32_t seen_tx, seen_event;

/* firmware -> DSP: publish what the firmware wrote to its private AP ring */
static int mdi_forward(void)
{
	volatile uint32_t *ap = (void *)(uintptr_t)MDI_AP;
	while (forwarded != (*ap & 31)) {
		volatile uint32_t *slot = ap + 1 + (forwarded & 15) * 4;
		uint32_t d[4] = {slot[0], slot[1], slot[2], slot[3]};
		if (d[2] >> 24 == 6 && d[3] + 0x10000000u - DATA_PA - SWD_STACK < 0x6000) {
			fprintf(stderr, "mdi publish into the PA_SWD stack blocked: %08x %08x %08x %08x\n", d[0], d[1], d[2], d[3]);
			return 1;
		}
		if (d[2] == 0x19020007 && (!d[0] || !d[1] || !d[3])) {
			fprintf(stderr, "mdi dealloc with an empty tag blocked: %08x %08x %08x %08x\n", d[0], d[1], d[2], d[3]);
			return 1;
		}
		ssize_t n = write(mdi, d, sizeof(d));
		if (n < 0 && errno == EAGAIN)
			return 0;
		if (n != sizeof(d)) {
			fprintf(stderr, "mdi publish failed: %s\n", strerror(errno));
			return 1;
		}
		if (!quiet)
			printf("mdi tx %08x %08x %08x %08x\n", d[0], d[1], d[2], d[3]);
		forwarded = (forwarded + 1) & 31;
	}
	return 0;
}

/* DSP -> firmware: completions, events, received descriptors */
static int mdi_pump(void)
{
	volatile uint32_t *dsp = (void *)(uintptr_t)MDI_DSP;
	void (*consumed)(void) = (void *)(uintptr_t)FW_MDI_TX_IRQ, (*event)(void) = (void *)(uintptr_t)FW_MDI_EVENT_IRQ;
	struct rapuyama_mdi_status st;
	uint32_t d[4];
	ssize_t n;
	if (ioctl(mdi, RAPUYAMA_MDI_STATUS, &st)) {
		fprintf(stderr, "mdi status failed: %s\n", strerror(errno));
		return 1;
	}
	*dsp = (*dsp & 0xffff) | (st.dsp & 0xffff0000);
	if (st.tx_interrupts != seen_tx) {
		seen_tx = st.tx_interrupts;
		consumed();
		schedule();
	}
	if (st.event_interrupts != seen_event) {
		seen_event = st.event_interrupts;
		printf("mdi event %u\n", seen_event);
		event();
		schedule();
	}
	while ((n = read(mdi, d, sizeof(d))) == sizeof(d)) {
		if (!quiet)
			printf("mdi rx %08x %08x %08x %08x\n", d[0], d[1], d[2], d[3]);
		unsigned producer = *dsp & 31;
		volatile uint32_t *slot = dsp + 1 + (producer & 15) * 4;
		slot[0] = d[0]; slot[1] = d[1]; slot[2] = d[2]; slot[3] = d[3];
		*dsp = (*dsp & 0xffff0000) | ((producer + 1) & 31);
		mdi_rx_irq();
		schedule();
		sbif_service();
		if (mdi_forward())
			return 1;
	}
	if (n >= 0 || errno != EAGAIN) {
		fprintf(stderr, "mdi receive failed: %s\n", n < 0 ? strerror(errno) : "short read");
		return 1;
	}
	return mdi_forward();
}

/* firmware -> host: to the Phonet backend (/dev/rapuyama-isi) or printed */
static int backend = -1;
static int drain(void)
{
	volatile unsigned char *q = (void *)(uintptr_t)HOST_QUEUE;
	for (unsigned i = 0; i < 64 && q[16] != q[17]; i++) {
		unsigned char *r = receive();
		if ((uintptr_t)r < DATA || (uintptr_t)r > DATA + DATA_SIZE - 16)
			return 1;
		unsigned size = ((unsigned)r[4] | (unsigned)r[5] << 8) + 6;
		if (size < 10 || (uintptr_t)r + size > DATA + DATA_SIZE)
			return 1;
		if (!quiet) {
			printf("host rx (%u):", size);
			for (unsigned j = 0; j < size && j < 48; j++)
				printf(" %02x", r[j]);
			printf("\n");
		}
		if (backend >= 0 && write(backend, r, size) != (ssize_t)size) {
			static unsigned dropped;	/* phonet0 down (nobody up yet) or full: drop, like a radio link */
			if (errno != ENETDOWN && errno != ENOBUFS && errno != EAGAIN) {
				fprintf(stderr, "backend write failed: %s\n", strerror(errno));
				return 1;
			}
			if (!dropped++ || !(dropped % 100))
				fprintf(stderr, "backend: %u host frame(s) dropped (%s)\n", dropped, strerror(errno));
		}
		release(r);
	}
	return 0;
}

/* SBIF RAM drives 9/10 (va cb600000 + cb67e000, 2 x 0x7e000) live at pa 0x11200000
 * (probe e5l2pt), below Linux RAM: map them read-only from /dev/mem and give the
 * firmware a private copy, the physical drives are never written (upstream
 * ramdrives_private) */
#define RAMDRIVES 0xcb600000u
#define RAMDRIVES_PA 0x11200000u
#define RAMDRIVES_SIZE 0xfc000u
static int ramdrives(void)
{
	const uint32_t expected[] = {RAMDRIVES, 0x200, 0x7e000, 0, RAMDRIVES + 0x7e000, 0x200, 0x7e000, 1};
	if (memcmp((void *)(uintptr_t)0xc6823f3cu, expected, sizeof(expected))) {
		fprintf(stderr, "unsupported SBIF RAM drive table\n");
		return 1;
	}
	int fd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	void *phys = mmap(NULL, RAMDRIVES_SIZE, PROT_READ, MAP_SHARED, fd, RAMDRIVES_PA);
	close(fd);
	if (phys == MAP_FAILED) {
		perror("RAM drives");
		return 1;
	}
	region(RAMDRIVES, RAMDRIVES_SIZE);
	memcpy((void *)(uintptr_t)RAMDRIVES, phys, RAMDRIVES_SIZE);
	munmap(phys, RAMDRIVES_SIZE);
	const unsigned char *d = (void *)(uintptr_t)RAMDRIVES;
	printf("SBIF RAM drives 9/10: private copy of pa %08x, headers %02x%02x%02x%02x / %02x%02x%02x%02x\n", RAMDRIVES_PA,
	       d[0], d[1], d[2], d[3], d[0x7e000], d[0x7e001], d[0x7e002], d[0x7e003]);
	return 0;
}

static int serve(unsigned seconds);
static uint64_t monotonic_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static unsigned os_fired;	/* firmware timers expired on the Linux clock (E5RT_OS_CLOCK=1) */
/* offline: RAM drives from the low-memory dump (pa 0x11200000) */
static void ramdrives_from_dump(const char *dir)
{
	region(0xcb600000u, 0xfc000);
	fill(dir, "low-11200000.bin", 0, 0xcb600000u, 0xfc000);
}
static int live(const char *device, int probe_dev, int probe_res, unsigned seconds, const char *packet_device)
{
	if (ramdrives())
		return 1;
	sbif_attach();
	struct rapuyama_mdi_status st;
	uint32_t buffers = 1;
	mdi = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (mdi < 0) {
		perror(device);
		return 1;
	}
	/* copied buffers: the driver copies DSP rx payloads into our cb500000 window
	 * (and snapshots the whole window now) and our tx payloads into real RAM */
	/* E5RT_DSP_DIRECT=1: the real DSP buffer window (kernel mdi mmap, buffers mode 2) instead of the
	 * copy: on CELL_DCH the DSP reads/writes transport-block buffers no descriptor names (ring of
	 * 0xa84-byte buffers from cb531874, firmware structures c68628f0 + n*0x168); with the copy
	 * nothing passes on DCH (phone xg14..xg17), only FACH data inside descriptor payloads */
	if (getenv("E5RT_DSP_DIRECT")) {
		if (mmap((void *)(uintptr_t)DSP_BUF, 0x80000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, mdi, 0) == MAP_FAILED) {
			perror("mdi dsp window mmap");
			return 1;
		}
		buffers = 2;
		printf("DSP buffer window mapped directly\n");
	}
	if (ioctl(mdi, RAPUYAMA_MDI_BUFFERS, &buffers) || ioctl(mdi, RAPUYAMA_MDI_STATUS, &st) || st.version != 2) {
		perror("mdi setup");
		return 1;
	}
	*(volatile uint32_t *)(uintptr_t)MDI_AP = st.ap;
	*(volatile uint32_t *)(uintptr_t)MDI_DSP = (st.dsp & 0xffff0000) | (st.ap >> 16);
	forwarded = st.ap & 31;
	seen_tx = st.tx_interrupts;
	seen_event = st.event_interrupts;
	printf("mdi status ap=%08x dsp=%08x rx=%u tx=%u event=%u\n", st.ap, st.dsp, st.rx_interrupts, st.tx_interrupts, st.event_interrupts);
	if (probe_dev >= 0) {
		unsigned char m[] = {0x15, probe_dev, 0x6c, probe_res, 6, 0, 0, 0xa1, 1, 0xf0, 0x12, 0};
		void *p = allocate(sizeof(m));
		if (!p)
			return 1;
		memcpy(p, m, sizeof(m));
		printf("probe: version request to dev %02x resource %02x\n", probe_dev, probe_res);
		alarm(2);
		send(p);
		alarm(0);
	}
	if (packet_device) {
		backend = open(packet_device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (backend < 0) {
			perror(packet_device);
			return 1;
		}
		printf("backend %s attached, serving %s\n", packet_device, seconds ? "a while" : "until killed");
		return serve(seconds);
	}
	for (uint64_t end = monotonic_ms() + seconds * 1000ull; monotonic_ms() < end;) {
		struct pollfd w[2] = {{.fd = mdi, .events = POLLIN | POLLPRI}, {.fd = isi_sim_fd(), .events = POLLIN}};
		if (poll(w, isi_sim_embedded() ? 2 : 1, isi_sim_embedded() ? (int)isi_sim_timeout() : 100) < 0 && errno != EINTR)
			return 1;
		alarm(2);
		sbif_service();
		isi_sim_service();
		isi_identity_report();
		os_fired += isi_os_timer_service();
		schedule();
		if (mdi_pump() || drain())
			return 1;
		alarm(0);
	}
	if (ioctl(mdi, RAPUYAMA_MDI_STATUS, &st))
		return 1;
	printf("mdi status ap=%08x dsp=%08x rx=%u tx=%u event=%u\n", st.ap, st.dsp, st.rx_interrupts, st.tx_interrupts, st.event_interrupts);
	printf("os timers fired %u\n", os_fired);
	return 0;
}

/* Phonet backend: frames from phonet0 (one per read, media 0x15, little endian length
 * at 4 counting the bytes after the first 6) into the firmware, host queue back out.
 * seconds 0 = until killed */
static int serve(unsigned seconds)
{
	static unsigned char frame[65541];
	for (uint64_t end = monotonic_ms() + seconds * 1000ull; !seconds || monotonic_ms() < end;) {
		struct pollfd w[3] = {{.fd = mdi, .events = POLLIN | POLLPRI}, {.fd = backend, .events = POLLIN}, {.fd = isi_sim_fd(), .events = POLLIN}};
		if (poll(w, isi_sim_embedded() ? 3 : 2, isi_sim_embedded() ? (int)isi_sim_timeout() : 100) < 0 && errno != EINTR)
			return 1;
		alarm(2);
		sbif_service();
		isi_sim_service();
		isi_identity_report();
		os_fired += isi_os_timer_service();
		schedule();
		if (mdi_pump())
			return 1;
		if (w[1].revents & POLLIN) {
			ssize_t n = read(backend, frame, sizeof(frame));
			if (n < 0 && errno != EAGAIN && errno != EINTR)
				return 1;
			if (n > 0) {
				if (n < 10 || ((unsigned)frame[4] | (unsigned)frame[5] << 8) + 6 != (unsigned)n) {
					fprintf(stderr, "backend: bad frame of %zd bytes\n", n);
					return 1;
				}
				if (!quiet) {
					printf("host tx (%zd):", n);
					for (ssize_t j = 0; j < n && j < 48; j++)
						printf(" %02x", frame[j]);
					printf("\n");
				}
				void *p = allocate(n);
				if (!p)
					return 1;
				if (msg_trace && (msg_trace == 2 ? frame[3] == 0xd9 && frame[9] == 0x20 : frame[3] == 0x31))
					msg_armed = 1;	/* E5RT_MSG_TRACE=data: from the first uplink user-data frame */
				memcpy(p, frame, n);
				send(p);
				if (mdi_pump())
					return 1;
			}
		}
		if (drain())
			return 1;
		alarm(0);
		if (msg_trace)
			msg_trace_flush();
	}
	if (msg_trace)
		printf("messages traced %u, dropped %u\n", msgs_printed, msgs_dropped);
	printf("os timers fired %u\n", os_fired);
	return 0;
}

int main(int argc, char **argv)
{
	int link = argc >= 4 && !strcmp(argv[2], "--mdi");
	quiet = getenv("E5RT_QUIET") != NULL;
	int os_test = argc == 3 && !strcmp(argv[2], "--os-timer-selftest") ? 1 :
		      argc == 3 && !strcmp(argv[2], "--os-timer-live-selftest") ? 2 : 0;
	const char *os_clock = getenv("E5RT_OS_CLOCK");
	int os_live = link && os_clock && !strcmp(os_clock, "1");
	const char *sim_device = link ? getenv("E5RT_SIM_DEVICE") : NULL;
	int identity_test = argc == 3 && !strcmp(argv[2], "--identity-selftest");
	int kasumi_test = argc == 3 && !strcmp(argv[2], "--kasumi-selftest");
	if (isi_identity_load(getenv("E5RT_IMEI_FILE"))) {
		fprintf(stderr, "invalid E5RT_IMEI_FILE (15 digits, Luhn)\n");
		return 2;
	}
	int soak = argc == 4 && !strcmp(argv[2], "--os-soak");
	int sim_backend_test = (argc == 3 && !strcmp(argv[2], "--sim-backend-selftest")) || soak;
	if (sim_device && !*sim_device)
		sim_device = NULL;
	if (os_live && !sim_device) {
		fprintf(stderr, "E5RT_OS_CLOCK=1 needs E5RT_SIM_DEVICE (firmware PMIC access spins on the hardware semaphore otherwise)\n");
		return 2;
	}
	/* upstream's SIM modes: simulated card (selftests) or the bridge (--sim-probe/--sim-reset <dev>) */
	int sim_mode = argc == 3 && !strcmp(argv[2], "--sim-selftest") ? 1 :
		       argc == 3 && !strcmp(argv[2], "--sim-reset-selftest") ? 2 :
		       argc == 4 && !strcmp(argv[2], "--sim-probe") ? 3 :
		       argc == 4 && !strcmp(argv[2], "--sim-reset") ? 4 : 0;
	if (argc != 2 && !link && !sim_mode && !os_test && !sim_backend_test && !identity_test && !kasumi_test && !(argc == 7 && !strcmp(argv[2], "--replay")) && !(argc == 3 && (!strcmp(argv[2], "--dsp-selftest") || !strcmp(argv[2], "--dsp-tx-selftest")))) {
		fprintf(stderr, "usage: e5rt <images dir> [--dsp-selftest | --dsp-tx-selftest | --mdi <dev> [--probe dev:res] [--seconds n, 0 = forever] [--backend /dev/rapuyama-isi]]\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	stack_t alt = {.ss_sp = altstack, .ss_size = sizeof(altstack)};
	sigaltstack(&alt, NULL);
	struct sigaction sa = {.sa_sigaction = failure, .sa_flags = SA_SIGINFO | SA_ONSTACK};
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGALRM, &sa, NULL);
	alarm(15);

	image(argv[1], "10480000.bin", CODE, CODE_SIZE);
	image(argv[1], "1fd50000.bin", DATA, DATA_SIZE);
	image(argv[1], "1ffe0000.bin", SECURE, SECURE_SIZE);
	if (((uint8_t *)(uintptr_t)SCHED)[2] != 0x2f) {
		fprintf(stderr, "snapshot not idle (current task %02x)\n", ((uint8_t *)(uintptr_t)SCHED)[2]);
		return 1;
	}
	/* I/O windows the firmware may touch: private anonymous pages, no hardware */
	region(0xc6018000, 0x4000);	/* SRAM, MDI rings */
	region(0xc6024000, 0x2000);	/* crypto */
	region(0xc6033000, 0x1000);	/* timers */
	if (!sim_mode && !sim_device && !sim_backend_test)
		region(0xc6038000, 0x1000);	/* SIM (sim modes: isi_sim_map traps it) */
	region(0xc6040000, 0x1000);	/* doorbells */
	region(0xc6060000, 0x1000);	/* INTC */
	region(0xc6e00000, 0x100000);
	region(0xc6f00000, 0x100000);	/* modem FIFO window */
	region(DSP_BUF, 0x100000);	/* DSP buffer window (cb500000..cb5fffff) */
	/* its snapshot contents (probe e5l2pt): the firmware reads the DSP's allocation tags
	 * from these buffers, e.g. for tags still waiting to be returned */
	if (!(argc >= 4 && !strcmp(argv[2], "--mdi")) && !sim_mode && !os_test && !sim_backend_test && !identity_test && !kasumi_test)
		fill(argv[1], "low-10000000.bin", 0x140000, DSP_BUF, 0x80000);	/* pa 10140000 (live: the driver fills it) */
	fill(argv[1], "1fb60000.bin", 0x130000, DSP_BUF + 0x80000, 0x50000);	/* pa 1fc90000 */
	if (sim_mode && isi_sim_open(sim_mode >= 3 ? argv[3] : NULL, 0)) {
		perror("SIM bridge");
		return 1;
	}
	if ((sim_device || sim_backend_test) && isi_sim_open(sim_device, 1)) {
		perror("SIM backend");
		return 1;
	}
	patches();
	if (getenv("E5RT_MSG_TRACE") && (link || !strcmp(getenv("E5RT_MSG_TRACE"), "all"))) {
		msg_trace_install();
		if (!strcmp(getenv("E5RT_MSG_TRACE"), "all")) {	/* offline check: armed at once, printed at exit */
			msg_armed = 1;
			atexit(msg_trace_flush);
		}
	}
	if (os_test || os_live || soak)
		isi_os_timer_patches();
	if (link || soak || kasumi_test)
		isi_kasumi_patches();	/* no crypto accelerator in e5rt: software KASUMI */
	if (isi_identity_attach()) {
		fprintf(stderr, "unsupported identity reader\n");
		return 2;
	}
	if (isi_sim_active()) {
		/* E5 SIM patch window: va c60f0000 = pa 0x100ec000 (Symbian page tables), executable */
		region(0xc60f0000u, 0x1000);
		fill(argv[1], "100ec000.bin", 0, 0xc60f0000u, 0x1000);
		isi_sim_patches();
		if (isi_sim_map()) {
			perror("SIM register page");
			return 1;
		}
	}
	__builtin___clear_cache((char *)(uintptr_t)CODE, (char *)(uintptr_t)(CODE + CODE_SIZE));
	if (mprotect((void *)(uintptr_t)CODE, CODE_SIZE, PROT_READ | PROT_EXEC)) {
		perror("mprotect");
		return 1;
	}
	if (isi_sim_active()) {
		__builtin___clear_cache((char *)0xc60f0000u, (char *)0xc60f1000u);
		if (mprotect((void *)0xc60f0000u, 0x1000, PROT_READ | PROT_EXEC)) {
			perror("mprotect SIM patch");
			return 1;
		}
	}

	volatile unsigned char *q = (void *)(uintptr_t)HOST_QUEUE;
	if (link) {
		int probe_dev = -1, probe_res = 0;
		unsigned seconds = 10;
		const char *packet_device = NULL;
		for (int i = 4; i + 1 < argc; i += 2) {
			if (!strcmp(argv[i], "--probe") && sscanf(argv[i + 1], "%x:%x", &probe_dev, &probe_res) == 2)
				continue;
			if (!strcmp(argv[i], "--seconds"))
				seconds = atoi(argv[i + 1]);
			if (!strcmp(argv[i], "--backend"))
				packet_device = argv[i + 1];
		}
		alarm(0);
		if (isi_sim_embedded())
			isi_sim_attach();
		if (os_live)
			isi_os_timer_attach();
		return live(argv[3], probe_dev, probe_res, seconds, packet_device);
	}
	if (identity_test)
		return isi_identity_selftest();
	if (kasumi_test)
		return isi_kasumi_selftest();
	if (soak) {
		ramdrives_from_dump(argv[1]);
		sbif_attach();
		isi_sim_attach();
		isi_os_timer_offline();
		isi_os_timer_attach();
		alarm(0);
		uint64_t end = monotonic_ms() + atoi(argv[3]) * 1000ull;
		unsigned rounds = 0, guards = 0;
		for (unsigned t = 0; t < 48; t++)
			guards += !stack_guard_broken(t);
		printf("os soak: %u of 48 task stack guards intact\n", guards);
		while (monotonic_ms() < end) {
			poll(NULL, 0, isi_sim_timeout());
			alarm(2);
			sbif_service();
			isi_sim_service();
			isi_identity_report();
			os_fired += isi_os_timer_service();
			schedule();
			if (drain())
				return 1;
			alarm(0);
			rounds++;
			for (unsigned t = 0; t < 48; t++) {
				if (stack_guard_broken(t)) {
					fprintf(stderr, "os soak: task %u stack guard overwritten after %u rounds\n", t, rounds);
					return 1;
				}
			}
		}
		printf("os soak: %u rounds, os timers fired %u, no hang\n", rounds, os_fired);
		return 0;
	}
	if (sim_backend_test) {
		isi_sim_attach();
		return isi_sim_backend_selftest();
	}
	if (os_test)
		return os_test == 2 ? isi_os_timer_live_selftest() : isi_os_timer_selftest();
	if (sim_mode)
		return isi_sim_run(sim_mode == 2 || sim_mode == 4, 0);	/* 2/4 = reset (ATR), 1/3 = STATUS + SIM_APDU */
	if (argc == 7 && !strcmp(argv[2], "--replay")) {
		/* offline: deliver one DSP descriptor from the snapshot's DSP window, like mdi_pump */
		uint32_t d[4];
		for (int i = 0; i < 4; i++)
			d[i] = strtoul(argv[3 + i], NULL, 16);
		const unsigned char *b = (void *)(uintptr_t)(DSP_BUF + d[3] + 0x10000000u - 0x10140000u);
		printf("replay %08x %08x %08x %08x, buffer:", d[0], d[1], d[2], d[3]);
		for (int i = 0; i < 32; i++)
			printf(" %02x", b[i]);
		printf("\n");
		ramdrives_from_dump(argv[1]);
		sbif_attach();
		volatile uint32_t *ap = (void *)(uintptr_t)MDI_AP, *dsp = (void *)(uintptr_t)MDI_DSP;
		*ap = 0;
		*dsp = 0;
		unsigned producer = *dsp & 31;
		volatile uint32_t *slot = dsp + 1 + (producer & 15) * 4;
		slot[0] = d[0]; slot[1] = d[1]; slot[2] = d[2]; slot[3] = d[3];
		*dsp = (producer + 1) & 31;
		alarm(5);
		mdi_rx_irq();
		puts("rx irq returned");
		for (int r = 0; r < 20; r++) {
			schedule();
			sbif_service();
			while (*ap & 31) {	/* show what the firmware publishes */
				unsigned i = (*ap & 31) - 1;
				volatile uint32_t *t = ap + 1 + (i & 15) * 4;
				printf("fw publishes %08x %08x %08x %08x\n", t[0], t[1], t[2], t[3]);
				*ap = (*ap & ~31u) | i;
			}
			drain();
		}
		puts("replay done, no hang");
		return 0;
	}
	if (argc == 3)
		return !strcmp(argv[2], "--dsp-selftest") ? dsp_selftest() : dsp_tx_selftest();
	if (getenv("E5RT_PROBE")) {	/* one request, then schedule rounds, dump everything that arrives */
		unsigned char message[] = {0x15, 0, 0x6c, 0x15, 6, 0, 0, 0xa1, 1, 0xf0, 0x12, 0};
		void *p = allocate(sizeof(message));
		memcpy(p, message, sizeof(message));
		send(p);
		printf("after send: queue %u/%u/%u\n", q[16], q[17], q[28]);
		for (unsigned round = 0; round < 20; round++) {
			schedule();
			while (q[16] != q[17]) {
				unsigned char *r = receive();
				printf("round %u reply:", round);
				for (unsigned i = 0; i < 20; i++)
					printf(" %02x", r[i]);
				printf("\n");
				release(r);
			}
		}
		return 0;
	}
	printf("host queue: head %u tail %u count %u\n", q[16], q[17], q[28]);
	for (unsigned t = 1; t <= 100; t++) {
		/* media 0x15, rdev 0, sdev 0x6c (host), resource 0x15 (MTC), len 6,
		 * robj 0, sobj 0xa1, tid, COMMON_MESSAGE 0xf0, ISI_VERSION_GET_REQ 0x12 */
		unsigned char message[] = {0x15, 0, 0x6c, 0x15, 6, 0, 0, 0xa1, t, 0xf0, 0x12, 0};
		void *p = allocate(sizeof(message));
		if (!p) {
			fprintf(stderr, "allocator exhausted at %u\n", t);
			return 1;
		}
		memcpy(p, message, sizeof(message));
		send(p);
		if (q[16] == q[17] || q[28] != 1) {
			fprintf(stderr, "transaction %u: reply queue %u/%u/%u\n", t, q[16], q[17], q[28]);
			return 1;
		}
		unsigned char *reply = receive();
		if (t == 1) {
			printf("first reply at %p:", (void *)reply);
			for (unsigned i = 0; i < 16; i++)
				printf(" %02x", reply[i]);
			printf("\n");
		}
		if (reply[8] != (t & 255) || reply[9] != 0xf0 || reply[10] != 0x13) {
			fprintf(stderr, "transaction %u: not a version reply\n", t);
			return 1;
		}
		release(reply);
		if (q[28]) {
			fprintf(stderr, "transaction %u: queue not empty\n", t);
			return 1;
		}
	}
	puts("E5 native ARM firmware: 100 MTC version request/reply/free cycles passed");
	return 0;
}
