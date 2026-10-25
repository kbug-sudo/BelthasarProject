// hdpresent.c - the HD presenter (hdpresent.h): the HD chips (a pack, else the automatic pass), the map's HD pictures
// kept as index maps beside the engine's own, and the field drawn from them at 4x the way fieldnative.c draws it at k x.
#include "hdpresent.h"
#include "assets.h"
#include "fieldnative.h"                           // CtFieldNativeFx: what the field composes over the map
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BL1 1
#define BL2 2
#define BL3 4
#define HDK CT_HD_K
#define HDN (8 * HDK)                              // an HD chip's side (32)
#define HDA (HDN * HDN)

static long g_asked, g_made, g_rebuilds, g_cellsDrawn;
static int g_nearest;                              // CtHdNearest: every chip its own pixels 4x4 (the check's art)

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
void CtHdUpscaleChip(const uint8_t in[64], uint8_t out[HDA]) {
  if (g_nearest) {
    for (int y = 0; y < HDN; y++) for (int x = 0; x < HDN; x++) out[y * HDN + x] = in[(y / HDK) * 8 + x / HDK];
    return;
  }
  uint8_t mid[16 * 16];
  scale2x(in, 8, 8, mid);
  scale2x(mid, 16, 16, out);
}

// ---- the chips: the pack's, and the ones made on the spot ----------------------------------------------------------
// Entries live in blocks (stable addresses); an open-addressed index on the chip's 64 indices finds them.
typedef struct { uint8_t src[64]; uint8_t hd[HDA]; } HdChip;
#define BLK 4096
static HdChip** g_blk; static size_t g_nent, g_nblk;
static uint32_t* g_idx; static size_t g_capidx;    // a power of two; 0 = empty, else entry + 1

static uint64_t fnv64(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}
static HdChip* entry(size_t i) { return &g_blk[i / BLK][i % BLK]; }
static HdChip* lookup(const uint8_t src[64]) {
  if (!g_capidx) return NULL;
  size_t m = g_capidx - 1, i = (size_t)fnv64(src, 64) & m;
  for (; g_idx[i]; i = (i + 1) & m) { HdChip* e = entry(g_idx[i] - 1); if (!memcmp(e->src, src, 64)) return e; }
  return NULL;
}
static int regrow(size_t cap) {
  uint32_t* ni = (uint32_t*)calloc(cap, sizeof *ni);
  if (!ni) return 0;
  for (size_t j = 0; j < g_nent; j++) {
    size_t i = (size_t)fnv64(entry(j)->src, 64) & (cap - 1);
    while (ni[i]) i = (i + 1) & (cap - 1);
    ni[i] = (uint32_t)(j + 1);
  }
  free(g_idx); g_idx = ni; g_capidx = cap;
  return 1;
}
static HdChip* insert(const uint8_t src[64]) {
  if (2 * (g_nent + 1) > g_capidx && !regrow(g_capidx ? g_capidx * 2 : 1 << 16)) return NULL;
  if (g_nent == g_nblk * BLK) {
    HdChip** nb = (HdChip**)realloc(g_blk, (g_nblk + 1) * sizeof *nb);
    if (!nb) return NULL;
    g_blk = nb;
    if (!(g_blk[g_nblk] = (HdChip*)malloc(BLK * sizeof(HdChip)))) return NULL;
    g_nblk++;
  }
  HdChip* e = entry(g_nent);
  memcpy(e->src, src, 64);
  size_t i = (size_t)fnv64(src, 64) & (g_capidx - 1);
  while (g_idx[i]) i = (i + 1) & (g_capidx - 1);
  g_idx[i] = (uint32_t)(++g_nent);
  return e;
}
static void clearChips(void) {
  for (size_t b = 0; b < g_nblk; b++) free(g_blk[b]);
  free(g_blk); free(g_idx); g_blk = NULL; g_idx = NULL; g_nblk = g_nent = g_capidx = 0;
}

