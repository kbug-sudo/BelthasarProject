// cutscene.c - see cutscene.h. The list, the moments' edges, and one clip's decoding (pl_mpeg) into a picture the host
// draws and sound it queues on the game's device. Nothing here reads or writes the game beyond the two bytes CtCutsceneDue
// is handed.
#include "cutscene.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CT_HAVE_PL_MPEG
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
#endif

enum { KIND_POWER_ON = 0, KIND_AT = 1 };
#define CT_CUT_CONDS 4
typedef struct {
  char id[16];
  char file[256];
  int kind, loc, story;
  int ncond, condAt[CT_CUT_CONDS], condMask[CT_CUT_CONDS], condVal[CT_CUT_CONDS];   // more bytes the moment needs (flat)
  int onChange;                // `story_change`: only on the frame the story byte becomes `story` inside the location
  int done;                    // the moment came (played or not): not again until the story byte is 00
} Clip;
static Clip g_clips[CT_CUTSCENE_MAX];
static int g_n;
static int g_prevLoc = -1, g_prevStory = -1;      // the frame before's
static char g_dir[512];
static char g_why[256] = "no clip list loaded";

int CtCutsceneCount(void) { return g_n; }
const char* CtCutsceneId(int i) { return i >= 0 && i < g_n ? g_clips[i].id : "?"; }
void CtCutsceneMoment(int i, int* kind, int* loc, int* story) {
  if (i < 0 || i >= g_n) { *kind = -1; *loc = *story = 0; return; }
  *kind = g_clips[i].kind; *loc = g_clips[i].loc; *story = g_clips[i].story;
}

int CtCutsceneLoad(const char* dir, char* err, int errN) {
  g_n = 0;
  if (!dir || !*dir) { const char* e = getenv("CT_CUTSCENE_DIR"); dir = e && *e ? e : "cutscenes"; }
  snprintf(g_dir, sizeof g_dir, "%s", dir);
  char path[600]; snprintf(path, sizeof path, "%s/cutscenes.txt", g_dir);
  FILE* f = fopen(path, "r");
  if (!f) { snprintf(g_why, sizeof g_why, "no clip list (%s)", path); if (err) snprintf(err, errN, "%s", g_why); return 0; }
  char line[600]; int ln = 0, ok = 0;
  while (fgets(line, sizeof line, f)) {
    ln++;
    char* p = line; while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#' || *p == '\r' || *p == '\n') continue;
    if (!ok) {                                       // the first line names the format
      if (strncmp(p, "ct-cutscenes 1", 14) || (p[14] && p[14] != '\r' && p[14] != '\n' && p[14] != ' ')) {
        snprintf(g_why, sizeof g_why, "%s is not a clip list of version 1 (line 1)", path); break; }
      ok = 1; continue;
    }
    if (g_n >= CT_CUTSCENE_MAX) { snprintf(g_why, sizeof g_why, "%s: more than %d clips", path, CT_CUTSCENE_MAX); break; }
    Clip c; memset(&c, 0, sizeof c);
    char kind[16] = ""; unsigned loc = 0, story = 0; int used = 0;
    int n = sscanf(p, "%15s %255s %15s %x %x%n", c.id, c.file, kind, &loc, &story, &used);
    if (n >= 3 && !strcmp(kind, "power_on")) c.kind = KIND_POWER_ON;
    else if (n == 5 && !strcmp(kind, "at") && loc < 0x200 && story < 0x100) {
      c.kind = KIND_AT; c.loc = (int)loc; c.story = (int)story;
      // ...and `ADDR=VAL` or `ADDR&MASK=VAL` (a byte of $7E:0000-$7F:FFFF, hex; a mask for a byte of event flags, whose
      // other bits another event sets) for each more condition the moment needs; `story_change` for a moment that is the
      // story byte changing inside the location (its value stays after the scene: a later visit is not the moment)
      const char* q = p + used; int bad = 0;
      for (;;) {
        while (*q == ' ' || *q == '\t') q++;
        if (!*q || *q == '\r' || *q == '\n' || *q == '#') break;
        if (!strncmp(q, "story_change", 12) && (!q[12] || q[12] == ' ' || q[12] == '\t' || q[12] == '\r' || q[12] == '\n')) {
          c.onChange = 1; q += 12; continue; }
        unsigned a = 0, m = 0xFF, v = 0; int k = 0;
        if (sscanf(q, "%x&%x=%x%n", &a, &m, &v, &k) != 3) { m = 0xFF; k = 0; if (sscanf(q, "%x=%x%n", &a, &v, &k) != 2) k = 0; }
        if (!k || a < 0x7E0000 || a > 0x7FFFFF || m > 0xFF || (v & ~m) || c.ncond >= CT_CUT_CONDS) { bad = 1; break; }
        c.condAt[c.ncond] = (int)(a - 0x7E0000); c.condMask[c.ncond] = (int)m; c.condVal[c.ncond++] = (int)v; q += k;
      }
      if (bad) { fprintf(stderr, "[cutscene] %s line %d: a condition not understood: %s", path, ln, p); continue; }
    }
    else { fprintf(stderr, "[cutscene] %s line %d not understood: %s", path, ln, p); continue; }
    g_clips[g_n++] = c;
  }
  fclose(f);
  if (ok && g_n) snprintf(g_why, sizeof g_why, "%d clip(s) in %s", g_n, g_dir);
  else if (ok) snprintf(g_why, sizeof g_why, "the list %s names no clip", path);
#ifndef CT_HAVE_PL_MPEG
  if (g_n) snprintf(g_why, sizeof g_why, "built without the video decoder (tools/get_pl_mpeg.py)");
#endif
  if (err) snprintf(err, errN, "%s", g_why);
  return g_n;
}

