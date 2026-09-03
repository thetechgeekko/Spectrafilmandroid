# JNI_OnLoad registers these exact names. Keep only the narrow native bridge.
-keep class com.spectrafilm.aimage.AImageDecoderNative {
    native <methods>;
    private static java.lang.Class fallbackExceptionClass();
    public static java.lang.String r8CanaryRuntimeClassName();
}

# Release androidTest is a separately compiled consumer of the minified AAR.
# Keep the public qualification contract binary-stable while still allowing R8
# to optimize method bodies. The disposable canary below remains renameable and
# proves that this is the minified target rather than a debug false positive.
-keep,allowoptimization class com.spectrafilm.aimage.AImageInputKind { *; }
-keep,allowoptimization class com.spectrafilm.aimage.AImagePixelFormat { *; }
-keep,allowoptimization class com.spectrafilm.aimage.AImageDataSpace { *; }
-keep,allowoptimization class com.spectrafilm.aimage.AImageHeader { *; }
-keep,allowoptimization class com.spectrafilm.aimage.AImageDecodePlan { *; }
-keep,allowoptimization class com.spectrafilm.aimage.DecodeContractKt { *; }
-keep,allowoptimization interface com.spectrafilm.aimage.AImageEncodedSource { *; }
-keep,allowoptimization class com.spectrafilm.aimage.AImageEncodedSource$* { *; }
-keep,allowoptimization class com.spectrafilm.aimage.AImageDecodeCancellation { *; }
-keep,allowoptimization class com.spectrafilm.aimage.AImageDecodeMetadata { *; }
-keep,allowoptimization class com.spectrafilm.aimage.AImagePixelResult { *; }
-keep,allowoptimization class com.spectrafilm.aimage.AImageDecoderExperiment { *; }

# JNI_OnLoad asks the kept bridge for this actual Class object, so renaming would
# be safe, but the public typed-fallback binary contract is intentionally kept
# stable for minified consumers and the String constructor used by ThrowNew.
-keep class com.spectrafilm.aimage.AImageDecoderFallbackException {
    *;
}
