// Spektrafilm for Android — app. GPLv3.
import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

// Release signing is read from keystore.properties in the project root when present.
// Expected keys: storeFile, storePassword, keyAlias, keyPassword. When the file is
// absent (e.g. CI without secrets) the release build falls back to debug signing.
val keystorePropsFile = rootProject.file("keystore.properties")
val keystoreProps = Properties().apply {
    if (keystorePropsFile.exists()) keystorePropsFile.inputStream().use { load(it) }
}
val hasReleaseKeystore = keystorePropsFile.exists() &&
    keystoreProps.getProperty("storeFile") != null

android {
    namespace = "com.spectrafilm.app"
    compileSdk = 34

    // build-tools 35.0.0 is the first whose zipalign supports `-P 16`; AGP uses it to
    // page-align the (uncompressed) bundled .so to 16 KB offsets inside the APK, which
    // is what lets a 16 KB-page device mmap them. Without it the libs are only 4 KB-
    // aligned in the zip and fail to load on Android 15 16 KB devices even when their
    // own ELF segments are 16 KB-aligned.
    buildToolsVersion = "35.0.0"

    defaultConfig {
        applicationId = "com.spectrafilm.app"
        minSdk = 24
        targetSdk = 34
        versionCode = 11
        versionName = "0.9.0"
        ndk { abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64") }
        // On-device instrumented test suite (app/src/androidTest). Headless (PR1):
        // engine .so load + parity, native RAW/TIFF/PNG, presets/recipes/masks.
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    signingConfigs {
        // Pin DEBUG signing to a committed keystore (standard public debug creds) so every
        // build — CI, any cloud container, every contributor — signs with ONE identical key,
        // and a freshly built debug APK always installs OVER a previous one. AGP otherwise
        // auto-generates a random ~/.android/debug.keystore per machine, whose differing
        // signature makes Android reject the update ("App not installed"). Not a release key.
        getByName("debug") {
            storeFile = rootProject.file("debug.keystore")
            storePassword = "android"
            keyAlias = "androiddebugkey"
            keyPassword = "android"
        }
        if (hasReleaseKeystore) {
            create("release") {
                storeFile = rootProject.file(keystoreProps.getProperty("storeFile"))
                storePassword = keystoreProps.getProperty("storePassword")
                keyAlias = keystoreProps.getProperty("keyAlias")
                keyPassword = keystoreProps.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            // Use the real release keystore when keystore.properties is present,
            // otherwise fall back to debug signing so assembleRelease works in CI.
            signingConfig = if (hasReleaseKeystore) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug")
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    buildFeatures { compose = true }

    lint {
        baseline = file("lint-baseline.xml")
        abortOnError = true
        checkReleaseBuilds = true
    }
}

dependencies {
    implementation(project(":engine:spektra-core"))
    implementation(project(":lib:libraw"))
    implementation(project(":lib:tiffwriter"))
    implementation(project(":lib:pngwriter"))
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.activity.compose)
    implementation("androidx.datastore:datastore-preferences:1.1.1")
    implementation("androidx.exifinterface:exifinterface:1.3.7")
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.ui)
    implementation(libs.androidx.ui.graphics)
    implementation(libs.androidx.ui.tooling.preview)
    implementation(libs.androidx.material3)

    testImplementation(libs.junit)
    // Real org.json on the unit-test classpath (the android.jar stub throws "not
    // mocked"); lets Presets JSON round-trip be tested on the plain JVM.
    testImplementation("org.json:json:20231013")

    // On-device instrumented tests (app/src/androidTest). AndroidJUnitRunner + rules +
    // ext.junit for @RunWith(AndroidJUnit4); espresso is unused in PR1 (headless) but
    // harmless. The compose ui-test deps are pinned by the same BOM the app uses so a
    // later PR2 can add testTag-driven UI tests without touching versions.
    androidTestImplementation(libs.junit)
    androidTestImplementation("androidx.test:runner:1.6.2")
    androidTestImplementation("androidx.test:rules:1.6.1")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("androidx.test.espresso:espresso-core:3.6.1")
    androidTestImplementation(platform(libs.androidx.compose.bom))
    androidTestImplementation("androidx.compose.ui:ui-test-junit4")
    debugImplementation("androidx.compose.ui:ui-test-manifest")
}
