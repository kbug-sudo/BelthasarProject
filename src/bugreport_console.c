// bugreport_console.c - the console's last lines for a bug report's console.txt (bugreport.h CtBugConsoleStart,
// CtBugConsoleText). stdout and stderr are each pointed at a pipe; a thread per stream copies what arrives on to where
// the stream went before (the console window) and, a whole line at a time, into one ring of the last 64 KB -- so a line
// of one stream is never cut by the other's. Both streams are unbuffered from then on (setvbuf after the first output:
// the CRT tolerates it). The pipes and the duplicates are not inherited: a child process cannot hold them open past the
// exit, where the readers are drained. Not on Android (its log is logcat's).
#include "bugreport.h"
#include "SDL.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#define ct_pipe(fds) _pipe(fds, 65536, _O_BINARY | _O_NOINHERIT)
static void ct_noinherit(int fd) { SetHandleInformation((HANDLE)_get_osfhandle(fd), HANDLE_FLAG_INHERIT, 0); }
#define ct_dup _dup
#define ct_dup2 _dup2
#define ct_read _read
#define ct_write _write
#define ct_close _close
#elif !defined(__ANDROID__)
#include <fcntl.h>
#include <unistd.h>
#define ct_pipe(fds) pipe(fds)
static void ct_noinherit(int fd) { fcntl(fd, F_SETFD, FD_CLOEXEC); }
#define ct_dup dup
#define ct_dup2 dup2
#define ct_read read
#define ct_write write
#define ct_close close
#endif

#define CON_RING (64 * 1024)
static char g_ring[CON_RING];
static size_t g_head, g_len;
static SDL_mutex* g_mx;
static int g_started;
static struct { int fd, orig, readEnd; SDL_Thread* th; } g_str[2] = { { 1, -1, -1, NULL }, { 2, -1, -1, NULL } };

static void ringAppend(const char* s, size_t n) {
  SDL_LockMutex(g_mx);
  for (size_t i = 0; i < n; i++) {
    g_ring[(g_head + g_len) % CON_RING] = s[i];
    if (g_len < CON_RING) g_len++; else g_head = (g_head + 1) % CON_RING;
  }
  SDL_UnlockMutex(g_mx);
}

#if !defined(__ANDROID__)
static int SDLCALL conReader(void* user) {
  int k = (int)(intptr_t)user;
  char buf[4096], line[1024];
  size_t ln = 0;                                 // this stream's line so far (a longer one goes in pieces)
  for (;;) {
    int n = (int)ct_read(g_str[k].readEnd, buf, sizeof buf);
    if (n <= 0) break;
    if (g_str[k].orig >= 0) { int w = (int)ct_write(g_str[k].orig, buf, (unsigned)n); (void)w; }
    for (int i = 0; i < n; i++) {
      line[ln++] = buf[i];
      if (buf[i] == '\n' || ln == sizeof line) { ringAppend(line, ln); ln = 0; }
    }
  }
  if (ln) ringAppend(line, ln);
  return 0;
}
static void conStop(void) {                    // at exit: the streams back where they were, the readers drained
  fflush(stdout); fflush(stderr);
  for (int k = 0; k < 2; k++) if (g_str[k].orig >= 0) ct_dup2(g_str[k].orig, g_str[k].fd);   // (closes the pipe's last writer)
  for (int k = 0; k < 2; k++) if (g_str[k].th) { SDL_WaitThread(g_str[k].th, NULL); g_str[k].th = NULL; }
}
#endif

void CtBugConsoleStart(void) {
#if !defined(__ANDROID__)
  if (g_started) return;
  g_started = 1;
  g_mx = SDL_CreateMutex();
  if (!g_mx) return;
  fflush(stdout); fflush(stderr);
  for (int k = 0; k < 2; k++) {
    int fds[2];
    if (ct_pipe(fds) != 0) continue;
    g_str[k].orig = ct_dup(g_str[k].fd);
    if (g_str[k].orig < 0 || ct_dup2(fds[1], g_str[k].fd) != 0) {
      ct_close(fds[0]); ct_close(fds[1]); if (g_str[k].orig >= 0) ct_close(g_str[k].orig); g_str[k].orig = -1; continue; }
    ct_close(fds[1]);
    ct_noinherit(g_str[k].orig); ct_noinherit(g_str[k].fd); ct_noinherit(fds[0]);
    g_str[k].readEnd = fds[0];
    g_str[k].th = SDL_CreateThread(conReader, k ? "bugcon-err" : "bugcon-out", (void*)(intptr_t)k);
    if (!g_str[k].th) { ct_dup2(g_str[k].orig, g_str[k].fd); ct_close(fds[0]); }
  }
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  atexit(conStop);
#endif
}

char* CtBugConsoleText(void) {
  if (!g_mx) return NULL;
  SDL_LockMutex(g_mx);
  char* s = (char*)malloc(g_len + 1);
  size_t n = 0;
  if (s) {
    for (size_t i = 0; i < g_len; i++) s[i] = g_ring[(g_head + i) % CON_RING];
    n = g_len; s[n] = 0;
    if (g_len == CON_RING) {                   // the ring wrapped: drop the cut line at its start
      char* nl = memchr(s, '\n', n);
      if (nl) { size_t k = (size_t)(nl + 1 - s); memmove(s, nl + 1, n - k + 1); }
    }
  }
  SDL_UnlockMutex(g_mx);
  return s;
}
