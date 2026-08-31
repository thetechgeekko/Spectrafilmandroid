/*
 * Spektrafilm for Android — lib:libraw build.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 * Decoding powered by statically included, dual-offered LibRaw. The selected
 * distribution route is a fail-closed release decision tracked by ticket #166.
 *
 * Android library wrapping the native RAW/DNG decoder (libsfraw.so, built via
 * CMake/NDK with LibRaw compiled in statically) plus the Kotlin RawDecoder facade.
 *
 * This mirrors engine:spektra-core's plain-AGP setup (com.android.library +
 * kotlin.android + externalNativeBuild CMake + abiFilters) so the module
 * configures and builds standalone the moment the project lead adds it to
 * settings.gradle.kts — no host-specific convention plugins required.
 */
plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "com.spectrafilm.libraw"
    compileSdk = 34

    // NDK r27+ links native LOAD segments with a 16 KB max-page-size by default and
    // ships a 16 KB-aligned libc++_shared.so — required for Android 15's 16 KB page
    // devices. The CMake link flag (CMakeLists.txt) pins the alignment explicitly too.
    ndkVersion = "27.0.12077973"

    defaultConfig {
        minSdk = 24
        testInstrumentationRunner =
            "com.spectrafilm.libraw.RawDecoderBoundaryInstrumentation"
        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
                arguments += "-DANDROID_STL=c++_shared"
                // Make the shipping policy explicit so a stale external-native
                // CMake cache cannot retain an earlier experimental OpenMP opt-in.
                arguments += "-DSFRAW_ENABLE_OPENMP=OFF"
                // Keep zlib enabled for LibRaw 0.22.2's supported floating-point
                // deflate subset. Integer/linear deflate remains typed fallback.
                arguments += "-DSFRAW_WITH_ZLIB=ON"
            }
        }
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}

dependencies {
    // The decoded result is a self-contained float32 ByteBuffer + width/height/
    // colorSpace (see RawDecoder.LinearResult); the engine's LinearImage is
    // constructed by the caller (feature:film-emulation), so this module needs no
    // compile dependency on engine:spektra-core. The project lead can add
    //   implementation(projects.engine.spektraCore)
    // if a future change makes lib:libraw construct LinearImage directly.

    // RawCoilDecoder.kt (the secondary "full-res RAW in the gallery" integration
    // point) is written against Coil 3's Decoder API. It is declared compileOnly so
    // this module compiles standalone without bundling Coil; the host app
    // (ImageToolbox) already provides coil3 at runtime when it registers
    // RawCoilDecoder.Factory(). Coil is not in the version catalog, so the
    // coordinate is pinned literally here; swap for libs.coil.core once the host
    // adds it to gradle/libs.versions.toml.
    compileOnly("io.coil-kt.coil3:coil-core:3.0.4")
    compileOnly("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
    testImplementation(libs.junit)
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-core:1.9.0")
}
