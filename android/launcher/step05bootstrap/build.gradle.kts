plugins { id("com.android.application") }

android {
    namespace = "com.zettabridge.bootstrap"
    compileSdk = 35
    defaultConfig {
        applicationId = "com.zettabridge.bootstrap"
        minSdk = 29
        targetSdk = 35
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
