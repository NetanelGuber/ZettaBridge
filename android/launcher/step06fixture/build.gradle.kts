plugins { id("com.android.application") }

android {
    namespace = "com.zettabridge.step06fixture"
    compileSdk = 35
    defaultConfig {
        applicationId = providers.gradleProperty("step06Package")
            .orElse("com.zettabridge.step06fixture").get()
        minSdk = 29
        targetSdk = 35
        versionCode = providers.gradleProperty("step06Version").orElse("1").get().toInt()
        versionName = "step06"
        ndk { abiFilters += "armeabi-v7a" }
    }
    if (!providers.gradleProperty("step06NoNative").orElse("false").get().toBoolean()) {
        sourceSets.getByName("main") {
            jniLibs.srcDir(rootProject.file("../../build/step05fixture/jniLibs"))
        }
    }
    packaging { jniLibs.useLegacyPackaging = false }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
