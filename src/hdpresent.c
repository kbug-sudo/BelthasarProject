// hdpresent.c - the HD presenter (hdpresent.h): the HD art of every source the pictures are drawn from (hdsrc.h; a pack,
// else the automatic pass), the map's HD pictures kept as index maps beside the engine's own, the field drawn from them at
// 4x the way fieldnative.c draws it at k x, and any other picture redrawn pixel by pixel from its records.
#include "hdpresent.h"
#include "hdsrc.h"
#include "assets.h"
#include "fieldnative.h"                           // CtFieldNativeFx: what the field composes over the map
#include <math.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#define BL1 1
#define BL2 2
#define BL3 4
#define HDK CT_HD_K
#define HDN (8 * HDK)                              // an HD chip's side (32)
#define HDA (HDN * HDN)

static long g_made, g_rebuilds, g_cellsDrawn, g_picFrames;
static uint64_t g_picHash = 1469598103934665603ull; static long g_hashed;   // (CT_HD_STATS: every HD picture, hashed)
// COVERAGE (CT_HD_STATS): per mode the host names (CtHdCoverMode), the picture's non-black pixels and how many of them
// were drawn from HD art (the map's cells, a record of their tile, sprite cell or glyph)
static long long g_cov[CT_HD_NMODES][2]; static int g_covMode;
// ...and the time the pictures took (CT_HD_STATS: ms a picture, the frames between included)
static double g_rasterMs; static long g_rasters;
static uint8_t g_covMask[CT_MOT_W * 224]; static int g_covMaskW;   // the last picture's: 0 black, 1 from HD art, 2 not
const uint8_t* CtHdCoverMask(int* w) { if (w) *w = g_covMaskW; return g_covMaskW ? g_covMask : NULL; }
static double nowMs(void) { struct timespec ts; timespec_get(&ts, TIME_UTC); return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6; }
void CtHdCoverMode(int m) { g_covMode = m >= 0 && m < CT_HD_NMODES ? m : CT_HD_NMODES - 1; }
static int g_nearest;                              // CtHdNearest: every source its own pixels 4x4 (the check's art)

// ---- the automatic pass: Scale2x (EPX) twice ---------------------------------------------------------------------
static void scale2x(const uint8_t* in, int w, int h, uint8_t* out) {
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      uint8_t P = in[y * w + x];
      uint8_t A = in[(y > 0 ? y - 1 : 0) * w + x], D = in[(y < h - 1 ? y + 1 : h - 1) * w + x];
      uint8_t C = in[y * w + (x > 0 ? x - 1 : 0)], B = in[y * w + (x < w - 1 ? x + 1 : w - 1)];
      uint8_t* o = out + (size_t)(2 * y) * (2 * w) + 2 * x;
      o[0] = (C == A && C != D && A != B) ? A : P;
      o[1] = (A == B && A != C && B != D) ? B : P;
      o[2 * w] = (D == C && D != B && C != A) ? C : P;
      o[2 * w + 1] = (B == D && B != A && D != C) ? D : P;
    }
}
void CtHdUpscale(const uint8_t* in, int w, int h, uint8_t* out) {
  if (g_nearest) {
    for (int y = 0; y < h * HDK; y++) for (int x = 0; x < w * HDK; x++) out[(size_t)y * w * HDK + x] = in[(y / HDK) * w + x / HDK];
    return;
  }
  uint8_t mid[(2 * CT_SRC_MAXWH) * (2 * CT_SRC_MAXWH)];
  scale2x(in, w, h, mid);
  scale2x(mid, 2 * w, 2 * h, out);
}
void CtHdUpscaleChip(const uint8_t in[64], uint8_t out[HDA]) { CtHdUpscale(in, 8, 8, out); }

// ---- the art: per source (hdsrc.h's pool), from the pack or made on the spot -----------------------------------------
static uint8_t** g_art; static uint8_t* g_kinds; static uint8_t* g_from; static size_t g_artN;   // index = source id
enum { FROM_NONE = 0, FROM_PACK = 1, FROM_PASS = 2, FROM_SPOT = 3 };
static int artRoom(int src) {
  if ((size_t)src < g_artN) return 1;
  size_t n = g_artN ? g_artN : 1 << 16;
  while (n <= (size_t)src) n *= 2;
  uint8_t** na = (uint8_t**)realloc(g_art, n * sizeof *na);
  if (!na) return 0;
  g_art = na;
  uint8_t* nk = (uint8_t*)realloc(g_kinds, n); if (!nk) return 0; g_kinds = nk;
  uint8_t* nf = (uint8_t*)realloc(g_from, n); if (!nf) return 0; g_from = nf;
  memset(g_art + g_artN, 0, (n - g_artN) * sizeof *na); memset(g_kinds + g_artN, 0, n - g_artN); memset(g_from + g_artN, 0, n - g_artN);
  g_artN = n;
  return 1;
}
static void clearArt(void) {
  for (size_t i = 0; i < g_artN; i++) free(g_art[i]);
  if (g_artN) { memset(g_art, 0, g_artN * sizeof *g_art); memset(g_kinds, 0, g_artN); memset(g_from, 0, g_artN); }
}
static uint8_t* makeArt(int src, int from) {
  CtSrcInfo si;
  if (!CtSrcInfoOf(src, &si) || !artRoom(src)) return NULL;
  if (!g_art[src]) {
    g_art[src] = (uint8_t*)malloc((size_t)si.w * si.h * HDK * HDK);
    if (!g_art[src]) return NULL;
    CtHdUpscale(si.idx, si.w, si.h, g_art[src]);
    g_from[src] = (uint8_t)from;
    if (from == FROM_SPOT) g_made++;
  }
  return g_art[src];
}
const uint8_t* CtHdArt(int src) {
  if (src < 1) return NULL;
  if ((size_t)src < g_artN && g_art[src]) return g_art[src];
  return makeArt(src, FROM_SPOT);
}
void CtHdArtKind(int src, int kinds) { if (src >= 1 && artRoom(src)) g_kinds[src] |= (uint8_t)kinds; }
const uint8_t* CtHdChip(const uint8_t chip[64]) {
  static uint8_t fallback[HDA];
  int id = CtSrcOf(chip, 8, 8);
  const uint8_t* a = id ? CtHdArt(id) : NULL;
  if (a) return a;
  CtHdUpscale(chip, 8, 8, fallback);
  return fallback;
}

