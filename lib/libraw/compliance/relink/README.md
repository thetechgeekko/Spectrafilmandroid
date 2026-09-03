# Recipient LibRaw relink project

This standalone Android-NDK CMake project rebuilds `libsfraw.so` from an
explicit, recipient-controlled `LIBRAW_SOURCE_DIR`. It intentionally does not
use the production resolver or patched-tree hash: a recipient may change the
bundled LibRaw source and relink the JNI wrapper while preserving its Java ABI.

The output keeps the `libsfraw.so` filename/SONAME, zlib support, `NO_JPEG`
fallback policy, security allocation ceilings, and 16 KiB ELF segment
alignment. It also exports `sfraw_recipient_relink_marker()` and contains a
plain-text `UNOFFICIAL RECIPIENT RELINK` marker so it cannot be confused with an
official release binary.

See the compliance bundle's top-level `RELINKING.md` for commands. Use your own
APK signing key when installing a repackaged build; no Spektrafilm production
signing material is required or distributed.
