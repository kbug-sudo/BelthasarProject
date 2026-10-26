#!/usr/bin/env python3
r"""The cutscene mod's moments in the game: each clip's trigger met on the cartridge and on the engine, from the same state
with the same pad.

    python tools/cutscene_moments.py [<id> ...]

The remaster places its clips (docs/fmv_manifest.json: the PS1 release's animated scenes) at moments found by playing the
cartridge from public battery saves. Each moment's drive is a recipe (tools/fixtures/cutscenes.json: a state a little
before it); from that state:

  - the CARTRIDGE (ct.exe --monitor under the rule) plays the moment's pad: the first frame on which the moment holds --
    `$0100` and `$7F:0000` (and the bytes it names) as the frame leaves them; with `story_change`, the story byte having
    changed on that frame inside the location;
  - the ENGINE plays the same pad from the same work RAM and sound (Belthasar --loadwram, CT_AUDIO_SEEDSTATE, lockstep:
    playthrough.py's port side), the cutscene mod ON with a list holding that one moment and a short clip made
    here (tools/ctlib/mpeg1_clip.py; five pictures), interactive (the probes' rule): the frame its own `cutscene:` line names. A clip
    plays at the top of the frame after the one that left the moment, so the engine's frame is the cartridge's + 1 where
    the two run in step.

One line a moment: `cutscene-moment: M2 at 111 6C: cartridge f759, engine M2 f760` (two clips at one moment -- the ending's
wedding, then the Fall of Guardia -- are listed in order and must play in it). A moment the engine does not meet says so.
The moments are the remaster's (its tools/fmv.py writes the same grammar); M1's pad presses A a few frames after the world
map's entrance latch (pressed one frame before it, the cartridge enters and the engine does not: an open finding).
"""
import json
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recipes as R                                        # noqa: E402
from ctlib import mpeg1_clip                               # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CT = os.path.join(ROOT, 'build', 'ct.exe')
FIELD = os.path.join(ROOT, 'build', 'Belthasar.exe')
ROM = os.path.join(ROOT, 'Chrono_Trigger_USA_SNES.sfc')
TMP = os.path.join(ROOT, 'build', '_cutscene_moments')
RULE = dict(os.environ, CT_ORACLE_LOGICAL='1')

M1 = ('80:64,20:48,80:64,20:64,0:60,0:30,80:96,0:4,20:16,0:4,80:128,0:4,80:128,0:4,10:16,0:2,80:128,0:4,80:64,0:2,20:64,'
      '0:4,10:20,0:2,80:56,0:2,20:96,0:4,80:16,0:2,20:32,0:10,80:64,0:2,20:96,0:4,20:40,0:2,40:56,0:4,60:32,0:4,40:16,0:4,'
      '100:4,0:40,40:20,0:4,10:40,0:4,100:4,0:60,40:12,0:2,10:24,0:4,40:8,0:2,10:24,0:4,40:8,0:2,10:24,0:30,10:4,0:2,100:4,'
      '0:40,(80:8,0:2,10:4,0:2,100:4,0:40)*4,80:8,0:2,10:4,0:2,10:2,0:8,100:4,0:80')
# (id, the fixture state, the moment as the list writes it, the pad from that state)
MOMENTS = [
    ('M1', 'saves/cutscene_re/m1_pre.state', 'at 0E2 37', M1),
    ('M2', 'saves/cutscene_re/m2_pre.state', 'at 111 6C 7E29AE=0B', '0:306,100:2,0:398,100:2,0:498'),  # (Ayla's song)
    ('M3', 'saves/cutscene_re/m3_pre.state', 'at 0A3 84 7F0057=03',
     '0:786,100:2,0:170,100:2,0:130,100:2,0:354,100:2,0:138,100:2,0:230,100:2,0:194,100:2,0:906,100:2,0:134,100:2,0:98,'
     '100:2,0:358,100:2,0:150,100:2,0:328,0:62,100:2,0:602,100:2,0:78,100:2,0:1982,100:2,0:382,100:2,0:874,100:2,0:162,'
     '100:2,0:1844'),
    ('M4', 'saves/cutscene_re/m4_pre.state', 'at 0AC 89', '40:42,10:32,0:40'),
    ('M5', 'saves/cutscene_re/m5_pre.state', 'at 127 93 story_change', '0:600,1:2,0:60,0:1000'),
    ('M6', 'saves/cutscene_re/m6_pre.state', 'at 0F3 AE story_change', '0:366,100:2,0:400'),
    ('M7', 'saves/cutscene_re/m7_pre.state', 'at 1A7 C9 7E29AE=0D', None),  # (Crono rises; lg_ocean_palace_19f's 1st step)
    ('ENOSP1+ENOSP2', 'saves/cutscene_re/ending_pre.state', 'at 05A D6 7E0384=00', '0:8700'),   # (two clips, in order)
]


def parse(moment):
    t = moment.split()
    loc, story = int(t[1], 16), int(t[2], 16)
    conds = []
    for c in t[3:]:
        m = re.fullmatch(r'([0-9A-F]{6})(?:&([0-9A-F]{2}))?=([0-9A-F]{2})', c)
        if m:
            conds.append((int(m.group(1), 16), int(m.group(2) or 'FF', 16), int(m.group(3), 16)))
    return loc, story, conds, 'story_change' in t