// ---- the pack ("CTHD": v1 8x8 entries; v2 w, h, kinds per entry -- tools/cthd.py writes it too) -----------------------
static char g_dir[1024];
const char* CtHdDir(void) { return g_dir; }
static void forgetPics(void);
static void recordAtExit(void);
void CtHdNearest(int on) { g_nearest = on != 0; clearArt(); forgetPics(); }
static uint32_t rd32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int readPack(const char* p, int from, long* count) {
  FILE* f = fopen(p, "rb");
  if (!f) return 0;
  uint8_t h[12];
  int ok = fread(h, 1, 12, f) == 12 && !memcmp(h, "CTHD", 4);
  uint32_t ver = ok ? rd32(h + 4) : 0, n = ok ? rd32(h + 8) : 0;
  ok = ok && (ver == 1 || ver == 2);
  static uint8_t src[CT_SRC_MAXWH * CT_SRC_MAXWH];
  for (uint32_t j = 0; ok && j < n; j++) {
    uint8_t wh[3] = { 8, 8, 1 };
    if (ver == 2 && fread(wh, 1, 3, f) != 3) { ok = 0; break; }
    int w = wh[0], hh = wh[1];
    if (w < 1 || hh < 1 || w > CT_SRC_MAXWH || hh > CT_SRC_MAXWH || fread(src, 1, (size_t)w * hh, f) != (size_t)w * hh) { ok = 0; break; }
    int id = CtSrcOf(src, w, hh);
    size_t na = (size_t)w * hh * HDK * HDK;
    if (!id || !artRoom(id)) { ok = 0; break; }
    if (!g_art[id] && !(g_art[id] = (uint8_t*)malloc(na))) { ok = 0; break; }
    if (fread(g_art[id], 1, na, f) != na) { ok = 0; break; }
    // the art names only indices its source's palette has: 16 colours (base + v, a map chip's row of the 128) unless the
    // source itself uses more -- a corrupt entry is no pack (the presenters index a palette with them)
    int lim = 16;
    for (int k = 0; k < w * hh; k++) if (src[k] >= lim) lim = 256;
    for (size_t k = 0; k < na && ok; k++) if (g_art[id][k] >= lim) ok = 0;
    if (!ok) break;
    g_from[id] = (uint8_t)from; g_kinds[id] |= wh[2];
  }
  if (ok && fgetc(f) != EOF) ok = 0;               // (bytes past its last entry: not a pack this reader knows)
  fclose(f);
  if (count) *count = ok ? (long)n : 0;
  return ok;
}
int CtHdOpen(const char* dir) {
  if (!dir || !*dir) { dir = getenv("CT_HD_DIR"); if (!dir || !*dir) dir = "hd"; }
  snprintf(g_dir, sizeof g_dir, "%s", dir);
  clearArt(); forgetPics();
  g_ctSrcOn = 1;                                   // (the pictures' records: hdsrc.h)
  { static int once; if (!once) { once = 1; atexit(recordAtExit); } }
  g_ctSrcCheck = getenv("CT_SRCCHECK") != NULL;
  if (g_nearest) return 0;                         // (the check's art is made, never read)
  char p[1100]; snprintf(p, sizeof p, "%s/chips4x.pack", g_dir);
  long n = 0;
  if (!readPack(p, FROM_PACK, &n)) {
    clearArt();
    fprintf(stderr, "[hd] %s: no pack (version 1 or 2): the automatic pass draws every source\n", p);
    return 0;
  }
  fprintf(stderr, "[hd] %s: %ld sources\n", p, n);
  return 1;
}
static int writePack(const char* p, int which) {   // which: FROM_* of the entries to write, 0 = every entry with art
  FILE* f = fopen(p, "wb");
  if (!f) return 0;
  uint32_t n = 0;
  for (size_t i = 1; i < g_artN; i++) if (g_art[i] && (!which || g_from[i] == which)) n++;
  uint8_t h[12] = { 'C', 'T', 'H', 'D', 2, 0, 0, 0, (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24) };
  if (fwrite(h, 1, 12, f) != 12) { fclose(f); return 0; }
  for (size_t i = 1; i < g_artN; i++) {
    if (!g_art[i] || (which && g_from[i] != which)) continue;
    CtSrcInfo si; CtSrcInfoOf((int)i, &si);
    uint8_t wh[3] = { (uint8_t)si.w, (uint8_t)si.h, (uint8_t)(g_kinds[i] | (g_from[i] == FROM_SPOT ? CT_HD_PLAYED : 0)) };
    size_t na = (size_t)si.w * si.h;
    if (fwrite(wh, 1, 3, f) != 3 || fwrite(si.idx, 1, na, f) != na || fwrite(g_art[i], 1, na * HDK * HDK, f) != na * HDK * HDK) {
      fclose(f); return 0; }
  }
  return fclose(f) == 0;
}
// CT_HD_RECORD=<file>: at the exit, every source the pack lacked (made on the spot), as a pack (tools/cthd.py merge)
static void recordAtExit(void) {
  const char* r = getenv("CT_HD_RECORD");
  if (r && *r) writePack(r, FROM_SPOT);
  if (g_ctSrcCheck) {
    long long n, bad; CtSrcCheckStats(&n, &bad);
    printf("srccheck: %lld texels recorded and checked, %lld name another index\n", n, bad);
  }
  if (getenv("CT_HD_STATS")) {
    static const char* names[CT_HD_NMODES] = { "field", "battle", "menu", "overworld", "other" };
    char line[512]; int n = snprintf(line, sizeof line, "hd-coverage:");
    for (int m = 0; m < CT_HD_NMODES; m++)
      if (g_cov[m][0]) n += snprintf(line + n, sizeof line - (size_t)n, " %s %.1f%% of %lld px%s", names[m],
                                     100.0 * (double)g_cov[m][1] / (double)g_cov[m][0], g_cov[m][0], m < CT_HD_NMODES - 1 ? "," : "");
    if (n > 0 && line[n - 1] == ',') line[n - 1] = 0;
    printf("%s\n", line);
    if (g_rasters) printf("hd-time: %.2f ms a picture (%ld pictures)\n", g_rasterMs / g_rasters, g_rasters);
    long long fr, tg, un; CtSrcStats(&fr, &tg, &un);
    printf("hd-stats: %lld frames recorded, %ld sources (%ld made on the spot), %lld pixels with a source, %lld without, "
           "%ld map pictures built, %ld cells drawn, %ld pictures redrawn from their records\n",
           fr, (long)CtSrcCount(), g_made, tg, un, g_rebuilds, g_cellsDrawn, g_picFrames);
    printf("hd-pictures: %ld pictures drawn, fnv %016llX\n", g_hashed, (unsigned long long)g_picHash);
    fflush(stdout);
  }
}
void CtHdHashPicture(const uint8_t* p, size_t n) {
  if (!getenv("CT_HD_STATS")) return;
  uint64_t h = g_picHash;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) { uint64_t v; memcpy(&v, p + i, 8); h ^= v; h *= 1099511628211ull; }
  for (; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
  g_picHash = h; g_hashed++;
}
void CtHdStats(long* chips, long* missing, long* rebuilds, long* cells) {
  if (chips) *chips = CtSrcCount(); if (missing) *missing = g_made; if (rebuilds) *rebuilds = g_rebuilds; if (cells) *cells = g_cellsDrawn;
}

