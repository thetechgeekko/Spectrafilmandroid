/*
 * Spektrafilm for Android — test-APK-only source replacement provider. GPLv3.
 */
package com.spectrafilm.app;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.util.Base64;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.concurrent.atomic.AtomicBoolean;

/** Two real persistable photo URIs; render completion is gated inside the production editor. */
public final class Ticket139SourceProvider extends ContentProvider {
    public static final String AUTHORITY = "com.spectrafilm.app.test.ticket139.sources";
    public static final Uri SOURCE_A = Uri.parse("content://" + AUTHORITY + "/source/a.png");
    public static final Uri SOURCE_B = Uri.parse("content://" + AUTHORITY + "/source/b.png");
    public static final String METHOD_ARM = "arm_source_a";
    public static final String METHOD_REVOKE = "revoke_sources";
    public static final String KEY_SUCCESS = "success";

    private static final String TARGET_PACKAGE = "com.spectrafilm.app";
    private static final int GRANT_FLAGS = Intent.FLAG_GRANT_READ_URI_PERMISSION
            | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION;
    private static final byte[] PNG_1X1 = Base64.decode(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=",
            Base64.DEFAULT
    );
    private static final AtomicBoolean READ_ENABLED = new AtomicBoolean(false);

    @Override
    public boolean onCreate() {
        writePayload(payloadFile("a.png"));
        writePayload(payloadFile("b.png"));
        return true;
    }

    @Override
    public Bundle call(String method, String arg, Bundle extras) {
        requireTargetCaller(arg);
        boolean success;
        long identity = Binder.clearCallingIdentity();
        try {
            if (METHOD_ARM.equals(method)) {
                READ_ENABLED.set(true);
                owner().grantUriPermission(TARGET_PACKAGE, SOURCE_A, GRANT_FLAGS);
                owner().grantUriPermission(TARGET_PACKAGE, SOURCE_B, GRANT_FLAGS);
                success = true;
            } else if (METHOD_REVOKE.equals(method)) {
                READ_ENABLED.set(false);
                owner().revokeUriPermission(SOURCE_A, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                owner().revokeUriPermission(SOURCE_B, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                success = true;
            } else {
                throw new IllegalArgumentException("unsupported provider method: " + method);
            }
        } finally {
            Binder.restoreCallingIdentity(identity);
        }
        Bundle result = new Bundle();
        result.putBoolean(KEY_SUCCESS, success);
        return result;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!READ_ENABLED.get() || !"r".equals(mode) || (!SOURCE_A.equals(uri) && !SOURCE_B.equals(uri))) {
            throw new FileNotFoundException("ticket #139 source is not authorized");
        }
        return ParcelFileDescriptor.open(
                payloadFile(SOURCE_A.equals(uri) ? "a.png" : "b.png"),
                ParcelFileDescriptor.MODE_READ_ONLY
        );
    }

    @Override public String getType(Uri uri) { return "image/png"; }
    @Override public Cursor query(Uri uri, String[] projection, String selection,
                                  String[] selectionArgs, String sortOrder) { return null; }
    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri uri, String selection, String[] selectionArgs) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri uri, ContentValues values, String selection,
                                String[] selectionArgs) { throw new UnsupportedOperationException(); }

    private void requireTargetCaller(String arg) {
        if (!TARGET_PACKAGE.equals(arg)) throw new IllegalArgumentException("wrong target package");
        String[] packages = owner().getPackageManager().getPackagesForUid(Binder.getCallingUid());
        if (packages != null) {
            for (String candidate : packages) {
                if (TARGET_PACKAGE.equals(candidate)) return;
            }
        }
        throw new SecurityException("source provider control came from another UID");
    }

    private Context owner() {
        Context context = getContext();
        if (context == null) throw new IllegalStateException("provider is detached");
        return context;
    }

    private File payloadFile(String name) {
        return new File(owner().getFilesDir(), "ticket139-" + name);
    }

    private static void writePayload(File target) {
        if (target.exists()) return;
        try (FileOutputStream output = new FileOutputStream(target)) {
            output.write(PNG_1X1);
            output.flush();
            output.getFD().sync();
        } catch (IOException failure) {
            throw new IllegalStateException("could not create ticket #139 source", failure);
        }
    }
}
