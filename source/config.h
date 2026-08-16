/* config.h -- Hill Climb Racing Switch wrapper configuration
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// newlib heap cap (MB). The engine's malloc imports resolve to newlib, so this
// must cover all engine allocations. VP Lenneth is a Cocos2d-x 3.x title with
// CRIWARE (Sofdec2 video / ADX audio) and streams most data off the .cpk packs,
// but keeps a lot of decoded texture/animation state resident, so be generous.
#define MEMORY_MB 1024

// Hill Climb Racing 1.71.1 is a self-contained Cocos2d-x native module.
#define SO_NAME "libgame.so"
#define DATA_DIR "."
#define GAME_APK "hcr.apk"
#define OBB_NAME "unused.obb"

// where the extracted split_assetPack.apk assets/ tree lives on the SD card,
// relative to the game folder (the CWD the NRO launched from)
#define ASSETS_DIR "assets"

#define CONFIG_NAME "config.txt"
// Java SimpleAudioEngine calls are decoded and mixed by opensles.c, then sent
// through Switch's native audout service.
#define HCR_ENABLE_COCOS_AUDIO 1

// pairip: libMyGame.so is NEEDED-linked against libpairipcore.so and imports
// ExecuteProgram, but the native game code never routes through the pairip VM
// (no call sites; JNI_OnLoad is the clean Cocos2d-x one). We satisfy the import
// with a stub so the loader resolves cleanly.
#define PAIRIP_STUB 0

extern int screen_width;
extern int screen_height;

// Cocos2d-x language index (Application::LanguageType), returned by the
// getCurrentLanguage reverse-JNI callback. VP Lenneth supports en/ja/fr/de/etc.
//   0 English 1 Chinese 2 French 3 Italian 4 German 5 Spanish ... (cocos order)
// We keep it simple and expose the raw int; default English.
#define LANG_EN 0
#define LANG_COUNT 20

typedef struct {
  int screen_width;
  int screen_height;
  int language;
  int cursor_speed; // virtual pointer speed (docked / no-touch play)
  int audio_rate;   // CRIWARE master-mix output rate (Hz); its content is ~32000,
                    // not the 44100 it requests -> playing at 44100 = fast-forward
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