// ---- the map's HD pictures: index maps (0 = transparent, else the palette index), redrawn cell by cell ---------------
typedef struct {
  unsigned scene, gen;                             // the scene and the cells they were drawn from (another scene: anew)
  uint8_t* idx[6]; int w[6], h[6];                 // HD pictures, w = cells across x 32
  CtSceneCell* cells[6]; int cw[6], ch[6];
  uint8_t* chips[6]; int nchips[6];                // the chips' contents when they were drawn
} HdPics;
static HdPics g_pics;
static void forgetPics(void) { g_pics.scene = 0; }   // (the chips changed: every picture is drawn again)

static void drawCell(int p, const CtSceneLayerCells* C, int cx, int cy) {
  const CtSceneCell* ce = &C->cell[p][(size_t)cy * C->cw[p] + cx];
  uint8_t* o = g_pics.idx[p] + (size_t)cy * HDN * g_pics.w[p] + (size_t)cx * HDN;
  if (ce->chip == 0xFFFF || ce->chip >= C->nchips[p]) {
    for (int y = 0; y < HDN; y++) memset(o + (size_t)y * g_pics.w[p], 0, HDN);
    return;
  }
  const uint8_t* hd = CtHdChip(C->chips[p] + (size_t)ce->chip * 64);
  int fx = ce->flags & 1, fy = (ce->flags & 2) != 0;
  for (int y = 0; y < HDN; y++) {
    const uint8_t* s = hd + (size_t)(fy ? HDN - 1 - y : y) * HDN;
    uint8_t* r = o + (size_t)y * g_pics.w[p];
    for (int x = 0; x < HDN; x++) { uint8_t v = s[fx ? HDN - 1 - x : x]; r[x] = v ? (uint8_t)(ce->base + v) : 0; }
  }
  g_cellsDrawn++;
}
static void artForThisFrame(void);
static int hdUpdate(const CtSceneLayerCells* C) {
  int full = !g_pics.scene || g_pics.scene != C->scene;
  for (int p = 0; p < 6 && !full; p++) full = g_pics.cw[p] != C->cw[p] || g_pics.ch[p] != C->ch[p] || g_pics.nchips[p] != C->nchips[p];
  if (!full && g_pics.gen == C->gen) return 1;
  for (int p = 0; p < 6; p++) {
    int cw = C->cw[p], ch = C->ch[p];
    size_t nc = (size_t)cw * ch;
    if (full) {
      free(g_pics.idx[p]); free(g_pics.cells[p]); free(g_pics.chips[p]);
      g_pics.w[p] = cw * HDN; g_pics.h[p] = ch * HDN; g_pics.cw[p] = cw; g_pics.ch[p] = ch; g_pics.nchips[p] = C->nchips[p];
      g_pics.idx[p] = (uint8_t*)calloc((size_t)g_pics.w[p] * g_pics.h[p], 1);
      g_pics.cells[p] = (CtSceneCell*)malloc(nc * sizeof(CtSceneCell));
      g_pics.chips[p] = (uint8_t*)malloc((size_t)C->nchips[p] * 64);
      if (!g_pics.idx[p] || !g_pics.cells[p] || !g_pics.chips[p]) { g_pics.scene = 0; return 0; }
      for (int cy = 0; cy < ch; cy++) for (int cx = 0; cx < cw; cx++) drawCell(p, C, cx, cy);
    } else {
      // a chip whose contents changed (its animation stepped) redraws every cell showing it; a changed cell redraws itself
      static uint8_t changed[1024];
      int nch = C->nchips[p] < 1024 ? C->nchips[p] : 1024;
      for (int c = 0; c < nch; c++) changed[c] = memcmp(g_pics.chips[p] + (size_t)c * 64, C->chips[p] + (size_t)c * 64, 64) != 0;
      for (size_t j = 0; j < nc; j++) {
        const CtSceneCell* ce = &C->cell[p][j];
        if (memcmp(ce, &g_pics.cells[p][j], sizeof *ce) || (ce->chip < nch && changed[ce->chip]))
          drawCell(p, C, (int)(j % cw), (int)(j / cw));
      }
    }
    memcpy(g_pics.cells[p], C->cell[p], nc * sizeof(CtSceneCell));
    memcpy(g_pics.chips[p], C->chips[p], (size_t)C->nchips[p] * 64);
  }
  g_pics.scene = C->scene; g_pics.gen = C->gen;
  g_rebuilds += full;
  return 1;
}

// ---- the field at 4x: fieldnative.c's compositor over the HD pictures ---------------------------------------------
static long fdiv(long a, long b) { long q = a / b; if ((a % b) && ((a < 0) != (b < 0))) q--; return q; }
static int wrapi(long v, int n) { long r = v % n; if (r < 0) r += n; return (int)r; }
static uint8_t dimc(uint8_t c, int bright) { return bright >= 15 ? c : bright <= 0 ? 0 : (uint8_t)(c * bright / 15); }

