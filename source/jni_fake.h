/* jni_fake.h -- fake JNI environment for the Cocos2d-x 3.x engine (libMyGame.so)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

extern void *fake_vm;  // JavaVM *
extern void *fake_env; // JNIEnv *

// set when the engine asks the activity to finish (terminateProcessJNI/finish)
extern volatile int jni_quit_requested;

void jni_init(void);
void jni_flush_prefs(void);

// the fake Context/AppActivity instance handed to the native methods
void *jni_make_thiz(void);
void *jni_make_string(const char *utf);
void jni_delete_local_ref(void *ref);

// MainActivity's Java billing layer creates popup IAP entries before querying
// Google Play. The Switch port has no Dalvik VM, so let main.c mirror that
// Java->native setup synchronously when the game invokes setPopupOfferId().
typedef void (*jni_popup_offer_callback)(const char *id);
void jni_set_popup_offer_callback(jni_popup_offer_callback callback);

typedef enum {
  HCR_SHOP_REWARD_NONE = 0,
  HCR_SHOP_REWARD_COINS,
  HCR_SHOP_REWARD_GEMS,
  HCR_SHOP_REWARD_PAINTS,
  HCR_SHOP_REWARD_BUNDLE,
} HcrShopRewardKind;

typedef struct {
  HcrShopRewardKind kind;
  int amount;
} HcrShopReward;

// main.c exposes the catalog lookup so the JNI shim can mirror the exact
// reward type and amount when freeshop purchases are auto-completed.
int hcr_lookup_shop_reward(const char *id, HcrShopReward *reward);

// persistent primitive arrays for the input pump (nativeTouchesMove batches)
void *jni_make_input_array(int max_len, int elem_size);
void jni_input_array_set(void *arr, const void *src, int n);

// Raised by the Java bridge when the game asks Android to show its consent UI.
int jni_take_consent_form_request(void);

#endif
