#!/usr/bin/env python3

import argparse
import hashlib
import json
import struct
from pathlib import Path

CODE_SHA256="8b7a100162a2331f9475d95d46f7179b62efada589ac1e27edbad614737d7d56"

parser=argparse.ArgumentParser()
parser.add_argument('--code',type=Path,required=True)
parser.add_argument('--runtime',type=Path,required=True)
parser.add_argument('--secure',type=Path,required=True)
parser.add_argument('--out',type=Path,required=True)
parser.add_argument('--sim-patches',type=Path)
args=parser.parse_args()
code=args.code.read_bytes()
runtime=args.runtime.read_bytes()
secure=args.secure.read_bytes()

if len(code)!=0x40c000 or hashlib.sha256(code).hexdigest()!=CODE_SHA256:
 raise SystemExit('unsupported firmware image')
if len(runtime)!=0x44d000 or len(secure)<0x8000:
 raise SystemExit('incomplete preserved firmware ram')
if runtime[0x1b2e2]!=0x30:
 raise SystemExit('the preserved firmware was not idle')
private=bytearray(code)

for address in [0xc62dae92,0xc62dae84,0xc62bb820,0xc62bb828,0xc62bb846,0xc65a6ecc,0xc62efb88,0xc62efbba,0xc62efbf8,0xc62efc34,0xc62efc72]:
 struct.pack_into('<H',private,address-0xc6200000,0x4770)

struct.pack_into('<2H',private,0xc62efd00-0xc6200000,0x2000,0x4770)
struct.pack_into('<2I',private,0xc65a6de4-0xc6200000,0xe3a00013,0xe12fff1e)
struct.pack_into('<I',private,0xc65a6ea4-0xc6200000,0xe12fff1e)
args.out.mkdir(parents=True,exist_ok=True)
manifest={'firmware_sha256':CODE_SHA256,'physical_hardware':False,'images':{}}
images=[('c6200000.bin',private),('c6800000.bin',runtime),('c6700000.bin',secure[:0x8000])]
if args.sim_patches is not None:
 patch=args.sim_patches.read_bytes()
 if len(patch)!=0x2000 or hashlib.sha256(patch[:0xd48]).hexdigest()!='05eec01470324abdf24423bcb50bab3331a19624e5bcd5369a0c416ca304a805':raise SystemExit('unsupported SIM patch image')
 images.append(('c60f0000.bin',patch[:0xd48]+bytes(0x1000-0xd48)))
for name,data in images:
 dest=args.out/name
 temp=dest.with_suffix('.tmp');temp.write_bytes(data);temp.replace(dest)
 manifest['images'][name]={'size':len(data),'sha256':hashlib.sha256(data).hexdigest()}
(args.out/'manifest.json').write_text(json.dumps(manifest,indent=2))
print('prepared private ISASW code and unchanged RAM copies')