static int fieldRasterHD(const CtMotFrame* cur, const CtMotFrame* prev, const uint8_t* fb, int w, int t, uint8_t* out);
int CtFieldRasterHD(const CtMotFrame* cur, const CtMotFrame* prev, const uint8_t* fb, int w, int t, uint8_t* out) {
  double t0 = nowMs();
  int r = fieldRasterHD(cur, prev, fb, w, t, out);
  if (r) { g_rasterMs += nowMs() - t0; g_rasters++; }
  return r;
}
static int fieldRasterHD(const CtMotFrame* cur, const CtMotFrame* prev, const uint8_t* fb, int w, int t, uint8_t* out) {
  const CtSceneLayers* L = (const CtSceneLayers*)cur->native;
  const CtSceneLayerCells* C = (const CtSceneLayerCells*)cur->hdCells;
  if (!L || !C || !cur->valid || cur->w != w) return 0;
  if (!prev || !prev->valid || prev->scene != cur->scene || prev->w != w ||
      abs(cur->camX - prev->camX) > 32 || abs(cur->camY - prev->camY) > 32) { prev = cur; t = 256; }
  if (!hdUpdate(C)) return 0;
  const int k = HDK;
  const uint8_t (*pal)[3] = C->pal;
  int dcx = cur->camX - prev->camX, dcy = cur->camY - prev->camY;
  const double tt = t / 256.0, back = 1.0 - tt;
  const int W = w * k, H = 224 * k;
  // the camera and the layers' own offsets at t, in HD pixels (fieldnative.c: every place at t by one rule, floor(p * k))
  double camXt = cur->camX - back * dcx, camYt = cur->camY - back * dcy;
  double o2x = cur->o2x - back * (cur->o2x - prev->o2x), o2y = cur->o2y - back * (cur->o2y - prev->o2y);
  double o3x = cur->o3x - back * (cur->o3x - prev->o3x), o3y = cur->o3y - back * (cur->o3y - prev->o3y);
  if (cur->qFrac && prev->qFrac) {
    o2x = prev->q2x + (cur->q2x - prev->q2x) * tt; o2y = prev->q2y + (cur->q2y - prev->q2y) * tt;
    o3x = prev->q3x + (cur->q3x - prev->q3x) * tt; o3y = prev->q3y + (cur->q3y - prev->q3y) * tt;
  }
  if (fabs(o2x - cur->o2x) > 32 || fabs(o2y - cur->o2y) > 32) { o2x = cur->o2x; o2y = cur->o2y; }
  if (fabs(o3x - cur->o3x) > 32 || fabs(o3y - cur->o3y) > 32) { o3x = cur->o3x; o3y = cur->o3y; }
  #define FLK(p) ((long)floor((p) * k + 1e-9))
  long offXk[6], offYk[6];
  { const double ox[6] = { o3x, o2x, 0, o2x, 0, o3x }, oy[6] = { o3y, o2y, 0, o2y, 0, o3y };
    for (int i = 0; i < 6; i++) { offXk[i] = FLK(camXt + ox[i]); offYk[i] = FLK(camYt + oy[i]); } }
  long camXk = FLK(camXt);
  int sx[128], sy[128];
  memset(sx, 0, sizeof sx); memset(sy, 0, sizeof sy);
  for (int i = 0; i < cur->nspr && i < 120; i++) {
    int j = 0; while (j < prev->nspr && prev->sprKey[j] != cur->sprKey[i]) j++;
    if (j < prev->nspr) {
      int wx = cur->sprX[i] - prev->sprX[j], wy = cur->sprY[i] - prev->sprY[j];
      if (abs(wx) <= 32 && abs(wy) <= 32) {
        sx[CT_MOT_SPR + i] = (int)(FLK(-back * wx) - FLK(-back * dcx)); sy[CT_MOT_SPR + i] = (int)(FLK(-back * wy) - FLK(-back * dcy));
      }
    }
  }
  #undef FLK
  // the sprites (their own pixels, in paint order), into their own plane: 0 none, 1 a sprite pixel, 2 one in front of
  // the foreground. A pixel with a record (hdsrc.h: its cell's chip and texel) is drawn from the chip's HD art -- the art's
  // transparent subpixels left to what is under it, its opaque ones in the colour the cell wrote for that index -- and
  // rounds into its cell's own transparent texels next to it, unless its own sprite painted those (the cells overlap);
  // a pixel without one is its colour, k x k
  static uint8_t* sp; static uint8_t* spf; static uint8_t* spp; static int* grp; static uint8_t* hole1; static size_t spN;
  if (spN < (size_t)W * H) {
    free(sp); free(spf); free(spp); free(grp); free(hole1);
    sp = (uint8_t*)malloc((size_t)W * H * 3); spf = (uint8_t*)malloc((size_t)W * H); spp = (uint8_t*)malloc((size_t)W * H);
    grp = (int*)malloc((size_t)CT_MOT_W * 224 * sizeof(int)); hole1 = (uint8_t*)malloc((size_t)CT_MOT_W * 224);
    spN = (sp && spf && spp && grp && hole1) ? (size_t)W * H : 0;
  }
  if (!spN) return 0;
  memset(spf, 0, (size_t)W * H);
  memset(grp, 0, (size_t)w * 224 * sizeof(int));     // (per 1x pixel: the group of the sprite that painted it last)
  memset(hole1, 0, (size_t)w * 224);                 // (...and whether it was an occluder's hole: the map's HD shows)
  for (int n = 0; n < cur->nspx; n++) {
    int own = cur->spx[n].own, o = own & 0x7F;
    int X0 = cur->spx[n].x * k + sx[o], Y0 = cur->spx[n].y * k + sy[o];
    uint8_t f = cur->spx[n].hole ? 0 : (own & CT_MOT_FRONT) ? 2 : 1;
    const uint8_t rgb[3] = { cur->spx[n].r, cur->spx[n].g, cur->spx[n].b };
    const int px1 = cur->spx[n].x, py1 = cur->spx[n].y, in1 = (unsigned)px1 < (unsigned)w && (unsigned)py1 < 224u;
    const uint32_t tag = f ? cur->spx[n].tag : 0;
    const CtSrcInst* e = tag ? CtSrcInstOf((int)(tag >> 8)) : NULL;
    CtSrcInfo si; const uint8_t* art = NULL;
    if (e && CtSrcInfoOf(e->src, &si)) art = CtHdArt(e->src);
    const int tx = (int)(tag & 15), ty = (int)((tag >> 4) & 15);
    if (in1) hole1[py1 * w + px1] = (uint8_t)(f == 0);
    if (!art || tx >= si.w || ty >= si.h) {
      for (int j = 0; j < k; j++) {
        int Y = Y0 + j; if ((unsigned)Y >= (unsigned)H) continue;
        for (int i = 0; i < k; i++) {
          int X = X0 + i; if ((unsigned)X >= (unsigned)W) continue;
          size_t q = (size_t)Y * W + X;
          spf[q] = f;
          if (f) { sp[q * 3] = cur->spx[n].r; sp[q * 3 + 1] = cur->spx[n].g; sp[q * 3 + 2] = cur->spx[n].b; spp[q] = cur->spx[n].pal; }
        }
      }
      if (in1) grp[py1 * w + px1] = 0;
      continue;
    }
    const int v = si.idx[ty * si.w + tx], fx = e->flags & 1, fy = (e->flags & 2) != 0, AW = si.w * k;
    for (int dy = -1; dy <= 1; dy++)                   // this pixel, then the cell's transparent texels round it
      for (int dx = -1; dx <= 1; dx++) {
        int ox = tx, oy = ty;
        if (dx || dy) {
          int qx = px1 + dx, qy = py1 + dy;
          if ((unsigned)qx >= (unsigned)w || (unsigned)qy >= 224u || grp[qy * w + qx] == e->group) continue;
          ox = tx + (fx ? -dx : dx); oy = ty + (fy ? -dy : dy);
          if (ox < 0 || oy < 0 || ox >= si.w || oy >= si.h || si.idx[oy * si.w + ox]) continue;
        }
        for (int j = 0; j < k; j++) {
          int Y = Y0 + dy * k + j; if ((unsigned)Y >= (unsigned)H) continue;
          for (int i = 0; i < k; i++) {
            int X = X0 + dx * k + i; if ((unsigned)X >= (unsigned)W) continue;
            int hv = art[(size_t)(oy * k + (fy ? k - 1 - j : j)) * AW + ox * k + (fx ? k - 1 - i : i)];
            if (!hv) continue;                         // (transparent: what is under it shows)
            size_t q = (size_t)Y * W + X;
            spf[q] = f; spp[q] = cur->spx[n].pal;
            memcpy(sp + q * 3, (hv == v || hv >= 16 || !(e->has >> hv & 1)) ? rgb : e->col[hv], 3);
          }
        }
      }
    if (in1) grp[py1 * w + px1] = e->group;
  }
  const int mainT = L->mainT, subT = L->subT, blend = L->blendEn, fgT = mainT | subT;
  static const uint8_t bitOf[6] = { BL3, BL2, BL1, BL2, BL1, BL3 };
  const int bright = cur->bright, wide = w > 256;
  int act[6];
  for (int i = 0; i < 6; i++) act[i] = ((mainT | subT) & bitOf[i]) != 0 && g_pics.idx[i];
  static int* tab; static size_t tabN;
  if (tabN < (size_t)W * 8) { free(tab); tab = (int*)malloc((size_t)W * 8 * sizeof(int)); tabN = tab ? (size_t)W * 8 : 0; }
  if (!tab) return 0;
  int* x1T = tab + 6 * W; int* inT = tab + 7 * W;
  for (int X = 0; X < W; X++) {
    x1T[X] = X / k;
    long cx = fdiv(camXk + X, k);
    inT[X] = !wide || (cx >= cur->mx0 && cx < cur->mx1);
    for (int i = 0; i < 6; i++) tab[i * W + X] = act[i] ? wrapi(offXk[i] + X, g_pics.w[i]) : 0;   // (HD columns)
  }
  const CtFieldNativeFx* fx = (const CtFieldNativeFx*)cur->nativeFx;
  long capXk = 0, capYk = 0;
  if (fx && fx->capOn) {
    double bx = cur->bg3X - back * (cur->bg3X - prev->bg3X), by = cur->bg3Y - back * (cur->bg3Y - prev->bg3Y);
    if (cur->qFrac && prev->qFrac) { bx = prev->qbx + (cur->qbx - prev->qbx) * tt; by = prev->qby + (cur->qby - prev->qby) * tt; }
    if (fabs(bx - cur->bg3X) > 32 || fabs(by - cur->bg3Y) > 32) { bx = cur->bg3X; by = cur->bg3Y; }
    capXk = (long)floor(bx * k + 1e-9); capYk = (long)floor(by * k + 1e-9);
  }
  // the screen-fixed pixels with a record (the text box, its glyphs): each one's block from its source's HD art
  static uint8_t* fixHd; static uint8_t* fixOn; static size_t fixN;
  CtSrcPlane SP; const int haveSP = CtSrcPlaneOf(fb, w, &SP);
  if (haveSP) {
    artForThisFrame();                             // (before the threads: making art grows the table they read)
    if (fixN < (size_t)W * H) {
      free(fixHd); free(fixOn);
      fixHd = (uint8_t*)malloc((size_t)W * H * 3); fixOn = (uint8_t*)malloc((size_t)w * 224);
      fixN = fixHd && fixOn ? (size_t)W * H : 0;
    }
    if (fixN) {
      int yy;
      #pragma omp parallel for schedule(dynamic, 8)
      for (yy = 0; yy < 224; yy++)
        for (int x = 0; x < w; x++) {
          uint8_t b[HDK * HDK][3];
          fixOn[yy * w + x] = cur->own[yy * w + x] == CT_MOT_FIXED && CtHdSrcBlock(&SP, fb, w, x, yy, b);
          if (fixOn[yy * w + x])
            for (int j = 0; j < HDK; j++) memcpy(fixHd + ((size_t)(yy * HDK + j) * W + (size_t)x * HDK) * 3, b[j * HDK], HDK * 3);
        }
    }
  }
  const uint8_t* fixHdP = haveSP && fixN ? fixHd : NULL;
  static long long covFrame = -1; long long covNow = 0; CtSrcStats(&covNow, NULL, NULL);
  if (t == 256 && covNow != covFrame) {            // (coverage: each frame once -- the map's pixels are its cells')
    covFrame = covNow;
    long long tot = 0, hd = 0;
    if (w <= CT_MOT_W) memset(g_covMask, 0, (size_t)w * 224);
    for (int y = 0; y < 224; y++)
      for (int x = 0; x < w; x++) {
        const uint8_t* f = fb + ((size_t)y * w + x) * 3;
        if (!(f[0] | f[1] | f[2])) continue;
        int own = cur->own[y * w + x];
        tot++;
        int h1 = own == CT_MOT_FIXED ? (fixHdP && fixOn[y * w + x]) :
                 (own & 0x7F) >= CT_MOT_SPR ? grp[y * w + x] != 0 || hole1[y * w + x] : 1;
        hd += h1;
        if (w <= CT_MOT_W) g_covMask[y * w + x] = (uint8_t)(h1 ? 1 : 2);
      }
    g_covMaskW = w <= CT_MOT_W ? w : 0;
    g_cov[g_covMode][0] += tot; g_cov[g_covMode][1] += hd;
  }
  int Y;
  #pragma omp parallel for schedule(dynamic, 16)
  for (Y = 0; Y < H; Y++) {
    int y1 = Y / k;
    uint8_t* o = out + (size_t)Y * W * 3;
    const uint8_t* ownRow = cur->own + (size_t)y1 * w; const uint8_t* fbRow = fb + (size_t)y1 * w * 3;
    const uint8_t* lrow[6];
    for (int i = 0; i < 6; i++)
      lrow[i] = act[i] ? g_pics.idx[i] + (size_t)wrapi(offYk[i] + Y, g_pics.h[i]) * g_pics.w[i] : NULL;
    const uint8_t* spRow = sp + (size_t)Y * W * 3; const uint8_t* spfRow = spf + (size_t)Y * W; const uint8_t* sppRow = spp + (size_t)Y * W;
    const int capY = fx && fx->capOn ? wrapi(fdiv(capYk + Y, k), fx->capH) : 0;
    for (int X = 0; X < W; X++, o += 3) {
      int x1 = x1T[X];
      if (ownRow[x1] == CT_MOT_FIXED) {
        if (fixHdP && fixOn[y1 * w + x1]) memcpy(o, fixHdP + ((size_t)Y * W + X) * 3, 3); else memcpy(o, fbRow + x1 * 3, 3);
        continue;
      }
      int ci[6];                                       // each picture's palette index here (0 = transparent)
      for (int i = 0; i < 6; i++) ci[i] = act[i] ? lrow[i][tab[i * W + X]] : 0;
      if (!inT[X]) {                                   // the widescreen's sides (fieldnative.c, fieldBeyondPixel)
        uint8_t c[3] = { 0, 0, 0 };
        if (L->beyond) {
          int i = 5;
          for (; i >= 0; i--) if ((L->beyond & bitOf[i]) && (mainT & bitOf[i]) && ci[i]) break;
          memcpy(c, i >= 0 ? pal[ci[i]] : pal[0], 3);
        }
        o[0] = dimc(c[0], bright); o[1] = dimc(c[1], bright); o[2] = dimc(c[2], bright);
        continue;
      }
      const int xv = fx ? x1 - fx->viewX : 0, inView = xv >= 0 && xv < 256;
      const int iris = fx && fx->irisOn && y1 < 224 && xv >= fx->irisL[y1] && xv <= fx->irisR[y1];
      const int mT = iris ? mainT & ~(BL1 | BL2) : mainT, sT = iris ? subT & ~(BL1 | BL2) : subT;
      int mt = -1, st = -1, mtFull = -1;
      for (int i = 5; i >= 0; i--) if ((mainT & bitOf[i]) && ci[i]) { mtFull = i; break; }
      for (int i = 5; i >= 0; i--) if ((mT & bitOf[i]) && ci[i]) { mt = i; break; }
      if (sT) for (int i = 5; i >= 0; i--) if ((sT & bitOf[i]) && ci[i]) { st = i; break; }
      int mc = mt >= 0 ? bitOf[mt] : 0, sc = st >= 0 ? bitOf[st] : 0;
      uint8_t c[3];
      if (mt >= 0) memcpy(c, pal[ci[mt]], 3);
      if (sT) {
        if ((mc & blend) && sc) {
          const uint8_t* s = pal[ci[st]];
          for (int ch = 0; ch < 3; ch++) {
            int d = c[ch] >> 3, v = s[ch] >> 3, r = L->subOp ? d - v : d + v;
            if (L->half) r >>= 1;
            if (r < 0) r = 0; if (r > 31) r = 31;
            c[ch] = (uint8_t)((r << 3) | (r >> 2));
          }
        } else if (!mc && sc) memcpy(c, pal[ci[st]], 3);
      }
      if (!mc && !(sT && sc)) memcpy(c, pal[0], 3);
      int capX = 0;
      if (fx && fx->capOn && inView) {
        capX = wrapi(fdiv(capXk + X, k), fx->capW);
        size_t cq = (size_t)capY * fx->capW + capX;
        if (!iris && mtFull < 0 && fx->capOpq[0][cq]) memcpy(c, fx->capRgb[0] + cq * 3, 3);
      }
      int sprPal = 0xFF;
      if (spfRow[X]) {
        int fg = !iris && (((fgT & BL1) && ci[4]) || ((fgT & BL2) && ci[3]));
        if (spfRow[X] == 2 || !fg) { memcpy(c, spRow + X * 3, 3); sprPal = sppRow[X]; }
      }
      if (fx && fx->capOn && inView) {
        size_t cq = (size_t)capY * fx->capW + capX;
        if (fx->capOpq[1][cq]) memcpy(c, fx->capRgb[1] + cq * 3, 3);
      }
      if (fx && fx->colOn && y1 < 224 && xv >= (fx->colL[y1] < 0 ? 0 : fx->colL[y1]) && xv <= (fx->colR[y1] > 255 ? 255 : fx->colR[y1])) {
        int on;
        if (sprPal != 0xFF) on = (fx->colMath & 0x10) && sprPal >= 4;
        else { int lay = mtFull >= 0 ? bitOf[mtFull] : 0; on = lay ? (fx->colMath & lay) != 0 : (fx->colMath & 0x20) != 0; }
        if (on)
          for (int ch = 0; ch < 3; ch++) {
            int c5 = c[ch] >> 3, r = (fx->colMath & 0x80) ? c5 - fx->colFix[ch] : c5 + fx->colFix[ch];
            if (fx->colHalf) r >>= 1;
            if (r > 31) r = 31;
            if (r < 0) r = 0;
            c[ch] = (uint8_t)((r << 3) | (r >> 2));
          }
      }
      o[0] = dimc(c[0], bright); o[1] = dimc(c[1], bright); o[2] = dimc(c[2], bright);
    }
  }
  return 1;
}

