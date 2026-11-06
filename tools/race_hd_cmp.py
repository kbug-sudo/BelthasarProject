#!/usr/bin/env python3
r"""The story race at the HD presenter (a mod), in the player's presentation: its pictures and how its mode 7 was drawn.

    python tools/race_hd_cmp.py [--exe build/Belthasar.exe] [--wide both|16:9|off] [--frames 700]

Runs Lab 32's race from saves/race_re/race_user.lakewram (A at f30: Johnny's `FF 00` at f66, the race from its setup's
NMI on) in lockstep with the HD presenter on and the widescreen as asked, the host made interactive by the overlay probe
(CT_OVERLAY_SHOT ends the run: the only kind of run that reads the settings' mods), and prints the HD presenter's lines
(both: 16:9, then the widescreen off):

    race-hd: widescreen <w>, <frames> frames
    hd-mode7: <n> mode-7 lines drawn at 4x, <k> of them spread over a team of <t>
    hd-pictures: <n> pictures drawn, fnv <hash>

The pictures' hash is what the HD presenter drew, the widescreen's sides with it (seq_pic.c draws them with a mode-7
line's 4x samples); `--exe` runs another build of the same tree (the hash before a change, laid against the one after).
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))


def run(exe, wide, frames):
    tag = 'wide' if wide != 'off' else 'off'
    ini = os.path.abspath('build/_racehd_%s.ini' % tag)
    os.makedirs('build', exist_ok=True)
    with open(ini, 'w') as f:
        f.write('[video]\nbackend=sdl\nfullscreen=0\nwindow_scale=1\nscale=fit\naspect=pixel\nfilter=nearest\n'
                'wide_room_wrap=1\nrender_scale=1\ninterpolation=0\nvsync=0\npresenter=hd\nwidescreen=%s\n' % wide)
    env = dict(os.environ, CT_SETTINGS=ini, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', CT_NOPACE='1',
               CT_AUDIO_LOCKSTEP='1', CT_AUDIO_SEEDSTATE='saves/race_re/race_user.state', CT_HD_PRESENTER='1',
               CT_HD_DIR='build/_hd_none', CT_HD_STATS='1',
               CT_OVERLAY_SHOT='%s@%d:9' % (os.path.abspath('build/_racehd_%s.bmp' % tag), frames))
    env.pop('OMP_NUM_THREADS', None)                   # (the host's own team: its cap, as a player gets it)
    p = subprocess.run([os.path.abspath(exe), '--assets', 'ct_assets.dat', '--scene', '223', '--loadwram',
                        'saves/race_re/race_user.lakewram', '--walkscript', 'n:30,a:2,n:%d' % (frames + 100)],
                       env=env, capture_output=True, text=True, errors='replace')
    print('race-hd: widescreen %s, %d frames' % (wide, frames))
    lines = [l for l in p.stdout.splitlines() if l.startswith(('hd-mode7:', 'hd-pictures:'))]
    for l in lines:
        print(l)
    if len(lines) != 2:
        print('race-hd: FAIL the run printed %d of the 2 lines (rc %d)' % (len(lines), p.returncode))
        print('\n'.join(p.stdout.splitlines()[-10:] + p.stderr.splitlines()[-10:]))
        return 1
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default='build/Belthasar.exe')
    ap.add_argument('--wide', default='both', choices=('both', '16:9', 'off'))
    ap.add_argument('--frames', type=int, default=700)
    a = ap.parse_args()
    os.chdir(ROOT)
    bad = 0
    for wide in (('16:9', 'off') if a.wide == 'both' else (a.wide,)):
        bad |= run(a.exe, wide, a.frames)
    return bad


if __name__ == '__main__':
    sys.exit(main())
