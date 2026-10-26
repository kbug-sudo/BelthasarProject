"""A test clip for the cutscene mod (src/cutscene.c), made here with no encoder and no video of anyone's: an MPEG program
stream (ISO 11172-1) holding MPEG-1 video (ISO 11172-2) of flat-grey intra pictures, the grey stepping each picture, and
silent MP2 audio (11172-3 Layer II, every allocation 0) -- the stream pl_mpeg decodes.

    from ctlib import mpeg1_clip
    mpeg1_clip.write('clip.mpg', pictures=30, w=64, h=36, fps=30, audio=True)

A picture is one slice per macroblock row; each macroblock intra-coded with its six blocks' DC only (the first luma
block's differential from the slice's predictor 128, the rest 0) and an end of block -- the smallest picture the syntax
allows. The pack carries a system header (one video, one audio stream) and each PES packet a PTS.
"""
import struct

LUMA_DC = ['100', '00', '01', '101', '110', '1110', '11110', '111110', '1111110']       # dct_dc_size_luminance
CHROMA_DC = ['00', '01', '10', '110', '1110', '11110', '111110', '1111110', '11111110']  # dct_dc_size_chrominance
RATE_CODE = {24: 2, 25: 3, 30: 5, 50: 6, 60: 8}


class Bits:
    def __init__(self):
        self.bits = []

    def put(self, value, n):
        for i in range(n - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def code(self, s):
        self.bits.extend(int(c) for c in s)

    def align(self):
        while len(self.bits) % 8:
            self.bits.append(0)

    def start(self, code):
        self.align()
        self.put(0x000001, 24)
        self.put(code, 8)

    def data(self):
        self.align()
        return bytes(int(''.join(map(str, self.bits[i:i + 8])), 2) for i in range(0, len(self.bits), 8))


def dc(b, diff, table):
    size = abs(diff).bit_length()
    b.code(table[size])
    if size:
        b.put(diff if diff > 0 else diff + (1 << size) - 1, size)


def picture(b, k, w, h, grey, head):
    if head:
        b.start(0xB3)                                    # sequence header
        b.put(w, 12); b.put(h, 12); b.put(1, 4); b.put(head['rate'], 4)
        b.put(2000, 18); b.put(1, 1); b.put(20, 10); b.put(0, 1); b.put(0, 1); b.put(0, 1)
        b.start(0xB8)                                    # group of pictures (closed)
        b.put(0, 1); b.put(0, 5); b.put(0, 6); b.put(1, 1); b.put(0, 6); b.put(0, 6); b.put(1, 1); b.put(0, 1)
    b.start(0x00)                                        # picture: intra
    b.put(k & 0x3FF, 10); b.put(1, 3); b.put(0xFFFF, 16); b.put(0, 1)
    for row in range((h + 15) // 16):
        b.start(row + 1)                                 # a slice a macroblock row
        b.put(8, 5); b.put(0, 1)
        for col in range((w + 15) // 16):
            b.code('1')                                  # address increment 1
            b.code('1')                                  # macroblock type: intra
            for blk in range(6):
                if blk < 4:
                    dc(b, (grey - 128) if (col == 0 and blk == 0) else 0, LUMA_DC)
                else:
                    dc(b, 0, CHROMA_DC)
                b.code('10')                             # end of block


def ts(marker, t):
    """A 33-bit time stamp's 5 bytes after a 4-bit marker."""
    return bytes([(marker << 4) | (((t >> 30) & 7) << 1) | 1, (t >> 22) & 0xFF, (((t >> 15) & 0x7F) << 1) | 1,
                  (t >> 7) & 0xFF, ((t & 0x7F) << 1) | 1])


def rate22(r):
    """A 22-bit rate between markers: '1' r[21..15], r[14..7], r[6..0] '1'."""
    return bytes([0x80 | ((r >> 15) & 0x7F), (r >> 7) & 0xFF, ((r & 0x7F) << 1) | 1])


MUX_RATE = 2000                                          # x 50 bytes/s


def pack(scr, system, audio):
    out = b'\x00\x00\x01\xBA' + ts(2, scr) + rate22(MUX_RATE)
    if system:                                           # rate bound; audio bound, fixed 0, CSPS 0; locks 0, marker, video
        body = rate22(MUX_RATE) + bytes([(1 if audio else 0) << 2, 0x20 | 1, 0xFF])   # bound 1; reserved byte
        body += bytes([0xE0, 0xE0, 46])                  # video 0: P-STD bound 46 x 1024 bytes
        if audio:
            body += bytes([0xC0, 0xC0, 32])              # audio 0: 32 x 128 bytes
        out += b'\x00\x00\x01\xBB' + struct.pack('>H', len(body)) + body
    return out


def pes(stream, pts, payload):
    head = ts(2, pts)
    return b'\x00\x00\x01' + bytes([stream]) + struct.pack('>H', len(head) + len(payload)) + head + payload


def write(path, pictures=30, w=64, h=36, fps=30, audio=True):
    head = {'rate': RATE_CODE[fps]}
    v = []
    for k in range(pictures):
        b = Bits()
        picture(b, k, w, h, 16 + (k * 7) % 220, head if k == 0 else None)
        if k == pictures - 1:
            b.start(0xB7)                                # sequence end
        v.append((int(3600 + k * 90000 / fps), 0xE0, b.data()))
    a = []
    if audio:
        frame = bytes([0xFF, 0xFD, 0x84, 0x00]) + bytes(380)   # Layer II, 128 kbit/s, 48 kHz, stereo: 384 bytes, silent
        n = int(pictures / fps * 48000 / 1152) + 1
        a = [(int(3600 + i * 90000 * 1152 / 48000), 0xC0, frame) for i in range(n)]
    out = bytearray()
    for i, (t, sid, data) in enumerate(sorted(v + a, key=lambda x: (x[0], x[1]))):
        out += pack(max(0, t - 3600), i == 0, audio) + pes(sid, t, data)
    out += b'\x00\x00\x01\xB9'                          # program end
    open(path, 'wb').write(bytes(out))
    return len(out)
