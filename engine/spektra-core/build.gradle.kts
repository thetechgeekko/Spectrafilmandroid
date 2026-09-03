// Spektrafilm for Android — engine:spektra-core. GPLv3.
// Android library wrapping the native spektrafilm engine (libspektra.so, built via
// CMake/NDK) plus the Kotlin facade (SpektraEngine / SpektraParams).
plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.android)
}

android {
    namespace = "com.spectrafilm.engine"
    compileSdk = 34

    // NDK r27+ links native LOAD segments with a 16 KB max-page-size by default and
    // ships a 16 KB-aligned libc++_shared.so — required for Android 15's 16 KB page
    // devices. The CMake link flag (CMakeLists.txt) pins the alignment explicitly too.
    ndkVersion = "27.0.12077973"

    defaultConfig {
        minSdk = 24
        testInstrumentationRunner = "com.spectrafilm.engine.EngineBoundaryInstrumentation"
        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
                arguments += "-DANDROID_STL=c++_shared"
                // GPU preview fast-path (GPU M1, #146): compile the Vulkan host
                // into the Android library. Runtime-gated (Settings toggle,
                // default OFF + device self-check + CPU fallback); the host
                // parity-test builds stay stub (flag defaults OFF in CMake).
                arguments += "-DSPK_ENABLE_VULKAN=ON"
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

    // C++ sources test code lives under src/main/cpp/tests but is host-only
    // (standalone g++ host parity tests); it is not part of the Android library.

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}

dependencies {
    testImplementation(libs.junit)
}