// ---- a picture redrawn from its records (hdsrc.h): each pixel's 4x4 block -----------------------------------------
// The colour a pixel's record leads to: the frame's own where the HD art keeps the original's index; another index's
// colour as the instance wrote it, times the pixel's transfer (the frame's colour over the colour written: a fade); a
// transparent HD texel shows the neighbour outside the instance on that side. A record whose colour the frame no longer
// has (another draw covered the pixel unrecorded, or colour math changed it) is not used: the pixel is the frame's.
// A blended pixel (CtSrcBlend: a translucent layer or the fixed colour over it) adds its change instead: the HD art under
// 600 A.D.'s mist, under a battle's effect layer.
typedef struct { float mul; int add[3]; } Transfer;
static int transferOf(const uint8_t* f, const uint8_t* wr, int blended, Transfer* t) {
  t->mul = 1.0f; t->add[0] = t->add[1] = t->add[2] = 0;
  if (f[0] == wr[0] && f[1] == wr[1] && f[2] == wr[2]) return 1;
  if (blended) { for (int c = 0; c < 3; c++) t->add[c] = (int)f[c] - (int)wr[c]; return 1; }
  int sw = wr[0] + wr[1] + wr[2], sf = f[0] + f[1] + f[2];
  if (!sw) return 0;
  float b = (float)sf / (float)sw;
  if (b > 1.0f) return 0;                          // (a fade only darkens)
  for (int c = 0; c < 3; c++) if (fabsf(f[c] - wr[c] * b) > 8.0f) return 0;
  t->mul = b;
  return 1;
}
static void scaled(const uint8_t* c, const Transfer* t, uint8_t* o) {
  for (int k = 0; k < 3; k++) { float v = c[k] * t->mul + (float)t->add[k] + 0.5f; o[k] = (uint8_t)(v > 255.0f ? 255 : v < 0.0f ? 0 : v); }
}
typedef struct { const CtSrcInst* e; CtSrcInfo s; const uint8_t* art; int tx, ty, inst; Transfer tr; } Rec;
static int recOf(const CtSrcPlane* P, const uint8_t* fb, int w, int x, int y, Rec* r) {
  long q = CtSrcAt(P, x, y);
  if (q < 0 || !P->tag) return 0;
  uint32_t t = P->tag[q];
  if (!t) return 0;
  r->inst = (int)(t >> 8); r->tx = (int)(t & 15); r->ty = (int)((t >> 4) & 15);
  r->e = CtSrcInstOf(r->inst);
  if (!r->e || !CtSrcInfoOf(r->e->src, &r->s) || r->tx >= r->s.w || r->ty >= r->s.h) return 0;
  if (!transferOf(fb + ((size_t)y * w + x) * 3, P->rgb + (size_t)q * 3, P->blend && P->blend[q], &r->tr)) return 0;
  r->art = CtHdArt(r->e->src);
  return r->art != NULL;
}
static inline int artAt(const Rec* r, int tx, int ty, int i, int j) {   // the HD index of subpixel (i, j) of texel (tx, ty)
  int hx = tx * HDK + ((r->e->flags & 1) ? HDK - 1 - i : i), hy = ty * HDK + ((r->e->flags & 2) ? HDK - 1 - j : j);
  return r->art[(size_t)hy * r->s.w * HDK + hx];
}
static inline uint32_t instAt(const CtSrcPlane* P, int x, int y) { long q = CtSrcAt(P, x, y); return q < 0 || !P->tag ? 0 : P->tag[q] >> 8; }
int CtHdSrcBlock(const CtSrcPlane* P, const uint8_t* fb, int w, int x, int y, uint8_t out[HDK * HDK][3]) {
  const uint8_t* f0 = fb + ((size_t)y * w + x) * 3;
  for (int k = 0; k < HDK * HDK; k++) memcpy(out[k], f0, 3);
  const long q = CtSrcAt(P, x, y);
  if (q < 0) return 0;
  // no record, but the picture's own drawer drew it at the presenter's scale (CtSrcNative: a mode-7 plane): its block
  if (P->native && P->nk == HDK && (!P->tag || !P->tag[q])) {
    const int vx = x - P->x0, NW = P->vw * HDK;
    for (int j = 0; j < HDK; j++)
      for (int i = 0; i < HDK; i++) memcpy(out[j * HDK + i], P->native + ((size_t)(y * HDK + j) * NW + (size_t)vx * HDK + i) * 3, 3);
    return 1;
  }
  int any = 0;
  Rec r;
  const int have = recOf(P, fb, w, x, y, &r);
  if (have && (r.e->flags & CT_SRC_CRISP)) return 1;  // (text: the pixel as it is, HDK x HDK -- out holds it already)
  if (have) {
    const int v = r.s.idx[r.ty * r.s.w + r.tx];
    for (int j = 0; j < HDK; j++)
      for (int i = 0; i < HDK; i++) {
        int hv = artAt(&r, r.tx, r.ty, i, j);
        if (hv == v) continue;
        uint8_t* o = out[j * HDK + i];
        if (!hv) {                                     // transparent here: what is next to the instance on that side
          const int sx = i < HDK / 2 ? -1 : 1, sy = j < HDK / 2 ? -1 : 1;
          const int cand[3][2] = { { sx, 0 }, { 0, sy }, { sx, sy } };
          for (int c = 0; c < 3; c++) {
            int nx = x + cand[c][0], ny = y + cand[c][1];
            if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= 224u || instAt(P, nx, ny) == (uint32_t)r.inst) continue;
            memcpy(o, fb + ((size_t)ny * w + nx) * 3, 3);
            break;
          }
        } else if (hv < 16 && (r.e->has >> hv & 1)) scaled(r.e->col[hv], &r.tr, o);
        any = 1;
      }
  }
  // GROWTH: an instance drawn after this pixel's content, whose box holds this pixel as one of its transparent texels,
  // puts down the HD art's opaque subpixels here (the art rounds a corner into it) -- never one of its own group's
  if (!P->tag) return have;
  const uint32_t seq = P->seq[q];
  const int ownGroup = have ? r.e->group : 0;
  Rec g; int gx = 0, gy = 0, best = 0;
  for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++) {
      if (!dx && !dy) continue;
      int nx = x + dx, ny = y + dy;
      if ((unsigned)nx >= (unsigned)w || (unsigned)ny >= 224u) continue;
      uint32_t ni = instAt(P, nx, ny);
      if (!ni || (int)ni <= best || ni <= seq) continue;
      Rec n;
      if (!recOf(P, fb, w, nx, ny, &n) || (ownGroup && n.e->group == ownGroup) || (n.e->flags & CT_SRC_CRISP)) continue;
      int ox = n.tx + ((n.e->flags & 1) ? dx : -dx), oy = n.ty + ((n.e->flags & 2) ? dy : -dy);   // (this pixel in it)
      if (ox < 0 || oy < 0 || ox >= n.s.w || oy >= n.s.h || n.s.idx[oy * n.s.w + ox]) continue;
      g = n; gx = ox; gy = oy; best = n.inst;
    }
  if (best)
    for (int j = 0; j < HDK; j++)
      for (int i = 0; i < HDK; i++) {
        int hv = artAt(&g, gx, gy, i, j);
        if (!hv || hv >= 16 || !(g.e->has >> hv & 1)) continue;
        scaled(g.e->col[hv], &g.tr, out[j * HDK + i]);
        any = 1;
      }
  return have || any;
}
// Every source this frame's draws name gets its art BEFORE the pictures are drawn in parallel: making one grows the art's
// table, which the drawing threads read.
static void artForThisFrame(void) {
  int n = CtSrcInstCount();
  for (int i = 1; i <= n; i++) { const CtSrcInst* e = CtSrcInstOf(i); if (e) (void)CtHdArt(e->src); }
}
// A whole picture (w x 224) at 4x from its records: each pixel's block (CtHdSrcBlock), the frame's own pixel 4x4 where
// it has none. 0 = the picture has no records (the caller draws it another way).
int CtHdRasterPicture(const uint8_t* fb, int w, uint8_t* out) {
  CtSrcPlane P;
  if (!CtSrcPlaneOf(fb, w, &P)) return 0;
  double t0 = nowMs();
  artForThisFrame();
  const int W = w * HDK;
  long tot = 0, hd = 0;
  int y;
  #pragma omp parallel for schedule(dynamic, 8) reduction(+:tot, hd)
  for (y = 0; y < 224; y++)
    for (int x = 0; x < w; x++) {
      uint8_t b[HDK * HDK][3];
      const int fromArt = CtHdSrcBlock(&P, fb, w, x, y, b);
      const uint8_t* f = fb + ((size_t)y * w + x) * 3;
      if (f[0] | f[1] | f[2]) { tot++; hd += fromArt != 0; }
      if (w <= CT_MOT_W) g_covMask[y * w + x] = (uint8_t)(!(f[0] | f[1] | f[2]) ? 0 : fromArt ? 1 : 2);
      for (int j = 0; j < HDK; j++) memcpy(out + ((size_t)(y * HDK + j) * W + (size_t)x * HDK) * 3, b[j * HDK], HDK * 3);
    }
  g_cov[g_covMode][0] += tot; g_cov[g_covMode][1] += hd;
  g_covMaskW = w <= CT_MOT_W ? w : 0;
  g_picFrames++;
  g_rasterMs += nowMs() - t0; g_rasters++;
  return 1;
}

