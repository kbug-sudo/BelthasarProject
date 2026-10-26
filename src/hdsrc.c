// hdsrc.c - what each pixel of the picture was drawn from (hdsrc.h): the source pool, this frame's instances, and each
// picture's per-pixel records.
#include "hdsrc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_ctSrcOn, g_ctSrcCheck;
static long long g_chk, g_chkBad;

// ---- the source pool: entries in blocks (stable addresses), an open-addressed index on (w, h, content) ------------------
typedef struct { uint8_t w, h; uint8_t idx[CT_SRC_MAXWH * CT_SRC_MAXWH]; } Src;
#define SBLK 4096
static Src** g_sblk; static size_t g_nsrc, g_nsblk;
static uint32_t* g_sidx; static size_t g_scap;       // a power of two; 0 = empty, else id

static uint64_t hashOf(const uint8_t* p, int w, int h) {
  uint64_t x = 1469598103934665603ull;
  x ^= (uint64_t)w; x *= 1099511628211ull; x ^= (uint64_t)h; x *= 1099511628211ull;
  for (int i = 0; i < w * h; i++) { x ^= p[i]; x *= 1099511628211ull; }
  return x;
}
static Src* srcAt(size_t id) { return &g_sblk[(id - 1) / SBLK][(id - 1) % SBLK]; }
static int regrow(size_t cap) {
  uint32_t* ni = (uint32_t*)calloc(cap, sizeof *ni);
  if (!ni) return 0;
  for (size_t id = 1; id <= g_nsrc; id++) {
    const Src* s = srcAt(id);
    size_t i = (size_t)hashOf(s->idx, s->w, s->h) & (cap - 1);
    while (ni[i]) i = (i + 1) & (cap - 1);
    ni[i] = (uint32_t)id;
  }
  free(g_sidx); g_sidx = ni; g_scap = cap;
  return 1;
}
int CtSrcOf(const uint8_t* idx, int w, int h) {
  if (!idx || w < 1 || h < 1 || w > CT_SRC_MAXWH || h > CT_SRC_MAXWH) return 0;
  uint64_t hv = hashOf(idx, w, h);
  if (g_scap)
    for (size_t i = (size_t)hv & (g_scap - 1); g_sidx[i]; i = (i + 1) & (g_scap - 1)) {
      const Src* s = srcAt(g_sidx[i]);
      if (s->w == w && s->h == h && !memcmp(s->idx, idx, (size_t)w * h)) return (int)g_sidx[i];
    }
  if (g_nsrc >= 0xFFFFFF) return 0;
  if (2 * (g_nsrc + 1) > g_scap && !regrow(g_scap ? g_scap * 2 : 1 << 16)) return 0;
  if (g_nsrc == g_nsblk * SBLK) {
    Src** nb = (Src**)realloc(g_sblk, (g_nsblk + 1) * sizeof *nb);
    if (!nb) return 0;
    g_sblk = nb;
    if (!(g_sblk[g_nsblk] = (Src*)malloc(SBLK * sizeof(Src)))) return 0;
    g_nsblk++;
  }
  Src* s = &g_sblk[g_nsrc / SBLK][g_nsrc % SBLK];
  s->w = (uint8_t)w; s->h = (uint8_t)h; memcpy(s->idx, idx, (size_t)w * h);
  g_nsrc++;
  size_t i = (size_t)hv & (g_scap - 1);
  while (g_sidx[i]) i = (i + 1) & (g_scap - 1);
  g_sidx[i] = (uint32_t)g_nsrc;
  return (int)g_nsrc;
}
int CtSrcOf4bpp(const uint8_t p[32]) {
  uint8_t idx[64];
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) {
      int b = 7 - x;
      idx[y * 8 + x] = (uint8_t)(((p[y * 2] >> b) & 1) | (((p[y * 2 + 1] >> b) & 1) << 1) |
                                 (((p[16 + y * 2] >> b) & 1) << 2) | (((p[16 + y * 2 + 1] >> b) & 1) << 3));
    }
  return CtSrcOf(idx, 8, 8);
}
int CtSrcOf2bpp(const uint8_t p[16]) {
  uint8_t idx[64];
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) {
      int b = 7 - x;
      idx[y * 8 + x] = (uint8_t)(((p[y * 2] >> b) & 1) | (((p[y * 2 + 1] >> b) & 1) << 1));
    }
  return CtSrcOf(idx, 8, 8);
}
int CtSrcInfoOf(int src, CtSrcInfo* o) {
  if (src < 1 || (size_t)src > g_nsrc || !o) return 0;
  const Src* s = srcAt((size_t)src);
  o->w = s->w; o->h = s->h; o->idx = s->idx;
  return 1;
}
int CtSrcCount(void) { return (int)g_nsrc; }

