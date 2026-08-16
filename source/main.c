/* main.c -- Valkyrie Profile Lenneth Switch wrapper entry point
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * Boot flow mirrors what Cocos2dxActivity does on Android:
 *   1. load libc++_shared.so (co-module) so the game's C++ ABI resolves,
 *      then libMyGame.so on top;
 *   2. relocate + resolve imports for both, finalize (map RX), run init arrays;
 *   3. JNI_OnLoad -> JniHelper::setJavaVM + cocos_android_app_init (AppDelegate);
 *   4. Cocos2dxHelper.nativeSetContext / nativeSetApkPath / nativeSetAudioDeviceInfo;
 *   5. Cocos2dxRenderer.nativeInit(w,h) -> applicationDidFinishLaunching  <-- Marco 1
 *   6. per frame: nativeRender, feeding nativeTouches* from touch/pad.
 */

#include <stdlib.h>
#include <string.h>
#include <stdalign.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <EGL/egl.h>
#include <switch.h>
#include <GLES2/gl2.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "opensles.h"
#include "asset_shim.h"
#include "overlay.h"
#include "installer.h"
#include "hcr_http.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module cxx_mod;   // libc++_shared.so
so_module game_mod;  // libMyGame.so

int vpl_resolve_imports(so_module *mod);

// Keep a dedicated exception stack so libnx can terminate safely after a fault.
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  (void)ctx;
  svcExitProcess();
}

// separate the newlib heap from the .so images
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  // reserve a slice for the two .so images; the rest is newlib malloc (the
  // engine allocates textures / animation state / CRIWARE work buffers there)
  size_t so_reserve = 96 * 1024 * 1024;
  if (so_reserve > size / 2)
    so_reserve = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_size  = size - so_reserve;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

static void check_data(void) {
  struct stat st;
  const char *files[] = { SO_NAME };
  for (unsigned i = 0; i < sizeof(files) / sizeof(*files); i++)
    if (stat(files[i], &st) < 0)
      fatal_error("Could not find\n%s.\nCheck your data files.", files[i]);
  if (stat(ASSETS_DIR, &st) < 0 || !S_ISDIR(st.st_mode))
    fatal_error("Could not find the '%s' folder.\nExtract split_assetPack.apk here.", ASSETS_DIR);
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    if (appletGetOperationMode() == AppletOperationMode_Console) { screen_width = 1920; screen_height = 1080; }
    else { screen_width = 1280; screen_height = 720; }
  } else { screen_width = w; screen_height = h; }
}

// ---------------------------------------------------------------------------
// EGL / GLES2 context on the default NWindow
// ---------------------------------------------------------------------------
static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLContext s_context = EGL_NO_CONTEXT;
static EGLSurface s_surface = EGL_NO_SURFACE;

static int egl_init(void) {
  s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!s_display) return 0;
  eglInitialize(s_display, NULL, NULL);
  if (!eglBindAPI(EGL_OPENGL_ES_API)) return 0;

  // Cocos2d-x default: RGBA8 + depth24 + stencil8, GLES2
  const EGLint cfg_attr[] = {
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_NONE
  };
  EGLConfig config;
  EGLint num = 0;
  if (!eglChooseConfig(s_display, cfg_attr, &config, 1, &num) || num < 1) return 0;

  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, screen_width, screen_height);
  s_surface = eglCreateWindowSurface(s_display, config, win, NULL);
  if (!s_surface) return 0;

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, ctx_attr);
  if (!s_context) return 0;

  eglMakeCurrent(s_display, s_surface, s_surface, s_context);
  eglSwapInterval(s_display, 1);
  return 1;
}

static void egl_deinit(void) {
  if (s_display == EGL_NO_DISPLAY) return;
  eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (s_context) eglDestroyContext(s_display, s_context);
  if (s_surface) eglDestroySurface(s_display, s_surface);
  eglTerminate(s_display);
  s_display = EGL_NO_DISPLAY;
}

// ---------------------------------------------------------------------------
// module loading (bump-allocate both images in the reserved so heap)
// ---------------------------------------------------------------------------
static char *so_cursor;
static size_t so_remaining;

static int load_module(so_module *mod, const char *name) {
  int r = so_load(mod, name, so_cursor, so_remaining);
  if (r != 0) return r;
  size_t used = ALIGN_MEM(mod->load_size, 0x1000);
  so_cursor += used;
  so_remaining -= used;
  return 0;
}

// ---------------------------------------------------------------------------
// Cocos2d-x native entry points (JNI-mangled exports of libMyGame.so)
// ---------------------------------------------------------------------------
#define RN "Java_org_cocos2dx_lib_Cocos2dxRenderer_"
#define AN "Java_org_cocos2dx_lib_Cocos2dxActivity_"

