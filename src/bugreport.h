// bugreport.h - the in-game bug reporter's engine side: the session's input log, the report it writes, and the replay
// that plays a report's inputs back. A host feature, outside the ROM mandate like a mod (CLAUDE.md): it reads the game
// and never writes it, so with the reporter unused the run is the vanilla game byte for byte.
//
// THE PLAYER'S SIDE (field.c, overlay.cpp): `~` (the key left of 1, SDL_SCANCODE_GRAVE) pauses the game over a panel
// that takes a note and a category; holding a DualSense's touchpad button for a second takes a report at once with no
// text ("note to add"). The panel is the presenter's; everything a report holds is written here, so every host writes
// the same reports.
//
// THE INPUT LOG. A session's input is what the host hands the game each frame: the LEVEL (the controller word in
// LakeSnes serial order -- B=1 Y=2 Sel=4 Start=8 Up=10 Down=20 Left=40 Right=80 A=100 X=200 L=400 R=800 -- the keys,
// the pad, its stick and the touch pad through the player's bindings) and the EDGES the host made from that frame's
// events (CT_BUG_EDGE_*). Both are kept from the session's first frame, run-length coded (a few bytes per change of
// input). A frame is one pass of the host's main loop: the log's frame k is the k-th pass of the session.
//
// THE REPLAY (CT_PLAYINPUT=<inputs.log>): the host takes each frame's level and edges from the log instead of SDL, so a
// headless run from the same start and battery (CT_SRAM) plays the session again. CT_REPLAY_CHECK=<report dir> grades
// it at the report's frame: the 128 KB of work RAM against the report's wram.bin and the 256x224 picture against its
// hash, one line (`bugreport-replay: ...`). The cartridge's side of a replay (the same controller words in the oracle)
// is the remaster's tools/bugs.py.
#ifndef CT_BUGREPORT_H
#define CT_BUGREPORT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// the host's edges (field.c's event flags), one bit each in the log
enum {
  CT_BUG_EDGE_A     = 0x001,   // talkPressed: A's press (talk, confirm)
  CT_BUG_EDGE_UP    = 0x002,   // upPressed .. rightPressed: the d-pad's presses (cursors, choices)
  CT_BUG_EDGE_DOWN  = 0x004,
  CT_BUG_EDGE_LEFT  = 0x008,
  CT_BUG_EDGE_RIGHT = 0x010,
  CT_BUG_EDGE_NMTYPE = 0x020,  // the naming screen's type / backspace / confirm (A, B, Start)
  CT_BUG_EDGE_NMDEL = 0x040,
  CT_BUG_EDGE_NMOK  = 0x080,
  CT_BUG_EDGE_F8    = 0x100,   // F8, the battle's debug instant end (C1/8357's controller-2 debug): not the cartridge's
};

// what a report is about (the panel's list; report.json's "category")
#define CT_BUG_NCAT 7
extern const char* const g_ctBugCategories[CT_BUG_NCAT];   // picture, sound, text, gameplay, controls, crash or freeze, other

// ---- the session ------------------------------------------------------------------------------------------------
// The session starts: how (`kind`: "boot" = the cartridge's power-on, else what the host was told to start from), and the
// battery image it loaded (NULL = none). Clears the log.
void CtBugSessionStart(const char* kind, const uint8_t* sram, size_t sramN);
const char* CtBugSessionKind(void);
// One frame's input, in order (the session's frame k is the k-th call).
void CtBugInputFrame(uint16_t level, uint16_t edges);
long CtBugInputFrames(void);
// A line in the log at the current frame (an option changed, a debug key): `replay` 0 marks the session's replay
// approximate from here (the reason is the text).
void CtBugEvent(int replay, const char* fmt, ...);
// The log as written to inputs.log (malloc'd, NUL-terminated; the caller frees).
char* CtBugInputText(void);

// ---- the replay ---------------------------------------------------------------------------------------------------
// Load an inputs.log; 1 = loaded. err gets why not.
int CtBugPlayOpen(const char* path, char* err, size_t errN);
// The log's frame k: 1 = a frame of it (level and edges set), 0 = past its end (both 0).
int CtBugPlayFrame(long k, uint16_t* level, uint16_t* edges);
long CtBugPlayFrames(void);
// The report a replay is graded against (CT_REPLAY_CHECK): 1 = read; *frame = the frame it was taken at.
int CtBugCheckOpen(const char* reportDir, long* frame, char* err, size_t errN);
// Grade the replay at the report's frame: work RAM (128 KB: $7E and $7F) and the 256x224 RGB picture. Prints the
// `bugreport-replay:` line; 1 = both identical.
int CtBugCheck(const uint8_t* wram, const uint8_t* native);

// ---- the report ---------------------------------------------------------------------------------------------------
typedef struct CtBugFacts {
  const char* note;              // the player's words ("" with noteToAdd: the DualSense report)
  int noteToAdd;
  const char* category;          // picture, sound, text, gameplay, controls, crash, other
  long frame;                    // the session's frame (the log's length: the replay stops here)
  long hostFrame;                // the host's frameNo (its own numbering: the attract vehicle starts at -91)
  const char* mode;              // field, overworld, battle, menu, sequence
  int location;                  // $7E:0100 & 0x1FF
  int haveLead; double leadX, leadY; int leadZ, leadFacing;   // the field's lead, when there is one
  const uint8_t* wram;           // the 128 KB work RAM image ($7E:0000-FFFF then $7F:0000-FFFF)
  const uint8_t* screen; int screenW, screenH;   // what the player saw (the window), RGB24; NULL = none
  const uint8_t* native;         // the 256x224 picture behind it, RGB24
  const char* options;           // the host's options in force, as JSON members ("\"presenter\": ..., ...")
  const char* exePath;           // the executable (its SHA-1 identifies the build)
} CtBugFacts;
// The reports' folder: CT_BUG_DIR, else bug_reports in the working directory.
const char* CtBugDir(void);
// Write a report: its folder <dir>/<NNNN>_<date>_<time>_loc<LLL>, report.json, wram.bin, screen.png, screen_native.png,
// inputs.log, sram.srm, console.txt, and a line in <dir>/index.csv. Returns its number (0 = failed); `outDir` gets the
// folder, where the host adds its own files (scene.state and its .apu).
int CtBugReportWrite(const CtBugFacts* f, char* outDir, size_t outN);

// ---- the console (bugreport_console.c) --------------------------------------------------------------------------
// The last lines the process printed (stdout and stderr), for console.txt. CtBugConsoleStart tees both through a pipe
// into a ring and on to where they went before; idempotent. Started by an interactive host only: a headless run's
// output stays exactly as it was.
void CtBugConsoleStart(void);
char* CtBugConsoleText(void);    // malloc'd, NUL-terminated (the caller frees); NULL = no console kept

// ---- helpers ----------------------------------------------------------------------------------------------------------
int CtBugPngWrite(const char* path, const uint8_t* rgb, int w, int h);   // RGB24 -> PNG (zlib, fixed Huffman); 1 = written
uint32_t CtBugFnv(const uint8_t* p, size_t n);                               // FNV-1a 32
#ifdef __cplusplus
}
#endif
#endif
