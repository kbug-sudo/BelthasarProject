// bugreport.c - the bug reporter's engine side (bugreport.h): the input log, the replay, the report, a PNG writer.
// Plain C and stdio; the console's tee is bugreport_console.c.
#include "bugreport.h"
#include "rom_verify.h"                 // CtSha1Hex
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define ct_mkdir(p) _mkdir(p)
#else
#include <dirent.h>
#include <sys/stat.h>
#define ct_mkdir(p) mkdir(p, 0777)
#endif

const char* const g_ctBugCategories[CT_BUG_NCAT] = { "picture", "sound", "text", "gameplay", "controls", "crash or freeze",
                                                     "other" };

#define BUG_WRAM 0x20000
#define BUG_SRAM 0x2000
#define NATIVE_W 256
#define NATIVE_H 224

uint32_t CtBugFnv(const uint8_t* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}

// ---- the session's log ------------------------------------------------------------------------------------------
typedef struct { uint32_t n; uint16_t level, edges; } Run;
typedef struct { long at; char text[200]; } Ev;
static Run* g_runs; static long g_nRuns, g_capRuns, g_frames;
static Ev* g_ev; static long g_nEv, g_capEv;
static char g_kind[64] = "none";
static uint8_t g_sram[BUG_SRAM]; static size_t g_sramN;
static char g_approx[200];             // the first reason the session's replay is approximate ("" = exact)

void CtBugSessionStart(const char* kind, const uint8_t* sram, size_t sramN) {
  snprintf(g_kind, sizeof g_kind, "%s", kind && *kind ? kind : "none");
  g_nRuns = 0; g_frames = 0; g_nEv = 0; g_approx[0] = 0;
  g_sramN = sram ? (sramN < BUG_SRAM ? sramN : BUG_SRAM) : 0;
  if (g_sramN) memcpy(g_sram, sram, g_sramN);
}
const char* CtBugSessionKind(void) { return g_kind; }
long CtBugInputFrames(void) { return g_frames; }

void CtBugInputFrame(uint16_t level, uint16_t edges) {
  if (g_nRuns && g_runs[g_nRuns - 1].level == level && g_runs[g_nRuns - 1].edges == edges && g_runs[g_nRuns - 1].n < 0xFFFFFFF0u) {
    g_runs[g_nRuns - 1].n++;
  } else {
    if (g_nRuns == g_capRuns) {
      long cap = g_capRuns ? g_capRuns * 2 : 4096;
      Run* r = (Run*)realloc(g_runs, (size_t)cap * sizeof *r);
      if (!r) return;                                   // (out of memory: the log stops growing; the game goes on)
      g_runs = r; g_capRuns = cap;
    }
    g_runs[g_nRuns].n = 1; g_runs[g_nRuns].level = level; g_runs[g_nRuns].edges = edges; g_nRuns++;
  }
  g_frames++;
}

void CtBugEvent(int replay, const char* fmt, ...) {
  if (g_nEv == g_capEv) {
    long cap = g_capEv ? g_capEv * 2 : 64;
    Ev* e = (Ev*)realloc(g_ev, (size_t)cap * sizeof *e);
    if (!e) return;
    g_ev = e; g_capEv = cap;
  }
  Ev* e = &g_ev[g_nEv++];
  e->at = g_frames;
  va_list ap; va_start(ap, fmt); vsnprintf(e->text, sizeof e->text, fmt, ap); va_end(ap);
  for (char* q = e->text; *q; q++) if (*q == '\n' || *q == '\r') *q = ' ';
  if (!replay && !g_approx[0]) snprintf(g_approx, sizeof g_approx, "frame %ld: %s", e->at, e->text);
}

typedef struct { char* p; size_t n, cap; } Buf;
static void bput(Buf* b, const char* fmt, ...) {
  for (;;) {
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, fmt, ap);
    va_end(ap);
    if (k < 0) return;
    if (b->p && b->n + (size_t)k < b->cap) { b->n += (size_t)k; return; }
    size_t cap = b->cap ? b->cap * 2 : 4096;
    while (cap < b->n + (size_t)k + 1) cap *= 2;
    char* np = (char*)realloc(b->p, cap);
    if (!np) return;
    b->p = np; b->cap = cap;
  }
}

