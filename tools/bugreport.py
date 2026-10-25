#!/usr/bin/env python3
r"""The bug reporter's reports (src/bugreport.h): replayed on both sides, and the reporter's own checks.

    python tools/bugreport.py replay <report folder> [-v] [--keep]   the session again in the engine and on the cartridge
    python tools/bugreport.py check                                  a report taken headless, then replayed on both sides
    python tools/bugreport.py check-panel                            `~` and the touchpad through the player's own events

A REPORT (Belthasar's `~`, or a second of the DualSense's touchpad button) is a folder: report.json, wram.bin (the 128 KB of
work RAM), screen.png and screen_native.png, inputs.log (every frame's input since the session started), sram.srm (the
battery it started from), console.txt, and in the field scene.state + .apu (F5's snapshot).

THE REPLAY of a session that started at power-on (`--boot`):
  - the ENGINE: Belthasar from power-on, headless, the log's input instead of SDL's (CT_PLAYINPUT), graded at the report's
    frame against the report itself (CT_REPLAY_CHECK): the 128 KB of work RAM and the 256x224 picture. Identical = the
    report reproduces exactly;
  - the CARTRIDGE: the same controller words (the log's levels) in the oracle from power-on under the rule
    (CT_ORACLE_LOGICAL=1) with the same battery (CT_ORACLE_SRAM), one frame earlier -- the engine's keys are the
    cartridge's + 1 (tools/playthrough.py). Graded as playthrough.py grades its legs: the chains of the location, the
    storyline, the party, gold, Crono's HP and the player's control, in order, from each side's first lit frame (the
    power-on's black stretch is the cold init's, where the two seed the party 22 frames apart and nothing shows), and the
    cartridge's last picture against the engine's within 6 frames either side -- or either side of where the engine stood
    when the two have drifted apart. Both sides play EXT idle frames past the report: the engine's loads skip the
    cartridge's black frames, so at the report the two can stand dozens of frames apart, and a value one side reached
    just before the report is no split when the other reaches it just after (it is printed as "past the window").
The verdict: the engine does not reproduce its own report (not deterministic, or an approximate session: free sync, F8,
F9, a scripted drive); a PORT bug candidate (the chains part: the first value that differs, its frames on both sides);
the picture alone differs (a port picture bug); or both agree (the original game's behaviour -- or, when screen.png is not
the 256x224 picture, the presenter's or an HD manifest's). A player's input is timed on the engine; where the two differ
in timing (a load's black frames, which the engine does not reproduce) an input can meet a different moment on the
cartridge, and the chains then part there: a lead, not a verdict.

THE CHECKS drive the engine with tools/bugreport/boot_to_bedroom.log (playthrough.py's first three legs as the engine's
input) from an erased cartridge:
  check        a report at frame 4700 without the panel (CT_BUGREPORT_AT): its files, its two pictures decoded against its
               hash, its log equal to the drive's first 4700 frames; then the replay on both sides
  check-panel  `~` at frame 3200 and a note typed into the panel (CT_BUGKEYS, CT_BUGPANEL_TYPE: the events a player's keys
               give), the touchpad held from frame 4000 (a report 60 frames later, "note to add"); each replayed in the
               engine (the panel's pause leaves the game as it was)
"""
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIELD = os.path.join(ROOT, 'build', 'Belthasar.exe')
CT = os.path.join(ROOT, 'build', 'ct.exe')
DRIVE = os.path.join(ROOT, 'tools', 'bugreport', 'boot_to_bedroom.log')
FILES = ['report.json', 'wram.bin', 'screen.png', 'screen_native.png', 'inputs.log', 'sram.srm', 'console.txt',
         'scene.state', 'scene.state.apu']
# What the replay grades on both sides (playthrough.py MON): (name, cartridge address, the engine's memmon name, length)
MON = [('loc', '7E0100', '0100', 2), ('story', '7F0000', '10000', 1), ('party', '7E2980', '2980', 3),
       ('gold', '7E2C53', '2C53', 3), ('hp', '7E2603', '2603', 2), ('ctrl', '7E011F', '011F', 1), ('bri', '7E0119', '0119', 1)]
GRADED = [k for k, _, _, _ in MON if k != 'bri']
SHOTS = range(-6, 7)
EXT = 300                       # idle frames both sides play past the report: a value one reached a little later


def fnv(b):
    h = 2166136261
    for c in b:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


