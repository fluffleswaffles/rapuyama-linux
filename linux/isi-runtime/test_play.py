from pathlib import Path
import os,struct,subprocess,sys,tempfile

root=Path(__file__).resolve().parents[2]
player=str(Path(sys.argv[1]).resolve())

with tempfile.TemporaryDirectory() as directory:
	folder=Path(directory);raw=folder/'sound.pcm';config=folder/'alsa.conf'
	config.write_text('pcm.!default { type file slave.pcm { type null } file "'+str(raw)+'" format "raw" }')
	env=os.environ|{'ALSA_CONFIG_PATH':str(config)}
	subprocess.run([player,str(root/'linux/yamashell/sounds/notif-ponderous.ogg')],env=env,check=True)
	data=raw.read_bytes();assert len(data)==261868,len(data)
	assert max(abs(v[0]) for v in struct.iter_unpack('<h',data))>1000
	broken=folder/'broken.ogg';broken.write_bytes(b'OggS')
	assert subprocess.run([player,str(broken)],env=env,capture_output=True).returncode==1
 
print('PASS shipped OGG: complete frame count, non-silent PCM, malformed input rejected')