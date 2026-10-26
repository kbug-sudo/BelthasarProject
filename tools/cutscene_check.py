#!/usr/bin/env python3
r"""The cutscene mod (src/cutscene.c; docs/CUTSCENES.md) plays its clips at their moments and leaves the game untouched.

    python tools/cutscene_check.py [--keep]
    python tools/cutscene_check.py selftest

The player's session from power-on (tools/hd_independence.py's `session`: the interactive host in a hidden window, the
input a VIRTUAL CONTROLLER driven by tools/bugreport/boot_to_bedroom.log -- the title, New Game, the name, the opening,
Crono's room), three times side by side, each writing CT_FRAMEHASH (every frame: the engine's 256x224 picture, its 128 KB
of work RAM and the 8 KB battery) until the report the drive takes at frame 4700:

  vanilla  no clip folder, the option off;
  off      the clip folder below, the option off (its moments are followed, nothing plays);
  on       the clip folder, `[video] cutscenes=on`, and CT_CUTSCENE_SKIP=10 (a key pressed after a clip's 10th picture).

The clips are made here (tools/ctlib/mpeg1_clip.py: flat-grey MPEG-1 pictures and silent MP2, no one's video): `long`
(60 pictures, with sound) is skipped, `short` (5 pictures) plays to its end. The list holds every kind of moment the drive
meets:

  BOOT  long   power_on      the power-on's first frame
  TITLE short  at 000 00     loc 000 and story 00 -- the power-on's own frame 0; NOT again when the drive comes back to the
                             title (the attract's loc 1B1 in between): a moment plays once (one of story 00 is never re-armed)
  ROOM  short  at 002 00     Crono's room as the opening enters it
  WAKE  long   at 002 03     ...and the story byte moving to 03 inside the room
  NEVER short  at 1B1 01     a location the drive meets, with a story it never has there: not played

Graded: the three hash files are the same, frame for frame; `on` printed exactly these clips at these frames, skipped or
played to the end; `off` and `vanilla` printed none. Lines:

    cutscene-check: on: BOOT at frame 0 (power-on): skipped
    ...
    cutscene-check: off: no clip played
    cutscene-check: the game untouched: vanilla, off and on identical over 4701 frames (picture, work RAM and battery)

`selftest` runs the moments alone (CT_CUTSCENE_SELFTEST: the list's moments over a scripted run of location / story / byte
values, no video): a moment once, not again on walking back in; again after the story byte was 00 (the title, where every
reload passes), not after a time gate's warp held 40 in it; a moment that also names a byte of the scene's own
(`7F0057=03`) only when the byte holds it. One
`cutscene-selftest:` line a frame, the clips due.
"""
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bugreport as br                                     # noqa: E402  (the probe's environment and its drive)
from ctlib import mpeg1_clip                               # noqa: E402

ROOT = br.ROOT
FIELD = os.path.join(ROOT, 'build', 'Belthasar.exe')
AT = 4700
LIST = """ct-cutscenes 1
# tools/cutscene_check.py: the moments the power-on drive (tools/bugreport/boot_to_bedroom.log) meets
BOOT long.mpg power_on
TITLE short.mpg at 000 00
ROOM short.mpg at 002 00
WAKE long.mpg at 002 03
NEVER short.mpg at 1B1 01
"""


def side(tmp, name, clips):
    d = os.path.join(tmp, name)
    os.makedirs(d)
    ini = os.path.join(d, 'opts.ini')
    open(ini, 'w').write('[video]\nwindow_scale=1\n' + ('cutscenes=on\n' if name == 'on' else ''))
    env = br.engine_env(d, os.path.join(d, 'none.srm'), None, 'session')
    env.update(CT_SETTINGS=ini, CT_BUGPAD=br.DRIVE, CT_FRAMEHASH=os.path.join(d, 'hashes'),
               CT_BUG_DIR=os.path.join(d, 'bugs'), CT_BUGREPORT_AT='%d:other:cutscene check' % AT, CT_BUGREPORT_EXIT='1')
    for k in ('CT_CUTSCENE_DIR', 'CT_CUTSCENE_SKIP', 'CT_HD_PRESENTER', 'CT_HD_DIR'):
        env.pop(k, None)
    if name != 'vanilla':
        env['CT_CUTSCENE_DIR'] = clips
    if name == 'on':
        env['CT_CUTSCENE_SKIP'] = '10'
    r = br.run([FIELD, '--assets', 'ct_assets.dat', '--boot'], env, timeout=1800)
    out = r.stdout + r.stderr
    hp = os.path.join(d, 'hashes')
    hashes = open(hp).read().splitlines() if os.path.exists(hp) else []
    played = re.findall(r'^cutscene: (\S+) at frame (\d+) \(([^)]*)\): (skipped|played to its end|the window closed|not played.*?)'
                        r'(?: \(|$)', out, re.M)
    pad = [l for l in out.splitlines() if l.startswith('bugreport-pad:')]
    return hashes, played, pad, r.returncode


