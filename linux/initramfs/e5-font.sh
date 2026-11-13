#!/bin/sh
# console font of the E5 screen (fbcon on tty1, 320x240), installed as
# /usr/bin/font on the sd rootfs (tools/sdroot.sh e5). uses kbd's setfont
# and /usr/share/consolefonts (kbd-misc + font-terminus: ter-112n = 6x12,
# ter-1xxn/b = normal/bold). the choice is kept in /etc/e5-font and
# init.sh puts it back at boot (`font apply`).
#   font               current font
#   font list [pat]    fonts that fit: name, WxH, columns x rows (pat: only
#                      names containing it, e.g. ter-1)
#   font set <name>    load it on tty1 and keep it
#   font next | prev [pat]  the next / previous one from `font list [pat]`
#   font apply         load the saved one (boot)
#   font reset         forget the choice (next boot = the kernel's
#                      fbcon=font: from the dtb)
# the kernel built-in fonts (VGA8x8, 6x10, MINI4x6 ...) can only be picked
# at boot with fbcon=font:<name> in the dtb bootargs
D=/usr/share/consolefonts
CONF=/etc/e5-font
CACHE=/var/cache/e5-font.list
TTY=/dev/tty1
SETFONT=/usr/sbin/setfont
W=320 H=240
# smallest usable grid: 26 columns x 10 rows
MINC=26 MINR=10

# "name w h" per font file, from the psf header (psf1: 8 wide)
scan() {
	for f in "$D"/*; do
		case "$f" in *.psf|*.psf.gz|*.psfu|*.psfu.gz|*.gz) ;; *) continue ;; esac
		[ -f "$f" ] || continue
		set -- $(zcat -f "$f" 2>/dev/null | head -c 32 | od -An -tu1 -v)
		n=$(basename "$f" | sed 's/\.gz$//;s/\.psfu$//;s/\.psf$//')
		if [ "$1" = 54 ] && [ "$2" = 4 ]; then
			echo "$n 8 $4"
		elif [ "$1" = 114 ] && [ "$2" = 181 ] && [ "$3" = 74 ] && [ "$4" = 134 ]; then
			shift 24
			echo "$n $(($5 + $6 * 256)) $(($1 + $2 * 256))"
		fi
	done
}

# the cache is rebuilt when the font dir changed (apk add/upgrade)
fonts() {
	if [ ! -s "$CACHE" ] || [ "$D" -nt "$CACHE" ]; then
		mkdir -p "${CACHE%/*}"
		scan | sort > "$CACHE.tmp" && mv "$CACHE.tmp" "$CACHE"
	fi
	while read -r n w h; do
		[ "$w" -gt 0 ] && [ "$h" -gt 0 ] || continue
		case "$n" in *"$PAT"*) ;; *) continue ;; esac
		c=$((W / w)) r=$((H / h))
		[ "$c" -ge $MINC ] && [ "$r" -ge $MINR ] && echo "$n ${w}x$h ${c}x$r"
	done < "$CACHE"
}

current() { [ -r "$CONF" ] && cat "$CONF"; }

load() {
	$SETFONT -C "$TTY" "$1" || { echo "font: setfont $1 failed" >&2; return 1; }
}

step() {
	cur=$(current)
	n=$(fonts | awk -v cur="$cur" -v d="$1" '
		{ name[NR] = $1; if ($1 == cur) at = NR }
		END {
			if (!NR) exit
			if (!at) i = d > 0 ? 1 : NR
			else i = (at - 1 + d + NR) % NR + 1
			print name[i]
		}')
	[ -n "$n" ] || { echo "font: no fonts in $D" >&2; return 1; }
	load "$n" && echo "$n" > "$CONF" && fonts | grep "^$n "
}

PAT=
case "${1:-}" in list|next|prev) PAT="${2:-}" ;; esac
case "${1:-}" in
	help)	echo "font: the console font, kept across reboots."
		echo "  font                current font"
		echo "  font list [pat]     fonts that fit (name, size, grid)"
		echo "  font set <name>     use it"
		echo "  font next | prev    step through the list"
		echo "  font reset          back to the kernel's font"
		echo "More fonts: apk add kbd-misc font-terminus." ;;
	"")	c=$(current); echo "${c:-kernel default (fbcon=font: in the dtb)}" ;;
	list)	cur=$(current)
		fonts | while read -r n s g; do
			m=" "; [ "$n" = "$cur" ] && m="*"
			printf '%s %-24s %-6s %s\n' "$m" "$n" "$s" "$g"
		done ;;
	set)	[ -n "${2:-}" ] || { echo "usage: font set <name> (see font list)" >&2; exit 1; }
		load "$2" && echo "$2" > "$CONF" ;;
	next)	step 1 ;;
	prev)	step -1 ;;
	apply)	c=$(current); [ -n "$c" ] && load "$c" && echo "font $c" ;;
	reset)	rm -f "$CONF"; echo "saved font removed, the kernel one comes back at the next boot" ;;
	*)	sed -n '2,/^D=/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 1 ;;
esac