static int  (*JNI_OnLoad)(void *vm, void *reserved);
static void (*nativeInit)(void *env, void *cls, int w, int h);
static void (*nativeRender)(void *env, void *cls);
static void (*nativeOnResume)(void *env, void *cls);
static void (*nativeOnPause)(void *env, void *cls);
static void (*nativeResize)(void *env, void *cls, int w, int h);
static void (*nativeTouchesBegin)(void *env, void *cls, int id, float x, float y);
static void (*nativeTouchesEnd)(void *env, void *cls, int id, float x, float y);
static void (*nativeTouchesMove)(void *env, void *cls, void *ids, void *xs, void *ys);
static void (*nativeSetPaths)(void *env, void *cls, void *asset_path);
static void (*nativeKeyDown)(void *env, void *cls, int keycode);
static void (*nativeOnRemoteConfigRead)(void *env, void *cls, int success);
static void (*nativeReturnCountryCode)(void *env, void *cls, void *code);
static void (*nativeSetNetworkAvailable)(void *env, void *cls, int available);
static void (*nativeConsentInfoFinished)(void *env, void *cls);
static void (*nativeConsentFlowFinished)(void *env, void *cls);
static void (*nativeReturnPopupOfferJson)(void *env, void *cls, void *json);
static void (*nativeReturnMissionJson)(void *env, void *cls, void *json);
static void (*nativeSetInAppItem)(void *env, void *cls,
    void *id, void *price, void *amount, void *bonus, void *icon,
    uint8_t ad_free, uint8_t unknown, uint8_t special,
    uint8_t gems, uint8_t paints, int bundle);
static void (*nativeSetInAppItemPrice)(void *env, void *cls, void *id, void *price);
static void (*nativeSetInAppItemCurrencyCode)(void *env, void *cls, void *id, void *currency);
// cocos GameControllerAdapter signatures: the first arg after (env,cls) is the
// controller's vendor-name jstring, used to look the controller up -- passing
// anything else drops the event.
static void (*nativeControllerConnected)(void *env, void *cls, void *vendorName, int ctrl);
static void (*nativeControllerButtonEvent)(void *env, void *cls, void *vendorName, int ctrl, int button, int pressed, float value, int isAnalog);
static void (*nativeControllerAxisEvent)(void *env, void *cls, void *vendorName, int ctrl, int axis, float value, int isAnalog);
static void *g_ctrl_name; // the jstring passed to connect + every event

static void resolve_entry_points(void) {
  JNI_OnLoad            = (void *)so_find_addr_rx(&game_mod, "JNI_OnLoad");
  nativeInit            = (void *)so_find_addr_rx(&game_mod, RN "nativeInit");
  nativeRender          = (void *)so_find_addr_rx(&game_mod, RN "nativeRender");
  nativeOnResume        = (void *)so_try_find_addr_rx(&game_mod, RN "nativeOnResume");
  nativeOnPause         = (void *)so_try_find_addr_rx(&game_mod, RN "nativeOnPause");
  nativeResize          = (void *)so_try_find_addr_rx(&game_mod, RN "nativeResize");
  nativeTouchesBegin    = (void *)so_try_find_addr_rx(&game_mod, RN "nativeTouchesBegin");
  nativeTouchesEnd      = (void *)so_try_find_addr_rx(&game_mod, RN "nativeTouchesEnd");
  nativeTouchesMove     = (void *)so_try_find_addr_rx(&game_mod, RN "nativeTouchesMove");
  nativeSetPaths        = (void *)so_find_addr_rx(&game_mod, AN "nativeSetPaths");
  nativeKeyDown         = (void *)so_try_find_addr_rx(&game_mod, RN "nativeKeyDown");
  nativeOnRemoteConfigRead = (void *)so_try_find_addr_rx(&game_mod,
      "Java_com_fingersoft_game_MainActivity_onRemoteConfigRead");
  nativeReturnCountryCode = (void *)so_try_find_addr_rx(&game_mod,
      "Java_com_fingersoft_game_MainActivity_returnCountryCode");
  nativeSetNetworkAvailable = (void *)so_try_find_addr_rx(&game_mod,
      "Java_com_fingersoft_game_MainActivity_setNetworkAvailable");
  nativeConsentInfoFinished = (void *)so_try_find_addr_rx(&game_mod,
      "Java_com_fingersoft_game_consent_ConsentManager_onConsentInfoUpdateFinished");
  nativeConsentFlowFinished = (void *)so_try_find_addr_rx(&game_mod,
      "Java_com_fingersoft_game_consent_ConsentManager_onConsentFlowFinishedNative");
  nativeReturnPopupOfferJson = (void *)so_try_find_addr_rx(&game_mod,
      "Java_com_fingersoft_game_MainActivity_returnPopupOfferJson");
  nativeReturnMissionJson = (void *)so_try_find_addr_rx(&game_mod,
      "Java_com_fingersoft_game_MainActivity_returnMissionJson");
  nativeSetInAppItem = (void *)so_try_find_addr_rx(&game_mod,
      "Java_com_fingersoft_game_MainActivity_setInAppItem");
  nativeSetInAppItemPrice = (void *)so_try_find_addr_rx(&game_mod,
      "Java_com_fingersoft_game_MainActivity_setInAppItemPrice");
  nativeSetInAppItemCurrencyCode = (void *)so_try_find_addr_rx(&game_mod,
      "Java_com_fingersoft_game_MainActivity_setInAppItemCurrencyCode");
  #define GC "Java_org_cocos2dx_lib_GameControllerAdapter_"
  nativeControllerConnected   = (void *)so_try_find_addr_rx(&game_mod, GC "nativeControllerConnected");
  nativeControllerButtonEvent = (void *)so_try_find_addr_rx(&game_mod, GC "nativeControllerButtonEvent");
  nativeControllerAxisEvent   = (void *)so_try_find_addr_rx(&game_mod, GC "nativeControllerAxisEvent");
}