int CtCutsceneStatus(char* why, int whyN) {
  if (why) snprintf(why, whyN, "%s", g_why);
#ifdef CT_HAVE_PL_MPEG
  return g_n > 0;
#else
  return 0;
#endif
}

int CtCutsceneDue(const uint8_t* w, int powerOnFrame, int* due, int maxDue) {
  int loc = (w[0x0100] | (w[0x0101] << 8)) & 0x1FF, story = w[0x10000], n = 0;
  for (int i = 0; i < g_n; i++) {
    Clip* c = &g_clips[i];
    // re-armed at story 00 -- the title, the attract and a new game, where every reload passes -- and only there: the byte
    // is not always the story in between (the time gate's warp holds 40 in it on the cartridge, ~270 frames a trip)
    if (c->kind == KIND_AT && story == 0 && c->story != 0) c->done = 0;
    int now = c->kind == KIND_POWER_ON ? powerOnFrame : (loc == c->loc && story == c->story);
    for (int k = 0; k < c->ncond && now; k++) now = (w[c->condAt[k]] & c->condMask[k]) == c->condVal[k];
    if (c->onChange && now) now = loc == g_prevLoc && story != g_prevStory;   // (a load changes the location too)
    if (now && !c->done) { c->done = 1; if (n < maxDue) due[n++] = i; }
  }
  g_prevLoc = loc; g_prevStory = story;
  return n;
}

int CtCutsceneSelftest(const char* seq) {
  int k = 0;
  static uint8_t w[0x20000];
  for (const char* q = seq; q && *q; k++) {
    char tok[64]; size_t len = strcspn(q, ",");
    snprintf(tok, sizeof tok, "%.*s", (int)(len < sizeof tok ? len : sizeof tok - 1), q);
    unsigned loc = 0, story = 0, a = 0, v = 0;
    int pon = !strcmp(tok, "on");
    if (!pon && sscanf(tok, "%x:%x", &loc, &story) != 2) break;
    if (!pon) { w[0x0100] = (uint8_t)loc; w[0x0101] = (uint8_t)(loc >> 8); w[0x10000] = (uint8_t)story; }
    // (`loc:story:ADDR=VAL`: a byte of work RAM set from this frame on)
    char* c3 = strchr(tok, ':') ? strchr(strchr(tok, ':') + 1, ':') : NULL;
    if (c3 && sscanf(c3 + 1, "%x=%x", &a, &v) == 2 && a >= 0x7E0000 && a <= 0x7FFFFF) w[a - 0x7E0000] = (uint8_t)v;
    int due[CT_CUTSCENE_MAX], nd = CtCutsceneDue(w, pon, due, CT_CUTSCENE_MAX);
    if (pon) printf("cutscene-selftest: %d power-on:", k);
    else printf("cutscene-selftest: %d loc %03X story %02X%s:", k, loc, story, c3 ? " (a byte set)" : "");
    if (!nd) printf(" -");
    for (int i = 0; i < nd; i++) printf(" %s", g_clips[due[i]].id);
    printf("\n");
    q = q[len] ? q + len + 1 : NULL;
  }
  return k;
}

// ---- one clip -------------------------------------------------------------------------------------------------------
struct CtClip {
#ifdef CT_HAVE_PL_MPEG
  plm_t* plm;
#endif
  SDL_AudioDeviceID dev;
  SDL_AudioStream* conv;         // the clip's float stereo at its rate -> the device's S16 stereo at its rate
  int gain;
  uint8_t* rgb; int w, h, fresh;
  long pictures, samples;
};