// ---- this frame's instances ----------------------------------------------------------------------------------------
static CtSrcInst* g_inst; static int g_ninst, g_capinst, g_groups;
int CtSrcGroup(void) { return ++g_groups; }
int CtSrcDraw(int src, int flags, int group, int x0, int y0) {
  if (!g_ctSrcOn || src < 1) return 0;
  if (g_ninst + 1 >= g_capinst) {
    int nc = g_capinst ? g_capinst * 2 : 4096;
    if (nc > 0xFFFFFF) return 0;
    CtSrcInst* ni = (CtSrcInst*)realloc(g_inst, (size_t)nc * sizeof *ni);
    if (!ni) return 0;
    g_inst = ni; g_capinst = nc;
  }
  CtSrcInst* e = &g_inst[++g_ninst];                 // (ids from 1: 0 = none)
  e->src = src; e->flags = (uint8_t)flags; e->has = 0; e->group = group ? group : ++g_groups;
  e->x0 = (int16_t)x0; e->y0 = (int16_t)y0;
  return g_ninst;
}
const CtSrcInst* CtSrcInstOf(int inst) { return inst >= 1 && inst <= g_ninst ? &g_inst[inst] : NULL; }
int CtSrcInstCount(void) { return g_ninst; }

// ---- the pictures' records -----------------------------------------------------------------------------------------
#define NPIC 8
#define PH 224
typedef struct { const uint8_t* pic; int w, used; uint32_t* tag; uint32_t* seq; uint8_t* rgb; uint8_t* blend;
                 const uint8_t* view; int vw, x0;              // view: this picture shows that one from column x0
                 const uint8_t* native; int nk; } Pic;         // native: its drawer's own k x drawing (CtSrcNative)
