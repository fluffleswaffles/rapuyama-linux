#!/usr/bin/env python3
"""Host tests for the E5 keymaps: tools/e5-kmap.py (console) and tools/e5-xkb.py (X).

run: python3 -I -m unittest discover -s tools -p 'test_e5_keymaps.py'
"""
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.dirname(os.path.abspath(__file__))
NR_KEYS = 128
FN, ALTGR = 128, 2


def read_kmap(path):
    """busybox loadkmap binary -> {map index: [NR_KEYS u16]}"""
    data = open(path, 'rb').read()
    assert data[:7] == b'bkeymap'
    flags, pos, maps = data[7:7 + 256], 7 + 256, {}
    for idx in range(256):
        if flags[idx]:
            maps[idx] = list(struct.unpack_from('<%dH' % NR_KEYS, data, pos))
            pos += 2 * NR_KEYS
    assert pos == len(data)
    return maps


def generate(script, suffix, extra=None):
    """run a generator into a temp file; extra = suffix of a second output
    (written next to the first as <out><extra>)"""
    fd, out = tempfile.mkstemp(suffix=suffix)
    os.close(fd)
    args = [out, out + extra] if extra else [out]
    subprocess.run([sys.executable, '-I', os.path.join(TOOLS, script)] + args, check=True)
    return out


