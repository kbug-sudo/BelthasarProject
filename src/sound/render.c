// ============================================================================
// sound/render.c -- `ct.exe --sndrender`: the game's music, effects and samples rendered offline, with no game logic.
//
//   ct.exe --sndrender song <id> <frames> <out.pcm|-> [voicemask]
//   ct.exe --sndrender sfx <set> <id> <frames> <out.pcm|-> [voicemask]
//   ct.exe --sndrender brr <instrument> <out.pcm|->
//
// SONG: a fresh CtAudio (the driver uploaded from the ROM, as the reset's JSL C7/0000 leaves it), the game's own loader
// (CtAudioLoadSong, command $10) on a cold mailbox page, then `frames` displayed frames of the sound processor -- the
// sound machine in C, so CT_AUDIO_SPC=machine. Every sample the S-DSP makes from the loader's return on is written as
// int16 L,R at its own 32,040 Hz. `voicemask` (hex, default FF) is the mixer's voice mask (CtMixer.voiceMask, the twin of
// CT_DSP_VOICEMASK): only those voices reach the main mix and the echo input, so mask 01..80 is one voice through the
// echo -- a stem. The driver runs the same whatever the mask (it never reads the mix back outside the meter mode).
//
// THE LOOP. At the top of each pass of the driver's main loop ($0299, between two ticks) the sequencer's musical state is
// taken: every track's pointer ($02+x) and countdown ($26+x), its loop slot ($27+x) and that slot's pass count, passes
// left and return pointer ($F560/$F580/$F5C0), its octave, instrument and transpose ($F200/$F201/$F361), the tempo
// ($46/$47) and the tracks playing ($53, $86). The first time a state comes back, the music repeats from the earlier
// time: `loop S E` (samples from the start). The tempo accumulator ($48), the modulations' phases and the S-DSP are left
// out, so the second pass can differ from the first by the tick phase (one tick, 168 samples) and the echo's tail -- not
// by a note. An infinite loop's pass count ($F580 = 0 passes left) is left out too: it counts for ever. `position S E`:
// the same without the octave, instrument and transpose -- where the sequence first reaches its own loop target. It can
// come earlier than `loop`: a track whose intro leaves another octave or instrument than its body's end plays its first
// pass from the target with those until the track sets its own (song $1E: 8.5 s against 41.4 s, the same period). `end
// X`: the sample the last music track ended at ($53 = 0), for a song that does not loop. `tracks S:E,...`: each music
// track's own position loop (its pointer, countdown and loop slot alone; -1:-1 for a track that ends or is empty). Some
// songs layer tracks of different loop lengths (song $05: 640 and 696 steps of wind over a 3072-step tune; song $14's
// track 7 is 2 steps short of the others' 3840 and drifts): their whole state comes back only at the lengths' common
// multiple, past any render, while the tracks that carry the tune share one loop.
//
// SFX: the same fresh CtAudio; set 0 is the field's effects (the set the driver's upload leaves resident), set 1 the
// battle's (command $70 $01, C7/08E3, which uploads set B to $2F00), and any other set is refused; then command $18
// (voices 7-6) with the effect and pan $80, and `frames` frames. `end X`
// is where the effect's tracks are done ($86 = 0) and its voices are silent. Both commands go through the game's service
// (CtAudioService, the mailbox at $1E00) as the game posts them.
//
// BRR: instrument k's sample block decoded by the mixer itself (the S-DSP's BRR decoder, src/sound/mixer.c): the block
// at a scratch address, a voice keyed on it at pitch $1000, each group of four samples taken from the voice's ring as the
// mixer decodes it, for the block's first pass (len / 9 x 16 samples). The second source for tools/ctaudio's decoder.
//
// `keyed`: the sample numbers (SRCN) the render's key-ons started -- a song's are $20 + its instrument slots; an
// effect that keys $20 and up plays the RESIDENT song's samples, which a cold driver does not have.
//
// The summary line is the result: `sndrender: ...` with an FNV-1a/32 of the PCM. Nothing here changes how the game runs.
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "sound/machine.h"
#include "sound/mixer.h"

// One loop finder: every distinct state the driver passes through, with the sample it began at, in hash chains.
typedef struct {
  uint8_t* snaps; long nsnap, capsnap;
  long* snapAt;
  long* head; long* next;
  long loopS, loopE;
  uint8_t last[256]; int haveLast;
} Finder;

typedef struct {
  int16_t* pcm; long n, cap;    // the captured samples (L,R pairs)
  int on;
  uint64_t start;               // the sample the capture began at
  Finder full, pos;             // the whole musical state; the sequence's position only (see the header)
  Finder track[8];              // each music track's position alone
  long endAt;
  int sfx, everPlayed;
  uint8_t keyed[256];           // the sample numbers (SRCN) a key-on started while capturing
  SndMachine* m;
} Render;

