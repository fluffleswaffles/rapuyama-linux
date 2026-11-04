"""Nokia E5 keyboard: the one table behind the console keymap (e5-kmap.py),
the X keymap (e5-xkb.py) and e5-keyd's key picture (e5-keyd-layer.py).

Linux keycodes as in linux,keymap of linux/boot/e5.dts (all < 128).
"""

KC = dict(Q=16, W=17, E=18, R=19, T=20, Y=21, U=22, I=23, O=24, P=25,
          A=30, S=31, D=32, F=33, G=34, H=35, J=36, K=37, L=38,
          Z=44, X=45, C=46, V=47, B=48, N=49, M=50)
COMMA, DOT, SPACE, BKSP, ENTER = 51, 52, 57, 14, 28
AT, QUEST = 41, 53                      # @/ key (KEY_GRAVE), ?! key (KEY_SLASH)
UP, DOWN, LEFT, RIGHT = 103, 108, 105, 106
FN_KEY, SYM_KEY, SHIFT_KEY, CTRL_KEY = 97, 100, 42, 29
RSK, MENU, MAIL, CALL, LSK = 125, 56, 58, 15, 1

# Sym layer = the second symbols printed on the keys (unchanged since K7)
SYM = dict(Q='"', W='(', E=')', R='1', T='2', Y='3', U='*', I='+', O='=',
           F='4', G='5', H='6', J='#', K='-', L="'",
           V='7', B='8', N='9', M='0')
SYM_KC = {COMMA: ';', DOT: ':', AT: '/', QUEST: '!'}
BASE_KC = {COMMA: ',', DOT: '.', AT: '@', QUEST: '?', SPACE: ' '}

# Fn layer, layout B: keycode -> one character, or a special key name
FN_LAYER = {KC[k]: 'F%d' % (i + 1) for i, k in enumerate('QWERTYUIOP')}
FN_LAYER.update({KC['A']: '&', KC['S']: '$', KC['D']: '%', KC['F']: '^',
                 KC['G']: '_', KC['H']: '<', KC['J']: '>', KC['K']: '[',
                 KC['L']: ']', BKSP: 'Del',
                 KC['Z']: '~', KC['X']: '`', KC['C']: '\\', KC['V']: '|',
                 KC['B']: '{', KC['N']: '}', COMMA: 'F11', DOT: 'F12',
                 UP: 'PgUp', DOWN: 'PgDn', LEFT: 'Home', RIGHT: 'End'})

# the picture's 3 x 10 grid in physical order (None = key without an Fn entry)
PICTURE_ROWS = [
    [KC[k] for k in 'QWERTYUIOP'],
    [KC[k] for k in 'ASDFGHJKL'] + [BKSP],
    [KC[k] for k in 'ZXCVBNM'] + [COMMA, DOT, ENTER],
]
KEY_LABEL = {v: k for k, v in KC.items()}
KEY_LABEL.update({COMMA: ',', DOT: '.', BKSP: '<-', ENTER: 'Ent'})

# special Fn values: kernel keysyms (linux/keyboard.h) and X keysym names
KERNEL_SPECIAL = dict({'F%d' % n: 0x100 + n - 1 for n in range(1, 13)},
                      Home=0x114, Del=0x116, End=0x117, PgUp=0x118, PgDn=0x119)
XKB_SPECIAL = dict({'F%d' % n: 'F%d' % n for n in range(1, 13)},
                   Home='Home', Del='Delete', End='End', PgUp='Prior', PgDn='Next')
XKB_CHAR = {'&': 'ampersand', '$': 'dollar', '%': 'percent', '^': 'asciicircum',
            '_': 'underscore', '<': 'less', '>': 'greater', '[': 'bracketleft',
            ']': 'bracketright', '~': 'asciitilde', '`': 'grave', '\\': 'backslash',
            '|': 'bar', '{': 'braceleft', '}': 'braceright', '"': 'quotedbl',
            '(': 'parenleft', ')': 'parenright', '*': 'asterisk', '+': 'plus',
            '=': 'equal', '#': 'numbersign', '-': 'minus', "'": 'apostrophe',
            ';': 'semicolon', ':': 'colon', '/': 'slash', '!': 'exclam',
            ',': 'comma', '.': 'period', '@': 'at', '?': 'question', ' ': 'space'}


def xkb_name(v):
    """one character or a special key name -> X keysym name"""
    if v in XKB_SPECIAL:
        return XKB_SPECIAL[v]
    if v.isalnum():
        return v
    return XKB_CHAR[v]
