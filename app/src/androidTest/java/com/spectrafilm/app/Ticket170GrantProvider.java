/*
 * Spektrafilm for Android — test-APK-only persistable URI grant provider. GPLv3.
 *
 * This component deliberately uses Java only. It runs in the standalone test
 * package process, whose class path must not assume Kotlin classes retained by
 * the R8-minified target APK.
 */
package com.spectrafilm.app;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Binder;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.concurrent.atomic.AtomicBoolean;

/** Test-only provider that exercises a real persistable Android URI grant. */
public final class Ticket170GrantProvider extends ContentProvider {
    public static final String AUTHORITY = "com.spectrafilm.app.test.ticket170.grants";
    public static final String METHOD_GRANT = "grant_persistable_read";
    public static final String METHOD_REVOKE = "revoke_read";
    public static final String KEY_SUCCESS = "success";
    public static final String TEST_URI = "content://" + AUTHORITY + "/source/ticket170";
    public static final Uri testUri = Uri.parse(TEST_URI);

    private static final String TARGET_PACKAGE = "com.spectrafilm.app";
    private static final AtomicBoolean READ_ENABLED = new AtomicBoolean(false);
    private static final byte[] TEST_PAYLOAD = new byte[] {0x54, 0x31, 0x37, 0x30};
    private static final int GRANT_FLAGS = Intent.FLAG_GRANT_READ_URI_PERMISSION
            | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION;

    @Override
    public boolean onCreate() {
        Context owner = getContext();
        if (owner == null) return false;
        File payload = payloadFile(owner);
        if (!payload.exists()) {
            try (FileOutputStream output = new FileOutputStream(payload)) {
                output.write(TEST_PAYLOAD);
                output.flush();
                output.getFD().sync();
            } catch (IOException failure) {
                throw new IllegalStateException("could not create grant probe payload", failure);
            }
        }
        return true;
    }

    @Override
    public Bundle call(String method, String arg, Bundle extras) {
        if (!TARGET_PACKAGE.equals(arg)) {
            throw new IllegalArgumentException("only the instrumentation target may be granted");
        }
        String[] callerPackages = ownerContext().getPackageManager()
                .getPackagesForUid(Binder.getCallingUid());
        boolean targetCaller = false;
        if (callerPackages != null) {
            for (String callerPackage : callerPackages) {
                if (TARGET_PACKAGE.equals(callerPackage)) {
                    targetCaller = true;
                    break;
                }
            }
        }
        if (!targetCaller) {
            throw new SecurityException("provider control call came from another package");
        }

        long identity = Binder.clearCallingIdentity();
        try {
            if (METHOD_GRANT.equals(method)) {
                READ_ENABLED.set(true);
                ownerContext().grantUriPermission(TARGET_PACKAGE, testUri, GRANT_FLAGS);
            } else if (METHOD_REVOKE.equals(method)) {
                READ_ENABLED.set(false);
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    ownerContext().revokeUriPermission(
                            TARGET_PACKAGE,
                            testUri,
                            Intent.FLAG_GRANT_READ_URI_PERMISSION
                    );
                } else {
                    ownerContext().revokeUriPermission(
                            testUri,
                            Intent.FLAG_GRANT_READ_URI_PERMISSION
                    );
                }
            } else {
                throw new IllegalArgumentException("unsupported provider method: " + method);
            }
            Bundle result = new Bundle();
            result.putBoolean(KEY_SUCCESS, true);
            return result;
        } finally {
            Binder.restoreCallingIdentity(identity);
        }
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!testUri.equals(uri) || !"r".equals(mode) || !READ_ENABLED.get()) {
            throw new FileNotFoundException("ticket #170 test URI is not authorized");
        }
        return ParcelFileDescriptor.open(
                payloadFile(ownerContext()),
                ParcelFileDescriptor.MODE_READ_ONLY
        );
    }

    @Override
    public String getType(Uri uri) {
        return testUri.equals(uri) ? "application/octet-stream" : null;
    }

    @Override
    public Cursor query(
            Uri uri,
            String[] projection,
            String selection,
            String[] selectionArgs,
            String sortOrder
    ) {
        return null;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("read-only test provider");
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("read-only test provider");
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("read-only test provider");
    }

    private Context ownerContext() {
        Context owner = getContext();
        if (owner == null) throw new IllegalStateException("provider is detached");
        return owner;
    }

    private static File payloadFile(Context context) {
        return new File(context.getFilesDir(), "ticket170-grant-probe.bin");
    }
}
