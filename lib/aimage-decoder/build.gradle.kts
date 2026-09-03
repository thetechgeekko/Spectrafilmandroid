/*
 * Spektrafilm for Android — API-30+ AImageDecoder qualification module.
 * GPL-3.0-only.
 *
 * This module is intentionally not a production dependency of :app. Ticket #198
 * must first prove its platform-decoder contract on the pinned OS corpus.
 */
plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.android)
}

val aimageAbiTargets = linkedMapOf(
    "arm64-v8a" to "aarch64-linux-android24",
    "armeabi-v7a" to "armv7a-linux-androideabi24",
    "x86_64" to "x86_64-linux-android24",
)

android {
    namespace = "com.spectrafilm.aimage"
    compileSdk = 34
    ndkVersion = "27.0.12077973"

    defaultConfig {
        minSdk = 24
        testInstrumentationRunner =
            "com.spectrafilm.aimage.AImageDecoderQualificationInstrumentation"
        consumerProguardFiles("consumer-rules.pro")
        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
                arguments += "-DANDROID_STL=c++_shared"
                // AImageDecoder is API 30 while the app keeps minSdk 24. Weak
                // references plus guarded callsites prevent pre-30 load failure.
                arguments += "-DANDROID_WEAK_API_DEFS=ON"
            }
        }
        ndk {
            abiFilters += aimageAbiTargets.keys.toList()
        }
    }

    // Default remains fast debug qualification. CI/device release smoke can set
    // -PaimageTestBuildType=release so the exact native fallback is exercised
    // through R8-minified target bytecode.
    testBuildType = providers.gradleProperty("aimageTestBuildType")
        .orElse("debug")
        .get()

    buildTypes {
        getByName("debug") {
            buildConfigField("boolean", "AIMAGE_TARGET_MINIFIED", "false")
        }
        getByName("release") {
            isMinifyEnabled = true
            buildConfigField("boolean", "AIMAGE_TARGET_MINIFIED", "true")
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "consumer-rules.pro",
                "proguard-rules.pro",
            )
        }
    }

    buildFeatures {
        buildConfig = true
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
    // Reuse the app's process-wide, coordinator-admitted native buffer owner.
    // This experiment must not introduce a second memory authority.
    implementation(project(":engine:spektra-core"))
    testImplementation(libs.junit)
}

/**
 * Reproducible non-device gate: host CTest plus the actual API-24 weak-link and
 * export audit for every configured ABI. It intentionally does not install,
 * fetch a corpus, or wire this experiment into :app.
 */
