// hdpresent.h - the HD presenter (a mod: settings.h `presenter`, off by default; CLAUDE.md "MODS ARE OUTSIDE THE
// MANDATE"). The field drawn at 4x from HD art keyed by the original chips: each 8x8 chip of the map has a 32x32 HD
// version -- an index map, coloured with the location's LIVE palette at draw time, so palette animation, colour math and
// fades stay the game's. The HD art comes from a pack (<dir>/chips4x.pack, written by the automatic pass, --hdautopass);
// a chip the pack lacks is made by the same automatic pass on the spot and counted as missing.
//
// It reads only what a presenter is handed (scene.h CtSceneLayerCells: the map's pictures as cells; motion.h CtMotFrame:
// the camera, the sprites' pixels, what is screen-fixed) and writes nothing of the game. The remaster's
// docs/ARCHITECTURE.md is the boundary it keeps.
#ifndef CT_HDPRESENT_H
#define CT_HDPRESENT_H
#include <stdint.h>
#include "motion.h"
#include "scene.h"

#define CT_HD_K 4                                  // the HD art's scale: a chip is 32x32

// Load the pack in `dir` (NULL or "": CT_HD_DIR, else "hd"). 1 = a pack was read; 0 = none (every chip then comes from
// the automatic pass). Safe to call again (reloads).
int  CtHdOpen(const char* dir);
const char* CtHdDir(void);
// The 32x32 HD index map (row by row, 0 = transparent, else the chip's own index 1..15) of an 8x8 chip given as its 64
// indices, unflipped. Never NULL.
const uint8_t* CtHdChip(const uint8_t chip[64]);
// The automatic pass: an 8x8 index map to 32x32 by Scale2x twice (EPX; the edges replicated). Indices are compared,
// never mixed, so the result is an index map of the same colours.
void CtHdUpscaleChip(const uint8_t in[64], uint8_t out[32 * 32]);
// The field at t (0..256) between `prev` and `cur` at 4x into `out` (w*4 x 896 RGB): the map from HD pictures built from
// cur->hdCells (scene.h CtSceneLayerCells) with the live palette, the sprites and the screen-fixed pixels as fieldnative.c
// draws them. 0 = it cannot (no cells): the caller draws the frame another way.
int  CtFieldRasterHD(const CtMotFrame* cur, const CtMotFrame* prev, const uint8_t* fb, int w, int t, uint8_t* out);
// The check's art (CT_HDCHECK): every chip drawn as its own pixels, each 4x4, and no pack -- so the HD picture must be
// the composed frame at 4x, pixel for pixel. 0 = the pack and the automatic pass again.
void CtHdNearest(int on);
// Counters: chips asked for, of them made on the spot (the pack lacked them); pictures rebuilt and cells redrawn.
void CtHdStats(long* chips, long* missing, long* rebuilds, long* cells);

// The automatic pass over every location (--hdautopass): every chip a location's pictures draw and every chip of its
// animation sets, each once, upscaled, into <dir>/chips4x.pack. Prints `hdautopass: ...`; 1 = written.
int  CtHdAutoPass(const CtAssets* a, const char* dir);
#endif