typedef struct {
  const char *id;
  const char *bonus;
  const char *icon;
  int amount;
  uint8_t ad_free, unknown, special, gems, paints;
  int bundle;
} ShopCatalogItem;

// Exact catalog from NewBillingHandle.Init() in classes2.dex. Android invokes
// setInAppItem() for these 69 entries before connecting to Google Play. This
// port does not execute Dalvik bytecode, so replay that required Java->native
// initialization directly; otherwise the native shop indexes an empty vector.
static const ShopCatalogItem original_shop_catalog[] = {
  { "com.fingersoft.hillclimb.adfree_t1", "", "coin0", 100000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.adfree_t2", "", "coin1", 300000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.adfree_t3", "", "coin2", 500000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.adfree_150000coins", "", "coin0", 150000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.adfree_300000coins", "+25%", "coin1", 300000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.adfree_750000coins", "+99%", "coin2", 750000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.adfree_2000000coins", "+166%", "coin3", 2000000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.adfree_4000000coins", "+212%", "coin4", 4000000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.adfree_8000000coins", "+308%", "coin5", 8000000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.adfree_20000000coins", "+431%", "coin6", 20000000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap2.adfree_300000coins", "", "coin0", 300000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap2.adfree_600000coins", "+25%", "coin1", 600000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap2.adfree_1500000coins", "+99%", "coin2", 1500000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap2.adfree_4000000coins", "+166%", "coin3", 4000000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap2.adfree_8000000coins", "+212%", "coin4", 8000000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap2.adfree_16000000coins", "+308%", "coin5", 16000000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap2.adfree_40000000coins", "+431%", "coin6", 40000000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap3.adfree_600000coins", "", "coin1", 600000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap3.adfree_1200000coins", "+25%", "coin1", 1200000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap3.adfree_3000000coins", "+99%", "coin2", 3000000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap3.adfree_8000000coins", "+166%", "coin3", 8000000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap3.adfree_16000000coins", "+212%", "coin4", 16000000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap3.adfree_32000000coins", "+308%", "coin5", 32000000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap3.adfree_80000000coins", "+431%", "coin6", 80000000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap4.adfree_1200000coins", "", "coin1", 1200000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap4.adfree_2400000coins", "+25%", "coin1", 2400000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap4.adfree_6000000coins", "+99%", "coin2", 6000000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap4.adfree_16000000coins", "+166%", "coin3", 16000000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap4.adfree_32000000coins", "+212%", "coin4", 32000000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap4.adfree_64000000coins", "+308%", "coin5", 64000000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap4.adfree_160000000coins", "+431%", "coin6", 160000000, 0, 0, 0, 0, 0, 0 },
  { "com.fingersoft.hillclimb.iap2.adfree_500gems", "", "diamond1", 500, 0, 0, 0, 1, 0, 0 },
  { "com.fingersoft.hillclimb.iap2.adfree_2000gems", "+132%", "diamond2", 2000, 0, 0, 0, 1, 0, 0 },
  { "com.fingersoft.hillclimb.iap2.adfree_5000gems", "+199%", "diamond3", 5000, 0, 0, 0, 1, 0, 0 },
  { "com.fingersoft.hillclimb.iap2.adfree_11000gems", "+219%", "diamond4", 11000, 0, 0, 0, 1, 0, 0 },
  { "com.fingersoft.hillclimb.iap2.adfree_23000gems", "+305%", "diamond5", 23000, 0, 0, 0, 1, 0, 0 },
  { "com.fingersoft.hillclimb.iap2.adfree_40000gems", "+318%", "diamond6", 40000, 0, 0, 0, 1, 0, 0 },
  { "com.fingersoft.hillclimb.iap1.adfree_1000gems", "", "diamond1", 1000, 0, 0, 0, 1, 0, 0 },
  { "com.fingersoft.hillclimb.iap1.adfree_3000gems", "+99%", "diamond2", 3000, 0, 0, 0, 1, 0, 0 },
  { "com.fingersoft.hillclimb.iap1.adfree_6000gems", "+119%", "diamond3", 6000, 0, 0, 0, 1, 0, 0 },
  { "com.fingersoft.hillclimb.iap1.adfree_15000gems", "+149%", "diamond4", 15000, 0, 0, 0, 1, 0, 0 },
  { "com.fingersoft.hillclimb.iap1.adfree_26000gems", "+172%", "diamond5", 26000, 0, 0, 0, 1, 0, 0 },
  { "com.fingersoft.hillclimb.iap1.adfree_50000gems", "+199%", "diamond6", 50000, 0, 0, 0, 1, 0, 0 },
  { "com.fingersoft.hillclimb.paint1", "", "paint1", 60, 0, 0, 0, 0, 1, 0 },
  { "com.fingersoft.hillclimb.paint2", "", "paint2", 160, 0, 0, 0, 0, 1, 0 },
  { "com.fingersoft.hillclimb.paint3", "", "paint3", 330, 0, 0, 0, 0, 1, 0 },
  { "com.fingersoft.hillclimb.specialoffer1", "+8212%", "coin2", 100000000, 0, 0, 1, 0, 0, 0 },
  { "com.fingersoft.hillclimb.specialoffer2", "+1562%", "coin2", 20000000, 0, 0, 1, 0, 0, 0 },
  { "com.fingersoft.hillclimb.specialoffer3", "+316%", "coin2", 5000000, 0, 0, 1, 0, 0, 0 },
  { "com.fingersoft.hillclimb.specialoffer1gems", "+232%", "diamond2", 30000, 0, 0, 1, 0, 0, 0 },
  { "com.fingersoft.hillclimb.special_garage1", "", "bundle1", 0, 0, 0, 0, 1, 0, 1 },
  { "com.fingersoft.hillclimb.special_garage2", "", "bundle1", 0, 0, 0, 0, 1, 0, 2 },
  { "com.fingersoft.hillclimb.bundle10", "", "", 0, 0, 0, 0, 1, 0, 10 },
  { "com.fingersoft.hillclimb.bundle11", "", "", 0, 0, 0, 0, 1, 0, 11 },
  { "com.fingersoft.hillclimb.bundle12", "", "", 0, 0, 0, 0, 1, 0, 12 },
  { "com.fingersoft.hillclimb.bundle13", "", "", 0, 0, 0, 0, 1, 0, 13 },
  { "com.fingersoft.hillclimb.bundle14", "", "", 0, 0, 0, 0, 1, 0, 14 },
  { "com.fingersoft.hillclimb.bundle15", "", "", 0, 0, 0, 0, 1, 0, 15 },
  { "com.fingersoft.hillclimb.bundle16", "", "", 0, 0, 0, 0, 1, 0, 16 },
  { "com.fingersoft.hillclimb.bundle17", "", "", 0, 0, 0, 0, 1, 0, 17 },
  { "com.fingersoft.hillclimb.bundle18", "", "", 0, 0, 0, 0, 1, 0, 18 },
  { "com.fingersoft.hillclimb.bundle19", "", "", 0, 0, 0, 0, 1, 0, 19 },
  { "com.fingersoft.hillclimb.bundle20", "", "", 0, 0, 0, 0, 1, 0, 20 },
  { "com.fingersoft.hillclimb.bundle21", "", "", 0, 0, 0, 0, 1, 0, 21 },
  { "com.fingersoft.hillclimb.bundle22", "", "", 0, 0, 0, 0, 0, 0, 22 },
  { "com.fingersoft.hillclimb.bundle23", "", "", 0, 0, 0, 0, 0, 0, 23 },
  { "com.fingersoft.hillclimb.vehicles141021", "", "", 0, 0, 0, 0, 1, 0, 996 },
  { "com.fingersoft.hillclimb.stages141021", "", "", 0, 0, 0, 0, 1, 0, 997 },
  { "com.fingersoft.hillclimb.vehiclesstages141021", "", "", 0, 0, 0, 0, 1, 0, 998 },
};
_Static_assert(sizeof(original_shop_catalog) / sizeof(original_shop_catalog[0]) == 69,
               "classes2.dex shop catalog count changed");

