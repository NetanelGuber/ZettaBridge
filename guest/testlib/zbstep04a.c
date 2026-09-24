#include <jni.h>

JNIEXPORT jint JNICALL Java_com_zettabridge_step04_Probe_add(JNIEnv* env, jclass cls, jint value) {
    (void)env;
    (void)cls;
    return value + 7;
}

static jint multiply(JNIEnv* env, jclass cls, jint value) {
    (void)env;
    (void)cls;
    return value * 3;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    JNIEnv* env = 0;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass probe = (*env)->FindClass(env, "com/zettabridge/step04/Probe");
    if (!probe) return JNI_ERR;
    jmethodID callback = (*env)->GetStaticMethodID(env, probe, "onGuestLoaded", "(I)I");
    if (!callback || (*env)->CallStaticIntMethod(env, probe, callback, 11) != 22) return JNI_ERR;
    JNINativeMethod methods[] = {{"multiply", "(I)I", (void*)multiply}};
    if ((*env)->RegisterNatives(env, probe, methods, 1) != JNI_OK) return JNI_ERR;
    return JNI_VERSION_1_6;
}