class ConsoleKmap(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.km = read_kmap(generate('e5-kmap.py', '.kmap'))

    def test_fn_key_is_oneshot(self):
        self.assertEqual(self.km[0][97], 0xc07)            # SCtrlR

    def test_fn_layer_b_fkeys(self):
        fn = self.km[FN]
        self.assertEqual([fn[k] for k in (16, 17, 25, 51, 52)],
                         [0x100, 0x101, 0x109, 0x10a, 0x10b])   # Q W P , . = F1 F2 F10 F11 F12

    def test_fn_layer_b_symbols(self):
        fn = self.km[FN]
        want = {30: '&', 31: '$', 32: '%', 33: '^', 34: '_', 35: '<', 36: '>',
                37: '[', 38: ']', 44: '~', 45: '`', 46: '\\', 47: '|', 48: '{', 49: '}'}
        self.assertEqual({k: chr(fn[k] & 0xff) for k in want}, want)
        for k in want:
            self.assertEqual(fn[k] >> 8 & 0xf, 0, 'latin keysym for %d' % k)

    def test_fn_layer_b_navigation(self):
        fn = self.km[FN]
        self.assertEqual([fn[k] for k in (103, 108, 105, 106, 14)],
                         [0x118, 0x119, 0x114, 0x117, 0x116])   # PgUp PgDn Home End Del

    def test_fn_layer_keeps_m_and_enter(self):
        self.assertEqual(self.km[FN][50], self.km[0][50])
        self.assertEqual(self.km[FN][28], 0x201)

    def test_sym_unchanged(self):
        self.assertEqual(chr(self.km[ALTGR][19] & 0xff), '1')   # Sym+R
        self.assertEqual(chr(self.km[ALTGR][36] & 0xff), '#')   # Sym+J

    def test_front_keys(self):
        self.assertEqual([self.km[0][k] for k in (125, 56, 58, 15, 1)],
                         [0xc02, 0xc03, 0xa01, 0x009, 0x01b])


class XKeymap(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.path = generate('e5-xkb.py', '.xkb')
        cls.text = open(cls.path).read()

    def key(self, name):
        m = re.search(r'key <%s>\s*\{(.*?)\};' % re.escape(name), self.text, re.S)
        self.assertIsNotNone(m, 'no key <%s>' % name)
        return m.group(1)

    def syms(self, name):
        m = re.search(r'symbols\[Group1\]\s*=\s*\[([^\]]*)\]', self.key(name))
        self.assertIsNotNone(m, 'no symbols for <%s>' % name)
        return [s.strip() for s in m.group(1).split(',')]

    def test_no_level5_include(self):
        # not in the phone's xkeyboard-config: the whole xkb_symbols failed
        self.assertNotIn('level5(modifier_mapping)', self.text)

    def test_compiles(self):
        fd, xkm = tempfile.mkstemp(suffix='.xkm')
        os.close(fd)
        src = self.text.replace('@STEP@', '2')
        r = subprocess.run(['xkbcomp', '-w', '0', '-', xkm], input=src.encode(),
                           capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr.decode())

    def test_fn_level5(self):
        self.assertEqual(self.syms('AD01')[4], 'F1')        # Fn+Q
        self.assertEqual(self.syms('AC05')[4], 'underscore')  # Fn+G
        self.assertEqual(self.syms('AB09')[4], 'F12')       # Fn+.

    def test_sym_level3(self):
        self.assertEqual(self.syms('AD04')[2], '1')         # Sym+R
        self.assertIn('ISO_Level3_Latch', self.key('RALT'))

    def test_fn_is_oneshot_latch(self):
        k = self.key('RCTL')
        self.assertIn('LatchMods(modifiers=LevelFive,clearLocks)', k.replace(' ', ''))
        self.assertNotIn('latchToLock', k)

    def test_rsk_is_oneshot_ctrl(self):
        self.assertIn('LatchMods(modifiers=Control,clearLocks)', self.key('LWIN').replace(' ', ''))

    def test_mail_toggles_pointer_mode(self):
        self.assertIn('Pointer_EnableKeys', self.key('CAPS'))

    def test_arrows(self):
        up = self.key('UP')
        self.assertEqual(self.syms('UP')[4], 'Prior')
        self.assertIn('MovePtr(x=+0,y=-@STEP@)', up.replace(' ', ''))


class WestonSymbols(unittest.TestCase):
    """the xkb symbols file for weston (keymap_layout=e5)"""
    @classmethod
    def setUpClass(cls):
        cls.path = generate('e5-xkb.py', '.xkb', extra='.symbols')
        cls.text = open(cls.path + '.symbols').read()

    def test_only_standard_includes(self):
        inc = re.findall(r'include "([^"]*)"', self.text)
        self.assertEqual(inc, ['us(basic)'])

    def test_no_x_only_bits(self):
        self.assertNotIn('@STEP@', self.text)
        self.assertNotIn('MovePtr', self.text)
        self.assertNotIn('Pointer_EnableKeys', self.text)

    def test_compiles_as_layout(self):
        d = tempfile.mkdtemp()
        os.mkdir(os.path.join(d, 'symbols'))
        with open(os.path.join(d, 'symbols', 'e5'), 'w') as f:
            f.write(self.text)
        src = ('xkb_keymap { xkb_keycodes { include "evdev+aliases(qwerty)" };'
               ' xkb_types { include "complete" }; xkb_compat { include "complete" };'
               ' xkb_symbols { include "pc+e5+inet(evdev)" }; };')
        r = subprocess.run(['xkbcomp', '-w', '0', '-I' + d, '-I/usr/share/X11/xkb', '-', os.path.join(d, 'o.xkm')],
                           input=src.encode(), capture_output=True)
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        self.assertTrue(os.path.getsize(os.path.join(d, 'o.xkm')) > 0)


class Generated(unittest.TestCase):
    """the committed generated files match their generators"""
    ROOT = os.path.dirname(TOOLS)

    def same(self, script, rel, suffix):
        fresh = open(generate(script, suffix), 'rb').read()
        self.assertEqual(open(os.path.join(self.ROOT, rel), 'rb').read(), fresh,
                         '%s is stale: run tools/%s %s' % (rel, script, rel))

    def test_kmap(self):
        self.same('e5-kmap.py', 'linux/initramfs/e5.kmap', '.kmap')

    def test_xkb(self):
        self.same('e5-xkb.py', 'linux/initramfs/e5.xkb', '.xkb')

    def test_symbols(self):
        fresh = generate('e5-xkb.py', '.xkb', extra='.symbols') + '.symbols'
        self.assertEqual(open(os.path.join(self.ROOT, 'linux/initramfs/e5.symbols'), 'rb').read(),
                         open(fresh, 'rb').read(), 'e5.symbols is stale: run tools/e5-xkb.py')

    def test_keyd_layer(self):
        self.same('e5-keyd-layer.py', 'linux/initramfs/e5-keyd/layer.h', '.h')


if __name__ == '__main__':
    unittest.main()
