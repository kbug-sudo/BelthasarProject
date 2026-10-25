#!/usr/bin/env python3
r"""The render-independence check: the HD presenter (src/hdpresent.c, a mod) changes nothing of the game.

    python tools/hd_independence.py [<leg> ...|all] [-j N] [--hd-dir DIR] [--keep]

Every leg of the playthrough route (tools/playthrough/route.txt: the cartridge's own pad from power-on to the end of the
route) is played twice by the port, as tools/playthrough.py's port side plays it -- from the leg's start state (its WRAM and
sound, --loadwram, CT_AUDIO_SEEDSTATE, CT_AUDIO_LOCKSTEP, the pad as walkscript tokens one frame later) -- once with every
mod off and once with the HD presenter drawing every frame (CT_HD_PRESENTER=1, its art from --hd-dir or CT_HD_DIR). The
first leg starts at power-on: it is played from there on an erased cartridge with the leg's pad as the engine's input
(CT_PLAYINPUT; the conversion of tools/bugreport/boot_to_bedroom.log: one frame later, a press's edge on the frame its bit
rises). Both runs write CT_FRAMEHASH: every frame, the engine's own 256x224 picture, its 128 KB of work RAM and the 8 KB
battery, hashed.
Graded: the two files, frame for frame. One line a leg:

    hd-independence: 05_bellstairs 807 frames identical (picture, work RAM and battery)
      (the HD run: field 99.9% of ... px | 1.2 ms a picture | ...)      -- informative: coverage, time, counts
    hd-independence: 33/33 legs identical, 210404 frames

A difference names its first frame and what differs. The leg's start states are tools/playthrough.py's (saves/playthrough):
made from power-on on the cartridge when missing or stale (playthrough.py ensure_states), as its own check makes them.
`--record DIR` keeps each HD run's sources the art lacked (CT_HD_RECORD, one pack a leg: tools/cthd.py merge folds them in).

    python tools/hd_independence.py session [--fullscreen] [--backend sdl|opengl|vulkan] [--headless] [--legs N] [--hd-dir DIR]

THE PLAYER'S PRESENTATION (`session`): the game from power-on in a real window of the player's backend (hidden: the
probes' rule), the HD presenter chosen in the options file as a player chooses it (`[video] presenter=hd`, 16:9), the
input a VIRTUAL CONTROLLER's buttons (CT_BUGPAD: SDL's controller state and events through the default pad bindings --
the DualSense mapping, no keyboard), driven by tools/bugreport/boot_to_bedroom.log (the title, New Game, the name, the opening, Crono's
room). A report is taken at frame 4700; its screen.png is the window as the presenter drew it (the desktop's size with
--fullscreen), and it is replayed headless with every mod off: the engine's state and picture must be the report's,
byte for byte. `--headless` keeps SDL's dummy video driver (the suite's: no GPU, the window 1592x896). `--legs N` drives
the route's legs 00..N instead (the conversion above) and reports 30 frames before their end: 13 is the canyon's fight
and the way out of it, in 16:9. Its report is graded on the work RAM: in 16:9 a report's 256x224 picture is the middle of
the wide one, which differs from the engine's own view where the camera stops at a room's edge (the vanilla presenter's
reports too).
"""
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import playthrough as pt                                    # noqa: E402  (the route, its legs and their start states)

ROOT = pt.ROOT
TMP = os.path.join(ROOT, 'build', '_hd_independence')
# a press's edge, the way the host makes them from SDL's events (bugreport.h CT_BUG_EDGE_*): A talks and types on the
# naming screen, B deletes there, Start confirms it, the d-pad's four
EDGES = [(0x100, 0x001 | 0x020), (0x001, 0x040), (0x008, 0x080), (0x010, 0x002), (0x020, 0x004), (0x040, 0x008), (0x080, 0x010)]


