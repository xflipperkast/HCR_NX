/* opensles.c -- minimal OpenSL ES shim backed by SDL2 audio
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * Implements the slice of OpenSL ES 1.0.1 the SQEX "Sd" sound driver uses:
 * the Object interface (Realize/GetInterface/Destroy), the Engine interface
 * (CreateOutputMix/CreateAudioPlayer), and on each player the Play, Volume and
 * AndroidSimpleBufferQueue interfaces. Players are software-mixed into one SDL2
 * audio output; the buffer-queue completion callback is fired while the
 * Switch audio service releases each finished buffer.
 */

#include <stdlib.h>
#include <malloc.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "opensles.h"
#include "asset_shim.h"
#include "util.h"
#include "config.h"

// Reuse the public-domain Vorbis decoder already used by the bundled S4EP2
// reference port. HCR's Android SimpleAudioEngine feeds it OGG assets through
// Java; this wrapper decodes those same assets directly from our asset shim.
#define STB_VORBIS_NO_STDIO
#include "../../references/s4ep2-nx/source/stb_vorbis.c"

// --- OpenSL ES constants ----------------------------------------------------

#define SL_RESULT_SUCCESS              0
#define SL_RESULT_PARAMETER_INVALID    0x0D
#define SL_RESULT_FEATURE_UNSUPPORTED  0x0C

#define SL_BOOLEAN_FALSE 0
#define SL_BOOLEAN_TRUE  1

#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED  2
#define SL_PLAYSTATE_PLAYING 3

#define SL_OBJECT_STATE_REALIZED 2

typedef uint32_t SLuint32;
typedef int32_t  SLint32;
typedef uint32_t SLresult;
typedef uint32_t SLboolean;
typedef int32_t  SLmillibel;

// PCM data format (samplesPerSec is in milliHz per the spec)
typedef struct {
  SLuint32 formatType;
  SLuint32 numChannels;
  SLuint32 samplesPerSec;
  SLuint32 bitsPerSample;
  SLuint32 containerSize;
  SLuint32 channelMask;
  SLuint32 endianness;
} SLDataFormat_PCM;

typedef struct {
  SLuint32 locatorType;
  SLuint32 numBuffers;
} SLDataLocator_BufferQueue;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSource;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSink;

typedef void *SLObjectItf;       // -> &obj->obj_vt
typedef void *SLInterfaceID;

// callback: (SLAndroidSimpleBufferQueueItf caller, void *pContext)
typedef void (*slBufferQueueCallback)(void *caller, void *context);

// --- interface-id sentinels -------------------------------------------------

#define DEF_IID(n) void *SL_IID_##n = &SL_IID_##n
DEF_IID(3DCOMMIT); DEF_IID(3DDOPPLER); DEF_IID(3DGROUPING); DEF_IID(3DLOCATION);
DEF_IID(3DMACROSCOPIC); DEF_IID(3DSOURCE); DEF_IID(ANDROIDCONFIGURATION);
DEF_IID(ANDROIDEFFECT); DEF_IID(ANDROIDEFFECTCAPABILITIES); DEF_IID(ANDROIDEFFECTSEND);
DEF_IID(ANDROIDSIMPLEBUFFERQUEUE); DEF_IID(AUDIODECODERCAPABILITIES); DEF_IID(AUDIOENCODER);
DEF_IID(AUDIOENCODERCAPABILITIES); DEF_IID(AUDIOIODEVICECAPABILITIES); DEF_IID(BASSBOOST);
DEF_IID(BUFFERQUEUE); DEF_IID(DEVICEVOLUME); DEF_IID(DYNAMICINTERFACEMANAGEMENT);
DEF_IID(DYNAMICSOURCE); DEF_IID(EFFECTSEND); DEF_IID(ENGINE); DEF_IID(ENGINECAPABILITIES);
DEF_IID(ENVIRONMENTALREVERB); DEF_IID(EQUALIZER); DEF_IID(LED); DEF_IID(METADATAEXTRACTION);
DEF_IID(METADATATRAVERSAL); DEF_IID(MIDIMESSAGE); DEF_IID(MIDIMUTESOLO); DEF_IID(MIDITEMPO);
DEF_IID(MIDITIME); DEF_IID(MUTESOLO); DEF_IID(NULL); DEF_IID(OBJECT); DEF_IID(OUTPUTMIX);
DEF_IID(PITCH); DEF_IID(PLAY); DEF_IID(PLAYBACKRATE); DEF_IID(PREFETCHSTATUS);
DEF_IID(PRESETREVERB); DEF_IID(RATEPITCH); DEF_IID(RECORD); DEF_IID(SEEK); DEF_IID(THREADSYNC);
DEF_IID(VIBRA); DEF_IID(VIRTUALIZER); DEF_IID(VISUALIZATION); DEF_IID(VOLUME);
#undef DEF_IID

// --- vtable structs (method order matches the OpenSL ES 1.0.1 spec) ---------

typedef struct {
  SLresult (*Realize)(void *self, SLboolean async);
  SLresult (*Resume)(void *self, SLboolean async);
  SLresult (*GetState)(void *self, SLuint32 *pState);
  SLresult (*GetInterface)(void *self, const SLInterfaceID iid, void *pInterface);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  SLresult (*AbortAsyncOperation)(void *self);
  void     (*Destroy)(void *self);
  SLresult (*SetPriority)(void *self, SLint32 priority, SLboolean preemptable);
  SLresult (*GetPriority)(void *self, SLint32 *pPriority);
  SLresult (*SetLossOfControlInterfaces)(void *self, SLint32 n, SLInterfaceID *ids, SLboolean enabled);
} SLObjectItf_;

