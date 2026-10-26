// hdsrc.h - WHAT EACH PIXEL OF THE PICTURE WAS DRAWN FROM (the HD presenter's description of a picture; a mod: settings.h
// `presenter`, off by default; CLAUDE.md "MODS ARE OUTSIDE THE MANDATE").
//
// The game's compositors draw the picture from 8x8 tiles, sprite cells, font glyphs and window chips, and once a pixel is
// RGB which of them it came from is gone. While the HD presenter is on (g_ctSrcOn), the places that write a pixel from
// one of them say which, as they write it:
//   - a SOURCE: the tile's content, an index map of up to 16x16 (0 = transparent), deduplicated by content -- the key the
//     HD art is looked up by (hdpresent.h; the remaster's tools/cthd.py names it by the SHA-256 of the indices);
//   - an INSTANCE: one draw of a source this frame -- its flips, the colour it wrote for each index, and its GROUP (the
//     draws one object is made of: a sprite's cells, so a cell never rounds into its own sprite's other cells);
//   - the TEXEL of the source the pixel shows.
// The HD presenter (hdpresent.c CtHdSrcBlock) redraws such a pixel at 4x from the source's HD art in exactly the
// original's place, coloured as the frame coloured it; a pixel written without a source is drawn as the frame has it.
// The records are kept per picture (the buffer a compositor writes), and a picture that SHOWS another one (the
// widescreen's assembly round the 256-column view) says so (CtSrcShows). Each frame's present ends them (CtSrcFrame).
//
// Nothing here is read by the game, and with the presenter off nothing is recorded: every call returns at once.
#ifndef CT_HDSRC_H
#define CT_HDSRC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define CT_SRC_MAXWH 16
extern int g_ctSrcOn;                      // 1 while the HD presenter records (hdpresent.c CtHdOpen turns it on)

// ---- sources (a process-wide pool: an id names the same content for the whole run) ----
int CtSrcOf(const uint8_t* idx, int w, int h);        // the source of this content (w, h 1..16): its id >= 1 (0 = full)
int CtSrcOf4bpp(const uint8_t planar[32]);            // an SNES 4bpp 8x8 tile: planes 0/1 in bytes 0-15, 2/3 in 16-31
int CtSrcOf2bpp(const uint8_t planar[16]);            // an SNES 2bpp 8x8 tile
typedef struct CtSrcInfo { int w, h; const uint8_t* idx; } CtSrcInfo;
int CtSrcInfoOf(int src, CtSrcInfo* out);             // 1 = filled
int CtSrcCount(void);                                 // the pool's size (ids 1..count)

// ---- instances (this frame's draws) ----
int CtSrcGroup(void);                                 // a new group (the draws of one object)
// A draw of `src` (flags 1 = x flip, 2 = y flip, CT_SRC_CRISP) in `group` (0 = a group of its own), its box's top-left at
// (x0, y0) of the picture it is drawn into (informative: a presenter that places objects, the 2.5D one): its id >= 1
// (0 = off).
int CtSrcDraw(int src, int flags, int group, int x0, int y0);
// CRISP: text -- the dialogue's, the menus' and the battle's glyphs, the damage digits. The presenter draws such a pixel as
// it is, k x k (nearest-neighbour, whole multiples), never through upscaled art: the automatic pass (and an AI upscale)
// blurs a glyph's strokes (the owner, 2026-10-26). A redrawn font replaces this with its own art (M8).
#define CT_SRC_CRISP 4
typedef struct CtSrcInst { int src, group; int16_t x0, y0; uint8_t flags; uint16_t has; uint8_t col[16][3]; } CtSrcInst;
const CtSrcInst* CtSrcInstOf(int inst);               // NULL = none
int CtSrcInstCount(void);

// ONE DRAW A CELL: a drawer that writes a tile layer pixel by pixel names each cell's draw once a frame -- `key` its cell
// (the drawer's own numbering, unique among the drawers of a frame: CT_SRC_KEY), CtSrcCellFind = 0 until CtSrcCellPut.
#define CT_SRC_KEY(site, n) ((uint32_t)(site) << 24 | ((uint32_t)(n) & 0xFFFFFF))
int CtSrcCellFind(uint32_t key);
int CtSrcCellPut(uint32_t key, int inst);

