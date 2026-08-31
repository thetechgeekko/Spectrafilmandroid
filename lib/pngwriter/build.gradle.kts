/*
 * Spektrafilm for Android — lib:pngwriter build.
 * Copyright (C) 2026 Spektrafilm Android contributors. GPLv3.
 *
 * Android library wrapping the native 16-bit PNG writer (libsfpng.so, built via
 * CMake/NDK using zlib for deflate + CRC32) plus the Kotlin PngWriter facade.
 *
 * zlib is available in every Android API level as a system shared library
 * (/system/lib[64]/libz.so); the NDK sysroot ships libz.a + libz.so stubs so
 * find_library(z-lib z) in CMakeLists resolves at configure-time and the
 * runtime linker binds to the on-device system libz at app startup — no bundled
 * zlib sources or extra AAR dependency needed.
 *
 * Mirrors lib:tiffwriter's plain-AGP setup (com.android.library + kotlin.android +
 * externalNativeBuild CMake + abiFilters) so it configures/builds standalone the
 * moment the project lead adds it to settings.gradle.kts.
 */
plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "com.spectrafilm.pngwriter"
    compileSdk = 34

    // NDK r27+ links native LOAD segments with a 16 KB max-page-size by default and
    // ships a 16 KB-aligned libc++_shared.so — required for Android 15's 16 KB page
    // devices. The CMake link flag (CMakeLists.txt) pins the alignment explicitly too.
    ndkVersion = "27.0.12077973"

    defaultConfig {
        minSdk = 24
        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
                arguments += "-DANDROID_STL=c++_shared"
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
    // No compile dependencies: zlib is a system library on Android; the writer
    // takes a 16-bit RGB buffer + optional ICC bytes + output path. The caller
    // (feature:film-emulation, a later wave) quantises the engine's display-
    // referred float output and supplies the matching ICC profile asset.
    testImplementation(libs.junit)
}