// only CreateAudioPlayer (slot 2) and CreateOutputMix (slot 7) are used; the
// rest keep the correct layout but are generic so a shared stub assigns
// cleanly. The engine calls each slot with its own typed vtable.
typedef struct {
  void *CreateLEDDevice;
  void *CreateVibraDevice;
  SLresult (*CreateAudioPlayer)(void *self, SLObjectItf *pPlayer, SLDataSource *src, SLDataSink *snk,
                                SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req);
  void *CreateAudioRecorder;
  void *CreateMidiPlayer;
  void *CreateListener;
  void *Create3DGroup;
  SLresult (*CreateOutputMix)(void *self, SLObjectItf *pMix, SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req);
  void *CreateMetadataExtractor;
  void *CreateExtensionObject;
  void *QueryNumSupportedInterfaces;
  void *QuerySupportedInterfaces;
  void *QueryNumSupportedExtensions;
  void *QuerySupportedExtension;
  void *IsExtensionSupported;
} SLEngineItf_;

typedef struct {
  SLresult (*SetPlayState)(void *self, SLuint32 state);
  SLresult (*GetPlayState)(void *self, SLuint32 *pState);
  SLresult (*GetDuration)(void *self, SLuint32 *pMsec);
  SLresult (*GetPosition)(void *self, SLuint32 *pMsec);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  SLresult (*SetCallbackEventsMask)(void *self, SLuint32 mask);
  SLresult (*GetCallbackEventsMask)(void *self, SLuint32 *pMask);
  SLresult (*SetMarkerPosition)(void *self, SLuint32 m);
  SLresult (*ClearMarkerPosition)(void *self);
  SLresult (*GetMarkerPosition)(void *self, SLuint32 *p);
  SLresult (*SetPositionUpdatePeriod)(void *self, SLuint32 m);
  SLresult (*GetPositionUpdatePeriod)(void *self, SLuint32 *p);
} SLPlayItf_;

typedef struct {
  SLresult (*Enqueue)(void *self, const void *pBuffer, SLuint32 size);
  SLresult (*Clear)(void *self);
  SLresult (*GetState)(void *self, void *pState);
  SLresult (*RegisterCallback)(void *self, slBufferQueueCallback cb, void *ctx);
} SLBufferQueueItf_;

typedef struct {
  SLresult (*SetVolumeLevel)(void *self, SLmillibel level);
  SLresult (*GetVolumeLevel)(void *self, SLmillibel *p);
  SLresult (*GetMaxVolumeLevel)(void *self, SLmillibel *p);
  SLresult (*SetMute)(void *self, SLboolean mute);
  SLresult (*GetMute)(void *self, SLboolean *p);
  SLresult (*EnableStereoPosition)(void *self, SLboolean enable);
  SLresult (*IsEnabledStereoPosition)(void *self, SLboolean *p);
  SLresult (*SetStereoPosition)(void *self, SLint32 perMille);
  SLresult (*GetStereoPosition)(void *self, SLint32 *p);
} SLVolumeItf_;

// --- objects ----------------------------------------------------------------

#define MAX_PLAYERS 32
// Keep enough queued buffers to cover several output callbacks without rejecting
// normal streaming writes.
#define BQ_SLOTS 64

typedef struct {
  const void *data;
  SLuint32 size;
} BQBuffer;

typedef struct Player {
  const SLObjectItf_ *obj_vt;
  const SLPlayItf_   *play_vt;
  const SLBufferQueueItf_ *bq_vt;
  const SLVolumeItf_ *vol_vt;

  int in_use;
  int channels;
  int rate;
  int playing;
  float gain; // linear, from SetVolumeLevel (millibels)

  slBufferQueueCallback cb;
  void *cb_ctx;

  // FIFO of enqueued buffers
  BQBuffer q[BQ_SLOTS];
  int q_head, q_tail; // count = (tail - head + N) % N
  // currently draining buffer
  const uint8_t *cur;
  SLuint32 cur_size, cur_pos;

  SDL_mutex *lock;

} Player;

typedef struct {
  const SLObjectItf_ *obj_vt;
} OutputMix;

typedef struct {
  const SLObjectItf_ *obj_vt;
  const SLEngineItf_ *eng_vt;
} Engine;

#define CONTAINER(ptr, type, member) \
  ((type *)((char *)(ptr) - offsetof(type, member)))

// --- global Switch audio output + player registry ---------------------------

#define OUT_BUFS 6
#define OUT_FRAMES 1024
#define OUT_BYTES (OUT_FRAMES * 2 * sizeof(int16_t))

static int g_dev = 0;
static int g_dev_rate = 44100;
static AudioOutBuffer g_out[OUT_BUFS];
static void *g_out_mem[OUT_BUFS];
static Thread g_audio_thread;
static volatile int g_audio_running = 0;
static int g_audio_thread_started = 0;
static Player *g_players[MAX_PLAYERS];
static int g_player_count = 0;
static SDL_mutex *g_reg_lock = NULL;

// Cocos2d-x SimpleAudioEngine normally crosses JNI into Android's
// Cocos2dxActivity. Keep decoded samples in the existing SDL mixer instead.
#define MAX_COCOS_SOUNDS 128
#define MAX_COCOS_EFFECTS 24
typedef struct {
  char path[256];
  int16_t *pcm;                 // interleaved S16 stereo at g_dev_rate
  uint32_t frames;
} CocosSound;
typedef struct {
  CocosSound *sound;
  float frame;
  unsigned int id;
  int loop;
  int playing;
  int paused;
  float left;
  float right;
  float volume;
  float rate;
} CocosVoice;
static CocosSound g_cocos_sounds[MAX_COCOS_SOUNDS];
static int g_cocos_sound_count = 0;
static CocosVoice g_cocos_bgm;
static CocosVoice g_cocos_effects[MAX_COCOS_EFFECTS];
static unsigned int g_cocos_next_id = 1;
static int g_cocos_effects_paused = 0;
static float g_cocos_bgm_volume = 1.0f;
static float g_cocos_effects_volume = 1.0f;

