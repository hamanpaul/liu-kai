package com.hamanpaul.liukai.doctor

import android.content.ContentResolver
import android.content.Context
import android.database.Cursor
import android.net.Uri
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import java.io.OutputStream

internal class DoctorSafExportTarget(
    context: Context,
    private val uri: Uri,
) : DoctorExportTarget {
    private val appContext = context.applicationContext
    private val resolver: ContentResolver = appContext.contentResolver

    override fun validate(): Boolean {
        if (!DocumentsContract.isDocumentUri(appContext, uri)) return false
        var cursor: Cursor? = null
        return try {
            cursor = resolver.query(
                uri,
                arrayOf(OpenableColumns.DISPLAY_NAME, DocumentsContract.Document.COLUMN_FLAGS),
                null,
                null,
                null,
            )
            cursor != null && cursor.moveToFirst()
        } catch (_: SecurityException) {
            false
        } finally {
            cursor?.close()
        }
    }

    override fun open(): OutputStream? = try {
        resolver.openOutputStream(uri, "wt")
    } catch (_: SecurityException) {
        null
    }

    override fun discard() = Unit
}