enum { SNAP = 32 + 32 + 16 * 7 + 4, HBITS = 18 };   // the snapshot's bytes (snapshot() below)

static void sink(void* c, const int16_t* lr, int n) {
  Render* r = (Render*)c;
  if (!r->on) return;
  if (r->n + n > r->cap) {
    long cap = r->cap ? r->cap * 2 : 1 << 20;
    while (cap < r->n + n) cap *= 2;
    int16_t* p = (int16_t*)realloc(r->pcm, (size_t)cap * 2 * sizeof *p);
    if (!p) { fprintf(stderr, "sndrender: out of memory\n"); exit(3); }
    r->pcm = p; r->cap = cap;
  }
  memcpy(r->pcm + r->n * 2, lr, (size_t)n * 2 * sizeof *lr);
  r->n += n;
}

static uint32_t fnv(const uint8_t* p, size_t n, uint32_t h) { while (n--) { h ^= *p++; h *= 16777619u; } return h; }

static void snapshot(const uint8_t* ram, uint8_t* s) {
  int k = 0;
  memcpy(s + k, ram + 0x02, 32); k += 32;             // the 16 tracks' pointers
  memcpy(s + k, ram + 0x26, 32); k += 32;             // countdowns and loop slots
  for (int x = 0; x < 32; x += 2) {
    uint8_t sp = ram[0x27 + x];
    uint8_t left = ram[0xF580 + sp];
    s[k++] = left ? ram[0xF560 + sp] : 0;             // the pass count (an infinite loop's counts for ever)
    s[k++] = left;
    s[k++] = ram[0xF5C0 + 2 * sp]; s[k++] = ram[0xF5C1 + 2 * sp];
    s[k++] = ram[0xF200 + x]; s[k++] = ram[0xF201 + x]; s[k++] = ram[0xF361 + x];
  }
  s[k++] = ram[0x46]; s[k++] = ram[0x47]; s[k++] = ram[0x53]; s[k++] = ram[0x86];
}

static void finderInit(Finder* f) {
  memset(f, 0, sizeof *f);
  f->loopS = f->loopE = -1;
  f->head = (long*)malloc(sizeof(long) << HBITS);
  if (!f->head) { fprintf(stderr, "sndrender: out of memory\n"); exit(3); }
  for (long i = 0; i < (1L << HBITS); i++) f->head[i] = -1;
}
static void finderFree(Finder* f) { free(f->snaps); free(f->snapAt); free(f->next); free(f->head); }
static void finderSee(Finder* f, const uint8_t* s, long t) {
  if (f->haveLast && !memcmp(s, f->last, SNAP)) return;
  memcpy(f->last, s, SNAP); f->haveLast = 1;
  if (f->loopS >= 0) return;
  uint32_t h = fnv(s, SNAP, 2166136261u) & ((1u << HBITS) - 1);
  for (long i = f->head[h]; i >= 0; i = f->next[i])
    if (!memcmp(f->snaps + (size_t)i * SNAP, s, SNAP)) { f->loopS = f->snapAt[i]; f->loopE = t; return; }
  if (f->nsnap == f->capsnap) {
    long cap = f->capsnap ? f->capsnap * 2 : 4096;
    f->snaps = (uint8_t*)realloc(f->snaps, (size_t)cap * SNAP);
    f->snapAt = (long*)realloc(f->snapAt, (size_t)cap * sizeof *f->snapAt);
    f->next = (long*)realloc(f->next, (size_t)cap * sizeof *f->next);
    if (!f->snaps || !f->snapAt || !f->next) { fprintf(stderr, "sndrender: out of memory\n"); exit(3); }
    f->capsnap = cap;
  }
  memcpy(f->snaps + (size_t)f->nsnap * SNAP, s, SNAP);
  f->snapAt[f->nsnap] = t;
  f->next[f->nsnap] = f->head[h];
  f->head[h] = f->nsnap++;
}

// Each key-on (KON, register $4C) while capturing: the SRCN of every voice it keys (register $x4, written before it).
static void onDsp(void* c, char kind, uint8_t reg, uint8_t val, uint64_t io) {
  Render* r = (Render*)c;
  (void)io;
  if (kind != 'D' || reg != 0x4C || !r->on) return;
  for (int v = 0; v < 8; v++) if (val & (1 << v)) r->keyed[r->m->mx.regs[v * 16 + 4]] = 1;
}