#define MOVIE_RING_FRAMES 65536
static SDL_mutex *g_movie_lock = NULL;
static int16_t *g_movie_pcm = NULL;
static int g_movie_active = 0;
static int g_movie_paused = 0;
static int g_movie_head = 0;
static int g_movie_count = 0;
static uint64_t g_movie_samples_queued = 0;
static uint64_t g_movie_samples_played = 0;

static float mb_to_linear(SLmillibel mb) {
  if (mb <= -9600) return 0.0f;
  return powf(10.0f, (float)mb / 2000.0f); // 100 mB = 1 dB
}

// mix one playing player into the S16 stereo accumulator (int32 to avoid clip).
// cur/cur_pos are touched only by this (audio) thread, so they need no lock;
// only the buffer queue is shared with Enqueue. Critically, the engine's
// completion callback is fired WITHOUT our lock held -- Android's contract --
// otherwise the engine's mixer thread (holding its own mutex, calling Enqueue
// which wants our lock) deadlocks against us.
static void mix_player(Player *p, int32_t *acc, int frames) {
  if (!p->playing)
    return;

  const float g = p->gain;
  for (int i = 0; i < frames; i++) {
    if (!p->cur || p->cur_pos >= p->cur_size) {
      // current buffer finished: notify the engine so it enqueues the next
      if (p->cur) {
        p->cur = NULL;
        if (p->cb) p->cb(&p->bq_vt, p->cb_ctx);
      }
      // pop the next buffer (queue access is the only thing that needs the lock)
      SDL_LockMutex(p->lock);
      const int have = (p->q_head != p->q_tail);
      BQBuffer b = { NULL, 0 };
      if (have) {
        b = p->q[p->q_head];
        p->q_head = (p->q_head + 1) % BQ_SLOTS;
      }
      SDL_UnlockMutex(p->lock);
      if (!have) {
        break; // underrun: rest is silence
      }
      p->cur = b.data;
      p->cur_size = b.size;
      p->cur_pos = 0;
    }

    const int16_t *s = (const int16_t *)(p->cur + p->cur_pos);
    int32_t l, r;
    if (p->channels >= 2) {
      l = s[0]; r = s[1];
      p->cur_pos += 4;
    } else {
      l = r = s[0];
      p->cur_pos += 2;
    }
    acc[i * 2 + 0] += (int32_t)(l * g);
    acc[i * 2 + 1] += (int32_t)(r * g);
  }
}

static void mix_movie(int32_t *acc, int frames) {
  if (!g_movie_lock)
    return;

  SDL_LockMutex(g_movie_lock);
  if (!g_movie_active || g_movie_paused || !g_movie_pcm) {
    SDL_UnlockMutex(g_movie_lock);
    return;
  }

  const int n = g_movie_count < frames ? g_movie_count : frames;
  for (int i = 0; i < n; i++) {
    const int idx = (g_movie_head + i) % MOVIE_RING_FRAMES;
    acc[i * 2 + 0] += g_movie_pcm[idx * 2 + 0];
    acc[i * 2 + 1] += g_movie_pcm[idx * 2 + 1];
  }
  g_movie_head = (g_movie_head + n) % MOVIE_RING_FRAMES;
  g_movie_count -= n;
  g_movie_samples_played += (uint64_t)n;
  SDL_UnlockMutex(g_movie_lock);
}

static void mix_cocos_voice(CocosVoice *voice, int32_t *acc, int frames, float volume) {
  if (!voice->playing || voice->paused || !voice->sound || !voice->sound->pcm || !voice->sound->frames)
    return;
  for (int i = 0; i < frames; i++) {
    if (voice->frame >= (float)voice->sound->frames) {
      if (!voice->loop) { voice->playing = 0; break; }
      voice->frame = fmodf(voice->frame, (float)voice->sound->frames);
    }
    const int16_t *s = voice->sound->pcm + (size_t)voice->frame * 2;
    acc[i * 2] += (int32_t)(s[0] * voice->left * voice->volume * volume);
    acc[i * 2 + 1] += (int32_t)(s[1] * voice->right * voice->volume * volume);
    voice->frame += voice->rate;
  }
}

static void mix_cocos_audio(int32_t *acc, int frames) {
  mix_cocos_voice(&g_cocos_bgm, acc, frames, g_cocos_bgm_volume);
  if (g_cocos_effects_paused)
    return;
  for (int i = 0; i < MAX_COCOS_EFFECTS; i++)
    mix_cocos_voice(&g_cocos_effects[i], acc, frames, g_cocos_effects_volume);
}

static void SDLCALL audio_callback(void *ud, Uint8 *stream, int len) {
  (void)ud;

  const int frames = len / 4; // S16 stereo
  static int32_t acc[8192 * 2];
  if (frames > 8192) { memset(stream, 0, len); return; }
  memset(acc, 0, frames * 2 * sizeof(int32_t));

  SDL_LockMutex(g_reg_lock);
  for (int i = 0; i < g_player_count; i++)
    if (g_players[i] && g_players[i]->in_use)
      mix_player(g_players[i], acc, frames);
  mix_cocos_audio(acc, frames);
  SDL_UnlockMutex(g_reg_lock);

  mix_movie(acc, frames);

  int16_t *out = (int16_t *)stream;
  for (int i = 0; i < frames * 2; i++) {
    int32_t v = acc[i];
    if (v > 32767) v = 32767;
    else if (v < -32768) v = -32768;
    out[i] = (int16_t)v;
  }

}

static int submit_audio_buffer(AudioOutBuffer *buffer) {
  audio_callback(NULL, buffer->buffer, OUT_BYTES);
  buffer->data_size = OUT_BYTES;
  buffer->data_offset = 0;
  if (R_FAILED(audoutAppendAudioOutBuffer(buffer))) {
    return 0;
  }
  return 1;
}