def png_rgb(p):
    """An 8-bit RGB PNG (as src/bugreport.c writes it, any filter): (w, h, RGB bytes)."""
    data = open(p, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('%s: not a PNG' % p)
    pos, idat, w, h = 8, b'', 0, 0
    while pos < len(data):
        n, t = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if zlib.crc32(t + body) & 0xFFFFFFFF != struct.unpack('>I', data[pos + 8 + n:pos + 12 + n])[0]:
            raise ValueError('%s: chunk %s has a bad CRC' % (p, t))
        if t == b'IHDR':
            w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
            if depth != 8 or ctype != 2:
                raise ValueError('%s: not 8-bit RGB' % p)
        elif t == b'IDAT':
            idat += body
        pos += 12 + n
    raw, row = zlib.decompress(idat), w * 3
    out, prev = bytearray(), bytearray(row)
    for y in range(h):
        ft, line = raw[y * (row + 1)], bytearray(raw[y * (row + 1) + 1:(y + 1) * (row + 1)])
        for x in range(row):
            a = line[x - 3] if x >= 3 else 0
            b, c = prev[x], prev[x - 3] if x >= 3 else 0
            if ft == 1:
                line[x] = (line[x] + a) & 255
            elif ft == 2:
                line[x] = (line[x] + b) & 255
            elif ft == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif ft == 4:
                p0 = a + b - c
                pa, pb, pc = abs(p0 - a), abs(p0 - b), abs(p0 - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line
        prev = line
    return w, h, bytes(out)


def read_log(path):
    """inputs.log -> (start kind, [level per frame], [edges per frame], [event lines])."""
    kind, lv, ed, ev = None, [], [], []
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        if not line or line.startswith('#') or line.startswith('ct-inputs'):
            continue
        if line.startswith('start '):
            kind = line.split()[1]
        elif line.startswith('@'):
            ev.append(line)
        elif line.startswith('end '):
            if int(line.split()[1]) != len(lv):
                raise ValueError('%s: the runs hold %d frames, its end line says %s' % (path, len(lv), line.split()[1]))
        else:
            n, lvl, edg = line.split()
            lv += [int(lvl, 16)] * int(n)
            ed += [int(edg, 16)] * int(n)
    return kind, lv, ed, ev


def pad_of(levels):
    """Per-frame words -> [(mask, frames)]: the --btnscript grammar's runs."""
    out = []
    for v in levels:
        if out and out[-1][0] == v:
            out[-1][1] += 1
        else:
            out.append([v, 1])
    return [tuple(x) for x in out]


def btnscript(pad):
    return ','.join('%X:%d' % (mk, n) for mk, n in pad)


def chains(tl):
    """The distinct values of each graded byte in order, with the frame each appeared (playthrough.py chains), from the
    first lit frame on: the power-on's black stretch is the cold init's -- the engine's CtWramCreate seeds the party and
    HP at its frame 0, the cartridge's reset 22 frames in, behind the sound driver's upload -- and shows nothing (as
    playthrough.py leaves a load's black inside ungraded). The control byte only where the brightness is not 0."""
    ch = {k: [] for k in GRADED}
    lit = next((i for i, (_, kv) in enumerate(tl) if kv['bri'] != '00'), len(tl))
    for f, kv in tl[lit:]:
        for k in ch:
            if k == 'ctrl' and kv['bri'] == '00':
                continue
            v = '%03X' % (int(kv[k][2:4] + kv[k][0:2], 16) & 0x1FF) if k == 'loc' else kv[k]
            if not ch[k] or ch[k][-1][1] != v:
                ch[k].append((f, v))
    return ch


def run(cmd, env, timeout=7200):
    return subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True, errors='replace', timeout=timeout)


def start(cmd, env):
    """A run in the background (the replay's four runs go side by side): finish() collects it."""
    return subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            errors='replace')


def finish(p, timeout=7200):
    out, err = p.communicate(timeout=timeout)
    return subprocess.CompletedProcess(p.args, p.returncode, out, err)


def engine_env(tmp, sram, log, name='replay'):
    """A headless engine from `sram` (a copy of its own: a save in the run writes the copy), replaying `log` when one is
    given (CT_PLAYINPUT)."""
    srm = os.path.join(tmp, name + '.srm')
    if os.path.exists(sram):
        shutil.copyfile(sram, srm)
    elif os.path.exists(srm):
        os.remove(srm)
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', CT_NOPACE='1', CT_AUDIO_LOCKSTEP='1',
               CT_SRAM=srm, CT_BUG_DIR=os.path.join(tmp, name + '_bugs'))
    if log:
        env['CT_PLAYINPUT'] = log
    return env