static Pic g_pics[NPIC];
static long long g_frames, g_tagged, g_untagged;   // (64 bits: a long run passes 2^31 pixels)
static int alloc1(Pic* p, int w) {
  free(p->tag); free(p->seq); free(p->rgb); free(p->blend);
  p->tag = (uint32_t*)calloc((size_t)w * PH, 4); p->seq = (uint32_t*)calloc((size_t)w * PH, 4);
  p->rgb = (uint8_t*)calloc((size_t)w * PH, 3); p->blend = (uint8_t*)calloc((size_t)w * PH, 1); p->w = w;
  if (!p->tag || !p->seq || !p->rgb || !p->blend) { p->pic = NULL; return 0; }
  return 1;
}
static Pic* find(const uint8_t* pic) {
  for (int i = 0; i < NPIC; i++) if (g_pics[i].pic == pic) return &g_pics[i];
  return NULL;
}
static Pic* picOf(const uint8_t* pic, int w) {     // (a picture's own records; a new picture takes a free slot)
  static Pic* last;
  if (last && last->pic == pic && last->w == w) return last;
  Pic* p = find(pic);
  if (p) {
    if ((p->w != w || !p->tag) && !alloc1(p, w)) return NULL;   // (another width: the records start again)
    return last = p;
  }
  for (int i = 0; i < NPIC; i++)
    if (!g_pics[i].pic) {
      g_pics[i].pic = pic;
      if (!alloc1(&g_pics[i], w)) return NULL;
      return last = &g_pics[i];
    }
  return NULL;
}
static void put1(Pic* p, size_t q, uint32_t tag, int r, int g, int b) {
  p->used = 1; p->blend[q] = 0;
  int inst = (int)(tag >> 8), tx = (int)(tag & 15), ty = (int)((tag >> 4) & 15);
  if (inst >= 1 && inst <= g_ninst) {
    p->tag[q] = tag; p->seq[q] = (uint32_t)inst;
    CtSrcInst* e = &g_inst[inst];
    const Src* s = srcAt((size_t)e->src);
    if (tx < s->w && ty < s->h) {
      int v = s->idx[ty * s->w + tx];
      if (v < 16) { e->col[v][0] = (uint8_t)r; e->col[v][1] = (uint8_t)g; e->col[v][2] = (uint8_t)b; e->has |= (uint16_t)(1u << v); }
    }
    g_tagged++;
  } else {
    p->tag[q] = 0; p->seq[q] = (uint32_t)g_ninst;
    g_untagged++;
  }
  p->rgb[q * 3] = (uint8_t)r; p->rgb[q * 3 + 1] = (uint8_t)g; p->rgb[q * 3 + 2] = (uint8_t)b;
}
void CtSrcPutTag(const uint8_t* pic, int w, int x, int y, uint32_t tag, int r, int g, int b) {
  if (!g_ctSrcOn || (unsigned)x >= (unsigned)w || (unsigned)y >= PH) return;
  Pic* p = picOf(pic, w);
  if (p) put1(p, (size_t)y * w + x, tag, r, g, b);
}
void CtSrcPut(const uint8_t* pic, int w, int x, int y, int inst, int tx, int ty, int r, int g, int b) {
  CtSrcPutTag(pic, w, x, y, inst > 0 ? CtSrcTag(inst, tx, ty) : 0, r, g, b);
}
void CtSrcNone(const uint8_t* pic, int w, int x, int y) {
  if (!g_ctSrcOn || (unsigned)x >= (unsigned)w || (unsigned)y >= PH) return;
  Pic* p = find(pic);
  if (!p || p->w != w || !p->tag) return;          // (nothing recorded there yet -- or an alias: nothing to drop)
  size_t q = (size_t)y * w + x;
  p->tag[q] = 0; p->seq[q] = (uint32_t)g_ninst; p->blend[q] = 0;
}
void CtSrcBlend(const uint8_t* pic, int w, int x, int y) {
  if (!g_ctSrcOn || (unsigned)x >= (unsigned)w || (unsigned)y >= PH) return;
  Pic* p = find(pic);
  if (p && p->w == w && p->blend) p->blend[(size_t)y * w + x] = 1;
}
void CtSrcNoneRect(const uint8_t* pic, int w, int x, int y, int rw, int rh) {
  if (!g_ctSrcOn) return;
  Pic* p = find(pic);
  if (!p || p->w != w || !p->tag) return;
  for (int yy = y < 0 ? 0 : y; yy < y + rh && yy < PH; yy++)
    for (int xx = x < 0 ? 0 : x; xx < x + rw && xx < w; xx++) {
      size_t q = (size_t)yy * w + xx; p->tag[q] = 0; p->seq[q] = (uint32_t)g_ninst; p->blend[q] = 0; }
}
void CtSrcCopyRow(const uint8_t* dst, int dw, int dx, int dy, const uint8_t* src, int sw, int sx, int sy, int n) {
  if (!g_ctSrcOn || (unsigned)dy >= PH || (unsigned)sy >= PH) return;
  Pic* s = find(src);
  if (s && (s->w != sw || !s->used)) s = NULL;
  Pic* d = s ? picOf(dst, dw) : find(dst);
  if (!d || d->w != dw || !d->tag) return;         // (no records to copy into one that has none: nothing to do)
  for (int k = 0; k < n; k++) {
    int x = dx + k, xs = sx + k;
    if ((unsigned)x >= (unsigned)dw) continue;
    size_t q = (size_t)dy * dw + x;
    if (s && (unsigned)xs < (unsigned)sw) {
      size_t qs = (size_t)sy * sw + xs;
      d->tag[q] = s->tag[qs]; d->seq[q] = s->seq[qs]; memcpy(d->rgb + q * 3, s->rgb + qs * 3, 3); d->blend[q] = s->blend[qs];
    } else { d->tag[q] = 0; d->seq[q] = (uint32_t)g_ninst; }
  }
  d->used = 1;
}
void CtSrcShows(const uint8_t* pic, int w, const uint8_t* view, int vw, int x0) {
  if (!g_ctSrcOn || pic == view) return;
  Pic* p = find(pic);
  if (!p) {
    for (int i = 0; i < NPIC && !p; i++) if (!g_pics[i].pic) p = &g_pics[i];
    if (!p) return;
    p->pic = pic; p->w = w; p->tag = NULL; p->seq = NULL; p->rgb = NULL;
  }
  p->view = view; p->vw = vw; p->x0 = x0;
}
void CtSrcNative(const uint8_t* pic, int w, const uint8_t* hd, int k) {
  if (!g_ctSrcOn || !pic || !hd || k < 1) return;
  Pic* p = find(pic);
  if (!p) {
    for (int i = 0; i < NPIC && !p; i++) if (!g_pics[i].pic) p = &g_pics[i];
    if (!p) return;
    p->pic = pic; p->w = w; p->tag = NULL; p->seq = NULL; p->rgb = NULL; p->blend = NULL;
  }
  if (p->w != w) return;                           // (another width: its records are another picture's)
  p->native = hd; p->nk = k;
}
int CtSrcPlaneOf(const uint8_t* pic, int w, CtSrcPlane* o) {
  if (!g_ctSrcOn || !o) return 0;
  Pic* p = find(pic);
  int x0 = 0;
  for (int hop = 0; p && p->view && hop < 4; hop++) { x0 += p->x0; p = find(p->view); }   // (a view of a view: the columns add)
  if (!p) return 0;
  const int recs = p->used && p->tag;
  if (!recs && !p->native) return 0;
  o->w = w; o->vw = p->w; o->x0 = x0;
  o->tag = recs ? p->tag : NULL; o->seq = recs ? p->seq : NULL; o->rgb = recs ? p->rgb : NULL; o->blend = recs ? p->blend : NULL;
  o->native = p->native; o->nk = p->native ? p->nk : 0;
  return 1;
}
void CtSrcFrame(void) {
  if (!g_ctSrcOn) return;
  g_frames++;
  for (int i = 0; i < NPIC; i++) {
    Pic* p = &g_pics[i];
    p->view = NULL; p->native = NULL; p->nk = 0;
    if (p->pic && p->used && p->tag) {
      memset(p->tag, 0, (size_t)p->w * PH * 4);
      memset(p->seq, 0, (size_t)p->w * PH * 4);
      memset(p->blend, 0, (size_t)p->w * PH);
      p->used = 0;
    }
  }
  g_ninst = 0;
}
int CtSrcSelfTest(void) {
  const int was = g_ctSrcOn;
  g_ctSrcOn = 1;
  static uint8_t pic[16 * PH * 3], view[8 * PH * 3];
  CtSrcFrame();
  CtSrcShows(pic, 16, view, 8, 4);                 // pic shows view from its column 4: an alias, no records of its own
  CtSrcNone(pic, 16, 1, 1); CtSrcNoneRect(pic, 16, 0, 0, 4, 4); CtSrcBlend(pic, 16, 2, 2);
  CtSrcCopyRow(pic, 16, 0, 0, view, 8, 0, 0, 8);   // (no records in view yet: nothing to copy)
  CtSrcPlane P;
  int quiet = !CtSrcPlaneOf(pic, 16, &P);          // the writes above took nothing: no records yet
  uint8_t idx[64]; memset(idx, 0, sizeof idx); idx[9] = 3;
  int inst = CtSrcDraw(CtSrcOf(idx, 8, 8), 0, 0, 0, 0);
  CtSrcPut(view, 8, 1, 1, inst, 1, 1, 10, 20, 30);
  long q = -1;
  int through = CtSrcPlaneOf(pic, 16, &P) && (q = CtSrcAt(&P, 5, 1)) >= 0 && P.tag[q] == CtSrcTag(inst, 1, 1) &&
                CtSrcAt(&P, 1, 1) < 0 && CtSrcAt(&P, 12, 1) < 0;
  // a native drawing (CtSrcNative) read through two aliases: top shows pic from 2, pic shows view from 4 -- view's 4x
  // drawing at top's column 2 + 4 + 1 is view's column 1
  static uint8_t top[20 * PH * 3], hd[8 * 4 * PH * 4 * 3];
  CtSrcFrame();
  CtSrcShows(top, 20, pic, 16, 2); CtSrcShows(pic, 16, view, 8, 4);
  CtSrcNative(view, 8, hd, 4);
  int nat = CtSrcPlaneOf(top, 20, &P) && P.native == hd && P.nk == 4 && !P.tag && P.x0 == 6 && CtSrcAt(&P, 7, 3) == 3 * 8 + 1;
  CtSrcFrame();
  nat = nat && !CtSrcPlaneOf(view, 8, &P);         // (and gone with the frame: the drawn picture itself has none left)
  g_ctSrcOn = was;
  printf("hdsrctest: %s -- writes to a picture that shows another: %s; the other's record read through it: %s; "
         "a native 4x drawing read through two: %s\n",
         quiet && through && nat ? "PASS" : "FAIL", quiet ? "none taken" : "TAKEN", through ? "at its column" : "WRONG",
         nat ? "at its column, gone with the frame" : "WRONG");
  return quiet && through && nat;
}
void CtSrcStats(long long* frames, long long* tagged, long long* untagged) {
  if (frames) *frames = g_frames; if (tagged) *tagged = g_tagged; if (untagged) *untagged = g_untagged;
}
void CtSrcExpect(uint32_t tag, int v) {
  if (!tag) return;
  g_chk++;
  const CtSrcInst* e = CtSrcInstOf((int)(tag >> 8));
  CtSrcInfo si;
  int tx = (int)(tag & 15), ty = (int)((tag >> 4) & 15);
  if (!e || !CtSrcInfoOf(e->src, &si) || tx >= si.w || ty >= si.h || si.idx[ty * si.w + tx] != v) {
    g_chkBad++;
    // CT_SRCCHECK=2 (debug): the first 24 that name another index -- the draw's box, size and flags, the texel, both indices
    static int level = -1, said = 0;
    if (level < 0) { const char* c = getenv("CT_SRCCHECK"); level = c ? atoi(c) : 0; }
    if (level >= 2 && said < 24) {
      said++;
      printf("srccheck-miss: frame %lld inst %d src %dx%d at (%d,%d) flags %d group %d texel (%d,%d): drawn %d, the source has %d\n",
             g_frames, (int)(tag >> 8), e && CtSrcInfoOf(e->src, &si) ? si.w : -1, e ? si.h : -1, e ? e->x0 : 0, e ? e->y0 : 0,
             e ? e->flags : 0, e ? e->group : 0, tx, ty, v,
             e && tx < si.w && ty < si.h ? si.idx[ty * si.w + tx] : -1);
    }
  }
}
void CtSrcExpectTrue(int ok) { g_chk++; if (!ok) g_chkBad++; }
void CtSrcCheckStats(long long* checked, long long* wrong) { if (checked) *checked = g_chk; if (wrong) *wrong = g_chkBad; }