// ---- the pixels of a picture (`pic`: the buffer, `w` its width; 224 lines) ----
// (x, y) was just written with (r, g, b) from texel (tx, ty) of `inst` -- or, inst 0, by something without a source.
void CtSrcPut(const uint8_t* pic, int w, int x, int y, int inst, int tx, int ty, int r, int g, int b);
static inline uint32_t CtSrcTag(int inst, int tx, int ty) { return (uint32_t)inst << 8 | (uint32_t)(ty & 15) << 4 | (uint32_t)(tx & 15); }
void CtSrcPutTag(const uint8_t* pic, int w, int x, int y, uint32_t tag, int r, int g, int b);   // (the same, a tag)
void CtSrcNone(const uint8_t* pic, int w, int x, int y);
void CtSrcNoneRect(const uint8_t* pic, int w, int x, int y, int rw, int rh);
// (x, y) was changed by colour math over what it records (a translucent layer drawn over it, the fixed colour added): the
// record stands, and the presenter adds the change to the HD art there. Cleared by the next CtSrcPut / CtSrcNone.
void CtSrcBlend(const uint8_t* pic, int w, int x, int y);
// `n` pixels of row sy of `src` (from sx) copied to row dy of `dst` (from dx): their records go with them.
void CtSrcCopyRow(const uint8_t* dst, int dw, int dx, int dy, const uint8_t* src, int sw, int sx, int sy, int n);
// Picture `pic` (w wide) shows picture `view` (vw wide) from its column x0: its records are the view's, moved.
void CtSrcShows(const uint8_t* pic, int w, const uint8_t* view, int vw, int x0);
// Picture `pic` (w wide) was also drawn at k x by its own drawer into `hd` ((w * k) x (224 * k) RGB, valid to the frame's
// end): a pixel without a record is that drawing's k x k block, not the frame's pixel k x k -- what no tile names, a mode-7
// plane, drawn at the presenter's resolution (seq_pic.c). Read through a picture that shows it (CtSrcShows).
void CtSrcNative(const uint8_t* pic, int w, const uint8_t* hd, int k);
// A picture's records: per pixel a tag (CtSrcTag; 0 = no source), the instance count when it was written (paint order:
// a draw with a later id is in front of it) and the colour written. For a picture that shows a view, the view's at x0
// (a view that shows another, that one's: the columns add up). tag NULL = no records, only its native drawing
// (`native`, nk x: CtSrcNative; NULL = none).
typedef struct CtSrcPlane { int w, vw, x0; const uint32_t* tag; const uint32_t* seq; const uint8_t* rgb; const uint8_t* blend;
                            const uint8_t* native; int nk; } CtSrcPlane;
int CtSrcPlaneOf(const uint8_t* pic, int w, CtSrcPlane* out);   // 0 = nothing recorded and no native drawing
static inline long CtSrcAt(const CtSrcPlane* P, int x, int y) {  // the record of pixel (x, y) of the picture, -1 = none
  int vx = x - P->x0;
  return (unsigned)vx < (unsigned)P->vw && (unsigned)y < 224u ? (long)y * P->vw + vx : -1;
}
// The records' contract (Belthasar --hdsrctest): a picture that shows another takes no writes of its own until written
// directly, and reads the other's records through it. Prints `hdsrctest: ...`; 1 = held.
int CtSrcSelfTest(void);
// The frame was presented: this frame's instances and every picture's records are dropped.
void CtSrcFrame(void);
// Counters since the start: frames, pixels written with a source, without one.
void CtSrcStats(long long* frames, long long* tagged, long long* untagged);
// THE RECORDS' CHECK (CT_SRCCHECK, set by hdpresent.c): a drawer says which index it drew where it records a texel; the
// record must name a texel of that index (the source, the flips, the texel's place all right).
extern int g_ctSrcCheck;
void CtSrcExpect(uint32_t tag, int v);
void CtSrcExpectTrue(int ok);                         // (a drawer's own test of its record: 1 = it holds)
void CtSrcCheckStats(long long* checked, long long* wrong);

#ifdef __cplusplus
}
#endif
#endif
