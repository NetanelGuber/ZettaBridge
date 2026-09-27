plugins {
    id("com.android.application")
}

val converterAssets = layout.buildDirectory.dir("generated/converterAssets")
val launcherBundle = rootProject.file("../../build/launcher")
val runtimeRoot = launcherBundle.resolve("assets/zb")
val bootstrapApk = rootProject.file("step05bootstrap/build/outputs/apk/debug/step05bootstrap-debug.apk")

val prepareConverterAssets by tasks.registering {
    dependsOn(":step05bootstrap:assembleDebug")
    inputs.dir(runtimeRoot)
    inputs.file(launcherBundle.resolve("jniLibs/arm64-v8a/libzbridge.so"))
    inputs.file(bootstrapApk)
    outputs.dir(converterAssets)
    doLast {
        val runtime = runtimeRoot
        val bridge = launcherBundle.resolve("jniLibs/arm64-v8a/libzbridge.so")
        val proxy = runtime.resolve("host/libzbproxy.so")
        if (!runtime.isDirectory || !bridge.isFile || !proxy.isFile || !bootstrapApk.isFile) {
            throw GradleException("Build the ZettaBridge runtime bundle before assembling :manager")
        }
        val out = converterAssets.get().asFile
        project.delete(out)
        val converterDir = out.resolve("converter")
        val runtimeOut = converterDir.resolve("runtime")
        val names = runtime.walkTopDown()
            .filter { it.isFile }
            .map { it to it.relativeTo(runtime).invariantSeparatorsPath }
            .filter { (_, relative) -> !relative.startsWith("app/") && !relative.startsWith("host/") }
            .sortedBy { (_, relative) -> relative }
            .toList()
        if (names.isEmpty()) throw GradleException("ZettaBridge runtime bundle is empty")
        val listing = ArrayList<String>()
        for ((source, relative) in names) {
            val target = runtimeOut.resolve("zb/$relative")
            target.parentFile.mkdirs()
            source.copyTo(target, overwrite = true)
            listing.add("zb/$relative")
        }
        converterDir.mkdirs()
        converterDir.resolve("runtime-files.txt").writeText(listing.joinToString("\n", postfix = "\n"))
        bridge.copyTo(converterDir.resolve("libzbridge.so"), overwrite = true)
        proxy.copyTo(converterDir.resolve("libzbproxy.so"), overwrite = true)
        bootstrapApk.copyTo(converterDir.resolve("bootstrap.apk"), overwrite = true)
    }
}

android {
    namespace = "com.zettabridge.manager"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.zettabridge.manager"
        minSdk = 26
        targetSdk = 35
        versionCode = 2
        versionName = "0.2.0"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

android.sourceSets.getByName("main").assets.srcDir(converterAssets)
tasks.matching { it.name == "preBuild" }.configureEach { dependsOn(prepareConverterAssets) }

dependencies {
    implementation("com.android.tools.build:apksig:8.7.3")
    testImplementation("junit:junit:4.13.2")
}