int hcr_lookup_shop_reward(const char *id, HcrShopReward *reward) {
  if (!id || !reward)
    return 0;

  const size_t count = sizeof(original_shop_catalog) / sizeof(original_shop_catalog[0]);
  for (size_t i = 0; i < count; i++) {
    const ShopCatalogItem *item = &original_shop_catalog[i];
    if (strcmp(item->id, id))
      continue;

    if (item->bundle > 0) {
      reward->kind = HCR_SHOP_REWARD_BUNDLE;
      reward->amount = item->bundle;
    } else if (item->gems) {
      reward->kind = HCR_SHOP_REWARD_GEMS;
      reward->amount = item->amount;
    } else if (item->paints) {
      reward->kind = HCR_SHOP_REWARD_PAINTS;
      reward->amount = item->amount;
    } else {
      reward->kind = HCR_SHOP_REWARD_COINS;
      reward->amount = item->amount;
    }
    return 1;
  }

  reward->kind = HCR_SHOP_REWARD_NONE;
  reward->amount = 0;
  return 0;
}

static void format_shop_amount(char out[24], int value) {
  char raw[16];
  snprintf(raw, sizeof(raw), "%d", value);
  const size_t len = strlen(raw);
  size_t dst = 0;
  for (size_t src = 0; src < len && dst + 1 < 24; src++) {
    if (src && (len - src) % 3 == 0) out[dst++] = ' ';
    out[dst++] = raw[src];
  }
  out[dst] = 0;
}