def drive_of(pad, note):
    """The leg's pad (the cartridge's levels from power-on) as the engine's input log (bugreport.c's ct-inputs 1)."""
    levels = [0]                                            # (the engine's frame f+1 is the cartridge's frame f)
    for hexbtn, n in pad:
        levels += [hexbtn] * n
    runs, prev = [], 0
    for lv in levels:
        ed = 0
        for bit, e in EDGES:
            if (lv & bit) and not (prev & bit):
                ed |= e
        if runs and runs[-1][1] == lv and runs[-1][2] == ed:
            runs[-1][0] += 1
        else:
            runs.append([1, lv, ed])
        prev = lv
    out = ['ct-inputs 1', '# ' + note, 'start boot sram none']
    out += ['%d %03X %03X' % (n, lv, ed) for n, lv, ed in runs]
    out.append('end %d' % len(levels))
    return '\n'.join(out) + '\n', len(levels)


def seed_of(start, d):
    """The start state's 128 KB of work RAM as a --loadwram file (ct.exe --peek, as playthrough.py's wram_of)."""
    raw = os.path.join(d, 'seed.raw')
    subprocess.run([pt.CT, '--peek', start, raw, '--btnscript', '0:0'], cwd=ROOT, env=pt.RULE, capture_output=True)
    w = open(raw + '.wram', 'rb').read()
    lw = os.path.join(d, 'seed.lakewram')
    open(lw, 'wb').write(b'#lakewram\nRAM:131072:' + w)
    return lw, (w[0x100] | (w[0x101] << 8)) & 0x1FF


def run_side(legs, i, d, side, hd_dir, record=None):
    lg = legs[i]
    start = pt.start_of(legs, i)
    hashes = os.path.join(d, side + '.hashes')
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', CT_NOPACE='1', CT_AUDIO_LOCKSTEP='1',
               CT_FRAMEHASH=hashes, CT_SHOTS=os.path.join(d, side + '_end'), CT_SHOTS_EXIT='1')
    for k in ('CT_HD_PRESENTER', 'CT_HDCHECK', 'CT_HD_DIR', 'CT_SETTINGS'):
        env.pop(k, None)
    if os.environ.get('CT_HDSHOT'):                    # (a probe's pictures: one prefix a leg and side)
        env['CT_HDSHOT'] = '%s_%s_%s' % (os.environ['CT_HDSHOT'], lg['name'], side)
    if side == 'hd':
        env.update(CT_HD_PRESENTER='1', CT_HD_DIR=hd_dir, CT_HD_STATS='1')
        if record:
            env['CT_HD_RECORD'] = os.path.join(record, lg['name'] + '.pack')
    if start == 'boot':
        log, n = drive_of(lg['pad'], 'hd_independence.py: route leg %s from power-on' % lg['name'])
        lp = os.path.join(d, 'drive.log')
        open(lp, 'w', newline='\n').write(log)
        env.update(CT_PLAYINPUT=lp, CT_SRAM=os.path.join(d, side + '.srm'), CT_SHOTS_AT=str(n))
        if os.path.exists(env['CT_SRAM']):
            os.remove(env['CT_SRAM'])
        cmd = [pt.FIELD, '--assets', 'ct_assets.dat', '--boot']
    else:
        lw, loc = seed_of(start, d) if side == 'vanilla' else (os.path.join(d, 'seed.lakewram'), None)
        if loc is None:
            w = open(os.path.join(d, 'seed.raw.wram'), 'rb').read()
            loc = (w[0x100] | (w[0x101] << 8)) & 0x1FF
        n = sum(k for _, k in lg['pad'])
        env.update(CT_AUDIO_SEEDSTATE=start, CT_WS_BATTLE_LEAD='1', CT_SHOTS_AT=str(n))
        cmd = [pt.FIELD, '--assets', 'ct_assets.dat', '--scene', str(loc), '--loadwram', lw, '--walkscript',
               pt.walkscript(lg['pad'])]
    r = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True, errors='replace', timeout=7200)
    lines = open(hashes).read().splitlines() if os.path.exists(hashes) else []
    stats = [l for l in (r.stdout + r.stderr).splitlines() if l.startswith(('hd-stats:', 'hd-coverage:', 'hd-time:'))]
    return lines, ' | '.join(l.split(': ', 1)[1] for l in stats), r.returncode


