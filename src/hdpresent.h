// hdpresent.h - the HD presenter (a mod: settings.h `presenter`, off by default; CLAUDE.md "MODS ARE OUTSIDE THE
// MANDATE"). Every picture drawn at 4x from HD art keyed by what it is made of: the field's map from its cells (scene.h
// CtFieldSceneLayerCells), every other pixel from the record of the tile, sprite cell or glyph it was drawn from
// (hdsrc.h). Each source has a 4x HD version -- an index map, coloured with the game's LIVE palette at draw time, so
// palette animation, colour math and fades stay the game's. The art comes from a pack (<dir>/chips4x.pack, written by
// the automatic pass, --hdautopass, and by the remaster's tools/cthd.py); a source the pack lacks is made by the same
// automatic pass on the spot and counted.
//
// It reads only what a presenter is handed (the cells; motion.h CtMotFrame: the camera, the sprites' pixels, what is
// screen-fixed; hdsrc.h's records) and writes nothing of the game. The remaster's docs/ARCHITECTURE.md is the boundary
// it keeps; its render-independence check (CT_FRAMEHASH, tools/hd_independence.py) proves it.
#ifndef CT_HDPRESENT_H
#define CT_HDPRESENT_H
#include <stdint.h>
#include "motion.h"
#include "scene.h"
#include "hdsrc.h"

#define CT_HD_K 4                                  // the HD art's scale: an 8x8 chip is 32x32
// What a source is (a pack entry's kinds; tools/cthd.py KINDS): bits, a source may be several
enum { CT_HD_MAP = 1, CT_HD_MAP_ANIM = 2, CT_HD_SPRITE = 4, CT_HD_FONT = 8, CT_HD_UI = 16, CT_HD_WORLD = 32,
       CT_HD_BATTLE = 64, CT_HD_PLAYED = 128 };

// Load the pack in `dir` (NULL or "": CT_HD_DIR, else "hd") and start recording (hdsrc.h g_ctSrcOn). 1 = a pack was
// read; 0 = none (every source then comes from the automatic pass). Safe to call again (reloads).
int  CtHdOpen(const char* dir);
const char* CtHdDir(void);
// The check's art (CT_HDCHECK): every source drawn as its own pixels, each 4x4, and no pack -- so the HD picture must be
// the composed frame at 4x, pixel for pixel. 0 = the pack and the automatic pass again.
void CtHdNearest(int on);
// The HD art of a source (hdsrc.h): its (4w x 4h) index map, row by row, 0 = transparent, else the source's own index.
// Made by the automatic pass when the pack lacks it. NULL only for an invalid source.
const uint8_t* CtHdArt(int src);
void CtHdArtKind(int src, int kinds);              // (what the automatic pass found the source as: CT_HD_*)
// ...of an 8x8 chip given as its 64 indices, unflipped. Never NULL.
const uint8_t* CtHdChip(const uint8_t chip[64]);
// The automatic pass: a w x h index map (w, h <= 16) to 4w x 4h by Scale2x twice (EPX; the edges replicated). Indices are
// compared, never mixed, so the result is an index map of the same colours.
void CtHdUpscale(const uint8_t* in, int w, int h, uint8_t* out);
void CtHdUpscaleChip(const uint8_t in[64], uint8_t out[32 * 32]);
// The field at t (0..256) between `prev` and `cur` at 4x into `out` (w*4 x 896 RGB): the map from HD pictures built from
// cur->hdCells with the live palette, the sprites from their records, the screen-fixed pixels from theirs, all with
// fieldnative.c's rules. 0 = it cannot (no cells): the caller draws the frame another way.
int  CtFieldRasterHD(const CtMotFrame* cur, const CtMotFrame* prev, const uint8_t* fb, int w, int t, uint8_t* out);
// The 4x4 HD block of pixel (x, y) of the frame `fb` (w wide) from the picture's records P: 1 = drawn from HD art, 0 = the
// pixel as the frame has it (out holds it either way).
int  CtHdSrcBlock(const CtSrcPlane* P, const uint8_t* fb, int w, int x, int y, uint8_t out[CT_HD_K * CT_HD_K][3]);
// A whole picture (w x 224) at 4x from its records into `out` (w*4 x 896 RGB). 0 = it has none.
int  CtHdRasterPicture(const uint8_t* fb, int w, uint8_t* out);
// The mode the next pictures are of, for the coverage CT_HD_STATS prints: CT_HDM_FIELD .. CT_HDM_OTHER.
enum { CT_HDM_FIELD = 0, CT_HDM_BATTLE = 1, CT_HDM_MENU = 2, CT_HDM_OVERWORLD = 3, CT_HDM_OTHER = 4, CT_HD_NMODES = 5 };
void CtHdCoverMode(int m);
const uint8_t* CtHdCoverMask(int* w);              // the last picture's, per pixel: 0 black, 1 from HD art, 2 not (a probe)
// CT_HD_STATS: the HD picture the host is about to upload (its n bytes), folded into `hd-pictures:`'s hash -- what the
// presenter drew, frame by frame (the checks pin it under the automatic pass, which is deterministic).
void CtHdHashPicture(const uint8_t* p, size_t n);
// Counters: sources, of them made on the spot (the pack lacked them); map pictures rebuilt and cells redrawn.
void CtHdStats(long* sources, long* missing, long* rebuilds, long* cells);

// The automatic pass (--hdautopass): every chip every location's pictures draw and every chip of their animation sets,
// every sprite's tiles, each once, upscaled, into <dir>/chips4x.pack (v2). Prints `hdautopass: ...`; 1 = written.
int  CtHdAutoPass(const CtAssets* a, const char* dir);
#endif