SELF_LIST = """ct-cutscenes 1
BOOT x.mpg power_on
ROOM x.mpg at 0E2 37
SPLIT x.mpg at 0A3 84 7F0057=03
FLAG x.mpg at 0F3 AB 7F00EE&02=02
LIFT x.mpg at 127 93 story_change
"""
# power-on; the title; the room's moment; the next room and back (once); the title again (story 00: re-armed); the
# moment again; the scene's location and story while its own byte says 00, 02, then 03; out to the map, through a time
# gate (its warp holds 40 in the story byte, below the moment's 84) and back with 03: not again; a room whose moment is an
# event flag's bit, set with another bit of the same byte already on; a `story_change` moment (the Dactyls' take-off: 90
# -> 93 on the nest's summit), then the map and the summit again, the title, and a save loaded on the summit: never again
SELF_SEQ = 'on,000:00,0E2:37,0E3:37,0E2:37,000:00,0E2:37,0A3:84,0A3:84:7F0057=02,0A3:84:7F0057=03,' \
           '1F1:84:7F0057=00,110:40:7F0057=03,0A3:84:7F0057=03,0F3:AB:7F00EE=01,0F3:AB:7F00EE=03,' \
           '127:90,127:93,1F3:93,127:93,000:00,127:93'


def selftest():
    tmp = os.path.join(ROOT, 'build', '_cutscene_check', 'self_%d' % os.getpid())
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    open(os.path.join(tmp, 'cutscenes.txt'), 'w', newline='\n').write(SELF_LIST)
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', CT_CUTSCENE_DIR=tmp,
               CT_CUTSCENE_SELFTEST=SELF_SEQ)
    r = br.run([FIELD, '--assets', 'ct_assets.dat'], env, timeout=120)
    lines = [l for l in r.stdout.splitlines() if l.startswith('cutscene-selftest:')]
    print('\n'.join(lines) if lines else 'cutscene-selftest: FAILED to run (rc %d): %s' % (r.returncode, (r.stdout + r.stderr)[-300:]))
    shutil.rmtree(tmp, ignore_errors=True)
    return 0 if lines else 1


def main():
    if sys.argv[1:2] == ['selftest']:
        return selftest()
    keep = '--keep' in sys.argv[1:]
    tmp = os.path.join(ROOT, 'build', '_cutscene_check', str(os.getpid()))
    shutil.rmtree(tmp, ignore_errors=True)
    clips = os.path.join(tmp, 'clips')
    os.makedirs(clips)
    mpeg1_clip.write(os.path.join(clips, 'long.mpg'), pictures=60, audio=True)
    mpeg1_clip.write(os.path.join(clips, 'short.mpg'), pictures=5, audio=False)
    open(os.path.join(clips, 'cutscenes.txt'), 'w', newline='\n').write(LIST)
    names = ('vanilla', 'off', 'on')
    with ThreadPoolExecutor(3) as ex:
        res = dict(zip(names, ex.map(lambda n: side(tmp, n, clips), names)))
    ok = True
    for n in names:
        hashes, played, pad, rc = res[n]
        if not hashes or not pad or 'a game controller' not in pad[0]:
            print('cutscene-check: %s FAILED to run (%d hash lines, rc %d, %s)' % (n, len(hashes), rc,
                                                                                   pad[0] if pad else 'no virtual pad'))
            ok = False
    for id_, frame, moment, how in res['on'][1]:
        print('cutscene-check: on: %s at frame %s (%s): %s' % (id_, frame, moment, how))
    print('cutscene-check: on: %d clip(s) played: %s' % (len(res['on'][1]), ' '.join('%s@%s' % (i, f) for i, f, _, _ in res['on'][1])))
    for n in ('off', 'vanilla'):
        played = res[n][1]
        print('cutscene-check: %s: %s' % (n, 'no clip played' if not played else
                                          'PLAYED ' + ', '.join('%s@%s' % (i, f) for i, f, _, _ in played)))
        ok = ok and not played
    va = res['vanilla'][0]
    for n in ('off', 'on'):
        other = res[n][0]
        for a, b in zip(va, other):
            if a != b:
                fa, fb = a.split(), b.split()
                what = ' and '.join(k for k, x, y in zip(('picture', 'work RAM', 'battery'), fa[1:], fb[1:]) if x != y)
                print('cutscene-check: %s DIFFERS from vanilla at frame %s (%s)' % (n, fa[0], what))
                ok = False
                break
        else:
            if len(va) != len(other):
                print('cutscene-check: %s DIFFERS from vanilla: %d frames, vanilla %d' % (n, len(other), len(va)))
                ok = False
    if ok and va:
        print('cutscene-check: the game untouched: vanilla, off and on identical over %d frames '
              '(picture, work RAM and battery)' % len(va))
    if not keep:
        shutil.rmtree(tmp, ignore_errors=True)
    else:
        print('cutscene-check: kept %s' % tmp)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