static void set_offline_shop_price(const char *id) {
  void *id_ref = jni_make_string(id);
  void *price_ref = jni_make_string("FREE");
  void *currency_ref = jni_make_string("");
  nativeSetInAppItemPrice(fake_env, jni_make_thiz(), id_ref, price_ref);
  nativeSetInAppItemCurrencyCode(fake_env, jni_make_thiz(), id_ref, currency_ref);
  jni_delete_local_ref(currency_ref);
  jni_delete_local_ref(price_ref);
  jni_delete_local_ref(id_ref);
}

// Mirrors NewBillingHandle.setPopupOfferId() from classes2.dex. Dynamic popup
// offers are not part of the fixed 69-item Init() catalog, so they must be
// inserted when native code asks Java to do so.
static void initialize_offline_popup_offer(const char *id) {
  if (!id || !*id || !nativeSetInAppItem) return;
  void *args[5] = {
    jni_make_string(id), jni_make_string("FREE"), jni_make_string(""),
    jni_make_string(""), jni_make_string(""),
  };
  nativeSetInAppItem(fake_env, jni_make_thiz(), args[0], args[1], args[2], args[3], args[4],
      0, 0, 0, 0, 1, 999);
  for (size_t i = 0; i < 5; i++) jni_delete_local_ref(args[i]);
  set_offline_shop_price(id);
}

static void initialize_original_shop_catalog(void) {
  if (game_mod.load_size != 0x817000) return;
  if (!nativeSetInAppItem || !nativeSetInAppItemPrice ||
      !nativeSetInAppItemCurrencyCode)
    fatal_error("Original HCR shop exports are missing.");

  const size_t count = sizeof(original_shop_catalog) / sizeof(original_shop_catalog[0]);
  char format_check[24];
  format_shop_amount(format_check, 1000000);
  if (strcmp(format_check, "1 000 000"))
    fatal_error("Shop amount formatter self-check failed.");
  for (size_t i = 0; i < count; i++) {
    const ShopCatalogItem *item = &original_shop_catalog[i];
    char amount[24];
    format_shop_amount(amount, item->amount);

    void *args[5] = {
      jni_make_string(item->id), jni_make_string("FREE"),
      jni_make_string(amount), jni_make_string(item->bonus),
      jni_make_string(item->icon),
    };
    nativeSetInAppItem(fake_env, jni_make_thiz(), args[0], args[1], args[2], args[3], args[4],
        item->ad_free, item->unknown, item->special,
        item->gems, item->paints, item->bundle);
    for (size_t j = 0; j < 5; j++) jni_delete_local_ref(args[j]);
    // The APK fills these fields later through Google Play's async product
    // query. Supply its own documented fallback immediately on Switch.
    set_offline_shop_price(item->id);
  }
}

// HCR 1.71.1's offline startup can construct no daily-event card, then pass
// that null card to cocos2d::Node::addChild. Android's Java layer normally
// supplies the event before this UI path runs. Keep every non-null addChild
// call intact and return only from this assertion-only null path.
static void patch_optional_daily_event_node(void) {
  enum { HCR_NULL_CHILD_PATH = 0x66d824 };
  static const u32 null_return[] = {
    0xf9400bf5, // ldr x21, [sp, #16]
    0xa9424ff4, // ldp x20, x19, [sp, #32]
    0xa8c37bfd, // ldp x29, x30, [sp], #48
    0xd65f03c0, // ret
    0xd503201f, 0xd503201f, 0xd503201f, 0xd503201f,
    0xd503201f, 0xd503201f, 0xd503201f,
  };
  _Static_assert(sizeof(null_return) == 11 * sizeof(u32), "unexpected patch size");
  // This guard is specific to the modded 1.71.1 binary. The original APK has
  // a different image layout; never write an offset from another build into it.
  if (game_mod.load_size != 0x8da000) {
    return;
  }
  u32 *site = (u32 *)((uintptr_t)game_mod.load_base + HCR_NULL_CHILD_PATH);
  if (HCR_NULL_CHILD_PATH + sizeof(null_return) > game_mod.load_size ||
      site[-1] != 0xb5000181u || site[0] != 0x90ffdb61u)
    fatal_error("Unsupported HCR 1.71.1 libgame.so build.");
  memcpy(site, null_return, sizeof(null_return));
}

