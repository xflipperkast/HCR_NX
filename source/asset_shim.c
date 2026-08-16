/* asset_shim.c -- NDK AAssetManager implemented over the SD-card assets/ tree
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * Cocos2d-x's FileUtilsAndroid reads everything under "assets/" through the NDK
 * AAssetManager. There is no such thing on Switch, so we back the whole API with
 * plain filesystem access rooted at ASSETS_DIR (extracted from
 * split_assetPack.apk). An AAsset is just a buffered FILE*; a "file descriptor"
 * asset (used by CRIWARE criFs and Cocos2dxHelper.getObbAssetFileDescriptor to
 * read .cpk ranges) is a real open() fd over the same file.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "config.h"
#include "asset_shim.h"

// AASSET_MODE_* are just hints on Android; we ignore them.

// Cocos reads PNG/JSON data in many small chunks.  The default newlib stream
// buffer is tiny for SD storage, so each asset gets one 128 KiB read-ahead
// buffer for its short lifetime.
#define ASSET_READ_AHEAD (128 * 1024)

struct AAssetManager { int _unused; };
static AAssetManager g_manager;

struct AAsset {
  FILE *f;
  long size;
  char path[512];
};

struct AAssetDir {
  DIR *dir;
  char base[512];   // assets-relative dir prefix (with trailing '/')
  char name[512];   // scratch for the returned name
};

AAssetManager *asset_manager(void) { return &g_manager; }

// Resolve a name the engine passes to AAssetManager into a real filesystem
// path. Absolute paths (save data / writable dir, e.g. "sdmc:/switch/vpl/..."
// or "/...") are opened directly -- they are NOT bundled assets, and prefixing
// "assets/" broke save read/write alignment. Relative names live under
// ASSETS_DIR (a leading "assets/" the engine sometimes prepends is stripped).
static void asset_path(char *out, size_t out_size, const char *name) {
  if (name[0] == '/' || strstr(name, ":/")) {
    snprintf(out, out_size, "%s", name);
    return;
  }
  if (strncmp(name, "assets/", 7) == 0)
    name += 7;
  snprintf(out, out_size, "%s/%s", ASSETS_DIR, name);
}

AAssetManager *AAssetManager_fromJava_fake(void *env, void *obj) {
  (void)env; (void)obj;
  return &g_manager;
}

AAsset *AAssetManager_open_fake(AAssetManager *mgr, const char *filename, int mode) {
  (void)mgr; (void)mode;
  if (!filename)
    return NULL;

  char path[512];
  asset_path(path, sizeof(path), filename);

  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  setvbuf(f, NULL, _IOFBF, ASSET_READ_AHEAD);

  AAsset *a = calloc(1, sizeof(*a));
  if (!a) { fclose(f); return NULL; }
  a->f = f;
  fseek(f, 0, SEEK_END);
  a->size = ftell(f);
  fseek(f, 0, SEEK_SET);
  snprintf(a->path, sizeof(a->path), "%s", path);
  return a;
}

int AAsset_read_fake(AAsset *a, void *buf, size_t count) {
  if (!a || !a->f)
    return -1;
  return (int)fread(buf, 1, count, a->f);
}

int64_t AAsset_seek_fake(AAsset *a, int64_t off, int whence) {
  if (!a || !a->f)
    return -1;
  if (fseek(a->f, (long)off, whence) != 0)
    return -1;
  return (int64_t)ftell(a->f);
}

void AAsset_close_fake(AAsset *a) {
  if (!a)
    return;
  if (a->f)
    fclose(a->f);
  free(a);
}

int64_t AAsset_getLength_fake(AAsset *a) { return a ? (int64_t)a->size : 0; }
int64_t AAsset_getLength64_fake(AAsset *a) { return a ? (int64_t)a->size : 0; }

// Hand back a real fd + [0, size) range. The engine reads the file itself.
int AAsset_openFileDescriptor64_fake(AAsset *a, int64_t *out_start, int64_t *out_length) {
  if (!a)
    return -1;
  int fd = open(a->path, O_RDONLY);
  if (fd < 0)
    return -1;
  if (out_start)  *out_start = 0;
  if (out_length) *out_length = (int64_t)a->size;
  return fd;
}

AAssetDir *AAssetManager_openDir_fake(AAssetManager *mgr, const char *dirName) {
  (void)mgr;
  char path[512];
  asset_path(path, sizeof(path), dirName ? dirName : "");

  DIR *d = opendir(path);
  if (!d)
    return NULL;

  AAssetDir *ad = calloc(1, sizeof(*ad));
  if (!ad) { closedir(d); return NULL; }
  ad->dir = d;
  // remember the assets-relative prefix so getNextFileName returns full paths
  const char *rel = dirName ? dirName : "";
  while (*rel == '/') rel++;
  if (strncmp(rel, "assets/", 7) == 0) rel += 7;
  if (*rel)
    snprintf(ad->base, sizeof(ad->base), "%s/", rel);
  else
    ad->base[0] = '\0';
  return ad;
}

const char *AAssetDir_getNextFileName_fake(AAssetDir *dir) {
  if (!dir || !dir->dir)
    return NULL;
  struct dirent *e;
  while ((e = readdir(dir->dir)) != NULL) {
    if (e->d_name[0] == '.') // skip "." / ".." / hidden
      continue;
    // AAssetDir only enumerates regular files, not subdirectories
    char full[1024];
    snprintf(full, sizeof(full), "%s/%s%s", ASSETS_DIR, dir->base, e->d_name);
    struct stat st;
    if (stat(full, &st) == 0 && S_ISDIR(st.st_mode))
      continue;
    snprintf(dir->name, sizeof(dir->name), "%.255s%.255s", dir->base, e->d_name);
    return dir->name;
  }
  return NULL;
}

void AAssetDir_close_fake(AAssetDir *dir) {
  if (!dir)
    return;
  if (dir->dir)
    closedir(dir->dir);
  free(dir);
}
