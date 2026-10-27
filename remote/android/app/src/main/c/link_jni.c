/* Copyright 2026 The Umbra Authors.
 * JNI glue for NativeLink.java: the app's network link is umbranet
 * (remote/net), the same C code the PC agent uses. */
#include <jni.h>
#include <stdint.h>

#include "umbranet.h"

#define EP(h) ((un_ep *)(intptr_t)(h))

JNIEXPORT jlong JNICALL Java_org_umbra_remote_NativeLink_connect(JNIEnv *env, jclass cls, jstring host, jint port) {
  (void)cls;
  const char *h = (*env)->GetStringUTFChars(env, host, NULL);
  if (!h) return 0;
  un_ep *ep = un_connect(h, (uint16_t)port, NULL, 0);
  (*env)->ReleaseStringUTFChars(env, host, h);
  return (jlong)(intptr_t)ep;
}

/* Returns the event type (0 = none); info = {channel, length, value}, the
 * data is at the start of buf. */
JNIEXPORT jint JNICALL Java_org_umbra_remote_NativeLink_poll(JNIEnv *env, jclass cls, jlong ep, jobject buf,
                                                             jintArray info, jint timeout_ms) {
  (void)cls;
  uint8_t *b = (uint8_t *)(*env)->GetDirectBufferAddress(env, buf);
  jlong cap = (*env)->GetDirectBufferCapacity(env, buf);
  if (!b || cap <= 0) return 0;
  un_event ev;
  if (!un_poll(EP(ep), &ev, b, (uint32_t)cap, timeout_ms)) return 0;
  jint out[3] = {ev.channel, (jint)ev.len, (jint)ev.value};
  (*env)->SetIntArrayRegion(env, info, 0, 3, out);
  return ev.type;
}

JNIEXPORT jint JNICALL Java_org_umbra_remote_NativeLink_send(JNIEnv *env, jclass cls, jlong ep, jint channel,
                                                             jbyteArray data, jint offset, jint length) {
  (void)cls;
  jbyte *d = (*env)->GetPrimitiveArrayCritical(env, data, NULL);
  if (!d) return -1;
  int r = un_send(EP(ep), 1, channel, (const uint8_t *)d + offset, (uint32_t)length);
  (*env)->ReleasePrimitiveArrayCritical(env, data, d, JNI_ABORT);
  return r;
}

JNIEXPORT void JNICALL Java_org_umbra_remote_NativeLink_closePeer(JNIEnv *env, jclass cls, jlong ep) {
  (void)env;
  (void)cls;
  un_close_peer(EP(ep), 1);
}

JNIEXPORT void JNICALL Java_org_umbra_remote_NativeLink_free(JNIEnv *env, jclass cls, jlong ep) {
  (void)env;
  (void)cls;
  un_free(EP(ep));
}
