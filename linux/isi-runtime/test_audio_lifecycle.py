import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time

source = Path(__file__).with_name("phone").joinpath("modem-audio").read_text()
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    state = root / "state"
    state.mkdir()
    lib = root / "lib"
    lib.mkdir()
    owner = root / "isasw.pid"
    owner.write_text(str(os.getpid()) + "\n")
    (state / "pid").write_text(str(os.getpid()) + "\n")
    coherent = root / "coherent_buffers"
    coherent.write_text('Y\n')
    (root / "readlink").write_text('#!/bin/sh\nprintf "%s\n" "$TEST_EXE"')
    (root / "readlink").chmod(0o700)
    script = root / "modem-audio"
    script.write_text(source.replace('/run/rapuyama-audio', str(state))
                     .replace('/sys/module/snd_aloop/parameters/coherent_buffers', str(coherent))
                     .replace('/usr/libexec/rapuyama-modem', str(lib))
                     .replace('/tmp/isasw', str(root / 'isasw')))
    env = os.environ | {'PATH': tmp + ':' + os.environ['PATH'],
                        'TEST_EXE': str(lib / 'modem-pcm')}
    def check(log, ok, **changes):
        (state / 'log').write_text(log)
        for action in ('status', 'start'):
            result = subprocess.run(['sh', str(script), action], env=env | changes,
                                    capture_output=True, timeout=7)
            assert (result.returncode == 0) == ok, (action, log, result)
        os.kill(os.getpid(), 0)
    check('AUDIO_READY buffer=101bbb58 owner=164\n', True)
    check('AUDIO_READY buffer=101bbb58 owner=164\nAUDIO_IDLE DSP stream closed\n', True)
    check('AUDIO_READY buffer=101bbb58 owner=164\nSOCKET_HELD cleanup unconfirmed\n', False)
    check('AUDIO_READY buffer=101bbb58 owner=164\nISI timeout resource=f3\n', False)
    check('', False)
    (state / 'log').write_text('')
    def publish_ready():
        time.sleep(0.2)
        (state / 'log').write_text('AUDIO_READY buffer=101bbb58 owner=164\n')
    ready = threading.Thread(target=publish_ready)
    ready.start()
    result = subprocess.run(['sh', str(script), 'start'], env=env,
                            capture_output=True, timeout=3)
    ready.join()
    assert result.returncode == 0, result
    check('AUDIO_READY buffer=101bbb58 owner=164\n', False, TEST_EXE='/unrelated/program')
    coherent.write_text('N\n')
    check('AUDIO_READY buffer=101bbb58 owner=164\n', False)
    coherent.unlink()
    check('AUDIO_READY buffer=101bbb58 owner=164\n', False)
    coherent.write_text('Y\n')
    (root / 'isasw.exit').write_text('121')
    check('AUDIO_READY buffer=101bbb58 owner=164\n', False)
assert 'pcm_substreams=1 coherent_buffers=1' in source
print('PASS coherent-only healthy idle/start, missing/disabled coherence rejection, held endpoint, timeout, missing readiness, PID identity and modem crash')