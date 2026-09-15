/* Guest probe library for jni_bridge_test: drives the guest JNIEnv (libzbjni.so) against the
 * mock JVM. Every probe is called like a native method (JNIEnv*, jobject argument) and returns 0,
 * or the source line of the first failed check. The mock model is defined in
 * tests/host/jni_bridge_test.cpp. */
#include <jni.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "zb/jni_hostcalls.h"

#define CHECK(cond)                   \
    do {                              \
        if (!(cond)) return __LINE__; \
    } while (0)

#define SAME(a, b) ((a) == (b))
#define SAME_REF(a, b) ((*env)->IsSameObject(env, (a), (b)) == JNI_TRUE)

/* ---- Classes, ids, references, frames, monitors, exceptions --------------------------------- */

static jobject held_global;
static jweak held_weak;

/* held: a zb/Probe instance from Java, which the mock collects before objects_after_gc. */
JNIEXPORT jint JNICALL zbjniprobe_objects(JNIEnv* env, jobject held) {
    CHECK((*env)->GetVersion(env) == JNI_VERSION_1_6);
    CHECK((*env)->DefineClass(env, "zb/Defined", NULL, NULL, 0) == NULL);

    const jclass probe = (*env)->FindClass(env, "zb/Probe");
    const jclass child_class = (*env)->FindClass(env, "zb/ProbeChild");
    CHECK(probe != NULL && child_class != NULL);
    CHECK(SAME_REF((*env)->GetSuperclass(env, child_class), probe));
    CHECK((*env)->IsAssignableFrom(env, child_class, probe) && !(*env)->IsAssignableFrom(env, probe, child_class));
    const jobject child = (*env)->AllocObject(env, child_class);
    CHECK(child != NULL && SAME_REF((*env)->GetObjectClass(env, child), child_class));
    CHECK((*env)->IsInstanceOf(env, child, probe) && !(*env)->IsInstanceOf(env, held, child_class));
    CHECK((*env)->IsInstanceOf(env, NULL, probe) && SAME_REF(NULL, NULL) && !SAME_REF(child, held));

    /* Member ids and reflection round trips. */
    const jmethodID echo_i = (*env)->GetMethodID(env, probe, "echoI", "(I)I");
    const jmethodID secho_i = (*env)->GetStaticMethodID(env, probe, "sechoI", "(I)I");
    const jfieldID field_i = (*env)->GetFieldID(env, probe, "i", "I");
    const jfieldID field_si = (*env)->GetStaticFieldID(env, probe, "si", "I");
    CHECK(echo_i != NULL && secho_i != NULL && field_i != NULL && field_si != NULL && echo_i != secho_i);
    CHECK((*env)->GetMethodID(env, probe, "echoI", "(I)I") == echo_i); /* ids are deduplicated */
    const jobject method = (*env)->ToReflectedMethod(env, probe, secho_i, JNI_TRUE);
    CHECK(method != NULL && (*env)->FromReflectedMethod(env, method) == secho_i);
    const jobject field = (*env)->ToReflectedField(env, probe, field_si, JNI_TRUE);
    CHECK(field != NULL && (*env)->FromReflectedField(env, field) == field_si);
    CHECK((*env)->FindClass(env, "zb/Missing") == NULL && (*env)->ExceptionCheck(env) == JNI_TRUE);
    (*env)->ExceptionClear(env);
    CHECK((*env)->GetMethodID(env, probe, "missing", "()V") == NULL && (*env)->ExceptionCheck(env) == JNI_TRUE);
    (*env)->ExceptionClear(env);
    CHECK((*env)->GetFieldID(env, probe, "missing", "I") == NULL && (*env)->ExceptionCheck(env) == JNI_TRUE);
    (*env)->ExceptionClear(env);

    /* Local frames and local references. */
    CHECK((*env)->EnsureLocalCapacity(env, 100) == 0);
    CHECK((*env)->PushLocalFrame(env, 8) == 0);
    const jobject inner = (*env)->AllocObject(env, probe);
    const jobject kept = (*env)->AllocObject(env, child_class);
    CHECK((*env)->GetObjectRefType(env, inner) == JNILocalRefType);
    const jobject result = (*env)->PopLocalFrame(env, kept);
    CHECK(result != NULL && (*env)->IsInstanceOf(env, result, child_class));
    CHECK((*env)->GetObjectRefType(env, inner) == JNIInvalidRefType); /* popped with its frame */
    const jobject copy = (*env)->NewLocalRef(env, result);
    CHECK(SAME_REF(copy, result));
    (*env)->DeleteLocalRef(env, copy);
    CHECK((*env)->GetObjectRefType(env, copy) == JNIInvalidRefType);
    CHECK((*env)->NewLocalRef(env, NULL) == NULL && (*env)->NewGlobalRef(env, NULL) == NULL);
    (*env)->DeleteLocalRef(env, NULL);

    /* Global and weak global references, kept until objects_after_gc. */
    held_global = (*env)->NewGlobalRef(env, child);
    held_weak = (*env)->NewWeakGlobalRef(env, held);
    CHECK((*env)->GetObjectRefType(env, held_global) == JNIGlobalRefType);
    CHECK((*env)->GetObjectRefType(env, held_weak) == JNIWeakGlobalRefType);
    CHECK(SAME_REF(held_global, child) && SAME_REF(held_weak, held));
    CHECK(SAME_REF((*env)->NewLocalRef(env, held_weak), held));

    /* Monitors are recursive. */
    CHECK((*env)->MonitorEnter(env, child) == 0 && (*env)->MonitorEnter(env, child) == 0);
    CHECK((*env)->MonitorExit(env, child) == 0 && (*env)->MonitorExit(env, child) == 0);

    /* Exceptions raised and inspected by guest code. */
    const jclass illegal_state = (*env)->FindClass(env, "java/lang/IllegalStateException");
    const jclass runtime_exception = (*env)->FindClass(env, "java/lang/RuntimeException");
    CHECK((*env)->ExceptionCheck(env) == JNI_FALSE && (*env)->ExceptionOccurred(env) == NULL);
    CHECK((*env)->ThrowNew(env, illegal_state, "guest") == 0 && (*env)->ExceptionCheck(env) == JNI_TRUE);
    const jthrowable thrown = (*env)->ExceptionOccurred(env);
    (*env)->ExceptionClear(env);
    CHECK(thrown != NULL && (*env)->IsInstanceOf(env, thrown, runtime_exception));
    CHECK((*env)->Throw(env, thrown) == 0 && (*env)->ExceptionCheck(env) == JNI_TRUE);
    (*env)->ExceptionDescribe(env); /* prints and clears */
    CHECK((*env)->ExceptionCheck(env) == JNI_FALSE);
    return 0;
}

