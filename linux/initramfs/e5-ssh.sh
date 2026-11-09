#!/bin/sh
# ssh server on the E5 (advanced setup), installed as /usr/bin/ssh-server
# on the sd rootfs (tools/sdroot.sh e5). Off at boot, on when you ask.
# Login: root with root's password (no keys). Needs alpine's openssh-server
# + nftables: apk add openssh-server nftables
#   ssh-server        status
#   ssh-server on     start sshd (+ firewall: only new connections to port 22)
#   ssh-server off    stop sshd, remove the firewall
# Host keys (ed25519, ecdsa) are made on this phone the first time, never
# shipped. scp/sftp work (sshd's own sftp). sshd runs with its own config
# (/etc/ssh/e5-sshd.conf, written by `on`), alpine's sshd_config is not used.
conf=/etc/ssh/e5-sshd.conf
pidf=/run/e5-sshd.pid
table=e5ssh

running() {
	[ -f "$pidf" ] && kill -0 "$(cat "$pidf")" 2>/dev/null
}

addrs() {
	ip -o addr show 2>/dev/null | awk '$2 != "lo" {sub(/\/.*/, "", $4); print "  " $2 " " $4}'
}

status() {
	if running; then
		echo "ssh server: on (pid $(cat "$pidf")), log in from the PC with: ssh root@<address>"
		addrs
	else
		echo "ssh server: off (ssh-server on)"
	fi
}

need() {
	miss=
	[ -x /usr/sbin/sshd ] || miss="$miss openssh-server"
	command -v nft > /dev/null || miss="$miss nftables"
	[ -z "$miss" ] && return
	echo "missing:$miss (wifi on, then: apk add$miss)" >&2
	exit 1
}

# root needs a real password: no keys, and sshd refuses empty passwords anyway
root_pw() {
	pw=$(awk -F: '$1 == "root" {print $2}' /etc/shadow)
	case "$pw" in
	'' | '!'* | '*'*)
		echo "root has no password yet, set one for ssh (twice):"
		passwd root || exit 1
		;;
	esac
}

hostkeys() {
	for t in ed25519 ecdsa; do
		k=/etc/ssh/ssh_host_${t}_key
		if [ ! -f "$k" ]; then
			echo "making the $t host key (first start only)"
			ssh-keygen -q -t "$t" -N '' -f "$k" || exit 1
		fi
	done
}

writeconf() {
	cat > "$conf" <<-EOF
	# written by ssh-server (/usr/bin/ssh-server), changes are overwritten
	Port 22
	HostKey /etc/ssh/ssh_host_ed25519_key
	HostKey /etc/ssh/ssh_host_ecdsa_key
	PidFile $pidf
	PermitRootLogin yes
	AuthenticationMethods password
	PasswordAuthentication yes
	PermitEmptyPasswords no
	KbdInteractiveAuthentication no
	PubkeyAuthentication no
	MaxAuthTries 3
	LoginGraceTime 30
	MaxStartups 3:50:10
	X11Forwarding no
	PrintMotd no
	Subsystem sftp internal-sftp
	EOF
}

# input: replies + port 22 (a few new connections per address and minute),
# drop the rest. output is not touched. removed again by `off`
firewall() {
	nft delete table inet $table 2>/dev/null
	nft -f - <<-EOF
	table inet $table {
		set ssh4 { type ipv4_addr; flags dynamic, timeout; timeout 1m; }
		set ssh6 { type ipv6_addr; flags dynamic, timeout; timeout 1m; }
		chain input {
			type filter hook input priority 0; policy drop;
			iif lo accept
			ct state established,related accept
			ct state invalid drop
			meta l4proto { icmp, ipv6-icmp } accept
			tcp dport 22 ct state new update @ssh4 { ip saddr limit rate over 6/minute burst 6 packets } drop
			tcp dport 22 ct state new update @ssh6 { ip6 saddr limit rate over 6/minute burst 6 packets } drop
			tcp dport 22 accept
		}
	}
	EOF
}

case "$1" in
'')
	status
	;;
on)
	[ "$(id -u)" = 0 ] || { echo "ssh-server: run it as root" >&2; exit 1; }
	if running; then
		status
		exit 0
	fi
	need
	root_pw
	hostkeys
	writeconf
	/usr/sbin/sshd -t -f "$conf" || exit 1
	firewall || { echo "ssh-server: firewall failed, not starting sshd" >&2; exit 1; }
	/usr/sbin/sshd -f "$conf" || { nft delete table inet $table 2>/dev/null; exit 1; }
	sleep 1
	status
	;;
off)
	[ "$(id -u)" = 0 ] || { echo "ssh-server: run it as root" >&2; exit 1; }
	running && kill "$(cat "$pidf")"
	rm -f "$pidf"
	nft delete table inet $table 2>/dev/null
	echo "ssh server: off (open ssh sessions keep running until they end)"
	;;
*)
	echo "usage: ssh-server [on|off]" >&2
	exit 1
	;;
esac
