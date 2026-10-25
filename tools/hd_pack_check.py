r"""The HD presenter's pack reader (src/hdpresent.c readPack) on two one-entry packs (the check
hd-presenter-refuses-art-past-its-palette).

An entry's HD art is an index map coloured by the live palette: a map chip's index v is drawn as colour base + v of the
128 (the chip's row of 16), a recorded pixel's through the 16 colours its draw wrote. So the art may name only indices
its source's palette has -- 16 unless the source itself uses more. The engine opens each pack with the HD presenter on
(CT_HD_PRESENTER=1, CT_HD_DIR) over two frames of the attract and says what it read:

    python tools/hd_pack_check.py
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIELD = os.path.join(ROOT, 'build', 'Belthasar.exe')


def write_pack(d, hd_index):
    os.makedirs(d, exist_ok=True)
    src = bytes(1 + (x + y) % 2 for y in range(8) for x in range(8))      # an 8x8 source of indices 1 and 2
    art = bytes([hd_index] * (8 * 8 * 16))                                # its art: every subpixel that index
    with open(os.path.join(d, 'chips4x.pack'), 'wb') as f:
        f.write(b'CTHD' + struct.pack('<II', 2, 1) + bytes([8, 8, 1]) + src + art)


def opened(d):
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', CT_NOPACE='1', CT_HD_PRESENTER='1',
               CT_HD_DIR=d)
    r = subprocess.run([FIELD, '--assets', 'ct_assets.dat', '--scene', '433', '--frames', '2', '--selftest'], cwd=ROOT,
                       env=env, capture_output=True, text=True, errors='replace', timeout=120)
    said = [l for l in (r.stdout + r.stderr).splitlines() if l.startswith('[hd] ')]
    if not said:
        return 'nothing said'
    s = said[0].split(': ', 1)[1] if ': ' in said[0] else said[0]
    return 'refused' if s.startswith('no pack') else s


def main():
    tmp = tempfile.mkdtemp(prefix='ct_hdpack_')
    try:
        write_pack(os.path.join(tmp, 'good'), 2)
        write_pack(os.path.join(tmp, 'bad'), 200)
        good, bad = opened(os.path.join(tmp, 'good')), opened(os.path.join(tmp, 'bad'))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    ok = good == '1 sources' and bad == 'refused'
    print('hd-pack: %s -- art naming an index its palette has: %s; art naming index 200 of a 16-colour source: %s' % (
        'PASS' if ok else 'FAIL', good, bad))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