static void sramSha(char out[41]) {
  if (g_sramN) CtSha1Hex(g_sram, (long)g_sramN, out); else snprintf(out, 41, "none");
}

char* CtBugInputText(void) {
  Buf b = { 0 };
  char sh[41]; sramSha(sh);
  bput(&b, "ct-inputs 1\n"
           "# One line per run of frames: <frames> <level> <edges>, hex. The level is the controller word in LakeSnes serial\n"
           "# order (B=1 Y=2 Sel=4 Start=8 Up=10 Down=20 Left=40 Right=80 A=100 X=200 L=400 R=800); the edges are the host's\n"
           "# presses from that frame's events (A=1 Up=2 Down=4 Left=8 Right=10 NameType=20 NameDel=40 NameOk=80 F8=100).\n"
           "# '@<frame> ...' is an event at that frame. src/bugreport.h.\n");
  bput(&b, "start %s sram %s\n", g_kind, sh);
  long p = 0, e = 0;
  for (long i = 0; i < g_nRuns; i++) {
    long n = (long)g_runs[i].n;
    while (e < g_nEv && g_ev[e].at < p + n) {           // an event inside this run: split the run there
      long k = g_ev[e].at - p;
      if (k > 0) { bput(&b, "%ld %03X %03X\n", k, g_runs[i].level, g_runs[i].edges); p += k; n -= k; }
      bput(&b, "@%ld %s\n", g_ev[e].at, g_ev[e].text); e++;
    }
    if (n > 0) bput(&b, "%ld %03X %03X\n", n, g_runs[i].level, g_runs[i].edges);
    p += n;
  }
  for (; e < g_nEv; e++) bput(&b, "@%ld %s\n", g_ev[e].at, g_ev[e].text);
  bput(&b, "end %ld\n", g_frames);
  return b.p;
}

// ---- the replay -------------------------------------------------------------------------------------------------
typedef struct { long at; uint16_t level, edges; } PRun;   // a run starting at frame `at`
static PRun* g_play; static long g_nPlay, g_playFrames, g_playCur;

int CtBugPlayOpen(const char* path, char* err, size_t errN) {
  FILE* f = fopen(path, "rb");
  if (!f) { snprintf(err, errN, "cannot open %s", path); return 0; }
  char line[512]; long cap = 0, at = 0, endAt = -1; int head = 0;
  g_nPlay = 0;
  while (fgets(line, sizeof line, f)) {
    if (!head) { if (strncmp(line, "ct-inputs 1", 11)) { snprintf(err, errN, "%s is not an inputs log", path); fclose(f); return 0; }
                 head = 1; continue; }
    if (line[0] == '#' || line[0] == '@' || line[0] == '\n' || line[0] == '\r' || !strncmp(line, "start ", 6)) continue;
    if (!strncmp(line, "end ", 4)) { endAt = atol(line + 4); break; }
    long n = 0; unsigned lv = 0, ed = 0;
    if (sscanf(line, "%ld %x %x", &n, &lv, &ed) != 3 || n <= 0) {
      snprintf(err, errN, "%s: bad line '%.40s'", path, line); fclose(f); return 0; }
    if (g_nPlay == cap) { cap = cap ? cap * 2 : 4096; PRun* r = (PRun*)realloc(g_play, (size_t)cap * sizeof *r);
                          if (!r) { snprintf(err, errN, "out of memory"); fclose(f); return 0; } g_play = r; }
    g_play[g_nPlay].at = at; g_play[g_nPlay].level = (uint16_t)lv; g_play[g_nPlay].edges = (uint16_t)ed; g_nPlay++;
    at += n;
  }
  fclose(f);
  if (endAt != at) { snprintf(err, errN, "%s: the runs hold %ld frames, its end line says %ld", path, at, endAt); return 0; }
  g_playFrames = at; g_playCur = 0;
  return 1;
}
long CtBugPlayFrames(void) { return g_playFrames; }