static char g_dir[1024];
const char* CtHdDir(void) { return g_dir; }
static void forgetPics(void);
void CtHdNearest(int on) { g_nearest = on != 0; clearChips(); forgetPics(); }
int CtHdOpen(const char* dir) {
  if (!dir || !*dir) { dir = getenv("CT_HD_DIR"); if (!dir || !*dir) dir = "hd"; }
  snprintf(g_dir, sizeof g_dir, "%s", dir);
  clearChips(); forgetPics();
  if (g_nearest) return 0;                         // (the check's art is made, never read)
  char p[1100]; snprintf(p, sizeof p, "%s/chips4x.pack", g_dir);
  FILE* f = fopen(p, "rb");
  if (!f) return 0;
  uint8_t h[12]; uint32_t n = 0;
  int ok = fread(h, 1, 12, f) == 12 && !memcmp(h, "CTHD", 4) && (h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24) == 1;
  if (ok) n = h[8] | h[9] << 8 | h[10] << 16 | (uint32_t)h[11] << 24;
  for (uint32_t j = 0; ok && j < n; j++) {
    uint8_t src[64];
    if (fread(src, 1, 64, f) != 64) { ok = 0; break; }
    HdChip* e = lookup(src); if (!e) e = insert(src);
    if (!e || fread(e->hd, 1, HDA, f) != HDA) ok = 0;
  }
  fclose(f);
  if (!ok) { fprintf(stderr, "[hd] %s is not a chips pack (version 1): the automatic pass draws every chip\n", p); clearChips(); return 0; }
  fprintf(stderr, "[hd] %s: %u chips\n", p, n);
  return 1;
}
const uint8_t* CtHdChip(const uint8_t chip[64]) {
  static uint8_t fallback[HDA];
  g_asked++;
  HdChip* e = lookup(chip);
  if (e) return e->hd;
  g_made++;
  e = insert(chip);
  uint8_t* out = e ? e->hd : fallback;
  CtHdUpscaleChip(chip, out);
  return out;
}
void CtHdStats(long* chips, long* missing, long* rebuilds, long* cells) {
  if (chips) *chips = g_asked; if (missing) *missing = g_made; if (rebuilds) *rebuilds = g_rebuilds; if (cells) *cells = g_cellsDrawn;
}

// ---- the map's HD pictures: index maps (0 = transparent, else the palette index), redrawn cell by cell ---------------
typedef struct {
  const void* key; unsigned gen;                   // the cells they were drawn from (a new location: new arrays)
  uint8_t* idx[6]; int w[6], h[6];                 // HD pictures, w = cells across x 32
  CtSceneCell* cells[6]; int cw[6], ch[6];
  uint8_t* chips[6]; int nchips[6];                // the chips' contents when they were drawn
} HdPics;
static HdPics g_pics;
static void forgetPics(void) { g_pics.key = NULL; }   // (the chips changed: every picture is drawn again)

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
static int hdUpdate(const CtSceneLayerCells* C) {
  int full = g_pics.key != (const void*)C->cell[0];
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
      if (!g_pics.idx[p] || !g_pics.cells[p] || !g_pics.chips[p]) { g_pics.key = NULL; return 0; }
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
  g_pics.key = C->cell[0]; g_pics.gen = C->gen;
  g_rebuilds += full;
  return 1;
}

// ---- the field at 4x: fieldnative.c's compositor over the HD pictures ---------------------------------------------
static long fdiv(long a, long b) { long q = a / b; if ((a % b) && ((a < 0) != (b < 0))) q--; return q; }
static int wrapi(long v, int n) { long r = v % n; if (r < 0) r += n; return (int)r; }
static uint8_t dimc(uint8_t c, int bright) { return bright >= 15 ? c : bright <= 0 ? 0 : (uint8_t)(c * bright / 15); }

