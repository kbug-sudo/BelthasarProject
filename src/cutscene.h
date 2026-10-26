// cutscene.h - THE CUTSCENE MOD (settings.h `cutscenes`, the F1 overlay's Video tab; docs/CUTSCENES.md): a player's own
// video clips -- the remaster's are the PS1 release's animated scenes -- played at the moments of the game they belong to.
//
// A mod, outside the ROM mandate (CLAUDE.md "MODS ARE OUTSIDE THE MANDATE"): off by default, read only by an interactive
// run, and it never touches the game. The host (field.c) looks at the game's state between two frames -- the location
// $7E:0100 and the story byte $7F:0000, read only -- and when a clip's moment arrives it stops ticking the game, as the F1
// overlay does (no frame, no sound), plays the clip over the window, and the game goes on from the same frame: its frames,
// RAM, battery and sound are those of a run without the clip (the check cutscene-session-leaves-the-game-untouched).
//
// The clips and their moments are a LIST in the clip folder (CT_CUTSCENE_DIR, `cutscenes` beside the game by default):
// `cutscenes.txt`,
//     ct-cutscenes 1
//     # comment
//     <id> <file> power_on                 the power-on session's first frame, before the title
//     <id> <file> at <loc hex> <story hex> [<addr>[&<mask>]=<byte> ...] [story_change]
//                                          the first frame on which `location == loc and story == story` (and each
//                                          named byte of $7E:0000-$7F:FFFF, masked, holds its value: up to four, for a
//                                          moment a scene's own flags mark inside one location and story value);
//                                          `story_change`: only on the frame the story byte becomes `story` while the
//                                          location stays -- a moment whose value outlives its scene (a later visit, or
//                                          a save loaded there, is not the moment)
// -- once: walking out of the room and back in does not play it again; it is met again only after the story byte has
// been 00 -- the title, the attract and a new game, where every reload of a save passes (the byte is not always the story
// in between: the time gate's warp holds 40 in it). The video is an
// MPEG program stream with MPEG-1 video and MP2 audio, decoded by pl_mpeg (tools/get_pl_mpeg.py -> third_party/pl_mpeg;
// without it the mod is compiled out and says so).
#ifndef CT_CUTSCENE_H
#define CT_CUTSCENE_H
#include <stdint.h>
#include "SDL.h"
#ifdef __cplusplus
extern "C" {
#endif

#define CT_CUTSCENE_MAX 32

// The list in `dir` (NULL: CT_CUTSCENE_DIR, else "cutscenes"); the number of clips read (0: none, `err` says why).
int         CtCutsceneLoad(const char* dir, char* err, int errN);
int         CtCutsceneCount(void);
const char* CtCutsceneId(int i);
// The state of the mod for the options: 1 = clips can be played; `why` says what is there or what is missing.
int         CtCutsceneStatus(char* why, int whyN);
// The frame's moments, from the game's work RAM as the frame before left it (128 KB; $7F:0000 at +0x10000) -- read only.
// `powerOnFrame`: this is the first frame of a power-on session. Every clip's moment is followed whether or not the mod is
// on (turning it on in the middle of a moment plays nothing). Fills `due` with the clips whose moments begin now, in the
// list's order (at most `maxDue`); returns how many.
int         CtCutsceneDue(const uint8_t* wram, int powerOnFrame, int* due, int maxDue);
// (the moment of clip i, for the host's line: kind 0 = power_on, 1 = at; loc / story)
void        CtCutsceneMoment(int i, int* kind, int* loc, int* story);
// CT_CUTSCENE_SELFTEST (test): the loaded list's moments over a scripted run -- `seq` is frames `loc:story` (hex) or `on`
// (a power-on frame), comma-separated -- one `cutscene-selftest: <k> ...: <ids due>` line a frame. Returns the frames run.
int         CtCutsceneSelftest(const char* seq);

// ---- one clip's playback -------------------------------------------------------------------------------------------
typedef struct CtClip CtClip;
// Opens clip i of the list; its sound goes to `dev` (the game's device: S16 stereo at devHz; 0 = no sound) at `gain`
// (0..100, the player's volume). NULL = not played (`err` says why).
CtClip*        CtClipOpen(int i, SDL_AudioDeviceID dev, int devHz, int gain, char* err, int errN);
// Decodes `seconds` of the clip (its pictures and its sound queued on the device); 1 = a new picture is ready.
int            CtClipAdvance(CtClip* c, double seconds);
const uint8_t* CtClipPicture(CtClip* c, int* w, int* h);   // RGB24, the latest picture (NULL before the first)
int            CtClipEnded(CtClip* c);
long           CtClipPictures(CtClip* c);                   // pictures decoded so far
long           CtClipSamples(CtClip* c);                    // sound frames queued so far (stereo pairs)
void           CtClipClose(CtClip* c);

#ifdef __cplusplus
}
#endif
#endif