int CtBugPlayFrame(long k, uint16_t* level, uint16_t* edges) {
  *level = 0; *edges = 0;
  if (k < 0 || k >= g_playFrames || !g_nPlay) return 0;
  if (g_playCur >= g_nPlay || g_play[g_playCur].at > k) g_playCur = 0;          // (a jump back: search from the start)
  while (g_playCur + 1 < g_nPlay && g_play[g_playCur + 1].at <= k) g_playCur++;
  *level = g_play[g_playCur].level; *edges = g_play[g_playCur].edges;
  return 1;
}

// the report a replay is graded against
static uint8_t* g_chkWram; static long g_chkFrame = -1; static uint32_t g_chkNative; static int g_chkNumber;
static char* readAll(const char* path, size_t* n) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
  char* p = (char*)malloc((size_t)len + 1);
  if (p) { size_t got = fread(p, 1, (size_t)len, f); p[got] = 0; if (n) *n = got; }
  fclose(f);
  return p;
}
static const char* jsonField(const char* js, const char* key) {   // the text after `"key":` (a flat field of ours)
  char k[80]; snprintf(k, sizeof k, "\"%s\":", key);
  const char* q = strstr(js, k);
  if (!q) return NULL;
  q += strlen(k);
  while (*q == ' ') q++;
  return q;
}
int CtBugCheckOpen(const char* dir, long* frame, char* err, size_t errN) {
  char p[1024]; size_t n = 0;
  snprintf(p, sizeof p, "%s/report.json", dir);
  char* js = readAll(p, NULL);
  if (!js) { snprintf(err, errN, "no %s", p); return 0; }
  const char* fr = jsonField(js, "frame"), * nf = jsonField(js, "native_fnv"), * nb = jsonField(js, "report");
  if (!fr || !nf) { snprintf(err, errN, "%s has no frame / native_fnv", p); free(js); return 0; }
  g_chkFrame = atol(fr); g_chkNative = (uint32_t)strtoul(nf + 1, NULL, 16); g_chkNumber = nb ? atoi(nb) : 0;
  free(js);
  snprintf(p, sizeof p, "%s/wram.bin", dir);
  free(g_chkWram); g_chkWram = (uint8_t*)readAll(p, &n);
  if (!g_chkWram || n != BUG_WRAM) { snprintf(err, errN, "%s is not %d bytes", p, BUG_WRAM); return 0; }
  *frame = g_chkFrame;
  return 1;
}
int CtBugCheck(const uint8_t* wram, const uint8_t* native) {
  long same = 0, first = -1;
  for (long i = 0; i < BUG_WRAM; i++) { if (wram[i] == g_chkWram[i]) same++; else if (first < 0) first = i; }
  uint32_t nf = CtBugFnv(native, (size_t)NATIVE_W * NATIVE_H * 3);
  printf("bugreport-replay: report %04d frame %ld: wram %ld/%d identical", g_chkNumber, g_chkFrame, same, BUG_WRAM);
  if (first >= 0) printf(" (first difference $%02X:%04lX replay %02X report %02X)", first < 0x10000 ? 0x7E : 0x7F,
                         first & 0xFFFF, wram[first], g_chkWram[first]);
  if (nf == g_chkNative) printf(", picture identical (fnv %08X)\n", nf);
  else printf(", picture differs (fnv %08X, report %08X)\n", nf, g_chkNative);
  fflush(stdout);
  return first < 0 && nf == g_chkNative;
}