// ---- one draw a cell: a drawer that writes tile by tile, pixel by pixel, names each of its cells once a frame ----------
#define CELLCAP (1 << 15)
static uint32_t g_cellKey[CELLCAP]; static int g_cellInst[CELLCAP]; static long g_cellStamp[CELLCAP];
static uint32_t cellSlot(uint32_t key, int* found) {
  uint32_t h = (key * 2654435761u) >> 17;
  for (int probe = 0; probe < 256; probe++, h = (h + 1) & (CELLCAP - 1)) {
    if (g_cellStamp[h] != g_frames + 1) { *found = 0; return h; }
    if (g_cellKey[h] == key) { *found = 1; return h; }
  }
  *found = -1;
  return 0;
}
int CtSrcCellFind(uint32_t key) {
  if (!g_ctSrcOn) return 0;
  int found; uint32_t h = cellSlot(key, &found);
  return found == 1 ? g_cellInst[h] : 0;
}
int CtSrcCellPut(uint32_t key, int inst) {
  if (!g_ctSrcOn || inst < 1) return inst;
  int found; uint32_t h = cellSlot(key, &found);
  if (found >= 0) { g_cellStamp[h] = g_frames + 1; g_cellKey[h] = key; g_cellInst[h] = inst; }
  return inst;
}
