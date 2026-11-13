#!/bin/sh
# tests for `<name> help` of every service command listed on the welcome
# screen, and the welcome screens themselves (e5-motd, e5-welcome).
# run: sh linux/initramfs/tests/test-e5-help.sh [test name...]
# runs the scripts with dash (busybox sh runs its own applets before PATH
# stubs); busybox ash (the phone's shell) parses them in t_syntax_busybox
HERE="$(cd "$(dirname "$0")" && pwd)"
I="$(cd "$HERE/.." && pwd)"
SH="$(command -v "${SH:-dash}")"
TMPBASE="$(mktemp -d)"
trap 'rm -rf "$TMPBASE"' EXIT
pass=0 fail=0 failed=
CMDS="ssh-server tailnet mobile wayland browser font blank"
no() { echo "  $cur: $*"; bad=1; }
check() { [ "$@" ] || no "check failed: [ $* ]"; }
script() {
	case "$1" in
	ssh-server) echo e5-ssh.sh ;;
	*) echo "e5-$1.sh" ;;
	esac
}

t_syntax_busybox() {
	command -v busybox > /dev/null || { echo "  (no busybox on this PC, skipped)"; return; }
	for c in $CMDS; do
		busybox sh -n "$I/$(script $c)" || no "$c does not parse"
	done
}

t_help_each() {
	for c in $CMDS; do
		# a normal user, an empty directory, no HOME: help must not need
		# root or touch anything
		d="$TMPBASE/h-$c"
		mkdir -p "$d"
		out=$(cd "$d" && HOME="$d" $SH "$I/$(script $c)" help 2> "$d/err")
		rc=$?
		check $rc = 0
		check ! -s "$d/err"
		n=$(printf '%s\n' "$out" | wc -l)
		[ "$n" -ge 2 ] && [ "$n" -le 12 ] || no "$c help has $n lines (want 2..12)"
		first=$(printf '%s\n' "$out" | head -n 1)
		case "$first" in *"$c"*) ;; *) no "$c help line 1 does not name it: $first" ;; esac
		# not cut mid-sentence: the last line ends with a full stop
		last=$(printf '%s\n' "$out" | tail -n 1)
		case "$last" in *.) ;; *) no "$c help ends mid-sentence: $last" ;; esac
		# nothing created in the working directory
		[ -z "$(ls -A "$d" | grep -v '^err$')" ] || no "$c help created files"
	done
}

t_wayland_help_full_comment() {
	# the seat-backend text (a three-line comment once cut by sed) must be complete
	out=$($SH "$I/e5-wayland.sh" help 2>&1)
	case "$out" in *"forces one)."*) ;; *) no "wayland help cut before the end of the LIBSEAT text: $out" ;; esac
}

t_motd() {
	f="$I/e5-motd"
	n=$(wc -l < "$f")
	[ "$n" -le 12 ] || no "e5-motd has $n lines (max 12)"
	w=$(awk '{ if (length($0) > m) m = length($0) } END { print m + 0 }' "$f")
	[ "$w" -le 40 ] || no "e5-motd has a line of $w columns (max 40)"
	for c in $CMDS; do
		grep -q "$c" "$f" || no "e5-motd does not name $c"
	done
	grep -q 'help' "$f" || no "e5-motd has no help hint"
}

t_welcome() {
	f="$I/e5-welcome"
	w=$(awk '{ if (length($0) > m) m = length($0) } END { print m + 0 }' "$f")
	[ "$w" -le 40 ] || no "e5-welcome has a line of $w columns (max 40)"
	# simple mode has none of the services
	for c in ssh-server tailnet mobile wayland browser; do
		grep -q "$c" "$f" && no "e5-welcome names $c (not in simple mode)"
	done
	# the simple mode commands, and only those that exist there
	for c in wifi brightness blank poweroff reboot; do
		grep -q "$c" "$f" || no "e5-welcome lacks $c"
	done
}

ALL="${*:-$(sed -n 's/^t_\([a-z0-9_]*\)() {$/\1/p' "$0")}"
for name in $ALL; do
	cur=$name bad=0
	"t_$name"
	if [ $bad = 0 ]; then pass=$((pass + 1)); echo "ok   $name"; else fail=$((fail + 1)); failed="$failed $name"; echo "FAIL $name"; fi
done
echo "$pass passed, $fail failed${failed:+:$failed}"
[ $fail = 0 ]