static void audio_thread(void *arg) {
  (void)arg;
  tls_setup_guard();
  while (g_audio_running) {
    AudioOutBuffer *released = NULL;
    u32 count = 0;
    Result rc = audoutWaitPlayFinish(&released, &count, UINT64_MAX);
    if (!g_audio_running)
      break;
    if (R_FAILED(rc))
      break;
    if (released && count && !submit_audio_buffer(released))
      break;
  }
}

void opensles_pump(void) {
  if (g_audio_thread_started)
    return;
  if (!g_dev)
    return;

  for (;;) {
    AudioOutBuffer *released = NULL;
    u32 count = 0;
    Result rc = audoutGetReleasedAudioOutBuffer(&released, &count);
    if (R_FAILED(rc))
      return;
    if (!released || !count)
      return;

    if (!submit_audio_buffer(released))
      return;
  }
}

static void ensure_device(int rate) {
  (void)rate;
  if (!g_reg_lock)
    g_reg_lock = SDL_CreateMutex();
  if (g_dev)
    return;
  if (R_FAILED(audoutInitialize()))
    return;
  if (R_FAILED(audoutStartAudioOut())) {
    audoutExit();
    return;
  }
  g_dev_rate = (int)audoutGetSampleRate();
  for (int i = 0; i < OUT_BUFS; i++) {
    g_out_mem[i] = memalign(0x1000, OUT_BYTES);
    if (!g_out_mem[i]) {
      audoutStopAudioOut();
      audoutExit();
      for (int j = 0; j < i; j++) { free(g_out_mem[j]); g_out_mem[j] = NULL; }
      return;
    }
    memset(&g_out[i], 0, sizeof(g_out[i]));
    g_out[i].buffer = g_out_mem[i];
    g_out[i].buffer_size = OUT_BYTES;
    g_out[i].data_size = OUT_BYTES;
    memset(g_out_mem[i], 0, OUT_BYTES);
    audoutAppendAudioOutBuffer(&g_out[i]);
  }
  g_dev = 1;
  g_audio_running = 1;
  Result thread_rc = threadCreate(&g_audio_thread, audio_thread, NULL, NULL,
                                  64 * 1024, 0x2b, 1);
  const int thread_created = R_SUCCEEDED(thread_rc);
  if (thread_created)
    thread_rc = threadStart(&g_audio_thread);
  if (R_FAILED(thread_rc)) {
    if (thread_created)
      threadClose(&g_audio_thread);
    g_audio_running = 0;
  } else {
    g_audio_thread_started = 1;
  }
}

static CocosSound *cocos_find_sound(const char *path) {
  for (int i = 0; i < g_cocos_sound_count; i++)
    if (!strcmp(g_cocos_sounds[i].path, path))
      return &g_cocos_sounds[i];
  return NULL;
}

static CocosSound *cocos_load_sound(const char *path) {
  if (!path || !path[0])
    return NULL;
  ensure_device(44100);
  if (!g_reg_lock)
    return NULL;

  SDL_LockMutex(g_reg_lock);
  CocosSound *cached = cocos_find_sound(path);
  SDL_UnlockMutex(g_reg_lock);
  if (cached)
    return cached;

  AAsset *asset = AAssetManager_open_fake(asset_manager(), path, 0);
  int64_t size = AAsset_getLength64_fake(asset);
  if (!asset || size <= 0 || size > 64 * 1024 * 1024) {
    if (asset) AAsset_close_fake(asset);
    return NULL;
  }
  uint8_t *encoded = malloc((size_t)size);
  if (!encoded || AAsset_read_fake(asset, encoded, (size_t)size) != size) {
    AAsset_close_fake(asset);
    free(encoded);
    return NULL;
  }
  AAsset_close_fake(asset);

  int channels = 0, rate = 0;
  short *decoded = NULL;
  int raw_frames = stb_vorbis_decode_memory(encoded, (int)size, &channels, &rate, &decoded);
  free(encoded);
  if (raw_frames <= 0 || !decoded || channels <= 0 || rate <= 0) {
    free(decoded);
    return NULL;
  }

  uint64_t output_frames = ((uint64_t)raw_frames * (uint64_t)g_dev_rate) / (uint64_t)rate;
  if (!output_frames || output_frames > 16 * 1024 * 1024) {
    free(decoded);
    return NULL;
  }
  int16_t *pcm = malloc((size_t)output_frames * 2 * sizeof(*pcm));
  if (!pcm) {
    free(decoded);
    return NULL;
  }
  for (uint64_t i = 0; i < output_frames; i++) {
    uint64_t source = (i * (uint64_t)rate) / (uint64_t)g_dev_rate;
    if (source >= (uint64_t)raw_frames) source = (uint64_t)raw_frames - 1;
    pcm[i * 2] = decoded[source * (uint64_t)channels];
    pcm[i * 2 + 1] = decoded[source * (uint64_t)channels + (channels > 1 ? 1 : 0)];
  }
  free(decoded);

  SDL_LockMutex(g_reg_lock);
  cached = cocos_find_sound(path);
  if (cached) {
    SDL_UnlockMutex(g_reg_lock);
    free(pcm);
    return cached;
  }
  if (g_cocos_sound_count == MAX_COCOS_SOUNDS) {
    SDL_UnlockMutex(g_reg_lock);
    free(pcm);
    return NULL;
  }
  CocosSound *sound = &g_cocos_sounds[g_cocos_sound_count++];
  snprintf(sound->path, sizeof(sound->path), "%s", path);
  sound->pcm = pcm;
  sound->frames = (uint32_t)output_frames;
  SDL_UnlockMutex(g_reg_lock);
  return sound;
}

