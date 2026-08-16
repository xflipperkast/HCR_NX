/* Asynchronous HTTPS bridge for HCR's Java HttpURLConnection calls. */
#ifndef HCR_HTTP_H
#define HCR_HTTP_H

typedef void (*HcrHttpJsonCallback)(void *env, void *cls, void *json);
int hcr_http_init(void);
int hcr_http_network_available(void);
void hcr_http_set_callbacks(void *env, void *cls, HcrHttpJsonCallback popup,
                            HcrHttpJsonCallback mission);
void hcr_http_enqueue_popup(const char *url);
void hcr_http_enqueue_mission(const char *url);
void hcr_http_pump(void);
void hcr_http_exit(void);
#endif