def grade(legs, i, hd_dir, keep, record=None, coverage=False):
    lg = legs[i]
    start = pt.start_of(legs, i)
    if start != 'boot' and not os.path.exists(start):
        return lg['name'], False, 'hd-independence: %s NOT GRADED -- no start state (%s)' % (
            lg['name'], os.path.relpath(start, ROOT))
    d = os.path.join(TMP, lg['name'])
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    va, _, rcv = run_side(legs, i, d, 'vanilla', hd_dir)
    hd, stats, rch = run_side(legs, i, d, 'hd', hd_dir, record)
    if not keep:
        shutil.rmtree(d, ignore_errors=True)
    if not va or not hd:
        return lg['name'], False, 'hd-independence: %s FAILED to run (vanilla %d lines rc %d, HD %d lines rc %d)' % (
            lg['name'], len(va), rcv, len(hd), rch)
    for a, b in zip(va, hd):
        if a != b:
            fa, fb_ = a.split(), b.split()
            names = ('picture', 'work RAM', 'battery')
            what = ' and '.join(n for n, x, y in zip(names, fa[1:], fb_[1:]) if x != y)
            return lg['name'], False, 'hd-independence: %s DIFFERENT at frame %s (%s: vanilla %s, HD %s)' % (
                lg['name'], fa[0], what, ' '.join(fa[1:]), ' '.join(fb_[1:]))
    if len(va) != len(hd):
        return lg['name'], False, 'hd-independence: %s DIFFERENT -- vanilla %d frames, HD %d' % (lg['name'], len(va), len(hd))
    parts = stats.split(' | ')
    px = [', '.join(p.split(', ')[2:4]) for p in parts if 'pixels with a source' in p]   # (hd-stats': exact)
    cov = ('\nhd-independence: %s coverage: %s%s' % (lg['name'], parts[0], ('; ' + px[0]) if px else '')
           if coverage and stats else '')
    return lg['name'], len(va), 'hd-independence: %s %d frames identical (picture, work RAM and battery)%s%s' % (
        lg['name'], len(va), ('\n  (the HD run: %s)' % stats) if stats else '', cov)