void cocos_audio_preload(const char *path) { (void)cocos_load_sound(path); }

int cocos_audio_play_effect(const char *path, int loop, float left, float right) {
  CocosSound *sound = cocos_load_sound(path);
  if (!sound || !g_reg_lock)
    return 0;
  SDL_LockMutex(g_reg_lock);
  int slot = 0;
  for (int i = 0; i < MAX_COCOS_EFFECTS; i++)
    if (!g_cocos_effects[i].playing) { slot = i; break; }
  CocosVoice *voice = &g_cocos_effects[slot];
  voice->sound = sound;
  voice->frame = 0;
  voice->id = g_cocos_next_id++;
  if (!voice->id) voice->id = g_cocos_next_id++;
  voice->loop = loop != 0;
  voice->playing = 1;
  voice->paused = 0;
  voice->left = left;
  voice->right = right;
  // Persistent vehicle sounds (engine, turbo/booster, supercharger, etc.) are
  // started while a level is still loading. The game sends their real volume
  // through setEffectVolume once simulation is ready, so every loop must begin
  // silent. One-shot UI/gameplay effects still begin at normal gain.
  voice->volume = loop ? 0.0f : 1.0f;
  voice->rate = 1.0f;
  SDL_UnlockMutex(g_reg_lock);
  return (int)voice->id;
}

void cocos_audio_stop_effect(int id) {
  if (!g_reg_lock) return;
  SDL_LockMutex(g_reg_lock);
  for (int i = 0; i < MAX_COCOS_EFFECTS; i++)
    if (g_cocos_effects[i].id == (unsigned int)id)
      g_cocos_effects[i].playing = g_cocos_effects[i].paused = 0;
  SDL_UnlockMutex(g_reg_lock);
}

void cocos_audio_pause_effect(int id, int paused) {
  if (!g_reg_lock) return;
  SDL_LockMutex(g_reg_lock);
  for (int i = 0; i < MAX_COCOS_EFFECTS; i++)
    if (g_cocos_effects[i].id == (unsigned int)id && g_cocos_effects[i].playing)
      g_cocos_effects[i].paused = paused != 0;
  SDL_UnlockMutex(g_reg_lock);
}

void cocos_audio_stop_all_effects(void) {
  if (!g_reg_lock) return;
  SDL_LockMutex(g_reg_lock);
  for (int i = 0; i < MAX_COCOS_EFFECTS; i++)
    g_cocos_effects[i].playing = g_cocos_effects[i].paused = 0;
  SDL_UnlockMutex(g_reg_lock);
}

void cocos_audio_pause_all_effects(int paused) {
  if (!g_reg_lock) return;
  SDL_LockMutex(g_reg_lock);
  g_cocos_effects_paused = paused != 0;
  SDL_UnlockMutex(g_reg_lock);
}

void cocos_audio_set_effects_volume(float volume) { g_cocos_effects_volume = volume; }

void cocos_audio_set_effect_volume(int id, float volume) {
  if (!g_reg_lock) return;
  SDL_LockMutex(g_reg_lock);
  for (int i = 0; i < MAX_COCOS_EFFECTS; i++)
    if (g_cocos_effects[i].id == (unsigned int)id)
      g_cocos_effects[i].volume = volume;
  SDL_UnlockMutex(g_reg_lock);
}

void cocos_audio_set_effect_rate(int id, float rate) {
  if (!g_reg_lock) return;
  SDL_LockMutex(g_reg_lock);
  for (int i = 0; i < MAX_COCOS_EFFECTS; i++)
    if (g_cocos_effects[i].id == (unsigned int)id)
      g_cocos_effects[i].rate = rate < 0.5f ? 0.5f : (rate > 2.0f ? 2.0f : rate);
  SDL_UnlockMutex(g_reg_lock);
}

void cocos_audio_play_background(const char *path, int loop) {
  CocosSound *sound = cocos_load_sound(path);
  if (!sound || !g_reg_lock) return;
  SDL_LockMutex(g_reg_lock);
  g_cocos_bgm.sound = sound;
  g_cocos_bgm.frame = 0;
  g_cocos_bgm.loop = loop != 0;
  g_cocos_bgm.playing = 1;
  g_cocos_bgm.paused = 0;
  g_cocos_bgm.left = g_cocos_bgm.right = 1.0f;
  g_cocos_bgm.volume = g_cocos_bgm.rate = 1.0f;
  SDL_UnlockMutex(g_reg_lock);
}

void cocos_audio_stop_background(void) {
  if (!g_reg_lock) return;
  SDL_LockMutex(g_reg_lock);
  g_cocos_bgm.playing = g_cocos_bgm.paused = 0;
  g_cocos_bgm.frame = 0;
  SDL_UnlockMutex(g_reg_lock);
}

void cocos_audio_pause_background(int paused) {
  if (!g_reg_lock) return;
  SDL_LockMutex(g_reg_lock);
  if (g_cocos_bgm.playing) g_cocos_bgm.paused = paused != 0;
  SDL_UnlockMutex(g_reg_lock);
}

void cocos_audio_rewind_background(void) {
  if (!g_reg_lock) return;
  SDL_LockMutex(g_reg_lock);
  g_cocos_bgm.frame = 0;
  SDL_UnlockMutex(g_reg_lock);
}

int cocos_audio_background_playing(void) {
  return g_cocos_bgm.playing && !g_cocos_bgm.paused;
}

void cocos_audio_set_background_volume(float volume) { g_cocos_bgm_volume = volume; }
float cocos_audio_background_volume(void) { return g_cocos_bgm_volume; }
float cocos_audio_effects_volume(void) { return g_cocos_effects_volume; }

