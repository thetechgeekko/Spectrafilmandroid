# Stage 1: shrink only, no renaming (near-zero JNI risk). Obfuscation deferred to Stage 2.
-dontobfuscate

# ---- Native (name-based JNI) boundary: all four *_jni.cpp bind via exported
# Java_<fqcn>_<method> symbols (no RegisterNatives) and resolve classes/methods/ctors
# by literal string from C++. Keep these un-renamed and un-removed. ----
-keep class com.spectrafilm.engine.** { *; }
-keep class com.spectrafilm.libraw.RawDecoder { *; }
-keep class com.spectrafilm.libraw.RawDecoder$NativeResult { *; }
-keep class com.spectrafilm.libraw.RawDecodeException { *; }
# Ticket #158's separately packaged release probe hashes the production
# malloc-backed result through this public lease ABI. R8 cannot see that
# cross-APK call edge, so retain the exact method/class names it invokes.
-keep class com.spectrafilm.libraw.LinearResult { *; }
-keep class com.spectrafilm.libraw.LinearResult$DataLease { *; }
-keep class com.spectrafilm.tiffwriter.TiffWriter { *; }
-keep class com.spectrafilm.pngwriter.PngWriter { *; }
-keep class com.spectrafilm.tiffwriter.TiffCancellationToken { *; }
-keep class com.spectrafilm.pngwriter.PngCancellationToken { *; }
-keepclasseswithmembernames class * { native <methods>; }

# ---- Release-candidate instrumentation boundary ----
# The separately packaged release AndroidTest APK exercises process-death recovery,
# failure cleanup, durable URI grants, and duplicate-name MediaStore publication
# against the exact minified app. These package-private Kotlin types are referenced
# across the APK boundary, so R8 must not merge/remove them from the target APK.
-keep class com.spectrafilm.app.EncodedArtifact { *; }
-keep class com.spectrafilm.app.EncodedArtifact$Companion { *; }
-keep class com.spectrafilm.app.ExportDestinationSpec { *; }
-keep class com.spectrafilm.app.ExportTransaction { *; }
-keep class com.spectrafilm.app.ExportRecoveryReport { *; }
-keep class com.spectrafilm.app.AndroidPendingExportBackend { *; }
-keep class com.spectrafilm.app.SharedPreferencesPendingExportJournal { *; }
-keep class com.spectrafilm.app.StoredExportState { *; }
-keep interface com.spectrafilm.app.PendingExportBackend { *; }
-keep interface com.spectrafilm.app.PendingExportJournal { *; }
-keep class com.spectrafilm.app.ExportTransactionKt { *; }
-keep class com.spectrafilm.app.PersistedSourceRef { *; }
-keep class com.spectrafilm.app.SourceAccessMode { *; }
-keep class com.spectrafilm.app.SourceAccessCoordinator { *; }
-keep class com.spectrafilm.app.SourceRestoreResult { *; }
-keep class com.spectrafilm.app.SourceRestoreResult$* { *; }
-keep class com.spectrafilm.app.SharedPreferencesSourceRefStore { *; }
-keep interface com.spectrafilm.app.SourceRefStore { *; }
-keep interface com.spectrafilm.app.UriGrantBackend { *; }
-keep class com.spectrafilm.app.AndroidUriGrantBackend { *; }
-keep class com.spectrafilm.app.Recipes { *; }

# Process-owned native/export probes are also compiled into the separate test
# APK. Keep their exact target-APK ABI: otherwise R8 may inline/staticize object
# methods, remove data-class accessors, or delete Kotlin interface DefaultImpls
# even though the instrumentation still invokes their original JVM descriptors.
-keep class com.spectrafilm.app.EngineHolder { *; }
-keep class com.spectrafilm.app.ExportWorkRuntime { *; }
-keep class com.spectrafilm.app.ExportRuntimeState { *; }
-keep class com.spectrafilm.app.ExportRuntimeState$* { *; }
-keep class com.spectrafilm.app.ExportTerminalOutcome { *; }
-keep class com.spectrafilm.app.ExportTerminalOutcome$* { *; }
-keep class com.spectrafilm.app.ExportPhaseSnapshot { *; }
-keep class com.spectrafilm.app.ExportFormat { *; }
-keep class com.spectrafilm.app.PendingExportBackend$DefaultImpls { *; }

# Ticket #139 deliberately exposes one narrow, fixed-fixture bridge instead of making the entire
# editor-session/store/source implementation a cross-APK ABI. The release test APK calls only these
# public methods; their bodies keep the app-internal implementation edges visible to target R8.
-keep class com.spectrafilm.app.Ticket139EditorTestBridge { public *; }