static void onIns(void* c, uint16_t pc, uint64_t cyc) {
  Render* r = (Render*)c;
  if (pc != 0x0299 || !r->on) return;
  const uint8_t* ram = r->m->d.m;
  uint64_t at = (cyc + 31) / 32;
  long t = at > r->start ? (long)(at - r->start) : 0;
  if (r->sfx) {
    if (ram[0x86]) r->everPlayed = 1;
    else if (r->everPlayed && r->endAt < 0 && !SndMachineVoicesSounding(r->m)) r->endAt = t;
  } else {
    if (ram[0x53]) r->everPlayed = 1;
    else if (r->everPlayed && r->endAt < 0) r->endAt = t;
  }
  uint8_t s[SNAP];
  snapshot(ram, s);
  finderSee(&r->full, s, t);
  for (int k = 0; k < 16; k++) memset(s + 64 + k * 7 + 4, 0, 3);   // the position only: no octave, instrument, transpose
  finderSee(&r->pos, s, t);
  for (int k = 0; k < 8; k++) {                       // each music track's own position
    uint8_t ts[SNAP];
    memset(ts, 0, sizeof ts);
    memcpy(ts + 2 * k, s + 2 * k, 2);
    memcpy(ts + 32 + 2 * k, s + 32 + 2 * k, 2);
    memcpy(ts + 64 + 7 * k, s + 64 + 7 * k, 4);
    finderSee(&r->track[k], ts, t);
  }
}

static int writePcm(const char* path, const int16_t* pcm, long n) {
  if (!strcmp(path, "-")) return 1;
  FILE* f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "sndrender: cannot write %s\n", path); return 0; }
  for (long i = 0; i < n * 2; i++) { uint16_t v = (uint16_t)pcm[i]; fputc(v & 0xFF, f); fputc(v >> 8, f); }   // little-endian
  fclose(f);
  return 1;
}

// The mixer's own BRR decoder over one instrument's block (see the header).
static int brrRender(const uint8_t* rom, long romLen, int instr, const char* out) {
  CtAudio* a = CtAudioInit(rom, romLen);
  int len = 0;
  const uint8_t* blk = a ? CtAudioSampleBlock(a, instr, &len) : NULL;
  if (!blk || len <= 0 || len % 9) { printf("sndrender: brr %02X FAIL (no block)\n", instr); CtAudioFree(a); return 1; }
  static uint8_t aram[0x10000];
  const uint16_t at = 0x1000, dir = 0x0200;
  if (at + len > 0x10000) { printf("sndrender: brr %02X FAIL (too long)\n", instr); CtAudioFree(a); return 1; }
  memset(aram, 0, sizeof aram);
  memcpy(aram + at, blk, (size_t)len);
  aram[dir] = at & 0xFF; aram[dir + 1] = at >> 8; aram[dir + 2] = at & 0xFF; aram[dir + 3] = at >> 8;
  CtMixer mx;
  ct_mixer_init(&mx, aram);
  ct_mixer_flags(&mx, 0x20);                          // out of reset, not muted, echo writes off
  ct_mixer_directory(&mx, dir >> 8);
  ct_mixer_voice_key_on(&mx, 0, 0, 0x1000, 0x00, 0x00, 0x7F);
  long want = (long)len / 9 * 16, got = 0;
  int16_t* pcm = (int16_t*)calloc((size_t)want * 2, sizeof *pcm);
  if (!pcm) { fprintf(stderr, "sndrender: out of memory\n"); exit(3); }
  int16_t tmp[2];
  for (long guard = 0; got < want && guard < want * 8 + 64; guard++) {
    uint8_t pos = mx.v[0].ringPos;
    ct_mixer_render(&mx, tmp, 1);
    if (mx.v[0].ringPos != pos)
      for (int i = 0; i < 4 && got < want; i++, got++) pcm[got * 2] = pcm[got * 2 + 1] = mx.v[0].ring[pos + i];
  }
  uint32_t h = 2166136261u;
  for (long i = 0; i < got; i++) { uint16_t v = (uint16_t)pcm[i * 2]; uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; h = fnv(b, 2, h); }
  int ok = got == want && writePcm(out, pcm, got);
  printf("sndrender: brr %02X len %d samples %ld fnv %08X%s\n", instr, len, got, h, ok ? "" : "  <-- FAIL");
  free(pcm);
  CtAudioFree(a);
  return ok ? 0 : 1;
}

static void renderFree(Render* r) {
  free(r->pcm); finderFree(&r->full); finderFree(&r->pos);
  for (int k = 0; k < 8; k++) finderFree(&r->track[k]);
}

