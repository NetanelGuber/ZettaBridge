plugins { id("com.android.application") }

android {
    namespace = "com.zettabridge.step05fixture"
    compileSdk = 35
    defaultConfig {
        applicationId = "com.zettabridge.step05fixture"
        minSdk = 29
        targetSdk = 35
        versionCode = 1
        versionName = "step05"
        ndk { abiFilters += "armeabi-v7a" }
    }
    sourceSets.getByName("main") {
        jniLibs.srcDir(rootProject.file("../../build/step05fixture/jniLibs"))
    }
    packaging { jniLibs.useLegacyPackaging = false }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
