// ZettaBridge launcher. Generated native/runtime inputs come only from build/launcher.
// Plain Java, UI built in code, no AndroidX.
plugins {
    id("com.android.application")
}

android {
    namespace = "com.zettabridge.launcher"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.zettabridge.launcher"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.0.1"
        ndk { abiFilters += "arm64-v8a" }
    }

    buildTypes {
        release { isMinifyEnabled = false }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    sourceSets.getByName("main") {
        assets.srcDir(rootProject.file("../../build/launcher/assets"))
        jniLibs.srcDir(rootProject.file("../../build/launcher/jniLibs"))
    }
}

dependencies {
    // Apache-2.0. Lifts hidden API restrictions for the framework hooks in the :guest process.
    implementation("org.lsposed.hiddenapibypass:hiddenapibypass:6.1")
}