int opensles_movie_begin(int requested_rate) {
  if (!g_movie_lock)
    g_movie_lock = SDL_CreateMutex();
  if (!g_movie_pcm)
    g_movie_pcm = calloc(MOVIE_RING_FRAMES * 2, sizeof(int16_t));
  if (!g_movie_lock || !g_movie_pcm)
    return 0;

  ensure_device(requested_rate > 0 ? requested_rate : 44100);
  if (!g_dev)
    return 0;

  SDL_LockMutex(g_movie_lock);
  g_movie_active = 1;
  g_movie_paused = 1;
  g_movie_head = 0;
  g_movie_count = 0;
  g_movie_samples_queued = 0;
  g_movie_samples_played = 0;
  SDL_UnlockMutex(g_movie_lock);
  return g_dev_rate;
}

int opensles_movie_queue(const int16_t *pcm, int frames) {
  int done = 0;
  while (done < frames) {
    if (!g_movie_lock)
      return done;

    SDL_LockMutex(g_movie_lock);
    if (!g_movie_active || !g_movie_pcm) {
      SDL_UnlockMutex(g_movie_lock);
      return done;
    }

    const int space = MOVIE_RING_FRAMES - g_movie_count;
    int n = frames - done;
    if (n > space)
      n = space;
    for (int i = 0; i < n; i++) {
      const int idx = (g_movie_head + g_movie_count + i) % MOVIE_RING_FRAMES;
      g_movie_pcm[idx * 2 + 0] = pcm[(done + i) * 2 + 0];
      g_movie_pcm[idx * 2 + 1] = pcm[(done + i) * 2 + 1];
    }
    g_movie_count += n;
    g_movie_samples_queued += (uint64_t)n;
    SDL_UnlockMutex(g_movie_lock);

    done += n;
    if (done < frames)
      SDL_Delay(2);
  }
  return done;
}

void opensles_movie_set_paused(int paused) {
  if (!g_movie_lock)
    return;
  SDL_LockMutex(g_movie_lock);
  if (g_movie_active)
    g_movie_paused = paused != 0;
  SDL_UnlockMutex(g_movie_lock);
}

uint64_t opensles_movie_samples_queued(void) {
  uint64_t ret = 0;
  if (!g_movie_lock)
    return 0;
  SDL_LockMutex(g_movie_lock);
  ret = g_movie_samples_queued;
  SDL_UnlockMutex(g_movie_lock);
  return ret;
}

uint64_t opensles_movie_samples_played(void) {
  uint64_t ret = 0;
  if (!g_movie_lock)
    return 0;
  SDL_LockMutex(g_movie_lock);
  ret = g_movie_samples_played;
  SDL_UnlockMutex(g_movie_lock);
  return ret;
}

int opensles_movie_buffered_frames(void) {
  int ret = 0;
  if (!g_movie_lock)
    return 0;
  SDL_LockMutex(g_movie_lock);
  ret = g_movie_count;
  SDL_UnlockMutex(g_movie_lock);
  return ret;
}

void opensles_movie_end(void) {
  if (!g_movie_lock)
    return;
  SDL_LockMutex(g_movie_lock);
  g_movie_active = 0;
  g_movie_paused = 0;
  g_movie_head = 0;
  g_movie_count = 0;
  SDL_UnlockMutex(g_movie_lock);
}

// --- buffer queue interface -------------------------------------------------