// ---- the automatic pass over every location and every sprite --------------------------------------------------------
static long passSource(const uint8_t* idx, int w, int h, int kinds) {
  int id = CtSrcOf(idx, w, h);
  if (!id) return 0;
  int fresh = (size_t)id >= g_artN || !g_art[id];
  if (!makeArt(id, FROM_PASS)) return 0;
  CtHdArtKind(id, kinds);
  return fresh;
}
int CtHdAutoPass(const CtAssets* a, const char* dir) {
  if (dir && *dir) snprintf(g_dir, sizeof g_dir, "%s", dir); else snprintf(g_dir, sizeof g_dir, "hd");
  clearArt();                                      // (the pass makes every source again: a pack of exactly what it draws)
  long locs = 0, picChips = 0, animChips = 0, sprites = 0, sprTiles = 0;
  for (int loc = 0; loc < 512; loc++)
    for (int ring = 0; ring < 2; ring++) {
      void* sc = CtFieldSceneOpen(a, loc, 0);
      if (!sc) continue;
      if (ring) CtFieldSceneSetLayerOffsets(sc, 0, 0, 0, 0, 6);
      CtSceneLayerCells C;
      if (CtFieldSceneLayerCells(sc, &C)) {
        locs += !ring;
        for (int p = 0; p < 6; p++)
          for (size_t j = 0; j < (size_t)C.cw[p] * C.ch[p]; j++) {
            const CtSceneCell* ce = &C.cell[p][j];
            if (ce->chip == 0xFFFF || ce->chip >= C.nchips[p]) continue;
            picChips += passSource(C.chips[p] + (size_t)ce->chip * 64, 8, 8, CT_HD_MAP);
          }
        int na = 0; const uint8_t* an = CtFieldSceneAnimChips(sc, &na);
        for (int j = 0; an && j < na; j++) animChips += passSource(an + (size_t)j * 64, 8, 8, CT_HD_MAP_ANIM);
      }
      CtFieldSceneClose(sc);
    }
  // every sprite's tiles (assets.h CT_ASSET_SPRITE: [size][nframes][ntiles u16] palette(48) tiles(ntiles * 64) ...)
  for (uint32_t id = 0; id < 4096; id++) {
    uint32_t sl = 0;
    const uint8_t* sb = CtAssetGet(a, CT_ASSET_SPRITE, id, &sl);
    if (!sb || sl < 52) continue;
    int nt = sb[2] | sb[3] << 8;
    if (52 + (uint32_t)nt * 64 > sl) continue;
    sprites++;
    for (int t = 0; t < nt; t++) sprTiles += passSource(sb + 52 + (size_t)t * 64, 8, 8, CT_HD_SPRITE);
  }
  char p[1100]; snprintf(p, sizeof p, "%s/chips4x.pack", g_dir);
  int ok = writePack(p, 0);
  uint64_t fnv = 1469598103934665603ull; long n = 0;
  for (size_t i = 1; i < g_artN; i++) {
    if (!g_art[i]) continue;
    CtSrcInfo si; CtSrcInfoOf((int)i, &si); n++;
    for (int q = 0; q < si.w * si.h; q++) { fnv ^= si.idx[q]; fnv *= 1099511628211ull; }
    for (int q = 0; q < si.w * si.h * HDK * HDK; q++) { fnv ^= g_art[i][q]; fnv *= 1099511628211ull; }
  }
  if (!ok) { printf("hdautopass: FAIL cannot write %s\n", p); return 0; }
  printf("hdautopass: %ld locations, %ld sprites, %ld sources (%ld drawn by the maps, %ld more in their animation sets, "
         "%ld more in the sprites) -> %s, fnv %016llX\n", locs, sprites, n, picChips, animChips, sprTiles, p, (unsigned long long)fnv);
  return 1;
}
