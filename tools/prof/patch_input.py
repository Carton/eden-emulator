#!/usr/bin/env python3
"""Switch qt-config.ini to the automated test key mapping (A=X code 88,
rstick J/L). Usage:
  python patch_input.py [ini_path]        # patch (backs up to .bak-autotest once)
  python patch_input.py --restore [ini]   # restore the pre-test backup
Default ini: EDEN_DIR env or build-vs22 master user config."""
import os
import re
import shutil
import sys

BS = chr(92)

restore = '--restore' in sys.argv
if restore:
    sys.argv.remove('--restore')
if len(sys.argv) > 1:
    ini = sys.argv[1]
else:
    _here = os.path.dirname(os.path.abspath(__file__))
    _repo = os.path.dirname(os.path.dirname(_here))
    eden_dir = os.environ.get(
        'EDEN_DIR', os.path.join(_repo, 'build-vs22', 'bin'))
    ini = os.path.join(eden_dir, 'user', 'config', 'qt-config.ini')
bak = ini + '.bak-autotest'

if restore:
    if os.path.exists(bak):
        shutil.copyfile(bak, ini)
        print('restored from', bak)
    else:
        print('no backup at', bak)
    sys.exit(0)

if not os.path.exists(bak):
    shutil.copyfile(ini, bak)

s = open(ini, encoding='utf-8', newline='').read()


def sub(pattern, repl):
    global s
    assert re.search(pattern, s), 'no match: ' + pattern
    s = re.sub(pattern, lambda m: repl.replace('\\', BS), s, count=1)


sub(r'keyboard_enabled\\default=.*', 'keyboard_enabled\\default=false')
sub(r'keyboard_enabled=.*', 'keyboard_enabled=true')
sub(r'player_0_button_a\\default=.*', 'player_0_button_a\\default=false')
sub(r'player_0_button_a=.*',
    'player_0_button_a="engine:keyboard,code:88,toggle:0"')
sub(r'player_0_rstick\\default=.*', 'player_0_rstick\\default=false')
sub(r'player_0_rstick=.*',
    'player_0_rstick="engine:analog_from_button,up:engine$0keyboard$1code$00$1toggle$00,'
    'down:engine$0keyboard$1code$00$1toggle$00,left:engine$0keyboard$1code$074$1toggle$00,'
    'right:engine$0keyboard$1code$076$1toggle$00,modifier:engine$0keyboard$1code$00$1toggle$00,'
    'modifier_scale:0.500000"')
open(ini, 'w', encoding='utf-8', newline='').write(s)
print('patched:', ini, '(backup:', bak + ')')