static SLresult bq_Enqueue(void *self, const void *pBuffer, SLuint32 size) {
  Player *p = CONTAINER(self, Player, bq_vt);
  SDL_LockMutex(p->lock);
  const int next = (p->q_tail + 1) % BQ_SLOTS;
  if (next == p->q_head) { // full
    SDL_UnlockMutex(p->lock);
    return SL_RESULT_PARAMETER_INVALID;
  }
  p->q[p->q_tail].data = pBuffer;
  p->q[p->q_tail].size = size;
  p->q_tail = next;
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult bq_Clear(void *self) {
  Player *p = CONTAINER(self, Player, bq_vt);
  SDL_LockMutex(p->lock);
  p->q_head = p->q_tail = 0;
  p->cur = NULL;
  p->cur_pos = p->cur_size = 0;
  SDL_UnlockMutex(p->lock);
  return SL_RESULT_SUCCESS;
}

typedef struct { SLuint32 count; SLuint32 index; } SLBufferQueueState;

static SLresult bq_GetState(void *self, void *pState) {
  Player *p = CONTAINER(self, Player, bq_vt);
  if (pState) {
    SLBufferQueueState *st = pState;
    SDL_LockMutex(p->lock);
    st->count = (p->q_tail - p->q_head + BQ_SLOTS) % BQ_SLOTS + (p->cur ? 1 : 0);
    st->index = 0;
    SDL_UnlockMutex(p->lock);
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_RegisterCallback(void *self, slBufferQueueCallback cb, void *ctx) {
  Player *p = CONTAINER(self, Player, bq_vt);
  p->cb = cb;
  p->cb_ctx = ctx;
  return SL_RESULT_SUCCESS;
}

static const SLBufferQueueItf_ bq_vtable = {
  bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback,
};

// --- play interface ---------------------------------------------------------

static SLresult play_SetPlayState(void *self, SLuint32 state) {
  Player *p = CONTAINER(self, Player, play_vt);
  // CRIWARE streams the master mix continuously and, in this build, only ever
  // calls SetPlayState(STOPPED) through our Play interface -- never PLAYING.
  // Honoring STOP kills the drain (queue fills -> Enqueue returns 13 -> silence).
  // Keep the player draining always: enqueued buffers play and the completion
  // callback fires so CRIWARE recycles buffers. (Draining an empty queue just
  // emits silence, so this is safe when nothing is actually playing.)
  SDL_LockMutex(p->lock);
  p->playing = 1;
  SDL_UnlockMutex(p->lock);
  (void)state;
  return SL_RESULT_SUCCESS;
}
static SLresult play_GetPlayState(void *self, SLuint32 *pState) {
  Player *p = CONTAINER(self, Player, play_vt);
  if (pState) *pState = p->playing ? SL_PLAYSTATE_PLAYING : SL_PLAYSTATE_STOPPED;
  return SL_RESULT_SUCCESS;
}
static SLresult play_ret0_u32(void *self, SLuint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult play_ok_u32(void *self, SLuint32 v) { (void)self; (void)v; return SL_RESULT_SUCCESS; }
static SLresult play_ok(void *self) { (void)self; return SL_RESULT_SUCCESS; }
static SLresult play_RegisterCallback(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }

static const SLPlayItf_ play_vtable = {
  play_SetPlayState, play_GetPlayState, play_ret0_u32, play_ret0_u32,
  play_RegisterCallback, play_ok_u32, play_ret0_u32, play_ok_u32,
  play_ok, play_ret0_u32, play_ok_u32, play_ret0_u32,
};

// --- volume interface -------------------------------------------------------

static SLresult vol_SetVolumeLevel(void *self, SLmillibel level) {
  Player *p = CONTAINER(self, Player, vol_vt);
  p->gain = mb_to_linear(level);
  return SL_RESULT_SUCCESS;
}
static SLresult vol_GetVolumeLevel(void *self, SLmillibel *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_GetMaxVolumeLevel(void *self, SLmillibel *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_SetMute(void *self, SLboolean m) {
  Player *p = CONTAINER(self, Player, vol_vt);
  if (m) p->gain = 0.0f;
  return SL_RESULT_SUCCESS;
}
static SLresult vol_GetMute(void *self, SLboolean *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_enable(void *self, SLboolean e) { (void)self; (void)e; return SL_RESULT_SUCCESS; }
static SLresult vol_isenabled(void *self, SLboolean *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_setpos(void *self, SLint32 v) { (void)self; (void)v; return SL_RESULT_SUCCESS; }
static SLresult vol_getpos(void *self, SLint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }

static const SLVolumeItf_ vol_vtable = {
  vol_SetVolumeLevel, vol_GetVolumeLevel, vol_GetMaxVolumeLevel, vol_SetMute,
  vol_GetMute, vol_enable, vol_isenabled, vol_setpos, vol_getpos,
};

// --- player object ----------------------------------------------------------

static SLresult player_GetInterface(void *self, const SLInterfaceID iid, void *pInterface);
static void player_Destroy(void *self);

static SLresult obj_Realize(void *self, SLboolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_Resume(void *self, SLboolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_GetState(void *self, SLuint32 *pState) { (void)self; if (pState) *pState = SL_OBJECT_STATE_REALIZED; return SL_RESULT_SUCCESS; }
static SLresult obj_RegisterCallback(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }
static SLresult obj_Abort(void *self) { (void)self; return SL_RESULT_SUCCESS; }
static SLresult obj_SetPriority(void *self, SLint32 a, SLboolean b) { (void)self; (void)a; (void)b; return SL_RESULT_SUCCESS; }
static SLresult obj_GetPriority(void *self, SLint32 *p) { (void)self; if (p) *p = 0; return SL_RESULT_SUCCESS; }
static SLresult obj_SetLOC(void *self, SLint32 a, SLInterfaceID *b, SLboolean c) { (void)self; (void)a; (void)b; (void)c; return SL_RESULT_SUCCESS; }

static SLresult mix_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  (void)self; (void)iid;
  if (pInterface) *(void **)pInterface = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
static void simple_Destroy(void *self) { free(self); }

static const SLObjectItf_ player_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, player_GetInterface, obj_RegisterCallback,
  obj_Abort, player_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};
static const SLObjectItf_ mix_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, mix_GetInterface, obj_RegisterCallback,
  obj_Abort, simple_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};

static SLresult player_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  Player *p = CONTAINER(self, Player, obj_vt);
  if (!pInterface)
    return SL_RESULT_PARAMETER_INVALID;
  if (iid == SL_IID_PLAY) {
    *(void **)pInterface = &p->play_vt;
  } else if (iid == SL_IID_BUFFERQUEUE || iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE) {
    *(void **)pInterface = &p->bq_vt;
  } else if (iid == SL_IID_VOLUME) {
    *(void **)pInterface = &p->vol_vt;
  } else {
    *(void **)pInterface = NULL;
    return SL_RESULT_FEATURE_UNSUPPORTED;
  }
  return SL_RESULT_SUCCESS;
}

static void player_Destroy(void *self) {
  Player *p = CONTAINER(self, Player, obj_vt);
  SDL_LockMutex(g_reg_lock);
  for (int i = 0; i < g_player_count; i++)
    if (g_players[i] == p) g_players[i] = NULL;
  SDL_UnlockMutex(g_reg_lock);
  if (p->lock) SDL_DestroyMutex(p->lock);
  free(p);
}

// --- engine interface -------------------------------------------------------

static SLresult eng_CreateAudioPlayer(void *self, SLObjectItf *pPlayer, SLDataSource *src, SLDataSink *snk,
                                      SLuint32 numIfaces, const SLInterfaceID *ids, const SLboolean *req) {
  (void)self; (void)snk; (void)numIfaces; (void)ids; (void)req;
  if (!pPlayer)
    return SL_RESULT_PARAMETER_INVALID;

  Player *p = calloc(1, sizeof(*p));
  if (!p)
    return SL_RESULT_PARAMETER_INVALID;
  p->obj_vt = &player_obj_vtable;
  p->play_vt = &play_vtable;
  p->bq_vt = &bq_vtable;
  p->vol_vt = &vol_vtable;
  p->in_use = 1;
  p->gain = 1.0f;
  p->channels = 2;
  p->rate = 44100;
  // Drain from creation: CRIWARE fills the buffer queue but (in this build)
  // never calls SetPlayState(PLAYING) through our Play interface, so without
  // this the queue fills and every Enqueue returns 13. Draining lets the
  // completion callback fire so CRIWARE recycles buffers normally.
  p->playing = 1;
  p->lock = SDL_CreateMutex();

  if (src && src->pFormat) {
    const SLDataFormat_PCM *fmt = src->pFormat;
    if (fmt->formatType == 2 /* SL_DATAFORMAT_PCM */) {
      p->channels = fmt->numChannels ? (int)fmt->numChannels : 2;
      p->rate = fmt->samplesPerSec ? (int)(fmt->samplesPerSec / 1000) : 44100;
    }
  }
  // CRIWARE requests a 44100 player but its master mix actually outputs ~32000 Hz
  // (measured via the enqueue rate) -> playing at 44100 is fast-forward. Override
  // the device rate to the real content rate (config.audio_rate, default 32000).
  if (config.audio_rate > 0)
    p->rate = config.audio_rate;

  ensure_device(p->rate);

  SDL_LockMutex(g_reg_lock);
  int slot = -1;
  for (int i = 0; i < g_player_count; i++)
    if (g_players[i] == NULL) { slot = i; break; }
  if (slot < 0 && g_player_count < MAX_PLAYERS)
    slot = g_player_count++;
  if (slot >= 0)
    g_players[slot] = p;
  SDL_UnlockMutex(g_reg_lock);

  *pPlayer = &p->obj_vt;
  return SL_RESULT_SUCCESS;
}

static SLresult eng_CreateOutputMix(void *self, SLObjectItf *pMix, SLuint32 numIfaces,
                                    const SLInterfaceID *ids, const SLboolean *req) {
  (void)self; (void)numIfaces; (void)ids; (void)req;
  OutputMix *m = calloc(1, sizeof(*m));
  if (!m)
    return SL_RESULT_PARAMETER_INVALID;
  m->obj_vt = &mix_obj_vtable;
  if (pMix) *pMix = &m->obj_vt;
  return SL_RESULT_SUCCESS;
}

static SLresult eng_unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

static const SLEngineItf_ engine_vtable = {
  .CreateLEDDevice = (void *)eng_unsupported,
  .CreateVibraDevice = (void *)eng_unsupported,
  .CreateAudioPlayer = eng_CreateAudioPlayer,
  .CreateAudioRecorder = (void *)eng_unsupported,
  .CreateMidiPlayer = (void *)eng_unsupported,
  .CreateListener = (void *)eng_unsupported,
  .Create3DGroup = (void *)eng_unsupported,
  .CreateOutputMix = eng_CreateOutputMix,
  .CreateMetadataExtractor = (void *)eng_unsupported,
  .CreateExtensionObject = (void *)eng_unsupported,
  .QueryNumSupportedInterfaces = (void *)eng_unsupported,
  .QuerySupportedInterfaces = (void *)eng_unsupported,
  .QueryNumSupportedExtensions = (void *)eng_unsupported,
  .QuerySupportedExtension = (void *)eng_unsupported,
  .IsExtensionSupported = (void *)eng_unsupported,
};

static SLresult engine_GetInterface(void *self, const SLInterfaceID iid, void *pInterface) {
  Engine *e = CONTAINER(self, Engine, obj_vt);
  if (!pInterface)
    return SL_RESULT_PARAMETER_INVALID;
  if (iid == SL_IID_ENGINE) {
    *(void **)pInterface = &e->eng_vt;
    return SL_RESULT_SUCCESS;
  }
  *(void **)pInterface = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static const SLObjectItf_ engine_obj_vtable = {
  obj_Realize, obj_Resume, obj_GetState, engine_GetInterface, obj_RegisterCallback,
  obj_Abort, simple_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLOC,
};

// --- entry point ------------------------------------------------------------

uint32_t slCreateEngine(void **pEngine, uint32_t numOptions, const void *pEngineOptions,
                        uint32_t numInterfaces, const void *pInterfaceIds,
                        const void *pInterfaceRequired) {
  (void)numOptions; (void)pEngineOptions; (void)numInterfaces;
  (void)pInterfaceIds; (void)pInterfaceRequired;
  if (!g_reg_lock)
    g_reg_lock = SDL_CreateMutex();
  if (!pEngine)
    return SL_RESULT_PARAMETER_INVALID;
  Engine *e = calloc(1, sizeof(*e));
  if (!e)
    return SL_RESULT_PARAMETER_INVALID;
  e->obj_vt = &engine_obj_vtable;
  e->eng_vt = &engine_vtable;
  *pEngine = &e->obj_vt;
  return SL_RESULT_SUCCESS;
}

void opensles_shutdown(void) {
  opensles_movie_end();
  if (g_dev) {
    g_audio_running = 0;
    audoutStopAudioOut();
    if (g_audio_thread_started) {
      threadWaitForExit(&g_audio_thread);
      threadClose(&g_audio_thread);
      g_audio_thread_started = 0;
    }
    audoutExit();
    for (int i = 0; i < OUT_BUFS; i++) {
      free(g_out_mem[i]);
      g_out_mem[i] = NULL;
    }
    g_dev = 0;
  }
  for (int i = 0; i < g_cocos_sound_count; i++) {
    free(g_cocos_sounds[i].pcm);
    g_cocos_sounds[i].pcm = NULL;
  }
  g_cocos_sound_count = 0;
  memset(&g_cocos_bgm, 0, sizeof(g_cocos_bgm));
  memset(g_cocos_effects, 0, sizeof(g_cocos_effects));
  free(g_movie_pcm);
  g_movie_pcm = NULL;
  if (g_movie_lock) {
    SDL_DestroyMutex(g_movie_lock);
    g_movie_lock = NULL;
  }
}
