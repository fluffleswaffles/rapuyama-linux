# app menu resources with a real caption + svg icon, the way ensymble's py2sis
# makes them (python 2.7, ensymble's own rscfile/miffile). used by
# linux/stage0/symbian-loader/build_app.sh when CAPTION= is set; the default
# path there (patched ptdump template, 6-char name, no icon) is unchanged.
# usage: python mkappres.py <ensymble dir> <approot> <name> <uid> <caption> [icon.svg]
import os, sys
sys.path.insert(0, os.path.join(sys.argv[1], "ensymble", "utils"))
import rscfile, miffile

ens, root, name, uid, caption = sys.argv[1:6]
icon = sys.argv[6] if len(sys.argv) > 6 else ""
uid = int(uid, 0)
caption = caption.decode("utf-8")
base = u"%s_0x%08x" % (name, uid)

def write(rel, data):
	path = os.path.join(root, rel)
	if not os.path.isdir(os.path.dirname(path)):
		os.makedirs(os.path.dirname(path))
	open(path, "wb").write(data)

# APP_REGISTRATION_INFO (appinfo.rh), as py2sis
rw = rscfile.RSCWriter(uid2=0x101f8021, uid3=uid)
rw.addresource(rscfile.Resource(
	["LONG", "LLINK", "LTEXT", "LONG", "LTEXT", "LONG",
	 "BYTE", "BYTE", "BYTE", "BYTE", "LTEXT", "BYTE",
	 "WORD", "WORD", "WORD", "LLINK"],
	0, 0, base, 0, u"\\resource\\apps\\%s_loc" % base, 1,
	0, 0, 0, 0, u"", 0,
	0, 0, 0, 0))
write("private/10003a3f/import/apps/%s_reg.rsc" % base, rw.tostring())

# LOCALISABLE_APP_INFO (appinfo.rh): caption + icon file; .r01 = english,
# the language the old template used too
iconpath = u"\\resource\\apps\\%s_aif.mif" % base if icon else u""
rw = rscfile.RSCWriter(uid2=0, offset="    ")
rw.addresource(rscfile.Resource(
	["LONG", "LLINK", "LTEXT", "LONG", "LLINK", "LTEXT",
	 "WORD", "LTEXT", "WORD", "LTEXT"],
	0, 0, caption, 0, 0, caption,
	icon and 1 or 0, iconpath, 0, u""))
write("resource/apps/%s_loc.r01" % base, rw.tostring())

if icon:
	mw = miffile.MIFWriter()
	mw.addfile(open(icon, "rb").read())
	write("resource/apps/%s_aif.mif" % base, mw.tostring())
print("resources: %s caption '%s' icon %s" % (base, caption.encode("utf-8"), icon or "none"))