static void *thiz;
static void *renderer_cls;
static void *helper_cls;

// ---------------------------------------------------------------------------
// input: touchscreen fingers map straight to cocos touches; the right stick
// drives a virtual pointer for docked play. (physical-button mapping TBD)
// ---------------------------------------------------------------------------
static PadState pad;
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

#define AKEYCODE_BUTTON_A 96
#define AKEYCODE_BUTTON_B 97
#define AKEYCODE_BUTTON_X 99
#define AKEYCODE_BUTTON_Y 100
#define AKEYCODE_BUTTON_L1 102
#define AKEYCODE_BUTTON_R1 103
#define AKEYCODE_BUTTON_START 108
#define AKEYCODE_BUTTON_SELECT 109

// --- hidden right-stick position retained for touch-only menus ---------------
static float g_cursor_x = -1.f, g_cursor_y = -1.f; // px, y-down; -1 = uninit
static float g_cursor_alpha = 0.f;                 // for the overlay renderer
#define CURSOR_TTL_FRAMES  90   // full-visible frames after last activity (~3s)
#define CURSOR_FADE_FRAMES 20   // fade-out span (~0.7s)

// movement is NOT via GetAnalogPosition (injecting there didn't move the char);
// it comes from the VirtualPad's touch->analog conversion. So we DON'T hook it
// -- instead we synthesize a floating-joystick touch drag from the stick/dpad
// (see update_input), letting the original GetAnalogPosition read it normally.
// pointer state for the touch pump: [0]=real touchscreen, [1]=synth joystick
typedef struct { int active; float x, y; } TouchPtr;

