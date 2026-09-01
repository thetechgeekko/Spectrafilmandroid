# Release instrumentation proof: retain this otherwise disposable class while
# allowing R8 to rename it. The runner rejects a claimed minified target if the
# runtime binary name is unchanged.
-keep,allowobfuscation class com.spectrafilm.aimage.AImageR8RenameCanary { *; }

# Kotlin/JVM 17 data-class string concatenation is rewritten by Android's R8
# pipeline; the Java SE bootstrap class is not part of the Android library jar.
-dontwarn java.lang.invoke.StringConcatFactory
