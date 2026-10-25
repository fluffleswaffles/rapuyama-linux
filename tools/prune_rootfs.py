#!/usr/bin/env python3
# shrink an alpine armhf rootfs to what the listed programs really load:
# keep the DT_NEEDED closure of the roots, drop every other elf, plus docs,
# locales and other data nobody reads. weston pulls mesa-gbm -> mesa ->
# llvm (200+ MB) as a package dep but only dlopens it for the gl renderer,
# we run pixman, so the closure leaves it out
# usage: prune_rootfs.py <rootfs> <root path>...
import os
import subprocess
import sys

# usr/lib/weston = weston's rpath (libexec_weston)
LIBDIRS = ['lib', 'usr/lib', 'usr/lib/weston']
# data dirs under usr/share that are read at runtime, the rest goes
KEEP_SHARE = {'X11', 'fonts', 'fontconfig', 'weston', 'libinput', 'wayland',
	'apk', 'misc', 'terminfo', 'icons', 'xkeyboard-config-2', 'applications', 'pixmaps'}


def is_elf(p):
	try:
		with open(p, 'rb') as f:
			return f.read(4) == b'\x7fELF'
	except OSError:
		return False


def needed(p):
	out = subprocess.run(['readelf', '-dW', p], capture_output=True,
		text=True).stdout
	return [l.split('[', 1)[1].split(']', 1)[0] for l in out.splitlines()
		if '(NEEDED)' in l]


def resolve(root, name):
	for d in LIBDIRS:
		p = os.path.join(root, d, name)
		if os.path.lexists(p):
			return p
	return None


def main():
	# normalized, the closure paths go through normpath and must match the walk
	root = os.path.realpath(sys.argv[1])
	keep = set()
	todo = [os.path.join(root, r) for r in sys.argv[2:]]
	missing = set()
	while todo:
		p = todo.pop()
		# keep the symlink chain and its target
		while os.path.islink(p):
			keep.add(p)
			t = os.readlink(p)
			p = os.path.normpath(os.path.join(root, t.lstrip('/')) if
				t.startswith('/') else os.path.join(os.path.dirname(p), t))
		if p in keep:
			continue
		if not os.path.exists(p):
			missing.add(p)
			continue
		keep.add(p)
		if not is_elf(p):
			continue
		for n in needed(p):
			q = resolve(root, n)
			if q:
				todo.append(q)
			else:
				missing.add(n)
	if missing:
		print('missing:', ' '.join(sorted(missing)))
		sys.exit(1)

	gone = 0
	for d, dirs, files in os.walk(root):
		for f in files:
			p = os.path.join(d, f)
			if p in keep or os.path.islink(p) or not is_elf(p):
				continue
			gone += os.path.getsize(p)
			os.unlink(p)
	share = os.path.join(root, 'usr/share')
	for e in os.listdir(share):
		if e not in KEEP_SHARE:
			subprocess.run(['rm', '-rf', os.path.join(share, e)], check=True)
	for e in ['usr/include', 'usr/lib/pkgconfig', 'usr/libexec/gstreamer-1.0',
			'usr/libexec/libcamera', 'usr/lib/gstreamer-1.0', 'usr/lib/dri',
			'usr/lib/gbm', 'lib/apk/db/scripts.tar.gz']:
		subprocess.run(['rm', '-rf', os.path.join(root, e)], check=True)
	# symlinks left dangling by the above
	for d, dirs, files in os.walk(root):
		for f in files + dirs:
			p = os.path.join(d, f)
			if os.path.islink(p) and not os.path.exists(p):
				os.unlink(p)
	print('kept %d files from the closure, dropped %.1f MB of elf'
		% (len(keep), gone / 1048576))


if __name__ == '__main__':
	main()