static void update_input(void) {
  padUpdate(&pad);
  u64 b = padGetButtons(&pad);

  // Cocos2d-x 2.x exposes key-down but not key-up. Emit only new presses;
  // touchscreen input remains the authoritative held/drag interaction.
  if (nativeKeyDown) {
    static u64 previous = 0;
    const u64 pressed = b & ~previous;
    const struct { u64 button; int keycode; } keys[] = {
      {HidNpadButton_A, AKEYCODE_BUTTON_A}, {HidNpadButton_B, AKEYCODE_BUTTON_B},
      {HidNpadButton_X, AKEYCODE_BUTTON_X}, {HidNpadButton_Y, AKEYCODE_BUTTON_Y},
      {HidNpadButton_L, AKEYCODE_BUTTON_L1}, {HidNpadButton_R, AKEYCODE_BUTTON_R1},
      {HidNpadButton_Plus, AKEYCODE_BUTTON_START}, {HidNpadButton_Minus, AKEYCODE_BUTTON_SELECT},
    };
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
      if (pressed & keys[i].button)
        nativeKeyDown(fake_env, renderer_cls, keys[i].keycode);
    previous = b;
  }

  // --- movement: normalized stick/dpad direction (sx,sy), screen y still up ---
  float sx = (float)padGetStickPos(&pad, 0).x / 32767.0f;
  float sy = (float)padGetStickPos(&pad, 0).y / 32767.0f;
  if (b & HidNpadButton_Left)  sx = -1.0f;
  if (b & HidNpadButton_Right) sx =  1.0f;
  if (b & HidNpadButton_Up)    sy =  1.0f;
  if (b & HidNpadButton_Down)  sy = -1.0f;
  float dz = 0.30f;
  if (sx > -dz && sx < dz) sx = 0.0f;
  if (sy > -dz && sy < dz) sy = 0.0f;
  if (sx > 1.f) sx = 1.f; else if (sx < -1.f) sx = -1.f;
  if (sy > 1.f) sy = 1.f; else if (sy < -1.f) sy = -1.f;
  int stick_move = (sx != 0.0f || sy != 0.0f);

  // --- hidden right-stick cursor ---------------------------------------------
  static int cursor_ttl = 0;
  {
    if (g_cursor_x < 0.f) { g_cursor_x = screen_width * 0.5f; g_cursor_y = screen_height * 0.5f; }
    float rx = (float)padGetStickPos(&pad, 1).x / 32767.0f;
    float ry = (float)padGetStickPos(&pad, 1).y / 32767.0f;
    float rdz = 0.20f;
    if (rx > -rdz && rx < rdz) rx = 0.0f;
    if (ry > -rdz && ry < rdz) ry = 0.0f;
    if (rx != 0.0f || ry != 0.0f) {
      float spd = (float)(config.cursor_speed > 0 ? config.cursor_speed : 32);
      g_cursor_x = clampf(g_cursor_x + rx * spd, 0, screen_width);
      g_cursor_y = clampf(g_cursor_y - ry * spd, 0, screen_height); // stick up -> screen up
      cursor_ttl = CURSOR_TTL_FRAMES;
    }
    if (cursor_ttl > 0) cursor_ttl--;
    g_cursor_alpha = cursor_ttl >= CURSOR_FADE_FRAMES ? 1.0f
                   : (float)cursor_ttl / (float)CURSOR_FADE_FRAMES;
  }

  // --- build the touch pointers ----------------------------------------------
  // [0] real touchscreen, [1] synth movement joystick, [2] brake, [3] gas
  TouchPtr pnew[4] = {{0}};

  // pointer 0: real touchscreen (now with proper move, so drag works)
  HidTouchScreenState st = {0};
  int nt = hidGetTouchScreenStates(&st, 1) ? st.count : 0;
  if (nt > 0) {
    pnew[0].active = 1;
    pnew[0].x = clampf(st.touches[0].x * (screen_width / 1280.0f), 0, screen_width);
    pnew[0].y = clampf(st.touches[0].y * (screen_height / 720.0f), 0, screen_height);
  }

  // pointer 1: joystick synthesized from the physical stick/dpad. Land the
  // finger on the on-screen pad, then drag it toward the direction -- exactly
  // what a held virtual-pad nub looks like, so the VirtualPad converts it to
  // analog movement. The field pad is FLOATING (appears wherever you touch) but
  // the world-map / dungeon-map screens have a FIXED pad at the bottom-left
  // (~150,490 px @1280x720). Landing on that fixed spot works in ALL contexts:
  // the fixed pads engage it, and the floating field pad just spawns there.
  static int move_began = 0;
  float ox = (float)screen_width * 0.117f, oy = (float)screen_height * 0.68f;
  float radius = (float)screen_height * 0.16f;
  if (stick_move) {
    pnew[1].active = 1;
    if (!move_began) { pnew[1].x = ox; pnew[1].y = oy; move_began = 1; }
    else { pnew[1].x = ox + sx * radius; pnew[1].y = oy - sy * radius; }
  } else {
    move_began = 0;
  }

  // HCR's native controls are touchscreen buttons at the bottom corners.
  // Map Switch triggers to those held touches: ZL brake, ZR gas.
  if (b & HidNpadButton_ZL) {
    pnew[2].active = 1;
    pnew[2].x = screen_width * 0.16f;
    pnew[2].y = screen_height * 0.82f;
  }
  if (b & HidNpadButton_ZR) {
    pnew[3].active = 1;
    pnew[3].x = screen_width * 0.84f;
    pnew[3].y = screen_height * 0.82f;
  }

  // --- dispatch begin/move/end from the pnew vs pcur diff --------------------
  static TouchPtr pcur[4] = {{0}};
  static void *mv_ids, *mv_xs, *mv_ys;
  if (!mv_ids) {
    mv_ids = jni_make_input_array(4, 4);
    mv_xs  = jni_make_input_array(4, 4);
    mv_ys  = jni_make_input_array(4, 4);
  }
  for (int i = 0; i < 4; i++)  // begins
    if (pnew[i].active && !pcur[i].active && nativeTouchesBegin)
      nativeTouchesBegin(fake_env, renderer_cls, i, pnew[i].x, pnew[i].y);
  int ids[4]; float xs[4], ys[4]; int nm = 0;  // moves (batched)
  for (int i = 0; i < 4; i++)
    if (pnew[i].active && pcur[i].active &&
        (pnew[i].x != pcur[i].x || pnew[i].y != pcur[i].y)) {
      ids[nm] = i; xs[nm] = pnew[i].x; ys[nm] = pnew[i].y; nm++;
    }
  if (nm > 0 && nativeTouchesMove) {
    jni_input_array_set(mv_ids, ids, nm);
    jni_input_array_set(mv_xs, xs, nm);
    jni_input_array_set(mv_ys, ys, nm);
    nativeTouchesMove(fake_env, renderer_cls, mv_ids, mv_xs, mv_ys);
  }
  for (int i = 0; i < 4; i++)  // ends
    if (!pnew[i].active && pcur[i].active && nativeTouchesEnd)
      nativeTouchesEnd(fake_env, renderer_cls, i, pcur[i].x, pcur[i].y);
  pcur[0] = pnew[0]; pcur[1] = pnew[1]; pcur[2] = pnew[2]; pcur[3] = pnew[3];
}