def cartridge(state, pad, moment):
    loc, story, conds, change = parse(moment)
    n = sum(int(x.split(':')[1]) for x in pad.split(','))
    mon = ['7E0100:2', '7F0000:1'] + ['%06X:1' % a for a, _, _ in conds]
    r = subprocess.run([CT, ROM, '--monitor', ','.join(mon), '--from', state, '--input', pad, '--frames', str(n), '--diff'],
                       cwd=ROOT, env=RULE, capture_output=True, text=True, errors='replace')
    prev = None
    for line in r.stdout.splitlines():
        m = re.match(r'\s*f(\d+)\s+(.*)', line)
        if not m:
            continue
        kv = {int(a, 16): int(v, 16) for a, v in re.findall(r'\b([0-9A-F]{6})=([0-9A-F]+)', m.group(2))}
        lw = kv.get(0x7E0100, 0)
        cur = ((((lw & 0xFF) << 8) | (lw >> 8)) & 0x1FF, kv.get(0x7F0000))
        holds = cur == (loc, story) and all((kv.get(a, -1) & mk) == v for a, mk, v in conds)
        if holds and change:
            holds = prev is not None and prev[0] == cur[0] and prev[1] != cur[1]
        if holds:
            return int(m.group(1))
        prev = cur
    return None


def engine(cid, state, pad, moment, d):
    raw = os.path.join(d, 'seed.raw')
    subprocess.run([CT, '--peek', state, raw, '--btnscript', '0:0'], cwd=ROOT, env=RULE, capture_output=True)
    w = open(raw + '.wram', 'rb').read()
    lw = os.path.join(d, 'seed.lakewram')
    open(lw, 'wb').write(b'#lakewram\nRAM:131072:' + w)
    sloc = (w[0x100] | (w[0x101] << 8)) & 0x1FF
    clips = os.path.join(d, 'clips')
    os.makedirs(clips, exist_ok=True)
    mpeg1_clip.write(os.path.join(clips, 'c.mpg'), pictures=5, audio=False)
    open(os.path.join(clips, 'cutscenes.txt'), 'w', newline='\n').write(
        'ct-cutscenes 1\n' + ''.join('%s c.mpg %s\n' % (i, moment) for i in cid.split('+')))
    ini = os.path.join(d, 'opts.ini')
    open(ini, 'w').write('[video]\ncutscenes=on\n')
    toks = [(int(a, 16), int(b)) for a, b in (t.split(':') for t in pad.split(','))]
    ws = ','.join(['.:1'] + ['H%X:%d' % (mk, k) if mk else '.:%d' % k for mk, k in toks])
    n = sum(k for _, k in toks)
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', CT_NOPACE='1', CT_AUDIO_LOCKSTEP='1',
               CT_AUDIO_SEEDSTATE=state, CT_WS_BATTLE_LEAD='1', CT_BUGKEYS='99999999:grave', CT_SETTINGS=ini,
               CT_CUTSCENE_DIR=clips, CT_BUG_DIR=os.path.join(d, 'bugs'), CT_SHOTS=os.path.join(d, 'end'),
               CT_SHOTS_AT=str(n + 2), CT_SHOTS_EXIT='1')
    for k in ('CT_CUTSCENE_SKIP', 'CT_CUTSCENE_SHOT', 'CT_HD_PRESENTER', 'CT_PLAYINPUT', 'CT_BUGPAD'):
        env.pop(k, None)
    r = subprocess.run([FIELD, '--assets', 'ct_assets.dat', '--scene', str(sloc), '--loadwram', lw, '--walkscript', ws],
                       cwd=ROOT, env=env, capture_output=True, text=True, errors='replace', timeout=1800)
    got = re.findall(r'^cutscene: (\S+) at frame (\d+) \([^)]*\): (\S+)', r.stdout, re.M)
    return [(i, int(f), how) for i, f, how in got]      # (every clip the engine played, in its order)


def main():
    want = set(sys.argv[1:])
    ok = True
    for cid, st, moment, pad in MOMENTS:
        if want and cid not in want:
            continue
        state = os.path.join(ROOT, st)
        if not os.path.exists(state):
            print('cutscene-moment: %s NOT RUN -- no %s (python tools/fixtures.py make)' % (cid, st))
            ok = False
            continue
        if pad is None:
            pad = R.load_all()['lg_ocean_palace_19f']['steps'][0][1]
        pad = R.expand(pad)
        d = os.path.join(TMP, cid)
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        cf = cartridge(state, pad, moment)
        played = engine(cid, state, pad, moment, d)
        ef = ' then '.join('%s f%d%s' % (i, f, '' if how == 'played' else ' (%s)' % how) for i, f, how in played)
        print('cutscene-moment: %s %s: cartridge f%s, engine %s' % (cid, moment, cf, ef or 'NONE'))
        ok = ok and cf is not None and [i for i, _, how in played if how == 'played'] == cid.split('+')
        shutil.rmtree(d, ignore_errors=True)
    print('cutscene-moments: %s' % ('every moment met on both sides' if ok else 'a moment NOT met'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
