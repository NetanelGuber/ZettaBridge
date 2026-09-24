#include <jni.h>

JNIEXPORT jint JNICALL Java_com_zettabridge_step04_Probe_subtract(JNIEnv* env, jclass cls, jint value) {
    (void)env;
    (void)cls;
    return value - 4;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    JNIEnv* env = 0;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass probe = (*env)->FindClass(env, "com/zettabridge/step04/Probe");
    if (!probe) return JNI_ERR;
    jmethodID callback = (*env)->GetStaticMethodID(env, probe, "onGuestLoaded", "(I)I");
    if (!callback || (*env)->CallStaticIntMethod(env, probe, callback, 13) != 26) return JNI_ERR;
    return JNI_VERSION_1_6;
}