int main(void) {
  cpu_boost(1);

  if (read_config(CONFIG_NAME) != 0)
    write_config(CONFIG_NAME);

  if (installer_prepare_game_files() != 0)
    fatal_error("%s", installer_last_error());
  check_syscalls();
  check_data();
  set_screen_size(config.screen_width, config.screen_height);

  plInitialize(PlServiceType_User);
  hcr_http_init();

  if (!egl_init())
    fatal_error("Failed to create an OpenGL ES 2 context.");

  // --- load the self-contained Cocos2d-x module ----------------------------
  so_cursor = heap_so_base;
  so_remaining = heap_so_limit;

  if (load_module(&game_mod, SO_NAME) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);

  // libgame.so bundles its C++ runtime; only its Android/Bionic imports need shims.
  if (vpl_resolve_imports(&game_mod) != 0)
    fatal_error("Unsupported libgame.so: unresolved Android imports.");

  resolve_entry_points();
  if (!JNI_OnLoad || !nativeInit || !nativeRender || !nativeSetPaths)
    fatal_error("Could not resolve Cocos2d-x engine entry points.");
  patch_optional_daily_event_node();
  so_finalize(&game_mod);
  so_flush_caches(&game_mod);

  tls_setup_guard();

  so_execute_init_array(&game_mod);
  so_free_temp(&game_mod);

  mkdir("save", 0777);
  jni_init();
  jni_set_popup_offer_callback(initialize_offline_popup_offer);
  thiz = jni_make_thiz();

  // JNI_OnLoad -> JniHelper::setJavaVM + cocos_android_app_init (AppDelegate)
  JNI_OnLoad(fake_vm, NULL);

  // the jclass the renderer/helper natives are "called on" is unused by the
  // natives themselves; any non-null object works for these statics
  helper_cls   = thiz;
  renderer_cls = thiz;
  hcr_http_set_callbacks(fake_env, renderer_cls, nativeReturnPopupOfferJson,
                         nativeReturnMissionJson);

  // CCNewFileUtilsAndroid accepts an APK path here, not an assets directory.
  // Keep hcr.apk after installation so its internal asset index remains valid.
  nativeSetPaths(fake_env, helper_cls, jni_make_string(DATA_DIR "/" GAME_APK));

  initialize_original_shop_catalog();

  // Cocos2dxRenderer.nativeInit(w,h) -> applicationDidFinishLaunching
  nativeInit(fake_env, renderer_cls, screen_width, screen_height);
  // Match MainActivity's NetworkCallback using Horizon's real NIFM state.
  if (nativeSetNetworkAvailable)
    nativeSetNetworkAvailable(fake_env, renderer_cls, hcr_http_network_available());
  if (nativeReturnCountryCode)
    nativeReturnCountryCode(fake_env, renderer_cls, jni_make_string("UNKNOWN"));
  // Android reports this asynchronously after Firebase falls back to its
  // built-in defaults. There is no Firebase service in the Switch port.
  if (nativeOnRemoteConfigRead) {
    nativeOnRemoteConfigRead(fake_env, renderer_cls, 0);
  }
  // ConsentManager starts an Android async request during nativeInit. There is
  // no Android UI flow on Switch, so complete both stages with the game's
  // no-form-needed state before the event loader reaches its final gate.
  if (nativeConsentInfoFinished) nativeConsentInfoFinished(fake_env, renderer_cls);
  if (nativeConsentFlowFinished) nativeConsentFlowFinished(fake_env, renderer_cls);
  if (nativeResize)
    nativeResize(fake_env, renderer_cls, screen_width, screen_height);
  if (nativeOnResume)
    nativeOnResume(fake_env, renderer_cls);

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();

  // register a virtual game controller so VirtualPad/Lib_GameControllerInput
  // accept our button events (the game reads physical input, not just touch)
  if (nativeControllerConnected) {
    g_ctrl_name = jni_make_string("Nintendo Switch Controller");
    nativeControllerConnected(fake_env, renderer_cls, g_ctrl_name, 0);
  }

  while (appletMainLoop() && !jni_quit_requested) {
    update_input();
    hcr_http_pump();
    nativeRender(fake_env, renderer_cls);
    opensles_pump();
    if (jni_take_consent_form_request() && nativeConsentFlowFinished) {
      nativeConsentFlowFinished(fake_env, renderer_cls);
    }
    eglSwapBuffers(s_display, s_surface);

    // eglSwapInterval(1) already paces this loop to the display refresh. An
    // additional 16.7 ms sleep here double-throttled gameplay, transitions and
    // frame-driven loading to roughly 30 FPS.
  }

  if (nativeOnPause) nativeOnPause(fake_env, renderer_cls);
  jni_flush_prefs();

  opensles_shutdown();
  egl_deinit();
  hcr_http_exit();
  plExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