#ifdef CT_HAVE_PL_MPEG
static void onVideo(plm_t* plm, plm_frame_t* fr, void* user) {
  CtClip* c = (CtClip*)user; (void)plm;
  if ((int)fr->width != c->w || (int)fr->height != c->h || !c->rgb) {
    free(c->rgb);
    c->w = (int)fr->width; c->h = (int)fr->height;
    c->rgb = (uint8_t*)malloc((size_t)c->w * c->h * 3);
    if (!c->rgb) { c->w = c->h = 0; return; }
  }
  plm_frame_to_rgb(fr, c->rgb, c->w * 3);
  c->fresh = 1; c->pictures++;
}
static void onAudio(plm_t* plm, plm_samples_t* s, void* user) {
  CtClip* c = (CtClip*)user; (void)plm;
  if (!c->dev || !c->conv) return;
  float buf[PLM_AUDIO_SAMPLES_PER_FRAME * 2];
  float g = (float)c->gain / 100.0f;
  for (unsigned i = 0; i < s->count * 2; i++) buf[i] = s->interleaved[i] * g;
  if (SDL_AudioStreamPut(c->conv, buf, (int)(s->count * 2 * sizeof(float))) != 0) return;
  int16_t out[4096];
  int got;
  while ((got = SDL_AudioStreamGet(c->conv, out, (int)sizeof out)) > 0) SDL_QueueAudio(c->dev, out, (Uint32)got);
  c->samples += s->count;
}
#endif

CtClip* CtClipOpen(int i, SDL_AudioDeviceID dev, int devHz, int gain, char* err, int errN) {
  if (i < 0 || i >= g_n) { snprintf(err, errN, "no clip %d", i); return NULL; }
#ifndef CT_HAVE_PL_MPEG
  (void)dev; (void)devHz; (void)gain;
  snprintf(err, errN, "built without the video decoder (tools/get_pl_mpeg.py)");
  return NULL;
#else
  const char* f = g_clips[i].file;                  // (relative to the list's folder, or a whole path)
  char path[800];
  if (f[0] == '/' || f[0] == '\\' || (f[0] && f[1] == ':')) snprintf(path, sizeof path, "%s", f);
  else snprintf(path, sizeof path, "%s/%s", g_dir, f);
  plm_t* plm = plm_create_with_filename(path);
  if (!plm) { snprintf(err, errN, "cannot open %s", path); return NULL; }
  if (!plm_probe(plm, 5000 * 1024) || plm_get_num_video_streams(plm) < 1) {
    snprintf(err, errN, "%s holds no MPEG-1 video", path); plm_destroy(plm); return NULL;
  }
  CtClip* c = (CtClip*)calloc(1, sizeof *c);
  if (!c) { plm_destroy(plm); snprintf(err, errN, "out of memory"); return NULL; }
  c->plm = plm; c->gain = gain < 0 ? 0 : gain > 100 ? 100 : gain;
  plm_set_video_decode_callback(plm, onVideo, c);
  int hz = plm_get_samplerate(plm);
  if (dev && devHz > 0 && plm_get_num_audio_streams(plm) > 0 && hz > 0) {
    c->conv = SDL_NewAudioStream(AUDIO_F32SYS, 2, hz, AUDIO_S16SYS, 2, devHz);
    if (c->conv) c->dev = dev;
  }
  plm_set_audio_enabled(plm, c->dev != 0);
  if (c->dev) {
    plm_set_audio_decode_callback(plm, onAudio, c);
    plm_set_audio_lead_time(plm, 0.15);             // the sound queued that far ahead of the picture
  }
  plm_set_loop(plm, 0);
  return c;
#endif
}

int CtClipAdvance(CtClip* c, double seconds) {
  if (!c) return 0;
#ifdef CT_HAVE_PL_MPEG
  c->fresh = 0;
  plm_decode(c->plm, seconds);
  return c->fresh;
#else
  (void)seconds; return 0;
#endif
}
const uint8_t* CtClipPicture(CtClip* c, int* w, int* h) { *w = c ? c->w : 0; *h = c ? c->h : 0; return c ? c->rgb : NULL; }
int CtClipEnded(CtClip* c) {
#ifdef CT_HAVE_PL_MPEG
  return !c || plm_has_ended(c->plm);
#else
  (void)c; return 1;
#endif
}
long CtClipPictures(CtClip* c) { return c ? c->pictures : 0; }
long CtClipSamples(CtClip* c) { return c ? c->samples : 0; }
void CtClipClose(CtClip* c) {
  if (!c) return;
#ifdef CT_HAVE_PL_MPEG
  plm_destroy(c->plm);
#endif
  if (c->conv) SDL_FreeAudioStream(c->conv);
  free(c->rgb);
  free(c);
}
