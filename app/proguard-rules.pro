# Stage 1: shrink only, no renaming (near-zero JNI risk). Obfuscation deferred to Stage 2.
-dontobfuscate

# ---- Native (name-based JNI) boundary: all four *_jni.cpp bind via exported
# Java_<fqcn>_<method> symbols (no RegisterNatives) and resolve classes/methods/ctors
# by literal string from C++. Keep these un-renamed and un-removed. ----
-keep class com.spectrafilm.engine.** { *; }
-keep class com.spectrafilm.libraw.RawDecoder { *; }
-keep class com.spectrafilm.libraw.RawDecoder$NativeResult { *; }
-keep class com.spectrafilm.libraw.RawDecodeException { *; }
-keep class com.spectrafilm.tiffwriter.TiffWriter { *; }
-keep class com.spectrafilm.pngwriter.PngWriter { *; }
-keepclasseswithmembernames class * { native <methods>; }

# ---- kotlin.Triple / kotlin.Pair are ALSO part of that JNI boundary ----
# spektra_jni.cpp reads 19 engine params out of Triple<Float,Float,Float> and
# Pair<Float,Float> by resolving getFirst/getSecond/getThird from C++ by literal
# string. No BYTECODE calls those getters, so R8 sees them as unreachable and
# REMOVES them -- and `-dontobfuscate` does not help, because it prevents
# renaming, not removal.
#
# The result shipped: every Triple/Pair param (grain particle scale, density min,
# uniformity, halation scatter/strength, the four coupler gammas, camera UV/IR,
# scanner unsharp, crop centre and size) marshalled as 0.0 in every release APK,
# with no crash and no log. Zero grain particle scale is degenerate, which is why
# the slide route rendered a flat constant.
#
# spektra_jni.cpp now falls back to the backing FIELDS (which R8 keeps) and no
# longer overwrites defaults on failure, so this rule is belt-and-braces rather
# than the only thing standing between us and wrong numbers. Keep both.
# tools/r8_check/check_release_dex.sh gates it.
-keep class kotlin.Triple { *; }
-keep class kotlin.Pair { *; }

# ---- Enum name<->value persistence (prefs/presets/Serializable saver) ----
-keepclassmembers enum * {
    public static **[] values();
    public static ** valueOf(java.lang.String);
    <fields>;
}