# Ticket #181's accessibility scan (same release test APK) walks every editor category by its
# tab label, so the enum's values()/getLabelRes() are cross-APK ABI. String ids are resolved by
# name (getIdentifier) on purpose: AGP 8 keeps R ids non-final and R8 drops R$string entirely.
-keep class com.spectrafilm.app.Category { *; }

# The #139 Activity-recreation probe binds its export's source identity from the release test APK
# (StorageReliabilityChecks), so the authority and the value types that cross with it are cross-APK
# ABI exactly like ExportWorkRuntime above. Without these, R8 class-inlines the singleton (its only
# in-app callers are direct) and the test APK dies with NoSuchFieldError: INSTANCE at runtime.
-keep class com.spectrafilm.app.ExportSourceIdentityAuthority { *; }
-keep class com.spectrafilm.app.ExportSourceIdentity { *; }
-keep class com.spectrafilm.app.ExportSourceIdentity$* { *; }
-keep class com.spectrafilm.app.SourceKind { *; }

# Ticket #174's separate release test APK calls the production descriptor/color contract and the
# production float-to-tagged-Bitmap bridge, then parses the encoded files on a physical device.
-keep class com.spectrafilm.app.OutputDescriptor { *; }
-keep class com.spectrafilm.app.OutputDescriptor$Companion { *; }
-keep class com.spectrafilm.app.OutputMetadataPolicy { *; }
-keep class com.spectrafilm.app.OutputBitDepth { *; }
-keep class com.spectrafilm.app.OutputEncoder { *; }
-keep class com.spectrafilm.app.OutputReleaseStatus { *; }
-keep class com.spectrafilm.app.ExportOptions { *; }
-keep class com.spectrafilm.app.ExportSize { *; }
-keep class com.spectrafilm.app.ColorManagement { *; }
-keepclassmembers class com.spectrafilm.app.EngineHelpersKt {
    public static android.graphics.Bitmap simResultToBitmap(...);
}

# Ticket #177's benchmark phase (same release test APK) measures the SHIPPING export path:
# it calls the production decode, params, engine, grade and encoder entry points on the
# minified app rather than re-implementing them, so those are cross-APK ABI too.
-keep class com.spectrafilm.app.ImagePipelineKt { *; }
-keep class com.spectrafilm.app.ParamsState { *; }
-keep class com.spectrafilm.app.BuiltInPresets { *; }
-keep class com.spectrafilm.app.BuiltInPreset { *; }
-keep class com.spectrafilm.app.ColorGrade { *; }
-keep class com.spectrafilm.app.ExportClock { *; }
# #179/#140: the benchmark drives the shipping export cache and the render-derived gain map
# across the APK boundary. The trailing ** matters -- ExportCacheKey.Source/Grade are nested
# types and compile to separate classes that a bare keep would not cover.
-keep class com.spectrafilm.app.ExportCache** { *; }
-keep class com.spectrafilm.app.HdrGainMap** { *; }
-keep class com.spectrafilm.app.masks.MaskCompositor { *; }
-keepclassmembers class com.spectrafilm.app.EngineHelpersKt {
    public static android.graphics.Bitmap simResultToBitmapGraded(...);
}

# Kotlin inline functions in the separately packaged release AndroidTest APK
# still emit calls to Result's JVM implementation ABI. R8 cannot see those call
# edges while shrinking the target APK, so it may remove Result.Companion or an
# *-impl method and leave a device-only NoSuchFieldError/NoSuchMethodError.
# Keep this small stdlib ABI surface intact and verify the physical dex below.
-keep class kotlin.Result { *; }
-keep class kotlin.Result$Companion { *; }
-keep class kotlin.Result$Failure { *; }
-keep class kotlin.ResultKt { *; }

