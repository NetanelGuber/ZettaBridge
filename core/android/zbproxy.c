// libzbproxy.so: the arm64 proxy ART loads in place of an arm32 plugin library.
//
// The launcher's plugin class loader returns one copy of this library per arm32 library
// (plugins/<pkg>/proxy/lib<name>.so). Its only job is to tell the translator which copy was
// loaded: JNI_OnLoad finds its own path with dladdr and calls
//     static int com.zettabridge.core.ZBridge.onProxyLoaded(String proxyPath)
// During JNI_OnLoad ART's class-loader override is the plugin loader, which delegates
// com.zettabridge.core.* to the launcher. The proxy links against nothing of ours (no
// libzbridge.so, no libc++): tools/check_zbproxy.py checks every Android link.
//
// Contract of onProxyLoaded: it returns the guest JNI version (0 means "no preference" and
// becomes JNI_VERSION_1_6) or throws UnsatisfiedLinkError. Any exception, or a version ART
// would reject, makes JNI_OnLoad return JNI_ERR. A pending exception is left pending; note
// that ART's JVM_NativeLoad clears it and System.loadLibrary throws its own
// "JNI_ERR returned from JNI_OnLoad" error, so the translator must record failure detail
// itself.

#define _GNU_SOURCE  // Dl_info and dladdr on glibc (host unit test).

#include <dlfcn.h>
#include <jni.h>
#include <stdarg.h>
#include <stddef.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

static const char kBridgeClass[] = "com/zettabridge/core/ZBridge";
static const char kMethodName[] = "onProxyLoaded";
static const char kMethodSignature[] = "(Ljava/lang/String;)I";

__attribute__((format(printf, 1, 2)))
static void log_error(const char* format, ...) {
#ifdef __ANDROID__
    va_list args;
    va_start(args, format);
    __android_log_vprint(ANDROID_LOG_ERROR, "zbproxy", format, args);
    va_end(args);
#else
    (void)format;
#endif
}

// The versions ART's JavaVMExt::IsBadJniVersion accepts from JNI_OnLoad.
static int supported_version(jint version) {
    return version == JNI_VERSION_1_2 || version == JNI_VERSION_1_4 || version == JNI_VERSION_1_6;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;

    Dl_info info;
    if (dladdr((void*)&JNI_OnLoad, &info) == 0 || info.dli_fname == NULL || info.dli_fname[0] != '/') {
        log_error("cannot find the proxy path with dladdr");
        return JNI_ERR;
    }
    const char* proxy_path = info.dli_fname;

    JNIEnv* env = NULL;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK || env == NULL) {
        log_error("%s: GetEnv(JNI_VERSION_1_6) failed", proxy_path);
        return JNI_ERR;
    }
    if ((*env)->ExceptionCheck(env)) {
        log_error("%s: exception already pending in JNI_OnLoad", proxy_path);
        return JNI_ERR;
    }

    jclass bridge = (*env)->FindClass(env, kBridgeClass);
    if (bridge == NULL) {
        log_error("%s: class %s not found", proxy_path, kBridgeClass);
        return JNI_ERR;
    }

    jint result = JNI_ERR;
    jstring path = NULL;
    jmethodID method = (*env)->GetStaticMethodID(env, bridge, kMethodName, kMethodSignature);
    if (method == NULL) {
        log_error("%s: method %s%s not found", proxy_path, kMethodName, kMethodSignature);
        goto done;
    }
    path = (*env)->NewStringUTF(env, proxy_path);
    if (path == NULL) {
        log_error("%s: NewStringUTF failed", proxy_path);
        goto done;
    }

    jint reported = (*env)->CallStaticIntMethod(env, bridge, method, path);
    if ((*env)->ExceptionCheck(env)) {
        log_error("%s: %s threw", proxy_path, kMethodName);
        goto done;
    }
    if (reported == 0) {
        result = JNI_VERSION_1_6;
    } else if (supported_version(reported)) {
        result = reported;
    } else {
        log_error("%s: %s returned unsupported JNI version 0x%08x", proxy_path, kMethodName,
                  (unsigned)reported);
    }

done:
    // DeleteLocalRef is one of the calls JNI allows with an exception pending.
    if (path != NULL) (*env)->DeleteLocalRef(env, path);
    (*env)->DeleteLocalRef(env, bridge);
    return result;
}
