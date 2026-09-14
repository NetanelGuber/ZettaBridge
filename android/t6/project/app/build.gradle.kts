// T6: runs the zbrun guest test suite through libzbridge.so inside an app process next to ART.
// Plain Java, no AndroidX. jniLibs/assets/java come from tools/make_t6_bundle.sh.
plugins {
    id("com.android.application")
}

android {
    namespace = "com.zettabridge.t6"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.zettabridge.t6"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1"
        ndk { abiFilters += "arm64-v8a" }
    }

    buildTypes {
        release { isMinifyEnabled = false }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
