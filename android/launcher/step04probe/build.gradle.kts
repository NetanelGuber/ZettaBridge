plugins { id("com.android.application") }

android {
    namespace = "com.zettabridge.step04"
    compileSdk = 35
    defaultConfig {
        applicationId = "com.zettabridge.step04"
        minSdk = 29
        targetSdk = 35
        versionCode = 1
        versionName = "step04"
        ndk { abiFilters += "arm64-v8a" }
    }
    sourceSets.getByName("main") {
        assets.srcDir(rootProject.file("../../build/step04/assets"))
        jniLibs.srcDir(rootProject.file("../../build/step04/jniLibs"))
    }
    packaging { jniLibs.useLegacyPackaging = true }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
