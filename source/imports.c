/* imports.c -- .so import resolution for libMyGame.so (Valkyrie Profile Lenneth)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * VP Lenneth is a Cocos2d-x 3.x title. Unlike FF Dimensions it does NOT
 * self-contain its C++ runtime: it imports the Itanium C++ ABI (operator new,
 * __cxa_throw, RTTI, std::cout, embedded libunwind) from a bundled
 * libc++_shared.so, which main.c loads as a co-module. Those imports are left
 * out of this table on purpose so so_resolve() satisfies them from that module.
 *
 * What we DO provide here: a libc/libm subset (mostly straight passthrough to
 * newlib via imports_direct.h, with bionic-ABI wrappers where they differ),
 * GLES2 (mesa libGLESv2, linked so gl* resolve directly; the two OES buffer
 * calls go through eglGetProcAddress), EGL, OpenSL ES (opensles.c shim), the NDK
 * AAssetManager (asset_shim.c, over the SD-card assets/ tree), Android log ABI,
 * a dlopen/dlsym stub, and the pairip ExecuteProgram stub.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <math.h>
#include <malloc.h>
#include <dirent.h>
#include <locale.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>
#include <errno.h>
#include <setjmp.h>
#include <sys/time.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <switch.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "error.h"
#include "libc_shim.h"
#include "opensles.h"
#include "asset_shim.h"

// ctype entries are macros in newlib; drop the macros so we can take addresses
#undef isalnum
#undef isalpha
#undef isspace
#undef islower
#undef isupper
#undef isxdigit
#undef tolower
#undef toupper
#undef getc

extern uintptr_t __cxa_atexit;
extern int *__errno(void);

// the global stack canary the game and libc++_shared read (compiled with the
// -fstack-protector global-variable model, not the TLS one)
uintptr_t __stack_chk_guard = (uintptr_t)0x000a0b0c0d0e0f00ull;
void __stack_chk_fail_fake(void) { fatal_error("stack smashing detected"); }

// ---------------------------------------------------------------------------
// pairip: satisfy the vestigial native-VM import. No game code reaches it.
// ---------------------------------------------------------------------------
#if PAIRIP_STUB
static long ExecuteProgram_stub(void *a, void *b, void *c, void *d) {
  (void)a; (void)b; (void)c; (void)d;
  return 0;
}
#endif

static int android_log_print_fake(int prio, const char *tag, const char *fmt, ...) {
  (void)prio; (void)tag; (void)fmt;
  return 0;
}
static int android_log_write_fake(int prio, const char *tag, const char *text) {
  (void)prio; (void)tag; (void)text;
  return 0;
}
static void android_log_assert_fake(const char *cond, const char *tag, const char *fmt, ...) {
  char buf[1024] = "";
  if (fmt) {
    va_list va; va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
  }
  fatal_error("assert failed [%s]: %s\n%s", tag ? tag : "MyGame",
              cond ? cond : "", buf);
}

// ---------------------------------------------------------------------------
// dlopen/dlsym: optional symbols use the normal fallback path.
// so it takes its fallback paths.
// ---------------------------------------------------------------------------
static void *dlopen_fake(const char *name, int flag) { (void)name; (void)flag; return (void *)1; }
static void *dlsym_fake(void *h, const char *name) { (void)h; (void)name; return NULL; }
static int dlclose_fake(void *h) { (void)h; return 0; }
static const char *dlerror_fake(void) { return NULL; }

// ---------------------------------------------------------------------------
// /dev/urandom virtualization: std::random_device (cocos2d::RandomHelper::
// getEngine) opens "/dev/urandom", which does not exist on Switch, so its
// constructor throws std::system_error -> terminate -> abort. Hand back a
// sentinel fd whose reads come from a tick-seeded xorshift.
// ---------------------------------------------------------------------------
#define URANDOM_FD 0x40000000
static uint64_t s_rng;
static uint64_t rng_next(void) {
  if (!s_rng) s_rng = armGetSystemTick() | 1ull;
  s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
  return s_rng;
}
static int is_urandom(const char *p) {
  return p && (!strcmp(p, "/dev/urandom") || !strcmp(p, "/dev/random"));
}
static int open_fake(const char *path, int flags, ...) {
  if (is_urandom(path)) return URANDOM_FD;
  va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
  return open(path, flags, mode);
}
static ssize_t read_fake(int fd, void *buf, size_t n) {
  if (fd == URANDOM_FD) {
    uint8_t *b = buf;
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)rng_next();
    return (ssize_t)n;
  }
  return read(fd, buf, n);
}
static int close_fake(int fd) {
  if (fd == URANDOM_FD) return 0;
  return close(fd);
}

// ---------------------------------------------------------------------------
// bionic fortify extras not already in libc_shim.c
// ---------------------------------------------------------------------------
static char *__strchr_chk_fake(const char *s, int c, size_t slen) { (void)slen; return strchr(s, c); }
static char *__strncpy_chk_fake(char *d, const char *s, size_t n, size_t dlen) { (void)dlen; return strncpy(d, s, n); }
// Android's secondary strncpy fortify variant carries both destination and
// source object sizes. The game uses it for bounded path/string copies; the
// newlib implementation already provides the required copy semantics.
static char *__strncpy_chk2_fake(char *d, const char *s, size_t n, size_t dlen, size_t slen) {
  (void)dlen; (void)slen;
  return strncpy(d, s, n);
}
static int __open_2_fake(const char *path, int flags) {
  if (is_urandom(path)) return URANDOM_FD;
  return open(path, flags);
}
static ssize_t __read_chk_fake(int fd, void *buf, size_t count, size_t buflen) {
  (void)buflen; return read_fake(fd, buf, count);
}

// Android's legacy ctype exports are data symbols. The game only needs their
// ASCII classification values, so do not depend on newlib's private layout.
enum {
  BIONIC_C_UPPER = 0x01, BIONIC_C_LOWER = 0x02, BIONIC_C_DIGIT = 0x04,
  BIONIC_C_SPACE = 0x08, BIONIC_C_PUNCT = 0x10, BIONIC_C_CNTRL = 0x20,
  BIONIC_C_XDIGIT = 0x40,
};
static unsigned char bionic_ctype[257];
static int h_errno_fake;
static void init_bionic_ctype(void) {
  static int ready;
  if (ready) return;
  ready = 1;
  for (int c = 0; c < 256; c++) {
    unsigned char v = 0;
    if (c < 0x20 || c == 0x7f) v |= BIONIC_C_CNTRL;
    if (c == ' ' || (c >= '\t' && c <= '\r')) v |= BIONIC_C_SPACE;
    if (c >= 'A' && c <= 'Z') {
      v |= BIONIC_C_UPPER;
      if (c <= 'F') v |= BIONIC_C_XDIGIT;
    } else if (c >= 'a' && c <= 'z') {
      v |= BIONIC_C_LOWER;
      if (c <= 'f') v |= BIONIC_C_XDIGIT;
    } else if (c >= '0' && c <= '9') {
      v |= BIONIC_C_DIGIT | BIONIC_C_XDIGIT;
    } else if (c >= 0x21 && c <= 0x7e) {
      v |= BIONIC_C_PUNCT;
    }
    bionic_ctype[c + 1] = v;
  }
}
static size_t __ctype_get_mb_cur_max_fake(void) { return 1; }
static int *__get_h_errno_fake(void) { return &h_errno_fake; }

// Android-only online services are unavailable in this standalone port. Fail
// quickly so their background workers cannot block offline gameplay startup.
static int socket_fake(int domain, int type, int protocol) {
  (void)domain; (void)type; (void)protocol; errno = ENOSYS; return -1;
}
static int bind_fake(int fd, const void *addr, unsigned addrlen) {
  (void)fd; (void)addr; (void)addrlen; errno = ENOSYS; return -1;
}
static int connect_fake(int fd, const void *addr, unsigned addrlen) {
  (void)fd; (void)addr; (void)addrlen; errno = ENOSYS; return -1;
}
static int listen_fake(int fd, int backlog) { (void)fd; (void)backlog; errno = ENOSYS; return -1; }
static long send_fake(int fd, const void *buf, size_t len, int flags) {
  (void)fd; (void)buf; (void)len; (void)flags; errno = ENOSYS; return -1;
}
static long recv_fake(int fd, void *buf, size_t len, int flags) {
  (void)fd; (void)buf; (void)len; (void)flags; errno = ENOSYS; return -1;
}
static int getsockname_fake(int fd, void *addr, unsigned *addrlen) {
  (void)fd; (void)addr; (void)addrlen; errno = ENOSYS; return -1;
}
static int getsockopt_fake(int fd, int level, int name, void *value, unsigned *len) {
  (void)fd; (void)level; (void)name; (void)value; (void)len; errno = ENOSYS; return -1;
}
static int select_fake(int nfds, void *readfds, void *writefds, void *exceptfds, void *timeout) {
  (void)nfds; (void)readfds; (void)writefds; (void)exceptfds; (void)timeout; errno = ENOSYS; return -1;
}
static void *gethostbyname_fake(const char *name) { (void)name; h_errno_fake = 1; return NULL; }

// Bionic's locale-qualified APIs are unavailable in newlib. HCR only uses
// them for ASCII parsing/formatting, where the C locale is equivalent.
static int isdigit_l_fake(int c, void *loc) { (void)loc; return c >= '0' && c <= '9'; }
static int islower_l_fake(int c, void *loc) { (void)loc; return c >= 'a' && c <= 'z'; }
static int isupper_l_fake(int c, void *loc) { (void)loc; return c >= 'A' && c <= 'Z'; }
static int isxdigit_l_fake(int c, void *loc) {
  (void)loc;
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int iswalpha_l_fake(wint_t c, void *loc) { (void)loc; return iswalpha(c); }
static int iswblank_l_fake(wint_t c, void *loc) { (void)loc; return iswblank(c); }
static int iswcntrl_l_fake(wint_t c, void *loc) { (void)loc; return iswcntrl(c); }
static int iswdigit_l_fake(wint_t c, void *loc) { (void)loc; return iswdigit(c); }
static int iswlower_l_fake(wint_t c, void *loc) { (void)loc; return iswlower(c); }
static int iswprint_l_fake(wint_t c, void *loc) { (void)loc; return iswprint(c); }
static int iswpunct_l_fake(wint_t c, void *loc) { (void)loc; return iswpunct(c); }
static int iswspace_l_fake(wint_t c, void *loc) { (void)loc; return iswspace(c); }
static int iswupper_l_fake(wint_t c, void *loc) { (void)loc; return iswupper(c); }
static int iswxdigit_l_fake(wint_t c, void *loc) { (void)loc; return iswxdigit(c); }
static int strcoll_l_fake(const char *a, const char *b, void *loc) { (void)loc; return strcmp(a, b); }
static size_t strftime_l_fake(char *s, size_t n, const char *fmt, const struct tm *tm, void *loc) {
  (void)loc; return strftime(s, n, fmt, tm);
}
static size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc) {
  (void)loc; return strxfrm(dst, src, n);
}
static int tolower_l_fake(int c, void *loc) { (void)loc; return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
static int toupper_l_fake(int c, void *loc) { (void)loc; return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c; }
static wint_t towlower_l_fake(wint_t c, void *loc) { (void)loc; return towlower(c); }
static wint_t towupper_l_fake(wint_t c, void *loc) { (void)loc; return towupper(c); }
static int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc) { (void)loc; return wcscoll(a, b); }
static size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc) {
  (void)loc; return wcsxfrm(dst, src, n);
}

// ---------------------------------------------------------------------------
// misc libc gaps / stubs
// ---------------------------------------------------------------------------
static void abort_fake(void) { abort(); }

static int getpid_fake(void) { return 1; }
static int sched_yield_fake(void) { svcSleepThread(0); return 0; }
static int system_fake(const char *cmd) { (void)cmd; return -1; }
static int setpriority_fake(int which, int who, int prio) { (void)which; (void)who; (void)prio; return 0; }
static int getpriority_fake(int which, int who) { (void)which; (void)who; return 0; }

// devkitPro's pthread lacks these; the engine only tweaks thread priorities
static int pthread_attr_setschedpolicy_fake(void *attr, int policy) { (void)attr; (void)policy; return 0; }
static int pthread_getschedparam_fake(pthread_t t, int *policy, void *param) {
  (void)t; if (policy) *policy = 0; (void)param; return 0;
}

// newlib lacks clock_nanosleep; back it with nanosleep.
// CRITICAL: the CRIWARE audio-server thread paces itself with
//   clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL)
// where deadline is an ABSOLUTE time on the same monotonic clock as
// clock_gettime_fake (deadline = clock_gettime() + period). nanosleep only
// understands RELATIVE durations, so passing the absolute timespec straight
// through makes it return immediately (bogus huge value) -> the server thread
// busy-loops -> the song is decoded far faster than real time = audio
// fast-forward, regardless of the output device rate. Convert abs -> rel here.
#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif
static int clock_nanosleep_fake(int clk, int flags, const struct timespec *req, struct timespec *rem) {
  (void)clk;
  if (req && (flags & TIMER_ABSTIME)) {
    uint64_t now_ns = armTicksToNs(armGetSystemTick());
    uint64_t tgt_ns = (uint64_t)req->tv_sec * 1000000000ull + (uint64_t)req->tv_nsec;
    if (tgt_ns <= now_ns) return 0; // deadline already passed -> no sleep
    uint64_t d = tgt_ns - now_ns;
    struct timespec rel = { (time_t)(d / 1000000000ull), (long)(d % 1000000000ull) };
    return nanosleep(&rel, NULL); // abs sleep never reports remaining
  }
  return nanosleep(req, rem);
}

// clock_gettime: newlib's returns garbage on Switch; back it with the tick
static int clock_gettime_fake(int clk, void *tp) {
  (void)clk;
  uint64_t ns = armTicksToNs(armGetSystemTick());
  int64_t *t = (int64_t *)tp;
  if (t) { t[0] = (int64_t)(ns / 1000000000ull); t[1] = (int64_t)(ns % 1000000000ull); }
  return 0;
}

// gettimeofday: cocos2d-x measures its per-frame delta with this; newlib's is
// unreliable on Switch -> wrong dt -> wrong game speed (and CRIWARE, driven per
// frame, plays audio fast). Back it with the same monotonic tick as clock_gettime.
static int gettimeofday_fake(void *tv_, void *tz) {
  (void)tz;
  uint64_t ns = armTicksToNs(armGetSystemTick());
  long *tv = (long *)tv_;  // struct timeval: tv_sec, tv_usec (both long on aarch64)
  if (tv) { tv[0] = (long)(ns / 1000000000ull); tv[1] = (long)((ns % 1000000000ull) / 1000); }
  return 0;
}

static void sincos_fake(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }

// thread_local destructor registration: we don't tear engine threads down
// cleanly, so registering nothing (and leaking the dtor) is safe
static int __cxa_thread_atexit_impl_fake(void (*func)(void *), void *obj, void *dso) {
  (void)func; (void)obj; (void)dso; return 0;
}

// newlib lacks sysconf; libc++ uses it for page size and core count
static long sysconf_fake(int name) {
  switch (name) {
    case _SC_PAGESIZE:
      return 0x1000;
    case _SC_NPROCESSORS_ONLN:
    case _SC_NPROCESSORS_CONF:
      return 3; // Switch exposes 3 cores to applications
    default:
      return -1;
  }
}

// ---------------------------------------------------------------------------
// pthread: bionic lays its opaque mutex/cond types out inline (>=8 bytes each
// on arm64), so we lazily back them with a heap newlib object stashed in the
// caller's first 8 bytes. Sentinel 0 == uninitialised; 0x4000 == the bionic
// PTHREAD_RECURSIVE_MUTEX_INITIALIZER first word.
// ---------------------------------------------------------------------------
int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *attr) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return -1;
  const int recursive = (attr && (*attr & 3) == 1);
  int ret;
  if (recursive) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    ret = pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
  } else {
    ret = pthread_mutex_init(m, NULL);
  }
  if (ret != 0) { free(m); return -1; }
  *uid = m;
  return 0;
}

static pthread_mutex_t *ensure_mutex(pthread_mutex_t **uid) {
  if (!*uid) pthread_mutex_init_fake(uid, NULL);
  else if ((uintptr_t)*uid == 0x4000) { int a = 1; pthread_mutex_init_fake(uid, &a); }
  return *uid;
}

int pthread_mutex_lock_fake(pthread_mutex_t **uid) { return pthread_mutex_lock(ensure_mutex(uid)); }
int pthread_mutex_unlock_fake(pthread_mutex_t **uid) { return pthread_mutex_unlock(ensure_mutex(uid)); }
int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (*uid && (uintptr_t)*uid != 0x4000) { pthread_mutex_destroy(*uid); free(*uid); }
  *uid = NULL;
  return 0;
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const void *attr) {
  (void)attr;
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return -1;
  if (pthread_cond_init(c, NULL) != 0) { free(c); return -1; }
  *cnd = c;
  return 0;
}
static pthread_cond_t *ensure_cond(pthread_cond_t **cnd) {
  if (!*cnd) pthread_cond_init_fake(cnd, NULL);
  return *cnd;
}
int pthread_cond_signal_fake(pthread_cond_t **cnd) { return pthread_cond_signal(ensure_cond(cnd)); }
int pthread_cond_broadcast_fake(pthread_cond_t **cnd) { return pthread_cond_broadcast(ensure_cond(cnd)); }
static int cond_wait_tick(pthread_cond_t *cnd, pthread_mutex_t *mtx) {
  // Android's last startup wait has no Switch-side producer. A condition wait
  // may wake spuriously, so tick it to let the loader re-check its predicate.
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_nsec += 16 * 1000 * 1000;
  if (until.tv_nsec >= 1000 * 1000 * 1000) { until.tv_sec++; until.tv_nsec -= 1000 * 1000 * 1000; }
  int rc = pthread_cond_timedwait(cnd, mtx, &until);
  return rc == ETIMEDOUT ? 0 : rc;
}
int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  return cond_wait_tick(ensure_cond(cnd), ensure_mutex(mtx));
}
int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  if (*cnd) { pthread_cond_destroy(*cnd); free(*cnd); }
  *cnd = NULL;
  return 0;
}

int pthread_mutex_trylock_fake(pthread_mutex_t **uid) { return pthread_mutex_trylock(ensure_mutex(uid)); }
int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *ts) {
  (void)ts;
  return cond_wait_tick(ensure_cond(cnd), ensure_mutex(mtx));
}

// bionic pthread_mutexattr_t is a plain int holding the type in its low bits;
// keep it internally consistent with pthread_mutex_init_fake's attr read.
int pthread_mutexattr_init_fake(int *a) { if (a) *a = 0; return 0; }
int pthread_mutexattr_settype_fake(int *a, int type) { if (a) *a = type; return 0; }
int pthread_mutexattr_destroy_fake(int *a) { (void)a; return 0; }

int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine) return -1;
  if (__sync_lock_test_and_set(once_control, 1) == 0)
    (*init_routine)();
  return 0;
}

// engine threads need tpidr set up for TLS before running guarded code
typedef struct { void *(*entry)(void *); void *arg; } ThreadStart;
static void *thread_trampoline(void *p) {
  ThreadStart ts = *(ThreadStart *)p;
  free(p);
  tls_setup_guard();
  return ts.entry(ts.arg);
}
// std::thread::detach() calls pthread_detach and throws system_error if it
// fails. The game's short-lived threads (e.g. startMeasureLogoTime) often
// terminate before detach() runs, and devkitPro's pthread_detach returns an
// error for an already-finished thread -> uncaught throw -> abort. Swallow the
// error (the thread is gone; detaching it is a safe no-op) and report success.
static int pthread_detach_fake(pthread_t t) {
  pthread_detach(t);
  return 0;
}

int pthread_create_fake(pthread_t *thread, const void *attr, void *entry, void *arg) {
  (void)attr;
  ThreadStart *ts = malloc(sizeof(*ts));
  if (!ts) return EAGAIN;
  ts->entry = (void *(*)(void *))entry;
  ts->arg = arg;
  // Android's default worker stack is much larger than newlib's default. The
  // event loader recurses through asset metadata, so retain an Android-sized
  // stack instead of silently corrupting its completion state.
  pthread_attr_t native_attr;
  pthread_attr_init(&native_attr);
  pthread_attr_setstacksize(&native_attr, 8 * 1024 * 1024);
  int rc = pthread_create(thread, &native_attr, thread_trampoline, ts);
  pthread_attr_destroy(&native_attr);
  if (rc != 0)
    free(ts);
  return rc;
}

// ---------------------------------------------------------------------------
// OES buffer-mapping: not core in GLES2; resolve at runtime via EGL
// ---------------------------------------------------------------------------
typedef void *(*PFN_MapBuffer)(GLenum, GLenum);
typedef GLboolean (*PFN_UnmapBuffer)(GLenum);
static void *glMapBufferOES_wrap(GLenum target, GLenum access) {
  static PFN_MapBuffer f = NULL;
  if (!f) f = (PFN_MapBuffer)eglGetProcAddress("glMapBufferOES");
  return f ? f(target, access) : NULL;
}
static GLboolean glUnmapBufferOES_wrap(GLenum target) {
  static PFN_UnmapBuffer f = NULL;
  if (!f) f = (PFN_UnmapBuffer)eglGetProcAddress("glUnmapBufferOES");
  return f ? f(target) : GL_FALSE;
}

GLuint g_cur_fbo = 0;
static void glBindFramebuffer_wrap(GLenum target, GLuint fb) {
  g_cur_fbo = fb;
  glBindFramebuffer(target, fb);
}

static void glViewport_wrap(GLint x, GLint y, GLsizei w, GLsizei h) {
  extern int screen_width, screen_height;
  if (g_cur_fbo == 0 && (w > screen_width || h > screen_height)) {
    x = 0; y = 0; w = screen_width; h = screen_height;
  }
  glViewport(x, y, w, h);
}

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------

#define FN(x) { #x, (uintptr_t)&x }

DynLibFunction dynlib_functions[] = {
  // straight passthrough to newlib / libm / GLESv2 / EGL
  #include "imports_direct.h"
  // libc/locale/wide-char that libc++_shared.so imports (bionic: from libc.so)
  #include "imports_cxx.h"

  // C runtime / bionic
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__cxa_thread_atexit_impl", (uintptr_t)&__cxa_thread_atexit_impl_fake },
  { "abort", (uintptr_t)&abort_fake },
  { "__errno", (uintptr_t)&__errno },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail_fake },
  { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard },
  { "__sF", (uintptr_t)&fake_sF },

  // fortify (_chk) wrappers
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_fake },
  { "__memmove_chk", (uintptr_t)&__memmove_chk_fake },
  { "__strcat_chk", (uintptr_t)&__strcat_chk_fake },
  { "__strcpy_chk", (uintptr_t)&__strcpy_chk_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__strchr_chk", (uintptr_t)&__strchr_chk_fake },
  { "__strncpy_chk", (uintptr_t)&__strncpy_chk_fake },
  { "__strncpy_chk2", (uintptr_t)&__strncpy_chk2_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__vsprintf_chk", (uintptr_t)&__vsprintf_chk_fake },
  { "__open_2", (uintptr_t)&__open_2_fake },
  { "__read_chk", (uintptr_t)&__read_chk_fake },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "__get_h_errno", (uintptr_t)&__get_h_errno_fake },
  { "_ctype_", (uintptr_t)(bionic_ctype + 1) },
  { "open", (uintptr_t)&open_fake },
  { "read", (uintptr_t)&read_fake },
  { "close", (uintptr_t)&close_fake },

  // bionic misc
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "gettid", (uintptr_t)&gettid_fake },
  { "syscall", (uintptr_t)&syscall_fake },
  { "getpid", (uintptr_t)&getpid_fake },

  // offline networking stubs
  { "socket", (uintptr_t)&socket_fake },
  { "bind", (uintptr_t)&bind_fake },
  { "connect", (uintptr_t)&connect_fake },
  { "listen", (uintptr_t)&listen_fake },
  { "send", (uintptr_t)&send_fake },
  { "recv", (uintptr_t)&recv_fake },
  { "getsockname", (uintptr_t)&getsockname_fake },
  { "getsockopt", (uintptr_t)&getsockopt_fake },
  { "select", (uintptr_t)&select_fake },
  { "gethostbyname", (uintptr_t)&gethostbyname_fake },

  // Android log ABI
  { "__android_log_print", (uintptr_t)&android_log_print_fake },
  { "__android_log_write", (uintptr_t)&android_log_write_fake },
  { "__android_log_assert", (uintptr_t)&android_log_assert_fake },

  // dynamic loader stubs
  { "dlopen", (uintptr_t)&dlopen_fake },
  { "dlsym", (uintptr_t)&dlsym_fake },
  { "dlclose", (uintptr_t)&dlclose_fake },
  { "dlerror", (uintptr_t)&dlerror_fake },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },

  // pairip
#if PAIRIP_STUB
  { "ExecuteProgram", (uintptr_t)&ExecuteProgram_stub },
#endif

  // time / sched
  { "clock_gettime", (uintptr_t)&clock_gettime_fake },
  { "clock_nanosleep", (uintptr_t)&clock_nanosleep_fake },
  { "sched_yield", (uintptr_t)&sched_yield_fake },
  { "system", (uintptr_t)&system_fake },
  { "setpriority", (uintptr_t)&setpriority_fake },
  { "getpriority", (uintptr_t)&getpriority_fake },
  { "pthread_attr_setschedpolicy", (uintptr_t)&pthread_attr_setschedpolicy_fake },
  { "pthread_getschedparam", (uintptr_t)&pthread_getschedparam_fake },

  // math gaps
  { "sincos", (uintptr_t)&sincos_fake },
  { "sincosf", (uintptr_t)&sincosf_fake },

  // pthread (bionic-ABI wrappers)
  { "pthread_create", (uintptr_t)&pthread_create_fake },
  { "pthread_detach", (uintptr_t)&pthread_detach_fake },
  { "pthread_once", (uintptr_t)&pthread_once_fake },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake },
  { "pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy_fake },
  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake },

  // locale-qualified bionic APIs
  { "isdigit_l", (uintptr_t)&isdigit_l_fake },
  { "islower_l", (uintptr_t)&islower_l_fake },
  { "isupper_l", (uintptr_t)&isupper_l_fake },
  { "isxdigit_l", (uintptr_t)&isxdigit_l_fake },
  { "iswalpha_l", (uintptr_t)&iswalpha_l_fake },
  { "iswblank_l", (uintptr_t)&iswblank_l_fake },
  { "iswcntrl_l", (uintptr_t)&iswcntrl_l_fake },
  { "iswdigit_l", (uintptr_t)&iswdigit_l_fake },
  { "iswlower_l", (uintptr_t)&iswlower_l_fake },
  { "iswprint_l", (uintptr_t)&iswprint_l_fake },
  { "iswpunct_l", (uintptr_t)&iswpunct_l_fake },
  { "iswspace_l", (uintptr_t)&iswspace_l_fake },
  { "iswupper_l", (uintptr_t)&iswupper_l_fake },
  { "iswxdigit_l", (uintptr_t)&iswxdigit_l_fake },
  { "strcoll_l", (uintptr_t)&strcoll_l_fake },
  { "strftime_l", (uintptr_t)&strftime_l_fake },
  { "strxfrm_l", (uintptr_t)&strxfrm_l_fake },
  { "tolower_l", (uintptr_t)&tolower_l_fake },
  { "toupper_l", (uintptr_t)&toupper_l_fake },
  { "towlower_l", (uintptr_t)&towlower_l_fake },
  { "towupper_l", (uintptr_t)&towupper_l_fake },
  { "wcscoll_l", (uintptr_t)&wcscoll_l_fake },
  { "wcsxfrm_l", (uintptr_t)&wcsxfrm_l_fake },

  // POSIX semaphore ABI is compatible with the newlib implementation.
  FN(sem_init),
  FN(sem_destroy),
  FN(sem_post),
  FN(sem_wait),

  // libc++_shared misc (bionic libc)
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },
  { "openlog", (uintptr_t)&ret0 },
  { "closelog", (uintptr_t)&ret0 },
  { "syslog", (uintptr_t)&ret0 },
  { "sysconf", (uintptr_t)&sysconf_fake },

  // NDK AAssetManager (over the SD-card assets/ tree)
  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake },
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open_fake },
  { "AAssetManager_openDir", (uintptr_t)&AAssetManager_openDir_fake },
  { "AAssetDir_getNextFileName", (uintptr_t)&AAssetDir_getNextFileName_fake },
  { "AAssetDir_close", (uintptr_t)&AAssetDir_close_fake },
  { "AAsset_close", (uintptr_t)&AAsset_close_fake },
  { "AAsset_getLength", (uintptr_t)&AAsset_getLength_fake },
  { "AAsset_getLength64", (uintptr_t)&AAsset_getLength64_fake },
  { "AAsset_openFileDescriptor64", (uintptr_t)&AAsset_openFileDescriptor64_fake },
  { "AAsset_read", (uintptr_t)&AAsset_read_fake },
  { "AAsset_seek", (uintptr_t)&AAsset_seek_fake },

  // GLES2 OES buffer mapping (via eglGetProcAddress)
  { "glMapBufferOES", (uintptr_t)&glMapBufferOES_wrap },
  { "glUnmapBufferOES", (uintptr_t)&glUnmapBufferOES_wrap },

  // OpenSL ES (opensles.c shim) + interface-id sentinels the game references
  { "slCreateEngine", (uintptr_t)&slCreateEngine },
  #define SL_IID(n) { "SL_IID_" #n, (uintptr_t)&SL_IID_##n }
  SL_IID(ENGINE), SL_IID(PLAY), SL_IID(VOLUME), SL_IID(BUFFERQUEUE),
  SL_IID(ANDROIDSIMPLEBUFFERQUEUE), SL_IID(OUTPUTMIX),
  #undef SL_IID
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

int vpl_resolve_imports(so_module *mod) {
  init_bionic_ctype();
  so_relocate(mod);
  return so_resolve(mod, dynlib_functions, dynlib_numfunctions, 1);
}
