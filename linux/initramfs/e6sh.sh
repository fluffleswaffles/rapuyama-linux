#!/bin/busybox sh
# login program for the usb shell (getty -l). the first thing the pc sends
# is often junk: ModemManager probes every new ttyACM with AT commands, and
# the pc tty echoes back whatever we print until picocom switches it to raw
# (that's where "sh: missing ]" and the endless ">" came from). so print
# nothing and echo nothing until a plain Enter (empty line), then the shell
stty -echo
while IFS= read -r l; do
	[ -z "$l" ] && break
done
stty echo
echo "$(hostname): shell. kernel log: dmesg (-w to follow)"
exec sh