int CtFieldRasterHD(const CtMotFrame* cur, const CtMotFrame* prev, const uint8_t* fb, int w, int t, uint8_t* out) {
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
  // the sprites (their own pixels, each k x k), into their own plane: 0 none, 1 a sprite pixel, 2 one in front of the foreground
  static uint8_t* sp; static uint8_t* spf; static uint8_t* spp; static size_t spN;
  if (spN < (size_t)W * H) {
    free(sp); free(spf); free(spp);
    sp = (uint8_t*)malloc((size_t)W * H * 3); spf = (uint8_t*)malloc((size_t)W * H); spp = (uint8_t*)malloc((size_t)W * H);
    spN = (sp && spf && spp) ? (size_t)W * H : 0;
  }
  if (!spN) return 0;
  memset(spf, 0, (size_t)W * H);
  for (int n = 0; n < cur->nspx; n++) {
    int own = cur->spx[n].own, o = own & 0x7F;
    int X0 = cur->spx[n].x * k + sx[o], Y0 = cur->spx[n].y * k + sy[o];
    uint8_t f = cur->spx[n].hole ? 0 : (own & CT_MOT_FRONT) ? 2 : 1;
    for (int j = 0; j < k; j++) {
      int Y = Y0 + j; if ((unsigned)Y >= (unsigned)H) continue;
      for (int i = 0; i < k; i++) {
        int X = X0 + i; if ((unsigned)X >= (unsigned)W) continue;
        size_t q = (size_t)Y * W + X;
        spf[q] = f;
        if (f) { sp[q * 3] = cur->spx[n].r; sp[q * 3 + 1] = cur->spx[n].g; sp[q * 3 + 2] = cur->spx[n].b; spp[q] = cur->spx[n].pal; }
      }
    }
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
      if (ownRow[x1] == CT_MOT_FIXED) { memcpy(o, fbRow + x1 * 3, 3); continue; }
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

// ---- the automatic pass over every location -----------------------------------------------------------------------
int CtHdAutoPass(const CtAssets* a, const char* dir) {
  if (dir && *dir) CtHdOpen(dir); else CtHdOpen(NULL);
  clearChips();                                    // (the pass makes every chip again: a pack of exactly what it draws)
  long locs = 0, picChips = 0, animChips = 0;
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
            const uint8_t* src = C.chips[p] + (size_t)ce->chip * 64;
            if (!lookup(src)) { (void)CtHdChip(src); picChips++; }
          }
        int na = 0; const uint8_t* an = CtFieldSceneAnimChips(sc, &na);
        for (int j = 0; an && j < na; j++) if (!lookup(an + (size_t)j * 64)) { (void)CtHdChip(an + (size_t)j * 64); animChips++; }
      }
      CtFieldSceneClose(sc);
    }
  char p[1100]; snprintf(p, sizeof p, "%s/chips4x.pack", g_dir);
  FILE* f = fopen(p, "wb");
  if (!f) { printf("hdautopass: FAIL cannot write %s\n", p); return 0; }
  uint8_t h[12] = { 'C', 'T', 'H', 'D', 1, 0, 0, 0, (uint8_t)g_nent, (uint8_t)(g_nent >> 8), (uint8_t)(g_nent >> 16), (uint8_t)(g_nent >> 24) };
  fwrite(h, 1, 12, f);
  uint64_t fnv = 1469598103934665603ull;
  for (size_t j = 0; j < g_nent; j++) {
    HdChip* e = entry(j);
    fwrite(e->src, 1, 64, f); fwrite(e->hd, 1, HDA, f);
    for (int q = 0; q < 64; q++) { fnv ^= e->src[q]; fnv *= 1099511628211ull; }
    for (int q = 0; q < HDA; q++) { fnv ^= e->hd[q]; fnv *= 1099511628211ull; }
  }
  int ok = fclose(f) == 0;
  printf("hdautopass: %ld locations, %ld chips (%ld drawn by the pictures, %ld more in animation sets) -> %s, fnv %016llX\n",
         locs, (long)g_nent, picChips, animChips, p, (unsigned long long)fnv);
  return ok;
}