def memmon(stdout):
    tl = []
    for l in stdout.splitlines():
        m = re.match(r'fmemmon f(\d+) loc=\S+\s+(.*)', l)
        if m:
            kv = {k: v.rstrip('*') for k, v in re.findall(r'\b([0-9A-F]+)=([0-9A-F*]+)', m.group(2))}
            if all(p in kv for _, _, p, _ in MON):
                tl.append((int(m.group(1)), {k: kv[p] for k, _, p, _ in MON}))
    return tl


def engine_check_start(d, tmp, name='replay'):
    """The engine's replay graded at the report, with its timeline (engine_check_finish reads them)."""
    env = engine_env(tmp, os.path.join(d, 'sram.srm'), os.path.join(d, 'inputs.log'), name)
    env.update(CT_REPLAY_CHECK=d, CT_FIELD_MEMMON=','.join('%s:%d' % (p, n) for _, _, p, n in MON))
    return start([FIELD, '--assets', 'ct_assets.dat', '--boot'], env)


def engine_check_finish(p):
    """(its `bugreport-replay:` line or None, its timeline, the run)."""
    r = finish(p)
    line = next((l for l in r.stdout.splitlines() if l.startswith('bugreport-replay:')), None)
    return line, memmon(r.stdout), r


def engine_check(d, tmp, name='replay'):
    return engine_check_finish(engine_check_start(d, tmp, name))


def engine_pictures_start(d, tmp, host_frame, center=0, timeline=True):
    """The engine's 256x224 pictures a few frames either side of the report's frame (+ `center`), and with `timeline` its
    graded bytes on to EXT idle frames past the report (the log over, no input: the player had stopped to report).
    CT_SHOTS numbers the host's frames (frameNo), which is what report.json's host_frame records."""
    env = engine_env(tmp, os.path.join(d, 'sram.srm'), os.path.join(d, 'inputs.log'), 'pictures%+d' % center)
    at = [host_frame + center + k for k in SHOTS if host_frame + center + k > 0]
    end = at + ([host_frame + EXT] if timeline else [])
    env.update(CT_SHOTS=os.path.join(tmp, 'p%+d' % center), CT_SHOTS_AT=','.join(map(str, end)), CT_SHOTS_EXIT='1')
    if timeline:
        env['CT_FIELD_MEMMON'] = ','.join('%s:%d' % (p, n) for _, _, p, n in MON)
    return start([FIELD, '--assets', 'ct_assets.dat', '--boot'], env), at, end, center


def engine_pictures_finish(job, tmp, host_frame):
    """({offset from the report's frame: RGB}, the timeline) of a run engine_pictures_start began."""
    p, at, end, center = job
    r = finish(p)
    pics = {}
    for f in end:
        fn = os.path.join(tmp, 'p%+d_f%d.raw' % (center, f))
        if os.path.exists(fn):
            if f in at:
                b = open(fn, 'rb').read()
                pw = struct.unpack('<I', b[:4])[0]
                x0 = (pw - 256) // 2 * 3
                pics[f - host_frame] = b''.join(b[8 + y * pw * 3 + x0:8 + y * pw * 3 + x0 + 768] for y in range(224))
            os.remove(fn)
    return pics, memmon(r.stdout)


def cartridge_start(d, tmp, pad, frames):
    """The cartridge from power-on with the report's battery: its timeline (on to EXT idle frames past the window) and
    its picture at the window's end, two runs side by side."""
    env = dict(os.environ, CT_ORACLE_LOGICAL='1')
    if os.path.exists(os.path.join(d, 'sram.srm')):
        env['CT_ORACLE_SRAM'] = os.path.join(d, 'sram.srm')
    raw = os.path.join(tmp, 'cart.raw')
    ext = list(pad) + [(0, EXT)]
    mon = start([CT, '--monitor', ','.join('%s:%d' % (a, n) for _, a, _, n in MON), '--from', 'boot', '--input',
                 btnscript(ext), '--frames', str(frames + EXT), '--diff'], env)
    peek = start([CT, '--peek', 'boot', raw, '--frames', str(frames), '--btnscript', btnscript(pad) or '0:0'], env)
    return mon, peek, raw, frames + EXT