tasks.register("verifyAImageDecoderHostAndAbi") {
    group = "verification"
    description = "Run AImageDecoder host CTest and API-24 weak-symbol/export audit"

    val hostSource = file("src/test/host")
    val hostBuild = layout.buildDirectory.dir("host-contract").get().asFile
    val nativeBuild = layout.buildDirectory.dir("ndk-contract").get().asFile
    val nativeSource = file("src/main/cpp")
    inputs.dir(hostSource)
    inputs.dir(nativeSource)
    inputs.file("consumer-rules.pro")
    inputs.property("ndkVersion", android.ndkVersion)
    inputs.property("ndkAuditApi", 24)
    inputs.property(
        "ndkAuditTargets",
        aimageAbiTargets.entries.joinToString(",") { (abi, target) -> "$abi=$target" },
    )
    outputs.dir(hostBuild)
    outputs.dir(nativeBuild)
    // Verification evidence must be freshly compiled/audited even if an NDK
    // installation is repaired in place under the same version directory.
    outputs.upToDateWhen { false }

    doLast {
        fun runChecked(command: List<String>): String {
            val output = providers.exec {
                commandLine(command)
                isIgnoreExitValue = true
            }
            val result = output.result.get()
            val stdout = output.standardOutput.asText.get()
            val stderr = output.standardError.asText.get()
            check(result.exitValue == 0) {
                "Command failed (${result.exitValue}): ${command.joinToString(" ")}\n" +
                    stdout + stderr
            }
            return stdout + stderr
        }

        hostBuild.mkdirs()
        nativeBuild.mkdirs()
        val windows = System.getProperty("os.name").startsWith("Windows", ignoreCase = true)
        val executableSuffix = if (windows) ".exe" else ""
        val sdkCmake = file(
            "${android.sdkDirectory}/cmake/3.22.1/bin/cmake$executableSuffix",
        )
        val sdkCtest = file(
            "${android.sdkDirectory}/cmake/3.22.1/bin/ctest$executableSuffix",
        )
        val cmake = if (sdkCmake.isFile) sdkCmake.absolutePath else "cmake"
        val ctest = if (sdkCtest.isFile) sdkCtest.absolutePath else "ctest"
        runChecked(listOf(cmake, "-S", hostSource.absolutePath, "-B", hostBuild.absolutePath))
        runChecked(listOf(cmake, "--build", hostBuild.absolutePath, "--config", "Release"))
        runChecked(
            listOf(
                ctest,
                "--test-dir",
                hostBuild.absolutePath,
                "--build-config",
                "Release",
                "--output-on-failure",
            ),
        )

        val ndk = file("${android.sdkDirectory}/ndk/${android.ndkVersion}")
        val prebuiltRoot = file("$ndk/toolchains/llvm/prebuilt")
        val prebuilts = prebuiltRoot.listFiles()?.filter { it.isDirectory }.orEmpty()
        check(prebuilts.size == 1) {
            "Expected one NDK host prebuilt under $prebuiltRoot, found $prebuilts"
        }
        val toolchain = prebuilts.single()
        val bin = file("$toolchain/bin")
        val clang = file("$bin/clang++$executableSuffix")
        val readelf = file("$bin/llvm-readelf$executableSuffix")
        val nm = file("$bin/llvm-nm$executableSuffix")
        check(clang.isFile && readelf.isFile && nm.isFile) {
            "NDK clang/readelf/nm are incomplete under $bin"
        }
        val sysroot = "--sysroot=${file("$toolchain/sysroot").absolutePath}"
        val decoderSymbolRegex = Regex("\\bAImageDecoder(?:HeaderInfo)?_[A-Za-z0-9_]+\\b")
        val expectedDecoderSymbols = setOf(
            "AImageDecoder_createFromFd",
            "AImageDecoder_createFromBuffer",
            "AImageDecoder_delete",
            "AImageDecoder_getHeaderInfo",
            "AImageDecoderHeaderInfo_getMimeType",
            "AImageDecoderHeaderInfo_getWidth",
            "AImageDecoderHeaderInfo_getHeight",
            "AImageDecoderHeaderInfo_getAndroidBitmapFormat",
            "AImageDecoderHeaderInfo_getAlphaFlags",
            "AImageDecoderHeaderInfo_getDataSpace",
            "AImageDecoder_setAndroidBitmapFormat",
            "AImageDecoder_setDataSpace",
            "AImageDecoder_setTargetSize",
            "AImageDecoder_setUnpremultipliedRequired",
            "AImageDecoder_getMinimumStride",
            "AImageDecoder_decodeImage",
        )
        aimageAbiTargets.forEach { (abi, targetTriple) ->
            val abiBuild = file("$nativeBuild/$abi").apply { mkdirs() }
            val target = "--target=$targetTriple"
            val common = listOf(
                clang.absolutePath,
                target,
                sysroot,
                "-std=c++17",
                "-fPIC",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Werror=unguarded-availability",
                "-ffp-contract=off",
                // The CMake option ANDROID_WEAK_API_DEFS translates to this
                // compiler definition in NDK r27's flags.cmake.
                "-D__ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__",
                "-I${nativeSource.absolutePath}",
            )
            val gateObject = file("$abiBuild/image_input_gate.o")
            val jniObject = file("$abiBuild/aimage_decoder_jni.o")
            val library = file("$abiBuild/libsfaimage.so")
            runChecked(
                common + listOf(
                    "-c",
                    file("$nativeSource/image_input_gate.cpp").absolutePath,
                    "-o",
                    gateObject.absolutePath,
                ),
            )
            runChecked(
                common + listOf(
                    "-c",
                    file("$nativeSource/aimage_decoder_jni.cpp").absolutePath,
                    "-o",
                    jniObject.absolutePath,
                ),
            )
            runChecked(
                listOf(
                    clang.absolutePath,
                    target,
                    sysroot,
                    "-shared",
                    gateObject.absolutePath,
                    jniObject.absolutePath,
                    "-ljnigraphics",
                    "-llog",
                    "-Wl,-z,max-page-size=16384",
                    "-Wl,--version-script=${file("$nativeSource/sfaimage.map.txt").absolutePath}",
                    "-o",
                    library.absolutePath,
                ),
            )
            val symbols = runChecked(
                listOf(readelf.absolutePath, "--dyn-syms", "--wide", library.absolutePath),
            )
            val decoderImports = symbols.lineSequence()
                .filter(decoderSymbolRegex::containsMatchIn)
                .toList()
            val decoderNames = decoderImports.map { line ->
                checkNotNull(decoderSymbolRegex.find(line)).value
            }.toSet()
            check(decoderNames == expectedDecoderSymbols) {
                "$abi decoder symbol set mismatch: expected=$expectedDecoderSymbols " +
                    "actual=$decoderNames"
            }
            check(decoderImports.all { "WEAK" in it && "UND" in it }) {
                "$abi API-30 decoder import is strong or defined at minSdk 24:\n" +
                    decoderImports.joinToString("\n")
            }
            val exports = runChecked(
                listOf(nm.absolutePath, "-D", "--defined-only", library.absolutePath),
            ).lineSequence().map(String::trim).filter(String::isNotEmpty).toList()
            val publicNames = exports.map { it.substringAfterLast(' ') }
                .filterNot { it == "SFAIMAGE_1" }
            check(publicNames == listOf("JNI_OnLoad@@SFAIMAGE_1") ||
                publicNames == listOf("JNI_OnLoad@SFAIMAGE_1") ||
                publicNames == listOf("JNI_OnLoad")
            ) { "$abi unexpected public native exports: $publicNames" }
            println("AIMAGE_API24_WEAK_SYMBOLS[$abi]: PASS (${decoderImports.size})")
            println("AIMAGE_PUBLIC_EXPORTS[$abi]: PASS JNI_OnLoad only")
        }

        val nativeText = file("$nativeSource/aimage_decoder_jni.cpp").readText()
        val createSites = Regex("AImageDecoder_createFrom(?:Fd|Buffer)\\(").findAll(nativeText).count()
        val safeAdoptions = Regex(
            "created != ANDROID_IMAGE_DECODER_SUCCESS \\|\\| raw_decoder == nullptr[\\s\\S]{0,480}" +
                "DecoderOwner decoder\\(raw_decoder",
        ).findAll(nativeText).count()
        check(createSites == 4 && safeAdoptions == 4) {
            "Decoder ownership audit failed: createSites=$createSites safeAdoptions=$safeAdoptions"
        }
        println("AIMAGE_HOST_CTEST: PASS")
        println("AIMAGE_API24_WEAK_SYMBOLS: PASS all configured ABIs")
        println("AIMAGE_PUBLIC_EXPORTS: PASS all configured ABIs")
        println("AIMAGE_CREATE_OWNERSHIP: PASS 4/4")
    }
}