def session(fullscreen, backend, hd_dir, headless=False, legs=None, presenter='hd', wide='16:9'):
    import json
    import struct
    import tempfile
    import bugreport as br
    tmp = tempfile.mkdtemp(prefix='ct_hdsession_')
    try:
        ini = os.path.join(tmp, 'hd.ini')
        open(ini, 'w').write('[video]\nbackend=%s\nfullscreen=%d\nwindow_scale=4\nwidescreen=%s\npresenter=%s\n' % (
            backend, 1 if fullscreen else 0, wide, presenter))
        env = br.engine_env(tmp, os.path.join(tmp, 'none.srm'), None, 'session')
        if not headless:
            env.pop('SDL_VIDEODRIVER', None)               # (a real window and its backend; the sound muted below)
        if fullscreen:                                     # (the probe's window is hidden: give it the desktop's size)
            import ctypes
            u = ctypes.windll.user32 if os.name == 'nt' else None
            if u:
                env['CT_WINDOW'] = '%dx%d' % (u.GetSystemMetrics(0), u.GetSystemMetrics(1))
        drive, at, note = br.DRIVE, 4700, 'Crono in his room, in HD'
        if legs is not None:
            route = pt.route()
            log, n = drive_of([p for lg in route[:legs + 1] for p in lg['pad']], 'hd_independence.py: legs 00-%02d' % legs)
            drive = os.path.join(tmp, 'legs.log')
            open(drive, 'w', newline='\n').write(log)
            at, note = n - 30, 'the end of leg %s, in HD' % route[legs]['name']
        env.update(SDL_AUDIODRIVER='dummy', CT_SETTINGS=ini, CT_BUGPAD=drive, CT_HD_DIR=hd_dir, CT_HD_STATS='1', CT_SRCCHECK='1',
                   CT_BUG_DIR=os.path.join(tmp, 'bugs'), CT_BUGREPORT_AT='%d:picture:%s' % (at, note),
                   CT_BUGREPORT_EXIT='1')
        # (through route legs the session is ~29k frames drawn at 4x: well past 900 s while a whole suite runs beside it)
        r = br.run([pt.FIELD, '--assets', 'ct_assets.dat', '--boot'], env, timeout=900 if legs is None else 3600)
        for l in (r.stdout + r.stderr).splitlines():
            if l.startswith(('bugreport-pad:', '[video]', 'hd-coverage:', 'srccheck:', 'bugreport: saved')):
                print('hd-session: ' + l)
        ds = br.report_dirs(os.path.join(tmp, 'bugs'))
        ok = len(ds) == 1
        for d in ds:
            rep = json.load(open(os.path.join(d, 'report.json'), encoding='utf-8'))
            png = open(os.path.join(d, 'screen.png'), 'rb').read()
            w, h = struct.unpack('>II', png[16:24])
            line, _, _ = br.engine_check(d, tmp)
            exact = bool(line) and 'identical' in line and 'differ' not in line
            # (in 16:9 a report's 256x224 picture is the middle of the wide one, which is not the engine's own view where
            #  the camera stops at a room's edge -- the bug reporter's, vanilla or HD: --legs grades the work RAM)
            if legs is not None:
                exact = bool(line) and 'wram 131072/131072 identical' in line
            drew = rep['options'].get('hd_presenter')       # (report.json: whether the HD presenter drew the picture)
            ok &= exact and drew == (1 if presenter == 'hd' else 0)
            print('hd-session: report %04d frame %d: the window %dx%d, drawn by %s; replayed with every mod off: %s' % (
                rep['report'], rep['frame'], w, h,
                {1: 'the HD presenter', 0: 'the vanilla presenter'}.get(drew, 'an unknown presenter (report.json)'),
                line[len('bugreport-replay: '):] if line else 'no replay line'))
            keep = os.environ.get('CT_HDSESSION_KEEP')
            if keep:
                os.makedirs(keep, exist_ok=True)
                shutil.copyfile(os.path.join(d, 'screen.png'), os.path.join(keep, 'session_f%d.png' % rep['frame']))
        print('hd-session: %s -- %d report(s), %s' % ('PASS' if ok else 'FAIL', len(ds), 'the game untouched' if ok else 'see above'))
        return 0 if ok else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    a = sys.argv[1:]
    if a[:1] == ['session']:
        fs = '--fullscreen' in a
        be = a[a.index('--backend') + 1] if '--backend' in a else 'opengl'
        hd = a[a.index('--hd-dir') + 1] if '--hd-dir' in a else (os.environ.get('CT_HD_DIR') or 'hd')
        lg = int(a[a.index('--legs') + 1]) if '--legs' in a else None
        pr = a[a.index('--presenter') + 1] if '--presenter' in a else 'hd'
        wd = a[a.index('--widescreen') + 1] if '--widescreen' in a else '16:9'
        return session(fs, be, hd, '--headless' in a, lg, pr, wd)
    j, hd_dir, keep, names, record, coverage = 4, os.environ.get('CT_HD_DIR') or 'hd', False, [], None, False
    k = 0
    while k < len(a):
        if a[k] == '-j':
            j = int(a[k + 1]); k += 2
        elif a[k] == '--hd-dir':
            hd_dir = a[k + 1]; k += 2
        elif a[k] == '--keep':
            keep = True; k += 1
        elif a[k] == '--coverage':                         # (a line a leg of how much the HD run drew from HD art)
            coverage = True; k += 1
        elif a[k] == '--record':
            record = os.path.abspath(a[k + 1]); k += 2
            os.makedirs(record, exist_ok=True)
        elif a[k] in ('-h', '--help'):
            sys.exit(__doc__)
        else:
            names.append(a[k]); k += 1
    legs = pt.route()
    idx = list(range(len(legs))) if not names or names == ['all'] else [pt.find(legs, n) for n in names]
    os.makedirs(TMP, exist_ok=True)
    if max(idx) > 0:                                   # (the start states, from power-on on the cartridge, one at a time)
        pt.ensure_states(legs, max(idx) - 1)
    with ThreadPoolExecutor(max_workers=j) as ex:
        res = list(ex.map(lambda i: grade(legs, i, hd_dir, keep, record, coverage), idx))
    ok = frames = graded = 0
    for _, r, line in res:
        print(line)
        graded += 1
        if r is not False:
            ok += 1
            frames += r
    print('hd-independence: %d/%d legs identical, %d frames' % (ok, graded, frames))
    return 0 if ok == graded and graded else 1


if __name__ == '__main__':
    sys.exit(main())