def cartridge_finish(job):
    """(its timeline, its last picture as 256x224 RGB or None, why it is unavailable or None)."""
    mon, peek, raw, frames = job
    r, rp = finish(mon), finish(peek)
    tl = []
    for line in r.stdout.splitlines():
        m = re.match(r'\s*f(\d+)\s+(.*)', line)
        if not m or int(m.group(1)) > frames:
            continue
        kv = dict(re.findall(r'\b([0-9A-F]{6})=([0-9A-F]+)', m.group(2)))
        if all(a in kv for _, a, _, _ in MON):
            tl.append((int(m.group(1)), {k: kv[a] for k, a, _, _ in MON}))
    why = None
    if r.returncode != 0 or not tl:
        why = 'ct.exe --monitor exited %d with %d samples: %s' % (r.returncode, len(tl), (r.stdout + r.stderr)[-300:].strip())
    pic = None
    if rp.returncode == 0 and os.path.exists(raw):
        o = open(raw, 'rb').read()
        px = bytearray()
        for y in range(224):
            rr = (y + 8) * 1024
            for x in range(256):
                i = (rr + x * 2) * 4
                px += bytes((o[i + 3], o[i + 2], o[i + 1]))
        pic = bytes(px)
    elif not why:
        why = 'ct.exe --peek exited %d: %s' % (rp.returncode, (rp.stdout + rp.stderr)[-300:].strip())
    return tl, pic, why


def picture_grade(c, pics):
    """(offset, differing pixels) of the engine's picture closest to the cartridge's (playthrough.py picture_grade)."""
    best = None
    for k, p in sorted(pics.items(), key=lambda t: abs(t[0])):
        n = sum(1 for i in range(0, len(c), 3) if c[i:i + 3] != p[i:i + 3])
        if best is None or n < best[1]:
            best = (k, n)
    return best or (None, -1)


def compare(o, p, cend, eend):
    """The two sides' chains compared over their windows (the cartridge's to `cend`, the engine's to `eend`), each side
    allowed its idle frames past the window to reach a value the other reached inside its own: the engine's loads skip
    the cartridge's black frames, so at the report the two can stand dozens of frames apart, and a value one side reached
    just before the report is not a split when the other reaches it just after. Returns (the split as (key, index) or
    None, the values reached only past a window as (key, value, side, frame), the matched values' (cartridge frame,
    engine frame - cartridge frame) inside both windows)."""
    split, pend, offs = None, [], []
    for key in GRADED:
        n = max(sum(1 for f, _ in o[key] if f <= cend), sum(1 for f, _ in p[key] if f <= eend))
        for i in range(n):
            ov = o[key][i] if i < len(o[key]) else None
            pv = p[key][i] if i < len(p[key]) else None
            if ov is None or pv is None or ov[1] != pv[1]:
                f = (ov or pv)[0]
                if split is None or f < split[0]:
                    split = (f, key, i)
                break
            if ov[0] > cend:
                pend.append((key, ov[1], 'cartridge', ov[0]))
            elif pv[0] > eend:
                pend.append((key, pv[1], 'engine', pv[0]))
            else:
                offs.append((ov[0], pv[0] - ov[0]))
    if split:                                    # (only what both sides agreed on before the split measures the drift)
        offs = [x for x in offs if x[0] < split[0]]
    return (split[1:] if split else None), pend, offs


