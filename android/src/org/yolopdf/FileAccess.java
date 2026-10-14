// SPDX-License-Identifier: AGPL-3.0-only
package org.yolopdf;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;

public final class FileAccess {
    private FileAccess() {}

    public static void persistReadPermission(Context context, String value) {
        try {
            context.getContentResolver().takePersistableUriPermission(
                Uri.parse(value), Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) {
            // Some providers grant session-only access. The cached copy remains usable.
        }
    }

    public static String copyToUri(Context context, String value, String cached) {
        try (ParcelFileDescriptor descriptor = context.getContentResolver()
                    .openFileDescriptor(Uri.parse(value), "rwt")) {
            if (descriptor == null) return "Document provider returned no writable descriptor";
            try (FileInputStream input = new FileInputStream(cached);
                 FileOutputStream output = new ParcelFileDescriptor.AutoCloseOutputStream(descriptor)) {
                byte[] buffer = new byte[65536];
                for (int count; (count = input.read(buffer)) >= 0;) {
                    if (count != 0) output.write(buffer, 0, count);
                }
                output.flush();
                output.getFD().sync();
            }
            return "";
        } catch (IOException | SecurityException error) {
            return "PDF save failed: " + error.getMessage();
        }
    }
}
