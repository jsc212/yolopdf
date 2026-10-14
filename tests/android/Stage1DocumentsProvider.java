// SPDX-License-Identifier: AGPL-3.0-only
package org.yolopdf;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import java.io.File;
import java.io.FileNotFoundException;

// Test builds only; not exported. Exercises ContentResolver's document API.
public final class Stage1DocumentsProvider extends DocumentsProvider {
    @Override public boolean onCreate() { return true; }
    private File document(String id) throws FileNotFoundException {
        if (!"stage1.pdf".equals(id)) throw new FileNotFoundException("Unknown test document");
        return new File(getContext().getFilesDir(), id);
    }
    @Override public Cursor queryRoots(String[] projection) {
        String[] columns = projection != null ? projection : new String[] {
            Root.COLUMN_ROOT_ID, Root.COLUMN_DOCUMENT_ID, Root.COLUMN_TITLE, Root.COLUMN_FLAGS };
        MatrixCursor cursor = new MatrixCursor(columns);
        cursor.newRow().add(Root.COLUMN_ROOT_ID, "stage1")
            .add(Root.COLUMN_DOCUMENT_ID, "stage1.pdf")
            .add(Root.COLUMN_TITLE, "Stage 1 test document")
            .add(Root.COLUMN_FLAGS, 0);
        return cursor;
    }
    @Override public Cursor queryDocument(String id, String[] projection) throws FileNotFoundException {
        File file = document(id);
        String[] columns = projection != null ? projection : new String[] {
            Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME,
            Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE, Document.COLUMN_FLAGS };
        MatrixCursor cursor = new MatrixCursor(columns);
        cursor.newRow().add(Document.COLUMN_DOCUMENT_ID, id)
            .add(Document.COLUMN_DISPLAY_NAME, id)
            .add(Document.COLUMN_MIME_TYPE, "application/pdf")
            .add(Document.COLUMN_SIZE, file.length())
            .add(Document.COLUMN_FLAGS, Document.FLAG_SUPPORTS_WRITE);
        return cursor;
    }
    @Override public Cursor queryChildDocuments(String id, String[] projection, String sortOrder) {
        return new MatrixCursor(projection != null ? projection : new String[] { Document.COLUMN_DOCUMENT_ID });
    }
    @Override public ParcelFileDescriptor openDocument(String id, String mode,
            CancellationSignal signal) throws FileNotFoundException {
        return ParcelFileDescriptor.open(document(id), ParcelFileDescriptor.parseMode(mode));
    }
}
