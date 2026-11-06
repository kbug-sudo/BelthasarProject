#!/usr/bin/env python3
r"""The widescreen's sides (a mod) past a narrow room's edge, with the player's wrap on and off.

    python tools/wide_sides_cmp.py [frame,frame,...]

Runs the attract (--scene 433, lockstep) at 16:9 twice -- `wide_room_wrap=1`, then `=0` -- with CT_WIDESHOTS on the
listed frames (Magus's castle under the moon, loc 0A5: a room as wide as its own map, with a sky of clouds that scrolls
on its own), and counts the lit pixels in the columns outside the 256 in the middle:

    wide-sides: f<N> wrap <1|0>: <lit> of <total> side pixels lit

With the wrap on the sides show the room's map going round and its clouds; with it off (settings.h wideRoomWrap, the F1
option's "Off: black past the room's edges") the sides are black, the self-scrolling sky too -- before 2026-11-06 the
clouds went on past the edge there (field.c fieldBeyondPixel), repeating what the screen shows (the owner: "it doubles
the side rather than just showing black bars").
"""
import os
import struct
import subprocess
import sys

FIELD = os.path.abspath('build/Belthasar.exe')
FRAMES = (5900, 6000, 6100)


def run(ini, out, frames):
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        os.remove(os.path.join(out, f))
    last = max(frames)
    env = dict(os.environ, CT_SETTINGS=ini, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', CT_NOPACE='1',
               CT_AUDIO_LOCKSTEP='1', CT_WIDESHOTS=os.path.join(out, 'w'), CT_WIDESHOTS_EVERY='1',
               CT_WIDESHOTS_FROM=str(min(frames)), CT_OVERLAY_SHOT='%s@%d:9' % (os.path.join(out, 'end.bmp'), last + 2))
    p = subprocess.run([FIELD, '--assets', 'ct_assets.dat', '--scene', '433'], env=env, capture_output=True, text=True,
                       errors='replace')
    if p.returncode != 0:
        print('wide-sides: the run ended with rc %d: %s' % (p.returncode, (p.stdout + p.stderr).strip()[-300:]))


def sides(path):
    b = open(path, 'rb').read()
    w, h = struct.unpack('<II', b[:8])
    px = b[8:]
    x0 = (w - 256) // 2
    lit = tot = 0
    for y in range(h):
        for x in list(range(0, x0)) + list(range(x0 + 256, w)):
            i = (y * w + x) * 3
            tot += 1
            lit += (px[i] | px[i + 1] | px[i + 2]) != 0
    return lit, tot


def main():
    frames = tuple(int(v) for v in sys.argv[1].split(',')) if len(sys.argv) > 1 else FRAMES
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    base = ('[video]\nbackend=sdl\nfullscreen=0\nwindow_scale=1\nscale=fit\naspect=pixel\nfilter=nearest\n'
            'render_scale=1\ninterpolation=0\nvsync=0\nwidescreen=16:9\n')
    for wrap in (1, 0):
        ini = os.path.abspath('build/_widesides_%d.ini' % wrap)
        open(ini, 'w').write(base + 'wide_room_wrap=%d\n' % wrap)
        out = os.path.abspath('build/_widesides_%d' % wrap)
        run(ini, out, frames)
        for f in frames:
            p = os.path.join(out, 'w_f%d.raw' % f)
            if not os.path.exists(p):
                print('wide-sides: f%d wrap %d: FAIL no picture' % (f, wrap))
                continue
            lit, tot = sides(p)
            print('wide-sides: f%d wrap %d: %d of %d side pixels lit' % (f, wrap, lit, tot))
    return 0


if __name__ == '__main__':
    sys.exit(main())
