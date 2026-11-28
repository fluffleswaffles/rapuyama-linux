#!/usr/bin/env python3
# e6ota server side. keygen, publish (by hand), github-sign (in github actions),
# mirror (cron on the server: newest signed -e6 release from github -> www), verify, prune
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import urllib.request
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

NAMES = ("Image", "e6.dtb")
MAX_MANIFEST = 4096
API = "https://api.github.com"


def sha512_file(path):
	h = hashlib.sha512()
	with open(path, "rb") as f:
		for block in iter(lambda: f.read(1 << 20), b""):
			h.update(block)
	return h.hexdigest()


def c_key(pub):
	return "const unsigned char e6ota_pubkey[32] = {%s};\n" % ", ".join("0x%02x" % b for b in pub)


def keygen(a):
	d = Path(a.keydir)
	d.mkdir(parents=True, exist_ok=True)
	if (d / "e6ota.key").exists():
		raise SystemExit("%s exists, not overwriting a signing key" % (d / "e6ota.key"))
	k = Ed25519PrivateKey.generate()
	raw = k.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw, serialization.NoEncryption())
	pub = k.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
	fd = os.open(d / "e6ota.key", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
	with os.fdopen(fd, "w") as f:
		f.write(raw.hex() + "\n")
	(d / "e6ota.pub").write_text(pub.hex() + "\n")
	(d / "e6ota_key.c").write_text(c_key(pub))
	print("%s: e6ota.key (private), e6ota.pub, e6ota_key.c" % d)


def key_from(hexkey):
	return Ed25519PrivateKey.from_private_bytes(bytes.fromhex(hexkey.strip()))


def load_pub(path):
	return Ed25519PublicKey.from_public_bytes(bytes.fromhex(Path(path).read_text().strip()))


def manifest_text(serial, version, files, key, source=None, cfw=None):
	if not re.fullmatch(r"[ -~]{1,63}", version):
		raise SystemExit("version: 1..63 printable ascii characters")
	lines = ["e6ota 1", "serial %d" % serial, "version %s" % version]
	lines += ["file %s %d %s %s" % f for f in files]
	if source:
		lines.append("source %s" % source)
	if cfw:
		lines.append("cfw %d %s" % cfw)
	body = ("\n".join(lines) + "\n").encode("ascii")
	text = body + b"sig " + key.sign(body).hex().encode() + b"\n"
	if len(text) > MAX_MANIFEST:
		raise SystemExit("manifest too long")
	parse(text, key.public_key())
	return text


# the clients' rules (ota/c/e6ota.c is the reference)
def parse(text, pub):
	if len(text) > MAX_MANIFEST or not text.endswith(b"\n"):
		raise ValueError("format")
	body, sig = text[:-(4 + 128 + 1)], text[-(4 + 128 + 1):]
	if not sig.startswith(b"sig ") or not body.endswith(b"\n"):
		raise ValueError("format")
	try:
		pub.verify(bytes.fromhex(sig[4:-1].decode()), body)
	except InvalidSignature:
		raise SystemExit("the manifest's signature doesn't verify")
	lines = body.decode("ascii").split("\n")[:-1]
	if lines[0] != "e6ota 1":
		raise ValueError("format")
	m = {"files": []}
	for l in lines[1:]:
		k, _, rest = l.partition(" ")
		if k in m:
			raise ValueError("%s twice" % k)
		if k == "serial":
			m["serial"] = int(rest)
		elif k in ("version", "source"):
			m[k] = rest
		elif k == "cfw":
			n, url = rest.split(" ")
			m["cfw"] = (int(n), url)
		elif k == "file":
			name, size, sha, path = rest.split(" ")
			if name not in NAMES or any(f["name"] == name for f in m["files"]):
				raise ValueError("name %s" % name)
			m["files"].append({"name": name, "size": int(size), "sha512": sha, "path": path})
		else:
			raise ValueError("line %r" % l)
	return m


def current(www):
	p = Path(www) / "manifest"
	if not p.exists():
		return 0
	m = re.search(rb"^serial (\d+)$", p.read_bytes(), re.M)
	return int(m.group(1)) if m else 0


def install(www, serial, text, files):
	www = Path(www)
	tmp = www / (".%d.tmp" % serial)
	shutil.rmtree(tmp, ignore_errors=True)
	tmp.mkdir(parents=True)
	for name, src in files.items():
		shutil.copyfile(src, tmp / name)
		os.chmod(tmp / name, 0o644)
	if (www / str(serial)).exists():
		shutil.rmtree(www / str(serial))
	os.rename(tmp, www / str(serial))
	fd, t = tempfile.mkstemp(dir=www, prefix=".manifest")
	with os.fdopen(fd, "wb") as f:
		f.write(text)
	os.chmod(t, 0o644)
	os.replace(t, www / "manifest")


def publish(a):
	www = Path(a.www)
	www.mkdir(parents=True, exist_ok=True)
	key = key_from(Path(a.key).read_text())
	srcs = {"Image": Path(a.image), "e6.dtb": Path(a.dtb)}
	today = int(datetime.date.today().strftime("%Y%m%d")) * 100
	serial = a.serial or max(today, current(www) + 1)
	if serial <= current(www):
		raise SystemExit("serial %d isn't newer than the published %d" % (serial, current(www)))
	files = [(n, srcs[n].stat().st_size, sha512_file(srcs[n]), "%d/%s" % (serial, n)) for n in NAMES]
	cfw = (a.cfw, a.cfw_url) if a.cfw else None
	install(www, serial, manifest_text(serial, a.version, files, key, a.source, cfw), srcs)
	print("published %d (%s)" % (serial, a.version))


def get(url, token=None, accept=None):
	req = urllib.request.Request(url, headers={"User-Agent": "e6ota"})
	if token:
		req.add_header("Authorization", "Bearer " + token)
	if accept:
		req.add_header("Accept", accept)
	return urllib.request.urlopen(req, timeout=120)


def api(path, token=None):
	with get(API + path, token, "application/vnd.github+json") as r:
		return json.load(r)


def fetch_asset(asset, dest, token=None):
	h = hashlib.sha256()
	with get(asset["browser_download_url"], token) as r, open(dest, "wb") as f:
		for block in iter(lambda: r.read(1 << 20), b""):
			h.update(block)
			f.write(block)
	digest = asset.get("digest") or ""
	if digest.startswith("sha256:") and digest[7:] != h.hexdigest():
		raise SystemExit("%s doesn't match github's digest" % asset["name"])
	if os.path.getsize(dest) != asset["size"]:
		raise SystemExit("%s: size" % asset["name"])


def github_sign(a):
	token = os.environ.get("GITHUB_TOKEN")
	key = key_from(os.environ["E6OTA_KEY"])
	rel = api("/repos/%s/releases/tags/%s" % (a.repo, a.tag), token)
	assets = {x["name"]: x for x in rel["assets"]}
	serial = int(re.sub(r"\D", "", rel["published_at"]))
	files = []
	with tempfile.TemporaryDirectory() as t:
		for n in NAMES:
			if n not in assets:
				raise SystemExit("%s has no %s" % (a.tag, n))
			fetch_asset(assets[n], Path(t) / n, token)
			files.append((n, assets[n]["size"], sha512_file(Path(t) / n), "%d/%s" % (serial, n)))
	cfw = None
	if a.cfw_repo:
		try:
			c = api("/repos/%s/releases/latest" % a.cfw_repo, token)
			n = re.search(r"(\d+)$", c["tag_name"])
			cfw = (int(n.group(1)), c["html_url"]) if n else None
		except Exception as e:
			print("no cfw release: %s" % e)
	text = manifest_text(serial, rel["tag_name"][:63], files, key, rel["html_url"], cfw)
	Path(a.out).write_bytes(text)
	print(text.decode())


def mirror(a):
	token = os.environ.get("GITHUB_TOKEN")
	pub = load_pub(a.pub)
	www = Path(a.www)
	www.mkdir(parents=True, exist_ok=True)
	rels = api("/repos/%s/releases?per_page=30" % a.repo, token)
	rels = [r for r in rels if not r["draft"] and not r["prerelease"] and r["tag_name"].endswith(a.suffix)
		and any(x["name"] == "e6ota-manifest" for x in r["assets"])]
	if not rels:
		print("no signed %s release" % a.suffix)
		return
	rel = max(rels, key=lambda r: r["published_at"])
	assets = {x["name"]: x for x in rel["assets"]}
	with tempfile.TemporaryDirectory() as t:
		fetch_asset(assets["e6ota-manifest"], Path(t) / "manifest", token)
		text = (Path(t) / "manifest").read_bytes()
		m = parse(text, pub)
		if m["serial"] <= current(www):
			print("up to date: %s (%d)" % (rel["tag_name"], current(www)))
			return
		files = {}
		for f in m["files"]:
			p = Path(t) / f["name"]
			fetch_asset(assets[f["name"]], p, token)
			if p.stat().st_size != f["size"] or sha512_file(p) != f["sha512"]:
				raise SystemExit("%s doesn't match the signed manifest" % f["name"])
			if f["path"] != "%d/%s" % (m["serial"], f["name"]):
				raise SystemExit("unexpected path %s" % f["path"])
			files[f["name"]] = p
		install(www, m["serial"], text, files)
	print("mirrored %s (%d)" % (rel["tag_name"], m["serial"]))
	prune(argparse.Namespace(www=www, keep=a.keep))


def fetch(src, rel):
	if re.match(r"https?://", src):
		with get(src.rstrip("/") + "/" + rel) as r:
			return r.read()
	return (Path(src) / rel).read_bytes()


def verify(a):
	m = parse(fetch(a.src, "manifest"), load_pub(a.pub))
	print("serial %d, %s%s" % (m["serial"], m["version"], ", from " + m["source"] if "source" in m else ""))
	for f in m["files"]:
		data = fetch(a.src, f["path"])
		ok = len(data) == f["size"] and hashlib.sha512(data).hexdigest() == f["sha512"]
		print("  %-7s %9d %s" % (f["name"], f["size"], "ok" if ok else "MISMATCH"))
		if not ok:
			raise SystemExit(1)


def prune(a):
	www = Path(a.www)
	cur = current(www)
	for s in sorted((int(p.name) for p in www.iterdir() if p.is_dir() and p.name.isdigit()), reverse=True)[a.keep:]:
		if s != cur:
			shutil.rmtree(www / str(s))
			print("removed %d" % s)


def main():
	ap = argparse.ArgumentParser()
	sub = ap.add_subparsers(dest="cmd", required=True)
	p = sub.add_parser("keygen"); p.add_argument("keydir"); p.set_defaults(fn=keygen)
	p = sub.add_parser("publish")
	p.add_argument("--key", required=True); p.add_argument("--version", required=True)
	p.add_argument("--serial", type=int); p.add_argument("--source")
	p.add_argument("--cfw", type=int); p.add_argument("--cfw-url")
	p.add_argument("www"); p.add_argument("image"); p.add_argument("dtb")
	p.set_defaults(fn=publish)
	p = sub.add_parser("github-sign")
	p.add_argument("--repo", required=True); p.add_argument("--tag", required=True)
	p.add_argument("--cfw-repo"); p.add_argument("--out", default="e6ota-manifest")
	p.set_defaults(fn=github_sign)
	p = sub.add_parser("mirror")
	p.add_argument("--repo", required=True); p.add_argument("--pub", required=True)
	p.add_argument("--suffix", default="-e6"); p.add_argument("--keep", type=int, default=3)
	p.add_argument("www"); p.set_defaults(fn=mirror)
	p = sub.add_parser("verify"); p.add_argument("--pub", required=True); p.add_argument("src")
	p.set_defaults(fn=verify)
	p = sub.add_parser("prune"); p.add_argument("www"); p.add_argument("--keep", type=int, default=3)
	p.set_defaults(fn=prune)
	a = ap.parse_args()
	a.fn(a)


if __name__ == "__main__":
	main()
