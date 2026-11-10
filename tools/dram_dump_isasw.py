#!/usr/bin/env python3
"""Pull preserved Symbian and modem RAM through the Linux USB shell.
Reserved shared RAM uses /sys/kernel/debug/rapuyama-shm; memory below
PHYS_OFFSET 0x11600000 uses an mmap of /dev/mem.

boot out/zImage + out/e6-vc.dtb (has e6.usbsh), plug usb, run this. it saves
every 256 kb chunk as soon as it arrives and skips chunks it already has, so
if the phone resets mid-way just boot again and rerun.

usage: dram_dump.py [/dev/ttyACM0] [outdir]
"""
import base64
import binascii
import hashlib
import os
import select
import subprocess
import sys
import termios
import time
import tty

CHUNK = 0x40000
# modem runtime first: the reset can stop the dump before the later regions
REGIONS = [
    (0x1f8b0000, 0x1f8f0000),   # guest/shared RAM start (token, stack)
    (0x10480000, 0x1088c000),   # ISASW code, all of it (V 0xc6200000, 0x40c000)
    (0x1f8f0000, 0x1fcfd000),   # rest of the ISASW RAM (V 0xc6800000, 0x44d000)
    (0x1ffe0000, 0x1fff0000),   # secure message pool (64 KB)
    (0x100ec000, 0x100f0000),   # SIM patch window (V 0xc60f0000, 8 KB used; memdump reads 16 KB)
]


def wait_for(fd, pat, tmo):
    buf = b""
    end = time.time() + tmo
    while pat not in buf:
        left = end - time.time()
        if left <= 0:
            raise TimeoutError("no %r, got %r" % (pat, buf[-200:]))
        r, _, _ = select.select([fd], [], [], left)
        if r:
            data = os.read(fd, 65536)
            if not data:
                raise ConnectionError("USB shell disconnected")
            buf += data
    return buf


def cmd(fd, s):
    data = s.encode() + b"\r"
    while data:
        n = os.write(fd, data)
        if n <= 0:
            raise ConnectionError("USB shell write failed")
        data = data[n:]


def dump_command(addr, size):
    # reserved shared RAM: /dev/mem refuses it, the kernel's debugfs files don't
    for base, end in ((0x1f8b0000, 0x1fcfd000), (0x1ffe0000, 0x20000000)):
        if base <= addr and addr + size <= end:
            path = "/sys/kernel/debug/rapuyama-shm/%08x" % base
            read = "dd if=%s bs=4096 skip=%d count=%d" % (
                path, (addr - base) // 4096, size // 4096)
            break
    else:
        read = "/tmp/md %x %x" % (addr, size)
    # keep the reader's status: a pipeline would report base64's success
    return ("echo @@$((0))S; %s >/tmp/e6-dram.bin 2>/tmp/e6-dram.err; "
            "r=$?; if [ \"$r\" -eq 0 ]; then base64 /tmp/e6-dram.bin; fi; "
            "echo @@$((0))R:$r; cat /tmp/e6-dram.err; echo @@$((0))E" % read)


def decode_dump(buf, size):
    try:
        payload = buf.split(b"@@0S", 1)[1].split(b"@@0E", 1)[0]
        encoded, result = payload.split(b"@@0R:", 1)
        status, _, detail = result.strip().partition(b"\n")
        code = int(status.strip())
    except (IndexError, ValueError) as e:
        raise RuntimeError("invalid dump response: %r" % buf[-300:]) from e
    if code:
        raise RuntimeError("reader exited %d: %s" % (code, detail.decode(errors="replace").strip()))
    try:
        data = base64.b64decode(b"".join(encoded.split()), validate=True)
    except binascii.Error as e:
        raise RuntimeError("invalid base64 in dump") from e
    if len(data) != size:
        raise RuntimeError("reader returned %d bytes, expected %d" % (len(data), size))
    return data


def dump(fd, out):
    # e6sh eats lines until an empty one, a running sh just ignores it.
    # $((..)) so the reply marker never matches the echoed command itself
    cmd(fd, "")
    cmd(fd, "")
    time.sleep(0.5)
    cmd(fd, "stty -echo; echo @@$((1+1))R")
    wait_for(fd, b"@@2R", 15)
    print("shell up")
    # busybox dd reads /dev/mem with read(), which arm refuses below PHYS_OFFSET.
    # tools/memdump mmaps instead, send it over as base64
    md = os.path.join(out, "memdump")
    subprocess.run(["arm-linux-gnu-gcc", "-march=armv6k", "-marm", "-static", "-nostdlib", "-Os",
                    "-o", md, os.path.join(os.path.dirname(os.path.abspath(__file__)), "memdump.c")], check=True)
    blob = open(md, "rb").read()
    cmd(fd, "base64 -d >/tmp/md <<'@@X'")
    b = base64.b64encode(blob).decode()
    for i in range(0, len(b), 76):
        cmd(fd, b[i:i + 76])
    cmd(fd, "@@X")
    cmd(fd, "chmod 755 /tmp/md; md5sum /tmp/md; echo @@$((1+2))U")
    r = wait_for(fd, b"@@3U", 15)
    if hashlib.md5(blob).hexdigest().encode() not in r:
        raise SystemExit("memdump upload broken: %r" % r[-200:])
    print("memdump uploaded")
    cmd(fd, "mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null; "
            "test -r /sys/kernel/debug/rapuyama-shm/1f8b0000; "
            "echo @@$((4))D:$?; echo @@$((4))Z")
    r = wait_for(fd, b"@@4Z", 15)
    if r.split(b"@@4D:", 1)[1].splitlines()[0].strip() != b"0":
        raise SystemExit("modem RAM dump file missing; boot the Image with rapuyama-shm debugfs support")
    t0 = time.time()
    got = 0
    for lo, hi in REGIONS:
        for a in range(lo, hi, CHUNK):
            size = min(CHUNK, hi - a)
            fn = os.path.join(out, "%08x.bin" % a)
            if os.path.exists(fn) and os.path.getsize(fn) == size:
                continue
            cmd(fd, dump_command(a, size))
            buf = wait_for(fd, b"@@0E", 60)
            try:
                data = decode_dump(buf, size)
            except RuntimeError as e:
                raise SystemExit("%08x: %s" % (a, e)) from e
            with open(fn + ".tmp", "wb") as f:
                f.write(data)
            os.rename(fn + ".tmp", fn)
            got += size
            print("%08x ok  %.1f s  %d kb/s" % (a, time.time() - t0, got / 1024 / (time.time() - t0)))
    print("done, chunks in", out)


def main():
    dev = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
    out = sys.argv[2] if len(sys.argv) > 2 else "logs/dram"
    os.makedirs(out, exist_ok=True)
    while not os.path.exists(dev):
        print("waiting for", dev)
        time.sleep(1)
    time.sleep(1)
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
    saved = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        termios.tcflush(fd, termios.TCIOFLUSH)
        dump(fd, out)
    finally:
        try:
            termios.tcsetattr(fd, termios.TCSANOW, saved)
        except termios.error:
            pass
        finally:
            os.close(fd)


if __name__ == "__main__":
    main()