plugins { id("com.android.application") }

android {
    namespace = "com.zettabridge.step08fixture"
    compileSdk = 35
    defaultConfig {
        applicationId = "com.zettabridge.step08fixture"
        minSdk = 29
        targetSdk = 35
        versionCode = 1
        ndk { abiFilters += "armeabi-v7a" }
    }
    sourceSets.getByName("main") {
        jniLibs.srcDir(rootProject.file("../../build/step08fixture/jniLibs"))
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