def replay(d, verbose=False, keep=False):
    """Replay the report in folder `d` on both sides; prints the lines and the verdict; returns a dict of the results."""
    rep = json.load(open(os.path.join(d, 'report.json'), encoding='utf-8'))
    kind, lv, ed, ev = read_log(os.path.join(d, 'inputs.log'))
    num, frames = rep['report'], rep['frame']
    res = {'report': num, 'engine': False, 'cartridge': False, 'verdict': None}
    if kind != 'boot':
        res['verdict'] = 'no replay: the session started by %s, not at power-on' % kind
        print('replay: report %04d -- %s' % (num, res['verdict']))
        return res
    if len(lv) != frames:
        res['verdict'] = 'no replay: the log holds %d frames, the report was taken at %d' % (len(lv), frames)
        print('replay: report %04d -- %s' % (num, res['verdict']))
        return res
    print('replay: report %04d, %d frames from power-on; the session\'s replay is %s' % (num, frames, rep['session']['replay']))
    for e in ev:
        print('  event %s' % e)
    # presses the controller word does not show (two keys on one button, a press and release inside one frame): the
    # engine's frames where the host made an A press with no A rising in the word -- the cartridge never sees them
    unseen = [k for k in range(1, len(lv)) if (ed[k] & 1) and not (lv[k] & 0x100 and not lv[k - 1] & 0x100)]
    tmp = tempfile.mkdtemp(prefix='ct_bugreplay_')
    try:
        hf = rep.get('host_frame', frames)
        cend = frames - 1                                          # the cartridge's frame k = the engine's k + 1
        jobs = (engine_check_start(d, tmp), engine_pictures_start(d, tmp, hf), cartridge_start(d, tmp, pad_of(lv[1:]), cend))
        line, _, er = engine_check_finish(jobs[0])
        pics, etl = engine_pictures_finish(jobs[1], tmp, hf)
        ctl, cpic, why = cartridge_finish(jobs[2])
        res['engine'] = bool(line) and ' wram 131072/131072 identical' in line and 'picture identical' in line
        print(line or 'bugreport-replay: no line (exit %d): %s' % (er.returncode, (er.stdout + er.stderr)[-400:]))
        if why:
            res['cartridge'] = None
            print('bugreport-cartridge: UNAVAILABLE -- %s' % why)
            res['verdict'] = ('%s; the cartridge could not be played (%s): no verdict on the port' % (
                'the engine reproduces its report' if res['engine'] else 'the engine does NOT reproduce its own report', why))
            print('verdict: %s' % res['verdict'])
            return res
        o, p = chains(ctl), chains(etl)
        split, pend, offs = compare(o, p, cend, hf)
        last = max(offs)[1] if offs else 0                         # the sides' offset at the latest value they share
        drift = max([abs(x[1]) for x in offs] or [0])
        k, npx = picture_grade(cpic, pics) if pics else (None, None)    # (None: the engine took no picture there)
        if npx and abs(last) > 6:                                  # the pictures where the engine stood at the report
            more, _ = engine_pictures_finish(engine_pictures_start(d, tmp, hf, last, False), tmp, hf)
            k2, n2 = picture_grade(cpic, more) if more else (None, None)
            if n2 is not None and n2 < npx:
                k, npx = k2, n2
        res['cartridge'] = split is None and npx == 0
        ow = {kk: [v for f, v in o[kk] if f <= cend] for kk in GRADED}
        print('bugreport-cartridge: %s -- locs %s | story %s | party %s | end picture %s' % (
            'SAME' if res['cartridge'] else 'DIFFERENT', ' '.join(ow['loc']), ' '.join(ow['story']), ' '.join(ow['party']),
            '%d px at engine %+d' % (npx, k) if k is not None else 'none'))
        for key, v, side, f in pend:
            print('  past the window: %s %s reached by the %s only in its idle frames (frame %d; the two sides %+d frames '
                  'apart at their last shared value)' % (key, v, side, f, last))
        if split or verbose:
            for key in GRADED:
                print('  %-5s cartridge %s' % (key, ' '.join('%s@%d' % (v, f) for f, v in o[key] if f <= cend + EXT)))
                print('  %-5s engine    %s' % (key, ' '.join('%s@%d' % (v, f) for f, v in p[key] if f <= hf + EXT)))
        if unseen:
            print('  note: %d frame(s) carry an A press the controller word does not show (two keys on one button, or a '
                  'press and release inside one frame): the cartridge cannot see those (the first at frame %d)' % (
                      len(unseen), unseen[0]))
        if not res['engine']:
            res['verdict'] = ('the engine does NOT reproduce its own report: not deterministic here, or the session\'s replay '
                              'is approximate (%s)' % rep['session']['replay'])
        elif split:
            key, i = split
            ov = '%s@%d' % (o[key][i][1], o[key][i][0]) if i < len(o[key]) else 'none'
            pv = '%s@%d' % (p[key][i][1], p[key][i][0]) if i < len(p[key]) else 'none'
            at = o[key][i][0] if i < len(o[key]) else p[key][i][0]
            res['verdict'] = ('a PORT bug candidate: the engine parts from the cartridge at %s value %d -- cartridge %s, '
                              'engine %s (start the hunt there: the triage-regression playbook)' % (key, i, ov, pv))
            if drift > 3:
                res['verdict'] += ('; but the two sides were already up to %d frames apart before it, and an input timed '
                                   'on the engine can meet another moment on the cartridge there (timing drift): a lead, '
                                   'not a verdict' % drift)
            if unseen and unseen[0] - 1 <= at:
                res['verdict'] += ('; and %d A press(es) the cartridge never saw came before it (the first at frame %d): '
                                   'one of them may be the split' % (sum(1 for u in unseen if u - 1 <= at), unseen[0]))
        elif npx is None:
            res['verdict'] = "the state agrees with the cartridge; the engine took no picture around the report's frame"
        elif npx != 0:
            res['verdict'] = ('the state agrees with the cartridge, the 256x224 picture does not (%d px at its closest, %+d '
                              'frames): a port picture bug' % (npx, k if k is not None else 0))
        else:
            res['verdict'] = ('both sides agree, state and 256x224 picture: a problem only in screen.png (what the player '
                              'saw) is the presenter\'s or an HD manifest\'s; else it is the original game\'s own behaviour')
        print('verdict: %s' % res['verdict'])
    finally:
        if keep:
            print('kept: %s' % tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    return res


# ---------------------------------------------------------------------------------------------------------- the checks
def report_dirs(base):
    return sorted(os.path.join(base, x) for x in os.listdir(base) if re.match(r'\d{4}_', x)) if os.path.isdir(base) else []


def check():
    tmp = tempfile.mkdtemp(prefix='ct_bugcheck_')
    try:
        bugs = os.path.join(tmp, 'bugs')
        env = engine_env(tmp, os.path.join(tmp, 'none.srm'), DRIVE)
        env.update(CT_BUG_DIR=bugs, CT_BUGREPORT_AT='4700:picture:the check\'s report', CT_BUGREPORT_EXIT='1')
        r = run([FIELD, '--assets', 'ct_assets.dat', '--boot'], env)
        saved = [l for l in r.stdout.splitlines() if l.startswith('bugreport: saved')]
        print(saved[0] if saved else 'bugreport-check: FAIL no report (exit %d): %s' % (r.returncode, (r.stdout + r.stderr)[-400:]))
        ds = report_dirs(bugs)
        if not ds:
            return 1
        d = ds[0]
        rep = json.load(open(os.path.join(d, 'report.json'), encoding='utf-8'))
        have = sum(1 for f in FILES if os.path.exists(os.path.join(d, f)))
        nw, nh, npx = png_rgb(os.path.join(d, 'screen_native.png'))
        sw, sh, _ = png_rgb(os.path.join(d, 'screen.png'))
        wram = open(os.path.join(d, 'wram.bin'), 'rb').read()
        _, dlv, ded, _ = read_log(DRIVE)
        kind, lv, ed, ev = read_log(os.path.join(d, 'inputs.log'))
        same_log = lv == dlv[:len(lv)] and ed == ded[:len(ed)]
        index = open(os.path.join(bugs, 'index.csv'), encoding='utf-8').read().splitlines()
        print('bugreport-check: report %04d frame %d loc %s mode %s: files %d/%d, native %dx%d fnv %08X (report %s), screen '
              '%dx%d, wram fnv %08X (report %s), log %s %d frames %s the drive\'s, %d events, index %d line(s), replay %s' % (
                  rep['report'], rep['frame'], rep['location'], rep['mode'], have, len(FILES), nw, nh, fnv(npx),
                  rep['hashes']['native_fnv'], sw, sh, fnv(wram), rep['hashes']['wram_fnv'], kind, len(lv),
                  '=' if same_log else '!=', len(ev), len(index) - 1, rep['session']['replay']))
        res = replay(d)
        print('bugreport-check: replay engine %s, cartridge %s' % ('EXACT' if res['engine'] else 'DIFFERENT',
                                                                 'SAME' if res['cartridge'] else 'DIFFERENT'))
        # the grade must be able to fail: the same log with its edges dropped (A, the d-pad, the naming keys never pressed)
        bad = os.path.join(tmp, 'damaged.log')
        with open(bad, 'w', newline='\n') as f:
            for l in open(os.path.join(d, 'inputs.log'), encoding='utf-8'):
                m = re.match(r'^(\d+) ([0-9A-F]{3}) [0-9A-F]{3}$', l.rstrip('\n'))
                f.write('%s %s 000\n' % (m.group(1), m.group(2)) if m else l)
        env = engine_env(tmp, os.path.join(d, 'sram.srm'), bad)
        env['CT_REPLAY_CHECK'] = d
        r = run([FIELD, '--assets', 'ct_assets.dat', '--boot'], env)
        line = next((l for l in r.stdout.splitlines() if l.startswith('bugreport-replay:')), 'no replay line')
        print('bugreport-check: the log without its edges: %s' % line[line.find('wram'):])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0


def panel_session(tmp, name, ini, keys, typed, reports):
    """A session under the interactive presentation without a window, driven by the drive, `keys` (CT_BUGKEYS) and the
    text typed into the panel; its reports' folders (each replayed in the engine afterwards)."""
    bugs = os.path.join(tmp, name)
    open(os.path.join(tmp, name + '.ini'), 'w').write(ini)
    env = engine_env(tmp, os.path.join(tmp, 'none.srm'), DRIVE)
    env.update(CT_BUG_DIR=bugs, CT_SETTINGS=os.path.join(tmp, name + '.ini'), CT_BUGKEYS=keys, CT_BUGPANEL_TYPE=typed,
               CT_BUGREPORT_EXIT=str(reports), CT_BUG_CONSOLE='1')
    try:
        r = run([FIELD, '--assets', 'ct_assets.dat', '--boot'], env, timeout=240)
        out = r.stdout
    except subprocess.TimeoutExpired as e:                 # (fewer reports than asked for: the run never leaves)
        out = e.stdout.decode('utf-8', 'replace') if isinstance(e.stdout, bytes) else (e.stdout or '')
        print('bugreport-check: %s ran out of time (240 s)' % name)
    for l in out.splitlines():
        if l.startswith('bugreport: saved'):
            print(l)
    return report_dirs(bugs)


def report_frames(ds):
    return ', '.join('%d' % json.load(open(os.path.join(d, 'report.json'), encoding='utf-8'))['frame'] for d in ds) or 'none'


def check_panel():
    tmp = tempfile.mkdtemp(prefix='ct_bugpanel_')
    try:
        # the default options: `~` and a note typed into the panel, then the touchpad held
        ds = panel_session(tmp, 'plain', '', '3200:grave,4000:touch', 'the bed\'s shadow is one pixel off', 2)
        # widescreen 16:9 at 2x internal resolution: the session's mods, all off in the replay
        ds += panel_session(tmp, 'wide', '[video]\nwidescreen=16:9\nrender_scale=2\n', '4500:grave', '16:9 at 2x', 1)
        if len(ds) != 3:
            print('bugreport-check: FAIL %d report(s), not 3' % len(ds))
            return 1
        for d in ds:
            rep = json.load(open(os.path.join(d, 'report.json'), encoding='utf-8'))
            sw, sh, _ = png_rgb(os.path.join(d, 'screen.png'))
            line, _, _ = engine_check(d, tmp)
            o = rep['options']
            print('bugreport-check: panel report %04d frame %d note %s%s category %s, screen %dx%d (the window; picture %d '
                  'wide at x%d), %s' % (
                      rep['report'], rep['frame'], json.dumps(rep['note']), ' (note to add)' if rep['note_to_add'] else '',
                      rep['category'], sw, sh, o['picture_width'], o['render_scale'],
                      line[len('bugreport-replay: '):] if line else 'no replay line'))
        # (the tee's two readers append as their pipes fill: which line is last can vary, the session's start cannot)
        con = open(os.path.join(ds[0], 'console.txt'), encoding='utf-8', errors='replace').read().splitlines()
        print('bugreport-check: console.txt %s the session\'s start' % (
            'holds' if any(l.startswith('[bugreport] replaying') for l in con) else 'LACKS'))
        # the touchpad bound to a button of the game is that button: no report from it, `~` still takes one
        fr = report_frames(panel_session(tmp, 'bound', '[controls]\npad_select=touchpad\n', '3100:touch,3300:grave',
                                         'the touchpad is Select', 1))
        print('bugreport-check: the touchpad bound to Select, held from 3100, `~` at 3300: reports at %s' % fr)
        # held over the panel (`~` at 3150): the panel takes its events, so its hold starts over when the panel closes --
        # no report 60 frames after a press the player may have let go of over the panel
        fr = report_frames(panel_session(tmp, 'modal', '', '3100:touch,3150:grave,3400:grave', 'over a held touchpad', 2))
        print('bugreport-check: the touchpad held from 3100, `~` at 3150 and 3400: reports at %s' % fr)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0


def pad_session(tmp, name, extra, reports):
    """A session played the way a player plays: the drive's level on a virtual controller (CT_BUGPAD: SDL's controller
    state and its button events), no CT_PLAYINPUT -- so the log is what the host records from the pad."""
    bugs = os.path.join(tmp, name)
    env = engine_env(tmp, os.path.join(tmp, 'none.srm'), None, name)
    env.update(CT_BUG_DIR=bugs, CT_SETTINGS=os.path.join(tmp, 'none.ini'), CT_BUGPAD=DRIVE, CT_BUGREPORT_EXIT=str(reports))
    env.update(extra)
    try:
        r = run([FIELD, '--assets', 'ct_assets.dat', '--boot'], env, timeout=240)
        out = r.stdout
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode('utf-8', 'replace') if isinstance(e.stdout, bytes) else (e.stdout or '')
        print('bugreport-check: %s ran out of time (240 s)' % name)
    for l in out.splitlines():
        if l.startswith('bugreport: saved') or l.startswith('bugreport-pad:'):
            print(l)
    return report_dirs(bugs)


def check_pad():
    tmp = tempfile.mkdtemp(prefix='ct_bugpad_')
    try:
        _, dlv, ded, _ = read_log(DRIVE)
        # the drive on the pad: the host's own recording must be the drive itself, and the game the drive's game
        ds = pad_session(tmp, 'pad', {'CT_BUGREPORT_AT': '4700:picture:the pad\'s report'}, 1)
        if len(ds) != 1:
            print('bugreport-check: FAIL the pad session wrote %d report(s)' % len(ds))
            return 1
        rep = json.load(open(os.path.join(ds[0], 'report.json'), encoding='utf-8'))
        _, lv, ed, _ = read_log(os.path.join(ds[0], 'inputs.log'))
        bad = [k for k in range(len(lv)) if lv[k] != dlv[k] or ed[k] != ded[k]]
        line, _, _ = engine_check(ds[0], tmp)
        print('bugreport-check: pad report %04d frame %d: log %d frames, %d differ from the drive%s; wram fnv %s, native '
              'fnv %s; %s' % (rep['report'], rep['frame'], len(lv), len(bad),
                              ' (the first at %d: level %03X edges %03X, the drive %03X %03X)' % (
                                  bad[0], lv[bad[0]], ed[bad[0]], dlv[bad[0]], ded[bad[0]]) if bad else '',
                              rep['hashes']['wram_fnv'], rep['hashes']['native_fnv'],
                              line[len('bugreport-replay: '):] if line else 'no replay line'))
        # Start held on the pad from 3195 to 3209 across the panel (`~` at 3200): from the panel's close the button is
        # kept from the game until it is let go -- the key that closed a panel never reaches the game
        ds = pad_session(tmp, 'held', {'CT_BUGPAD_HOLD': '8@3195:15', 'CT_BUGKEYS': '3200:grave',
                                       'CT_BUGPANEL_TYPE': 'Start held over the panel',
                                       'CT_BUGREPORT_AT': '4000:other:after the panel'}, 2)
        if len(ds) != 2:
            print('bugreport-check: FAIL the held session wrote %d report(s)' % len(ds))
            return 1
        _, lv, ed, _ = read_log(os.path.join(ds[1], 'inputs.log'))
        start_on = [k for k in range(len(lv)) if (lv[k] & 8) and not (dlv[k] & 8)]
        rest = [k for k in range(len(lv)) if (lv[k] & ~8) != (dlv[k] & ~8)]
        print('bugreport-check: held Start on the game\'s side at frames %s; the rest of the level %s the drive\'s' % (
            '%d-%d' % (start_on[0], start_on[-1]) if start_on else 'none', 'is' if not rest else 'is NOT (%d frames)' % len(rest)))
        for d in ds:
            rep = json.load(open(os.path.join(d, 'report.json'), encoding='utf-8'))
            line, _, _ = engine_check(d, tmp)
            print('bugreport-check: held report %04d frame %d note %s, %s' % (
                rep['report'], rep['frame'], json.dumps(rep['note']), line[len('bugreport-replay: '):] if line else 'no replay line'))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 0


def main():
    a = sys.argv[1:]
    if a[:1] == ['replay'] and len(a) >= 2:
        res = replay(os.path.abspath(a[1]), '-v' in a, '--keep' in a)
        return 0 if res['engine'] else 1
    if a == ['check']:
        return check()
    if a == ['check-panel']:
        return check_panel()
    if a == ['check-pad']:
        return check_pad()
    sys.exit(__doc__)


if __name__ == '__main__':
    sys.exit(main())