int CtSndRender(const uint8_t* rom, long romLen, int argc, char** argv) {
  if (argc >= 2 && !strcmp(argv[0], "brr")) return brrRender(rom, romLen, (int)strtol(argv[1], NULL, 16), argc >= 3 ? argv[2] : "-");
  int sfx = argc >= 1 && !strcmp(argv[0], "sfx");
  int need = sfx ? 5 : 4;
  if (argc < need || (!sfx && strcmp(argv[0], "song"))) {
    fprintf(stderr, "usage: --sndrender song <id> <frames> <out|-> [mask] | sfx <set> <id> <frames> <out|-> [mask] | brr <instr> [out|-]\n");
    return 2;
  }
  int set = sfx ? (int)strtol(argv[1], NULL, 16) : 0;
  if (set != 0 && set != 1) { printf("sndrender: sfx set %d FAIL (set 0 is the field's effects, 1 the battle's)\n", set); return 2; }
  int id = (int)strtol(argv[sfx ? 2 : 1], NULL, 16);
  long frames = strtol(argv[sfx ? 3 : 2], NULL, 0);
  const char* out = argv[sfx ? 4 : 3];
  int mask = argc > need ? (int)(strtol(argv[need], NULL, 16) & 0xFF) : 0xFF;

  CtAudio* a = CtAudioInit(rom, romLen);
  SndMachine* m = a ? CtAudioMachine(a) : NULL;
  if (!a || !CtAudioReady(a) || !m) {
    printf("sndrender: FAIL (%s)\n", !a || !CtAudioReady(a) ? "the driver upload" : "needs CT_AUDIO_SPC=machine");
    CtAudioFree(a);
    return 3;
  }
  CtAudioSetRuleOff(a, 1);
  static uint8_t wram[0x20000];
  memset(wram, 0, sizeof wram);
  CtAudioSeedMailboxPage(a, wram);
  static Render r;
  memset(&r, 0, sizeof r);
  r.m = m; r.sfx = sfx; r.endAt = -1;
  finderInit(&r.full); finderInit(&r.pos);
  for (int k = 0; k < 8; k++) finderInit(&r.track[k]);
  SndMachineSetSink(m, sink, &r);
  SndMachineSetTrace(m, onIns, &r);
  SndMachineSetObserver(m, onDsp, &r);
  m->mx.voiceMask = (uint8_t)mask;
  for (int f = 0; f < 30; f++) CtAudioFrame(a);       // (after a boot the driver still waits out its echo buffer)

  int rc;
  if (!sfx) rc = CtAudioLoadSong(a, wram, id, 0x10);
  else {
    rc = 1;
    if (set) {                                        // the battle's effect set: command $70 through the service
      wram[0x1E00] = 0x70; wram[0x1E01] = 0x01; wram[0x1E02] = wram[0x1E03] = 0;
      rc = CtAudioService(a, wram);
      for (int f = 0; f < 10; f++) CtAudioFrame(a);
    }
    if (rc == 1) {                                    // the effect on voices 7-6, pan $80, through the service too
      wram[0x1E00] = 0x18; wram[0x1E01] = (uint8_t)id; wram[0x1E02] = 0x80; wram[0x1E03] = 0;
      rc = CtAudioService(a, wram);
    }
  }
  if (rc != 1) { printf("sndrender: %s %02X FAIL (rc=%d)\n", sfx ? "sfx" : "song", id, rc); renderFree(&r); CtAudioFree(a); return 1; }
  SndMachineSync(m);
  r.start = m->mixed;
  r.on = 1;
  for (long f = 0; f < frames; f++) CtAudioFrame(a);
  SndMachineSync(m);
  r.on = 0;

  uint32_t h = 2166136261u;
  long peak = 0;
  for (long i = 0; i < r.n * 2; i++) {
    uint16_t v = (uint16_t)r.pcm[i]; uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    h = fnv(b, 2, h);
    long av = r.pcm[i] < 0 ? -(long)r.pcm[i] : r.pcm[i];
    if (av > peak) peak = av;
  }
  const uint8_t* ram = m->d.m;
  int ok = writePcm(out, r.pcm, r.n);
  char keyed[256 * 3 + 2] = "-";
  for (int k = 0, n = 0; k < 256; k++) if (r.keyed[k]) n += sprintf(keyed + n, n ? ",%02X" : "%02X", k);
  if (sfx) printf("sndrender: sfx set %d id %02X mask %02X samples %ld peak %ld end %ld loop %ld %ld position %ld %ld keyed %s fnv %08X\n",
                  set, id, mask, r.n, peak, r.endAt, r.full.loopS, r.full.loopE, r.pos.loopS, r.pos.loopE, keyed, h);
  else {
    char tracks[8 * 24] = "";
    for (int k = 0, n = 0; k < 8; k++)
      n += sprintf(tracks + n, "%s%ld:%ld", k ? "," : "", r.track[k].loopS, r.track[k].loopE);
    printf("sndrender: song %02X mask %02X samples %ld peak %ld tempo %02X end %ld loop %ld %ld position %ld %ld tracks %s keyed %s fnv %08X\n",
           id, mask, r.n, peak, ram[0x47], r.endAt, r.full.loopS, r.full.loopE, r.pos.loopS, r.pos.loopE, tracks, keyed, h);
  }
  renderFree(&r);
  CtAudioFree(a);
  return ok ? 0 : 1;
}
