import re

BS = chr(92)

def parse(path):
    kv = {}
    for line in open(path, encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('[') or line.startswith('#'):
            continue
        if '=' in line:
            k, v = line.split('=', 1)
            kv[k.strip()] = v.strip()
    return kv

off = parse(r'F:' + BS + 'Switch' + BS + 'Yuzu' + BS + 'user' + BS + 'config' + BS + 'qt-config.ini')
loc = parse(r'F:' + BS + 'devel' + BS + 'opensource' + BS + 'eden-v0.2.1' + BS + 'build' + BS + 'bin' + BS + 'user' + BS + 'config' + BS + 'qt-config.ini')

TUNED = {
    'accelerate_astc', 'astc_recompression', 'use_asynchronous_shaders',
    'cpu_accuracy', 'keyboard_enabled', 'record_frame_times', 'confirmStop',
    'player_0_rstick',
}
SKIP = ('path', 'Paths', 'game_dir', 'log_filter', 'theme', 'window', 'geometry',
        'fullscreen', 'ui', 'screenshot', 'shortcut', 'main_window', 'callout',
        'Updater', 'WebService', 'Multiplayer', 'Network', 'discord', 'enable_discord_presence')

print('--- value differs: official vs local ---')
for k in sorted(set(off) & set(loc)):
    base = k.split(BS)[0]
    if off[k] != loc[k] and not any(t in k.lower() for t in SKIP):
        tag = ' [tuned-ok]' if base in TUNED else ''
        print(f'  {k}: official={off[k]!r} local={loc[k]!r}{tag}')
print('--- local only ---')
for k in sorted(set(loc) - set(off)):
    base = k.split(BS)[0]
    if not any(t in k.lower() for t in SKIP):
        tag = ' [tuned-ok]' if base in TUNED else ''
        print(f'  {k}={loc[k]!r}{tag}')
print('--- official only ---')
for k in sorted(set(off) - set(loc)):
    if not any(t in k.lower() for t in SKIP):
        print(f'  {k}={off[k]!r}')