# The release AndroidTest APK intentionally does not package a second Kotlin or
# coroutines runtime. Its lowered suspend/use/loop code resolves these bounded
# facades and interfaces from the target APK. Preserve only the families/types
# proven by the final test dex; do not replace this with kotlin.**/kotlinx.**.
# The locked stdlib's Collections/Intrinsics/Ranges facades are empty subclasses;
# the locked coroutines Builders/Job/Flow facades are forwarding classes. Keep
# each public owner plus its exact declaring/delegate part without retaining the
# unrelated members of either runtime family.
-keep class kotlin.KotlinNothingValueException { *; }
-keep class kotlin.collections.CollectionsKt
-keep class kotlin.collections.CollectionsKt__IterablesKt {
    public static int collectionSizeOrDefault(java.lang.Iterable, int);
}
-keep class kotlin.collections.CollectionsKt__CollectionsKt {
    public static void throwCountOverflow();
}
-keep class kotlin.collections.IntIterator { *; }
-keep class kotlin.coroutines.intrinsics.IntrinsicsKt
-keep class kotlin.coroutines.intrinsics.IntrinsicsKt__IntrinsicsKt {
    public static java.lang.Object getCOROUTINE_SUSPENDED();
}
-keep class kotlin.coroutines.jvm.internal.Boxing { *; }
-keep class kotlin.jdk7.AutoCloseableKt { *; }
-keep class kotlin.io.CloseableKt {
    public static void closeFinally(java.io.Closeable, java.lang.Throwable);
}
# A `var` captured by a lambda in the release test APK (Ticket141MaskMemoryChecks) boxes
# through Ref$ObjectRef, which the app APK otherwise never instantiates -- a device-only
# NoClassDefFoundError that no build step can see.
-keep class kotlin.jvm.internal.Ref$ObjectRef { *; }
-keep class kotlin.ranges.RangesKt
-keep class kotlin.ranges.RangesKt___RangesKt {
    public static kotlin.ranges.IntRange until(int, int);
}
-keep class kotlin.ranges.IntRange { *; }
-keep class kotlin.ranges.IntProgression {
    public java.util.Iterator iterator();
}
-keep class kotlin.ranges.IntProgressionIterator {
    public boolean hasNext();
    public int nextInt();
}
-keep class kotlinx.coroutines.AwaitKt { *; }
-keep class kotlinx.coroutines.BuildersKt {
    public static java.lang.Object runBlocking$default(kotlin.coroutines.CoroutineContext, kotlin.jvm.functions.Function2, int, java.lang.Object);
    public static kotlinx.coroutines.Job launch$default(kotlinx.coroutines.CoroutineScope, kotlin.coroutines.CoroutineContext, kotlinx.coroutines.CoroutineStart, kotlin.jvm.functions.Function2, int, java.lang.Object);
    public static kotlinx.coroutines.Deferred async$default(kotlinx.coroutines.CoroutineScope, kotlin.coroutines.CoroutineContext, kotlinx.coroutines.CoroutineStart, kotlin.jvm.functions.Function2, int, java.lang.Object);
}
-keep class kotlinx.coroutines.BuildersKt__BuildersKt {
    public static java.lang.Object runBlocking$default(kotlin.coroutines.CoroutineContext, kotlin.jvm.functions.Function2, int, java.lang.Object);
}
-keep class kotlinx.coroutines.BuildersKt__Builders_commonKt {
    public static kotlinx.coroutines.Job launch$default(kotlinx.coroutines.CoroutineScope, kotlin.coroutines.CoroutineContext, kotlinx.coroutines.CoroutineStart, kotlin.jvm.functions.Function2, int, java.lang.Object);
    public static kotlinx.coroutines.Deferred async$default(kotlinx.coroutines.CoroutineScope, kotlin.coroutines.CoroutineContext, kotlinx.coroutines.CoroutineStart, kotlin.jvm.functions.Function2, int, java.lang.Object);
}
-keep interface kotlinx.coroutines.CompletableDeferred { *; }
-keep class kotlinx.coroutines.CompletableDeferredKt { *; }
-keep interface kotlinx.coroutines.CoroutineScope { *; }
-keep class kotlinx.coroutines.CoroutineScopeKt { *; }
-keep interface kotlinx.coroutines.Deferred { *; }
-keep class kotlinx.coroutines.DelayKt { *; }
-keep class kotlinx.coroutines.Dispatchers { *; }
-keep interface kotlinx.coroutines.Job { *; }
-keep class kotlinx.coroutines.JobKt {
    public static java.lang.Object cancelAndJoin(kotlinx.coroutines.Job, kotlin.coroutines.Continuation);
}
-keep class kotlinx.coroutines.JobKt__JobKt {
    public static java.lang.Object cancelAndJoin(kotlinx.coroutines.Job, kotlin.coroutines.Continuation);
}
-keep class kotlinx.coroutines.TimeoutKt { *; }
-keep class kotlinx.coroutines.CoroutineDispatcher { *; }
-keep class kotlinx.coroutines.CoroutineStart { *; }
-keep class kotlinx.coroutines.flow.FlowKt {
    public static java.lang.Object first(kotlinx.coroutines.flow.Flow, kotlin.coroutines.Continuation);
}
-keep class kotlinx.coroutines.flow.FlowKt__ReduceKt {
    public static java.lang.Object first(kotlinx.coroutines.flow.Flow, kotlin.coroutines.Continuation);
}
-keep interface kotlinx.coroutines.flow.Flow { *; }
-keep interface kotlinx.coroutines.flow.FlowCollector { *; }
-keep interface kotlinx.coroutines.flow.StateFlow { *; }

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
