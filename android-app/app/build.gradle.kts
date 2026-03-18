plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

import org.gradle.api.tasks.Copy
import java.security.MessageDigest

fun sha256(file: File): String {
    val digest = MessageDigest.getInstance("SHA-256")
    file.inputStream().use { input ->
        val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
        while (true) {
            val read = input.read(buffer)
            if (read < 0) break
            digest.update(buffer, 0, read)
        }
    }
    return digest.digest().joinToString("") { "%02x".format(it) }
}

android {
    namespace = "org.openscreen.controlcast"
    compileSdk = 35

    defaultConfig {
        applicationId = "org.openscreen.controlcast"
        minSdk = 29
        targetSdk = 35
        versionCode = 1
        versionName = "0.1"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        vectorDrawables {
            useSupportLibrary = true
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        compose = true
    }

    composeOptions {
        kotlinCompilerExtensionVersion = "1.5.14"
    }

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }
}

val controlcastSo = rootProject.file("../out/android/libcontrolcast.so")
val packagedControlcastSo = project.file("src/main/jniLibs/arm64-v8a/libcontrolcast.so")

val syncControlcastJniLib by tasks.registering(Copy::class) {
    group = "build"
    description = "Copy the freshly built GN controlcast native library into app jniLibs."
    from(controlcastSo)
    into(packagedControlcastSo.parentFile)
    rename { "libcontrolcast.so" }
    doFirst {
        require(controlcastSo.exists()) {
            "Missing native library at ${controlcastSo.path}. Build it first with: ninja -C out/android controlcast"
        }
        packagedControlcastSo.parentFile.mkdirs()
    }
}

val verifyControlcastJniLib by tasks.registering {
    group = "verification"
    description = "Fail if the packaged controlcast JNI library does not match the GN build output."
    dependsOn(syncControlcastJniLib)
    doLast {
        require(packagedControlcastSo.exists()) {
            "Packaged JNI library missing at ${packagedControlcastSo.path}"
        }
        val sourceHash = sha256(controlcastSo)
        val packagedHash = sha256(packagedControlcastSo)
        check(sourceHash == packagedHash) {
            "Packaged JNI library is stale: ${packagedControlcastSo.path} does not match ${controlcastSo.path}"
        }
    }
}

tasks.matching {
    it.name in setOf(
        "mergeDebugJniLibFolders",
        "mergeDebugNativeLibs",
        "assembleDebug",
        "packageDebug",
        "installDebug",
    )
}.configureEach {
    dependsOn(syncControlcastJniLib, verifyControlcastJniLib)
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.06.00")

    implementation(composeBom)
    androidTestImplementation(composeBom)

    implementation("androidx.activity:activity-compose:1.9.0")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.foundation:foundation")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.3")
    implementation("androidx.media3:media3-exoplayer:1.4.0")
    implementation("androidx.media3:media3-ui:1.4.0")
    implementation("androidx.documentfile:documentfile:1.0.1")
    implementation("androidx.camera:camera-core:1.3.4")
    implementation("androidx.camera:camera-camera2:1.3.4")
    implementation("androidx.camera:camera-lifecycle:1.3.4")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")

    debugImplementation("androidx.compose.ui:ui-tooling")
    debugImplementation("androidx.compose.ui:ui-test-manifest")

    androidTestImplementation("androidx.test.ext:junit:1.1.5")
    androidTestImplementation("androidx.test.espresso:espresso-core:3.5.1")
    androidTestImplementation("androidx.compose.ui:ui-test-junit4")

    testImplementation("junit:junit:4.13.2")
}
