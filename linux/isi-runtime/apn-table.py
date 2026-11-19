#!/usr/bin/env python3
# internet apn per home network for modem-data-connect, from the GNOME
# mobile-broadband-provider-info database (public domain):
#   apn-table.py serviceproviders.xml > apns.tsv
# one line per mcc/mnc: mcc mnc apn auth username password name, tab separated.
# auth is pap only where the database says so. modem-gprs has no chap
# negotiation, and operators listing a login without a method (beeline:
# beeline/beeline) have taken auth=none on the e6
import sys
import xml.etree.ElementTree as ET

def clean(text):
	text = (text or "").strip()
	# the profile file is line based and modem-gprs rejects control characters
	return "" if any(c in text for c in "\t\r\n") else text

def main():
	root = ET.parse(sys.argv[1]).getroot()
	seen = {}
	for country in root.findall("country"):
		for provider in country.findall("provider"):
			name = clean(provider.findtext("name"))
			for gsm in provider.findall("gsm"):
				networks = [(n.get("mcc"), n.get("mnc")) for n in gsm.findall("network-id")]
				for apn in gsm.findall("apn"):
					usage = [u.get("type") for u in apn.findall("usage")]
					if usage and "internet" not in usage:
						continue
					value = clean(apn.get("value"))
					if not value or len(value) > 100:
						continue
					method = apn.find("authentication")
					method = method.get("method") if method is not None else ""
					user, password = clean(apn.findtext("username")), clean(apn.findtext("password"))
					auth = "pap" if method == "pap" and user else "none"
					if auth == "none":
						user = password = ""
					for mcc, mnc in networks:
						if mcc and mnc and mcc.isdigit() and mnc.isdigit():
							# first internet apn listed wins, like the database's own order
							seen.setdefault((mcc, mnc), (value, auth, user, password, name))
	for (mcc, mnc), row in sorted(seen.items()):
		print("\t".join((mcc, mnc) + row))

if __name__ == "__main__":
	main()