/* Runs after the mock collected the held object: the global survives, the weak reads as null. */
JNIEXPORT jint JNICALL zbjniprobe_objects_after_gc(JNIEnv* env, jobject unused) {
    CHECK(held_global != NULL && !SAME_REF(held_global, NULL));
    CHECK(SAME_REF(held_weak, NULL) && (*env)->NewLocalRef(env, held_weak) == NULL);
    (*env)->DeleteWeakGlobalRef(env, held_weak);
    (*env)->DeleteGlobalRef(env, held_global);
    CHECK((*env)->GetObjectRefType(env, held_global) == JNIInvalidRefType);
    CHECK((*env)->GetObjectRefType(env, held_weak) == JNIInvalidRefType);
    held_global = NULL;
    held_weak = NULL;
    return 0;
}

/* Returns with a pending exception, which Java sees after the native method returns. */
JNIEXPORT jint JNICALL zbjniprobe_throw(JNIEnv* env, jobject unused) {
    const jclass illegal = (*env)->FindClass(env, "java/lang/IllegalArgumentException");
    CHECK((*env)->ThrowNew(env, illegal, "from guest") == 0);
    return 0;
}

/* Invalid handle: the host reports a fatal error and does not return. */
JNIEXPORT jint JNICALL zbjniprobe_bad_handle(JNIEnv* env, jobject unused) {
    (*env)->GetObjectClass(env, (jobject)(uintptr_t)0x12345);
    return __LINE__;
}

