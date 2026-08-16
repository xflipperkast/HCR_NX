/* Replacement for HCR's Java HttpURLConnection AsyncTasks. */
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <switch.h>
#include <curl/curl.h>
#include "hcr_http.h"
#include "jni_fake.h"

typedef struct { unsigned char *data; size_t size, capacity; } HttpBuffer;
typedef enum { HCR_HTTP_POPUP, HCR_HTTP_MISSION } HcrHttpKind;
typedef struct { CURL *curl; HttpBuffer response; char *url; HcrHttpKind kind; } HcrHttpRequest;

#define HCR_HTTP_MAX_REQUESTS 4
static CURLM *multi;
static HcrHttpRequest *requests[HCR_HTTP_MAX_REQUESTS];
static int request_count, socket_ready, nifm_ready, curl_ready;
static int network_status = -1;
static void *callback_env, *callback_cls;
static HcrHttpJsonCallback popup_callback, mission_callback;

static size_t http_write(void *data, size_t size, size_t count, void *opaque) {
  HttpBuffer *buffer = opaque;
  const size_t bytes = size * count;
  if (!bytes || bytes > SIZE_MAX - buffer->size) return 0;
  const size_t needed = buffer->size + bytes;
  if (needed > buffer->capacity) {
    size_t cap = buffer->capacity ? buffer->capacity : 4096;
    while (cap < needed) { if (cap > SIZE_MAX / 2) return 0; cap *= 2; }
    unsigned char *grown = realloc(buffer->data, cap);
    if (!grown) return 0;
    buffer->data = grown; buffer->capacity = cap;
  }
  memcpy(buffer->data + buffer->size, data, bytes);
  buffer->size = needed;
  return bytes;
}

int hcr_http_init(void) {
  if (!socket_ready) {
    if (R_FAILED(socketInitializeDefault())) return 0;
    socket_ready = 1;
  }
  if (!nifm_ready) {
    if (R_FAILED(nifmInitialize(NifmServiceType_User))) return 0;
    nifm_ready = 1;
  }
  return 1;
}

int hcr_http_network_available(void) {
  // ShopView polls this JNI value while opening. NIFM status calls are
  // synchronous, so repeating them on every poll can stall the render thread.
  if (network_status >= 0) return network_status;
  if (!hcr_http_init()) return 0;
  NifmInternetConnectionType type = 0;
  NifmInternetConnectionStatus status = 0;
  u32 strength = 0;
  network_status = R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status)) &&
                   status == NifmInternetConnectionStatus_Connected;
  return network_status;
}

void hcr_http_set_callbacks(void *env, void *cls, HcrHttpJsonCallback popup, HcrHttpJsonCallback mission) {
  callback_env = env; callback_cls = cls; popup_callback = popup; mission_callback = mission;
}

static void request_free(HcrHttpRequest *request) {
  if (!request) return;
  if (request->curl) curl_easy_cleanup(request->curl);
  free(request->response.data); free(request->url); free(request);
}

static void enqueue(HcrHttpKind kind, const char *url) {
  if (!url || !*url || request_count >= HCR_HTTP_MAX_REQUESTS) {
    return;
  }
  HcrHttpJsonCallback callback = kind == HCR_HTTP_POPUP ? popup_callback : mission_callback;
  if (!hcr_http_network_available()) {
    if (callback) callback(callback_env, callback_cls, jni_make_string(""));
    return;
  }
  if (!curl_ready && curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    return;
  }
  curl_ready = 1;
  if (!multi && !(multi = curl_multi_init())) return;
  HcrHttpRequest *request = calloc(1, sizeof(*request));
  if (!request) return;
  request->url = strdup(url); request->kind = kind; request->curl = curl_easy_init();
  if (!request->url || !request->curl) { request_free(request); return; }
  curl_easy_setopt(request->curl, CURLOPT_URL, request->url);
  curl_easy_setopt(request->curl, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(request->curl, CURLOPT_WRITEFUNCTION, http_write);
  curl_easy_setopt(request->curl, CURLOPT_WRITEDATA, &request->response);
  curl_easy_setopt(request->curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(request->curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(request->curl, CURLOPT_USERAGENT, "Hill Climb Racing/1.71.1 (Nintendo Switch)");
  curl_easy_setopt(request->curl, CURLOPT_CONNECTTIMEOUT_MS, 250L);
  curl_easy_setopt(request->curl, CURLOPT_TIMEOUT_MS, 1000L);
  /* Homebrew has no system CA store; encryption remains enabled, but no CA validation is possible. */
  curl_easy_setopt(request->curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(request->curl, CURLOPT_SSL_VERIFYHOST, 0L);
  curl_easy_setopt(request->curl, CURLOPT_PRIVATE, request);
  if (curl_multi_add_handle(multi, request->curl) != CURLM_OK) { request_free(request); return; }
  requests[request_count++] = request;
}

void hcr_http_enqueue_popup(const char *url) { enqueue(HCR_HTTP_POPUP, url); }
void hcr_http_enqueue_mission(const char *url) { enqueue(HCR_HTTP_MISSION, url); }

void hcr_http_pump(void) {
  if (!multi) return;
  int running; CURLMcode result;
  do { result = curl_multi_perform(multi, &running); } while (result == CURLM_CALL_MULTI_PERFORM);
  if (result != CURLM_OK) return;
  int pending = 0; CURLMsg *message;
  while ((message = curl_multi_info_read(multi, &pending))) {
    if (message->msg != CURLMSG_DONE) continue;
    char *private_data = NULL; long status = 0;
    curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &private_data);
    HcrHttpRequest *request = (HcrHttpRequest *)private_data;
    curl_easy_getinfo(message->easy_handle, CURLINFO_RESPONSE_CODE, &status);
    curl_multi_remove_handle(multi, message->easy_handle);
    if (!request) continue;
    const int ok = message->data.result == CURLE_OK && status >= 200 && status < 400 && request->response.size <= INT_MAX;
    HcrHttpJsonCallback callback = request->kind == HCR_HTTP_POPUP ? popup_callback : mission_callback;
    if (callback && ok) {
      char *json = malloc(request->response.size + 1);
      if (json) { memcpy(json, request->response.data, request->response.size); json[request->response.size] = 0; callback(callback_env, callback_cls, jni_make_string(json)); free(json); }
      else callback(callback_env, callback_cls, jni_make_string(""));
    } else if (callback) callback(callback_env, callback_cls, jni_make_string(""));
    for (int i = 0; i < request_count; i++) if (requests[i] == request) {
      memmove(&requests[i], &requests[i + 1], (size_t)(request_count - i - 1) * sizeof(requests[0])); request_count--; break;
    }
    request_free(request);
  }
}

void hcr_http_exit(void) {
  while (request_count) { HcrHttpRequest *request = requests[--request_count]; if (multi && request && request->curl) curl_multi_remove_handle(multi, request->curl); request_free(request); }
  if (multi) { curl_multi_cleanup(multi); multi = NULL; }
  if (curl_ready) { curl_global_cleanup(); curl_ready = 0; }
  if (nifm_ready) { nifmExit(); nifm_ready = 0; }
  if (socket_ready) { socketExit(); socket_ready = 0; }
}