// ---- the report -------------------------------------------------------------------------------------------------
const char* CtBugDir(void) {
  const char* d = getenv("CT_BUG_DIR");
  return d && *d ? d : "bug_reports";
}
static void mkdirs(const char* path) {
  char p[1024]; snprintf(p, sizeof p, "%s", path);
  for (char* q = p + 1; *q; q++)
    if (*q == '/' || *q == '\\') { char c = *q; *q = 0; if (!(q == p + 2 && p[1] == ':')) ct_mkdir(p); *q = c; }
  ct_mkdir(p);
}
static int highestReport(const char* dir) {     // the highest NNNN_ folder in dir
  int hi = 0;
#ifdef _WIN32
  char pat[1024]; snprintf(pat, sizeof pat, "%s\\*", dir);
  WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do { const char* s = fd.cFileName;
       if (strlen(s) > 5 && s[4] == '_' && s[0] >= '0' && s[0] <= '9') { int n = atoi(s); if (n > hi) hi = n; }
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR* d = opendir(dir);
  if (!d) return 0;
  for (struct dirent* e; (e = readdir(d)) != NULL; ) { const char* s = e->d_name;
    if (strlen(s) > 5 && s[4] == '_' && s[0] >= '0' && s[0] <= '9') { int n = atoi(s); if (n > hi) hi = n; } }
  closedir(d);
#endif
  return hi;
}
static void jsonStr(FILE* f, const char* s) {
  fputc('"', f);
  for (const unsigned char* q = (const unsigned char*)(s ? s : ""); *q; q++) {
    if (*q == '"' || *q == '\\') { fputc('\\', f); fputc(*q, f); }
    else if (*q == '\n') fputs("\\n", f);
    else if (*q == '\r') fputs("\\r", f);
    else if (*q == '\t') fputs("\\t", f);
    else if (*q < 0x20) fprintf(f, "\\u%04x", *q);
    else fputc(*q, f);
  }
  fputc('"', f);
}
static int writeFile(const char* dir, const char* name, const void* p, size_t n) {
  char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE* f = fopen(path, "wb");
  if (!f) return 0;
  size_t w = fwrite(p, 1, n, f);
  return fclose(f) == 0 && w == n;
}
static void firstLine(const char* s, char* out, size_t n) {     // the note's first line, for the index
  size_t k = 0;
  for (; s && *s && *s != '\n' && *s != '\r' && k + 1 < n; s++) out[k++] = *s;
  out[k] = 0;
}
// META: CT_BUG_META="key=value key=value" from the launcher (the commits it ran from), as JSON members
static void jsonMeta(FILE* f) {
  const char* m = getenv("CT_BUG_META");
  fputs("{", f);
  int first = 1;
  for (const char* q = m; q && *q; ) {
    while (*q == ' ') q++;
    const char* eq = strchr(q, '='); const char* sp = strchr(q, ' ');
    if (!*q || !eq || (sp && sp < eq)) break;
    if (!sp) sp = q + strlen(q);
    char k[64], v[256];
    snprintf(k, sizeof k, "%.*s", (int)(eq - q), q); snprintf(v, sizeof v, "%.*s", (int)(sp - eq - 1), eq + 1);
    fputs(first ? "" : ", ", f); jsonStr(f, k); fputs(": ", f); jsonStr(f, v); first = 0;
    q = sp;
  }
  fputs("}", f);
}

int CtBugReportWrite(const CtBugFacts* f, char* outDir, size_t outN) {
  const char* base = CtBugDir();
  mkdirs(base);
  time_t t = time(NULL); struct tm lt = *localtime(&t);
  char day[16], tod[16], taken[32];
  strftime(day, sizeof day, "%Y-%m-%d", &lt); strftime(tod, sizeof tod, "%H-%M-%S", &lt);
  strftime(taken, sizeof taken, "%Y-%m-%d %H:%M:%S", &lt);
  int num = highestReport(base) + 1;
  char dir[1024];
  snprintf(dir, sizeof dir, "%s/%04d_%s_%s_loc%03X", base, num, day, tod, f->location & 0x1FF);
  if (ct_mkdir(dir) != 0) { fprintf(stderr, "[bugreport] cannot make %s\n", dir); return 0; }
  if (outDir) snprintf(outDir, outN, "%s", dir);

  const uint8_t* w = f->wram;
  uint32_t wf = w ? CtBugFnv(w, BUG_WRAM) : 0, nf = f->native ? CtBugFnv(f->native, (size_t)NATIVE_W * NATIVE_H * 3) : 0;
  int ok = 1;
  if (w) ok &= writeFile(dir, "wram.bin", w, BUG_WRAM);
  { char p[1024];
    if (f->native) { snprintf(p, sizeof p, "%s/screen_native.png", dir); ok &= CtBugPngWrite(p, f->native, NATIVE_W, NATIVE_H); }
    if (f->screen && f->screenW > 0 && f->screenH > 0) {
      snprintf(p, sizeof p, "%s/screen.png", dir); ok &= CtBugPngWrite(p, f->screen, f->screenW, f->screenH); } }
  { char* in = CtBugInputText();
    if (in) { ok &= writeFile(dir, "inputs.log", in, strlen(in)); free(in); } else ok = 0; }
  if (g_sramN) ok &= writeFile(dir, "sram.srm", g_sram, g_sramN);
  { char* con = CtBugConsoleText();
    const char* none = "(this host kept no console lines: a headless run's output is its own)\n";
    ok &= con ? writeFile(dir, "console.txt", con, strlen(con)) : writeFile(dir, "console.txt", none, strlen(none));
    free(con); }
  char exeSha[41] = "unknown";
  if (f->exePath) { size_t n = 0; char* ex = readAll(f->exePath, &n); if (ex) { CtSha1Hex((const uint8_t*)ex, (long)n, exeSha); free(ex); } }
  char srSha[41]; sramSha(srSha);
  int replayable = !strcmp(g_kind, "boot");

  char p[1024]; snprintf(p, sizeof p, "%s/report.json", dir);
  FILE* js = fopen(p, "wb");
  if (!js) { fprintf(stderr, "[bugreport] cannot write %s\n", p); return 0; }
  fprintf(js, "{\n  \"report\": %d,\n  \"version\": 1,\n  \"note\": ", num); jsonStr(js, f->note);
  fprintf(js, ",\n  \"note_to_add\": %s,\n  \"category\": ", f->noteToAdd ? "true" : "false"); jsonStr(js, f->category);
  fprintf(js, ",\n  \"taken\": "); jsonStr(js, taken);
  fprintf(js, ",\n  \"frame\": %ld,\n  \"host_frame\": %ld,\n  \"mode\": ", f->frame, f->hostFrame); jsonStr(js, f->mode);
  fprintf(js, ",\n  \"location\": \"%03X\"", f->location & 0x1FF);
  if (w) {
    fprintf(js, ",\n  \"play_time\": \"%d%d:%d%d:%02d\"", w[0x405], w[0x404], w[0x403], w[0x402], w[0x401]);
    fprintf(js, ",\n  \"party\": [%d, %d, %d]", w[0x2980], w[0x2981], w[0x2982]);
    fprintf(js, ",\n  \"story_flags\": \"");
    for (int i = 0; i < 0x200; i++) fprintf(js, "%02X", w[0x10000 + i]);
    fputc('"', js);
  }
  if (f->haveLead) fprintf(js, ",\n  \"lead\": {\"x\": %.2f, \"y\": %.2f, \"z\": %d, \"facing\": %d}", f->leadX, f->leadY, f->leadZ, f->leadFacing);
  fprintf(js, ",\n  \"options\": {%s}", f->options ? f->options : "");
  fprintf(js, ",\n  \"session\": {\"start\": "); jsonStr(js, g_kind);
  fprintf(js, ", \"frames\": %ld, \"sram_sha1\": \"%s\", \"replay\": ", g_frames, srSha);
  { char rp[300];
    if (!replayable) snprintf(rp, sizeof rp, "unavailable: the session did not start at power-on (%s)", g_kind);
    else if (g_approx[0]) snprintf(rp, sizeof rp, "approximate: %s", g_approx);
    else snprintf(rp, sizeof rp, "exact");
    jsonStr(js, rp); }
  fprintf(js, "}");
  fprintf(js, ",\n  \"build\": {\"exe\": "); jsonStr(js, f->exePath ? f->exePath : "");
  fprintf(js, ", \"exe_sha1\": \"%s\", \"meta\": ", exeSha); jsonMeta(js); fputc('}', js);
  fprintf(js, ",\n  \"hashes\": {\"wram_fnv\": \"%08X\", \"native_fnv\": \"%08X\"}", wf, nf);
  fprintf(js, ",\n  \"files\": [\"report.json\", \"wram.bin\", \"screen.png\", \"screen_native.png\", \"inputs.log\"%s, \"console.txt\"]",
          g_sramN ? ", \"sram.srm\"" : "");
  fprintf(js, ",\n  \"status\": \"open\"\n}\n");
  ok &= fclose(js) == 0;

  // the index: one line a report
  { snprintf(p, sizeof p, "%s/index.csv", base);
    FILE* ix = fopen(p, "rb"); int fresh = ix == NULL; if (ix) fclose(ix);
    ix = fopen(p, "ab");
    if (ix) {
      if (fresh) fputs("number,date,location,category,note,status\n", ix);
      char fl[200]; firstLine(f->noteToAdd && !(f->note && *f->note) ? "(note to add)" : f->note, fl, sizeof fl);
      fprintf(ix, "%04d,%s,%03X,%s,\"", num, taken, f->location & 0x1FF, f->category ? f->category : "");
      for (const char* q = fl; *q; q++) { if (*q == '"') fputc('"', ix); fputc(*q, ix); }
      fputs("\",open\n", ix);
      ok &= fclose(ix) == 0;
    } else ok = 0; }
  if (!ok) fprintf(stderr, "[bugreport] report %04d: some files were not written (%s)\n", num, dir);
  return num;
}

// ---- PNG: RGB24, each row filtered Up, one fixed-Huffman deflate block with a greedy LZ77 -------------------------
typedef struct { uint8_t* p; size_t n, cap; uint32_t bits; int nb; } Bits;
static void bgrow(Bits* b, size_t need) {
  if (b->n + need <= b->cap) return;
  size_t cap = b->cap ? b->cap : 65536;
  while (cap < b->n + need) cap *= 2;
  uint8_t* np = (uint8_t*)realloc(b->p, cap);
  if (np) { b->p = np; b->cap = cap; }
}
static void bbyte(Bits* b, uint8_t v) { bgrow(b, 1); if (b->n < b->cap) b->p[b->n++] = v; }
static void bput32(Bits* b, uint32_t v) { bbyte(b, v >> 24); bbyte(b, v >> 16); bbyte(b, v >> 8); bbyte(b, v); }
static void bitsPut(Bits* b, uint32_t v, int n) {                 // LSB first, as deflate packs its fields
  b->bits |= v << b->nb; b->nb += n;
  while (b->nb >= 8) { bbyte(b, (uint8_t)b->bits); b->bits >>= 8; b->nb -= 8; }
}
static void huffPut(Bits* b, uint32_t code, int n) {               // a Huffman code goes in MSB first
  uint32_t r = 0;
  for (int i = 0; i < n; i++) r |= ((code >> i) & 1u) << (n - 1 - i);
  bitsPut(b, r, n);
}
static void litPut(Bits* b, int s) {                               // RFC 1951 3.2.6, the fixed literal/length code
  if (s < 144) huffPut(b, 0x30 + s, 8);
  else if (s < 256) huffPut(b, 0x190 + s - 144, 9);
  else if (s < 280) huffPut(b, s - 256, 7);
  else huffPut(b, 0xC0 + s - 280, 8);
}
static const uint16_t kLBase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t kLExt[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t kDBase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,
                                     8193,12289,16385,24577 };
static const uint8_t kDExt[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
static void matchPut(Bits* b, int len, int dist) {
  int c = 28; while (kLBase[c] > len) c--;
  litPut(b, 257 + c); if (kLExt[c]) bitsPut(b, (uint32_t)(len - kLBase[c]), kLExt[c]);
  int d = 29; while (kDBase[d] > dist) d--;
  huffPut(b, (uint32_t)d, 5); if (kDExt[d]) bitsPut(b, (uint32_t)(dist - kDBase[d]), kDExt[d]);
}
static uint32_t crcTab[256];
static uint32_t crc32Of(const uint8_t* p, size_t n, uint32_t c) {
  if (!crcTab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t k = i; for (int j = 0; j < 8; j++) k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1; crcTab[i] = k; }
  c = ~c;
  for (size_t i = 0; i < n; i++) c = crcTab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return ~c;
}
static void chunk(FILE* f, const char* type, const uint8_t* d, size_t n) {
  uint8_t h[8] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n,
                   (uint8_t)type[0], (uint8_t)type[1], (uint8_t)type[2], (uint8_t)type[3] };
  fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
  uint32_t c = crc32Of(h + 4, 4, 0); c = crc32Of(d, n, c);
  uint8_t t[4] = { (uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
  fwrite(t, 1, 4, f);
}
#define LZ_HASH 15
#define LZ_WIN 32768
int CtBugPngWrite(const char* path, const uint8_t* rgb, int w, int h) {
  if (!path || !rgb || w <= 0 || h <= 0) return 0;
  size_t row = (size_t)w * 3 + 1, n = row * (size_t)h;
  uint8_t* raw = (uint8_t*)malloc(n);
  int32_t* head = (int32_t*)malloc(sizeof(int32_t) << LZ_HASH);
  int32_t* prev = (int32_t*)malloc(sizeof(int32_t) * LZ_WIN);
  Bits b = { 0 };
  int ok = 0;
  if (raw && head && prev) {
    for (int y = 0; y < h; y++) {                                   // filter 2 (Up): the byte minus the one above it
      uint8_t* o = raw + row * (size_t)y; const uint8_t* s = rgb + (size_t)w * 3 * y;
      o[0] = 2;
      for (size_t x = 0; x < (size_t)w * 3; x++) o[1 + x] = (uint8_t)(s[x] - (y ? s[x - (size_t)w * 3] : 0));
    }
    bbyte(&b, 0x78); bbyte(&b, 0x01);                               // zlib: deflate, 32 KB window
    bitsPut(&b, 1, 1); bitsPut(&b, 1, 2);                           // the last block, fixed Huffman
    for (int i = 0; i < (1 << LZ_HASH); i++) head[i] = -1;
    size_t i = 0;
    while (i < n) {
      int best = 0, bestD = 0;
      if (i + 3 <= n) {
        uint32_t hv = ((uint32_t)raw[i] << 16 | (uint32_t)raw[i + 1] << 8 | raw[i + 2]) * 2654435761u >> (32 - LZ_HASH);
        int32_t c = head[hv];
        for (int chain = 0; c >= 0 && chain < 48; chain++) {
          size_t d = i - (size_t)c;
          if (d > LZ_WIN - 1) break;
          if (raw[c + best] == raw[i + best]) {
            size_t lim = n - i < 258 ? n - i : 258, k = 0;
            while (k < lim && raw[c + k] == raw[i + k]) k++;
            if ((int)k > best) { best = (int)k; bestD = (int)d; if (k == lim) break; }
          }
          int32_t nx = prev[c & (LZ_WIN - 1)];
          if (nx >= c) break;
          c = nx;
        }
        prev[i & (LZ_WIN - 1)] = head[hv]; head[hv] = (int32_t)i;
      }
      if (best >= 3) {
        matchPut(&b, best, bestD);
        for (size_t k = 1; k < (size_t)best && i + k + 3 <= n; k++) {   // the matched bytes join the hash chains too
          size_t j = i + k;
          uint32_t hv = ((uint32_t)raw[j] << 16 | (uint32_t)raw[j + 1] << 8 | raw[j + 2]) * 2654435761u >> (32 - LZ_HASH);
          prev[j & (LZ_WIN - 1)] = head[hv]; head[hv] = (int32_t)j;
        }
        i += (size_t)best;
      } else { litPut(&b, raw[i]); i++; }
    }
    litPut(&b, 256);                                                // the block's end
    if (b.nb) bitsPut(&b, 0, 8 - b.nb);
    uint32_t s1 = 1, s2 = 0;                                        // adler-32 of the filtered bytes
    for (size_t k = 0; k < n; k++) { s1 = (s1 + raw[k]) % 65521u; s2 = (s2 + s1) % 65521u; }
    bput32(&b, s2 << 16 | s1);
    FILE* f = b.p && b.n ? fopen(path, "wb") : NULL;
    if (f) {
      static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
      fwrite(sig, 1, 8, f);
      uint8_t ih[13] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                         (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0 };
      chunk(f, "IHDR", ih, 13);
      chunk(f, "IDAT", b.p, b.n);
      chunk(f, "IEND", NULL, 0);
      ok = fclose(f) == 0;
    }
  }
  free(raw); free(head); free(prev); free(b.p);
  return ok;
}
