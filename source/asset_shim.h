/* asset_shim.h -- NDK AAssetManager implemented over the SD-card assets/ tree
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __ASSET_SHIM_H__
#define __ASSET_SHIM_H__

#include <stdint.h>
#include <stddef.h>

// the opaque handles the NDK hands the engine; we only ever pass our own back
typedef struct AAssetManager AAssetManager;
typedef struct AAsset AAsset;
typedef struct AAssetDir AAssetDir;

// the single manager backing every AAssetManager_fromJava() result
AAssetManager *asset_manager(void);

// NDK entry points (registered in the import table)
AAssetManager *AAssetManager_fromJava_fake(void *env, void *obj);
AAsset *AAssetManager_open_fake(AAssetManager *mgr, const char *filename, int mode);
AAssetDir *AAssetManager_openDir_fake(AAssetManager *mgr, const char *dirName);
const char *AAssetDir_getNextFileName_fake(AAssetDir *dir);
void AAssetDir_close_fake(AAssetDir *dir);
int AAsset_read_fake(AAsset *a, void *buf, size_t count);
int64_t AAsset_seek_fake(AAsset *a, int64_t off, int whence);
void AAsset_close_fake(AAsset *a);
int64_t AAsset_getLength_fake(AAsset *a);
int64_t AAsset_getLength64_fake(AAsset *a);
int AAsset_openFileDescriptor64_fake(AAsset *a, int64_t *out_start, int64_t *out_length);

#endif
