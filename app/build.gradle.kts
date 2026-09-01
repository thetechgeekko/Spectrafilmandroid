// Spektrafilm for Android — app. GPLv3.
import java.util.Properties
import org.gradle.api.artifacts.component.ModuleComponentIdentifier
import org.gradle.api.artifacts.component.ProjectComponentIdentifier

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

// Optional local release signing is read from keystore.properties in the project root.
// Expected keys: storeFile, storePassword, keyAlias, keyPassword. Without that file,
// assembleRelease deliberately emits an unsigned APK; it never falls back to the
// public debug key. The protected release workflow signs that qualified artifact
// later with pinned Android build-tools, outside Gradle's dependency graph.
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
        testInstrumentationRunner = "com.spectrafilm.app.ReleaseCandidateSmokeInstrumentation"
        ndk { abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64") }
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
            ndk {
                // Retain complete native symbols for the exact shipping build.
                // release.yml hash-binds and publishes AGP's symbols ZIP.
                debugSymbolLevel = "FULL"
            }
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            // Local maintainers may opt in to a release key. CI intentionally leaves
            // this unset and transfers the unsigned, minified candidate to the
            // protected signing job.
            if (hasReleaseKeystore) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    buildFeatures { compose = true }
    // The gate APK must target the minified release variant that is later
    // externally signed, not AGP's default debug app.
    testBuildType = "release"

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
    implementation(libs.androidx.datastore.preferences)
    implementation(libs.androidx.exifinterface)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.ui)
    implementation(libs.androidx.ui.graphics)
    implementation(libs.androidx.ui.tooling.preview)
    implementation(libs.androidx.material3)

    testImplementation(libs.junit)
    // Real org.json on the unit-test classpath (the android.jar stub throws "not
    // mocked"); lets Presets JSON round-trip be tested on the plain JVM.
    testImplementation(libs.org.json)
}

// Canonical provenance input: Gradle's `dependencies` console report includes
// timing/task footers, so its bytes are not reproducible across identical runs.
val releaseRuntimeClasspath = providers.provider {
    configurations.getByName("releaseRuntimeClasspath")
}
val releaseRuntimeReport = rootProject.layout.buildDirectory.file(
    "release-runtime-classpath.txt"
)
tasks.register("writeReleaseRuntimeClasspath") {
    outputs.file(releaseRuntimeReport)
    outputs.upToDateWhen { false }
    doLast {
        val components = releaseRuntimeClasspath.get()
            .incoming.resolutionResult.allComponents
            .map { component ->
                when (val id = component.id) {
                    is ModuleComponentIdentifier ->
                        "module\t${id.group}\t${id.module}\t${id.version}"
                    is ProjectComponentIdentifier -> "project\t${id.projectPath}"
                    else -> "component\t${id.displayName}"
                }
            }
            .toSortedSet()
        releaseRuntimeReport.get().asFile.apply {
            parentFile.mkdirs()
            writeText(components.joinToString("\n", postfix = "\n"), Charsets.UTF_8)
        }
    }
}
