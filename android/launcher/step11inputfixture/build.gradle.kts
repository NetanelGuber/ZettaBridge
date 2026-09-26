plugins { id("com.android.application") }

android {
    namespace = "com.zettabridge.step11inputfixture"
    compileSdk = 35
    defaultConfig {
        applicationId = "com.zettabridge.step11inputfixture"
        minSdk = 33
        targetSdk = 35
        versionCode = 1
        ndk { abiFilters += "arm64-v8a" }
    }
    sourceSets.getByName("main") {
        jniLibs.srcDir(rootProject.file("../../build/step11inputfixture/jniLibs"))
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
