/* libc_shim.c -- bionic-compatible libc wrappers for libjniproxy.so
 *
 * Where the bionic and newlib ABIs differ (struct layouts, flag values, missing
 * functions) we provide converting wrappers here; everything that matches is
 * passed straight through from imports.c.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <malloc.h>
#include <switch.h>

#include "config.h"
#include "libc_shim.h"

// NULL-source-tolerant memcpy/memmove. VP blits sprite data into its emulated
// PS1 VRAM via XpPs1VRam::loadImage2, which memmove()s from an image-data pointer
// that is sometimes NULL (a Camp graphic whose data never loaded). On Android the
// call happened to survive; on newlib memmove(dst, NULL, n) faults. A NULL src (or
// dst) is always a bug, so treat it as a no-op instead of crashing -- the sprite
// is skipped, the scene proceeds. n==0 is also a safe no-op.
// A src/dst in the first 64KB of address space is never a real pointer (heap and
// modules live far higher); such a memcpy/memmove is a bug (NULL or a garbage
// low value like 0x20 -- e.g. loadImage2 blitting from an uninitialized Camp
// image-data pointer). Skip it instead of faulting.
#define BOGUS_PTR(p) ((uintptr_t)(p) < 0x10000u)
void *memcpy_fake(void *dst, const void *src, size_t n) {
  if (n == 0 || BOGUS_PTR(dst) || BOGUS_PTR(src)) return dst;
  return memcpy(dst, src, n);
}
void *memmove_fake(void *dst, const void *src, size_t n) {
  if (n == 0 || BOGUS_PTR(dst) || BOGUS_PTR(src)) return dst;
  return memmove(dst, src, n);
}

// fortify (_chk) wrappers: ignore the object-size argument
void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen; return memcpy_fake(dst, src, n);
}
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen; return memmove_fake(dst, src, n);
}
char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen; return strcat(dst, src);
}
char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen; return strcpy(dst, src);
}
size_t __strlen_chk_fake(const char *s, size_t slen) {
  (void)slen; return strlen(s);
}
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen; return vsnprintf(s, maxlen, fmt, va);
}
int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen; return vsprintf(s, fmt, va);
}

// misc bionic functions
int __system_property_get_fake(const char *name, char *value) {
  (void)name; value[0] = '\0'; return 0;
}

unsigned long getauxval_fake(unsigned long type) {
  (void)type; return 0;
}

int gettid_fake(void) {
  u64 thread_id = 1;
  if (R_SUCCEEDED(svcGetThreadId(&thread_id, CUR_THREAD_HANDLE)) && thread_id)
    return (int)(thread_id & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_GETTID 178

long syscall_fake(long number, ...) {
  if (number == ARM64_SYS_GETTID)
    return gettid_fake();
  errno = ENOSYS;
  return -1;
}

void sincosf_fake(float x, float *s, float *c) {
  *s = sinf(x); *c = cosf(x);
}

void android_set_abort_message_fake(const char *msg) {
  (void)msg;
}

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p) return ENOMEM;
  *out = p;
  return 0;
}

// ---------------------------------------------------------------------------
// stdio over the fake bionic __sF (stdin/stdout/stderr). libc++_shared inits
// std::cout/cerr against &__sF[1]/&__sF[2]; these wrappers absorb those and
// forward everything else to real FILEs.
// ---------------------------------------------------------------------------

uint8_t fake_sF[3][0x100];

static int is_fake_file(const void *f) {
  const uint8_t *p = f;
  const uint8_t *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

FILE *fopen_fake(const char *path, const char *mode) {
  FILE *f = fopen(path, mode);
  // The offline Android build normally creates this server-event cache before
  // the shop is opened. On Switch there is no backend fetch, but the native
  // shop loader assumes the cache exists. An empty JSON object selects its
  // no-event path.
  const char *name = path ? strrchr(path, '/') : NULL;
  if (!f && path && mode && mode[0] == 'r' &&
      !strcmp(name ? name + 1 : path, "gcEventDetails")) {
    f = fopen(path, "w+");
    if (f) {
      fputs("{}", f);
      rewind(f);
    }
  }
  return f;
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) return n;
  return fwrite(ptr, size, n, f);
}

size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) return 0;
  return fread(ptr, size, n, f);
}

int fputc_fake(int c, FILE *f) {
  if (is_fake_file(f)) return c;
  return fputc(c, f);
}

char *fgets_fake(char *s, int n, FILE *f) {
  if (is_fake_file(f)) return NULL;
  return fgets(s, n, f);
}

int fputs_fake(const char *s, FILE *f) {
  if (is_fake_file(f)) return 0;
  return fputs(s, f);
}

int ferror_fake(FILE *f) {
  return is_fake_file(f) ? 0 : ferror(f);
}

int fflush_fake(FILE *f) {
  if (is_fake_file(f) || f == NULL) return 0;
  return fflush(f);
}

int fclose_fake(FILE *f) {
  if (is_fake_file(f)) return 0;
  return fclose(f);
}

int fseek_fake(FILE *f, long off, int whence) {
  if (is_fake_file(f)) return -1;
  return fseek(f, off, whence);
}

long ftell_fake(FILE *f) {
  return is_fake_file(f) ? 0 : ftell(f);
}

void rewind_fake(FILE *f) {
  if (!is_fake_file(f)) rewind(f);
}

int fprintf_fake(FILE *f, const char *fmt, ...) {
  if (is_fake_file(f)) return 0;
  va_list va;
  va_start(va, fmt);
  int ret = vfprintf(f, fmt, va);
  va_end(va);
  return ret;
}

int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) return 0;
  return vfprintf(f, fmt, va);
}

// ---------------------------------------------------------------------------
// pthread rwlocks: bionic allocates the opaque type inline, so we stash a
// pointer to a real libnx RwLock in the caller's storage (like the mutex fakes)
// ---------------------------------------------------------------------------

typedef struct { RwLock lock; } FakeRwLock;

static FakeRwLock *get_rwlock(void **storage) {
  if (!*storage) {
    FakeRwLock *l = calloc(1, sizeof(*l));
    rwlockInit(&l->lock);
    *storage = l;
  }
  return *storage;
}

int pthread_rwlock_rdlock_fake(void **rw) {
  rwlockReadLock(&get_rwlock(rw)->lock);
  return 0;
}

int pthread_rwlock_wrlock_fake(void **rw) {
  rwlockWriteLock(&get_rwlock(rw)->lock);
  return 0;
}

int pthread_rwlock_unlock_fake(void **rw) {
  FakeRwLock *l = get_rwlock(rw);
  if (rwlockIsWriteLockHeldByCurrentThread(&l->lock))
    rwlockWriteUnlock(&l->lock);
  else
    rwlockReadUnlock(&l->lock);
  return 0;
}
