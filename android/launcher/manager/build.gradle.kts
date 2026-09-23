plugins {
    id("com.android.application")
}

android {
    namespace = "com.zettabridge.manager"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.zettabridge.manager"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
