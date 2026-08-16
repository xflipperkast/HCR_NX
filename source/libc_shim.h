/* libc_shim.h -- bionic-compatible libc wrappers for libjniproxy.so
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __LIBC_SHIM_H__
#define __LIBC_SHIM_H__

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>

// fortify
void *memcpy_fake(void *dst, const void *src, size_t n);
void *memmove_fake(void *dst, const void *src, size_t n);
void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen);
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen);
char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen);
char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen);
size_t __strlen_chk_fake(const char *s, size_t slen);
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va);
int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va);

// misc bionic
int __system_property_get_fake(const char *name, char *value);
unsigned long getauxval_fake(unsigned long type);
int gettid_fake(void);
long syscall_fake(long number, ...);
void sincosf_fake(float x, float *s, float *c);
void android_set_abort_message_fake(const char *msg);
int posix_memalign_fake(void **out, size_t align, size_t size);

// stdio over fake __sF
extern uint8_t fake_sF[3][0x100];
FILE *fopen_fake(const char *path, const char *mode);
size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f);
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f);
int fputc_fake(int c, FILE *f);
char *fgets_fake(char *s, int n, FILE *f);
int fputs_fake(const char *s, FILE *f);
int ferror_fake(FILE *f);
int fflush_fake(FILE *f);
int fclose_fake(FILE *f);
int fseek_fake(FILE *f, long off, int whence);
long ftell_fake(FILE *f);
void rewind_fake(FILE *f);
int fprintf_fake(FILE *f, const char *fmt, ...);
int vfprintf_fake(FILE *f, const char *fmt, va_list va);

// pthread rwlocks
int pthread_rwlock_rdlock_fake(void **rw);
int pthread_rwlock_wrlock_fake(void **rw);
int pthread_rwlock_unlock_fake(void **rw);

#endif
