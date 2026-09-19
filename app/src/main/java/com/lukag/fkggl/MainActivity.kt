package com.lukag.fkggl

import android.net.Uri
import android.os.Bundle
import android.os.SystemClock
import android.provider.OpenableColumns
import android.system.Os
import android.system.OsConstants
import android.view.View
import android.widget.EditText
import android.widget.TableRow
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.lifecycleScope
import com.lukag.fkggl.databinding.ActivityMainBinding
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.IOException

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding

    private data class Part(val index: Int, val start: Long, val size: Long)

    private var encodeUri: Uri? = null
    private var encodeSize = -1L
    private var partSize = DEFAULT_PART_SIZE
    private var outputTree: Uri? = null
    private val encodeParts = mutableListOf<Part>()

    private var decodeUris: List<Uri> = emptyList()
    private var outFileUri: Uri? = null
    private var running = false

    private val pickFile =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
            if (uri != null) onFilePicked(uri)
        }
    private val pickParts =
        registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
            if (!uris.isNullOrEmpty()) onPartsPicked(uris)
        }
    private val pickTree =
        registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
            if (uri != null) {
                outputTree = uri
                contentResolver.takePersistableUriPermission(
                    uri,
                    android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION or
                        android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                )
                binding.btnEncode.isEnabled = encodeUri != null
                showFolder(uri)
            }
        }
    private val pickOutFile =
        registerForActivityResult(
            ActivityResultContracts.CreateDocument("application/octet-stream")
        ) { uri ->
            if (uri != null) {
                outFileUri = uri
                binding.txtOutFile.text = uri.lastPathSegment ?: uri.toString()
                binding.btnDecode.isEnabled = decodeUris.isNotEmpty()
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.btnPickFile.setOnClickListener { pickFile.launch(arrayOf("*/*")) }
        binding.btnPartSize.setOnClickListener { askPartSize() }
        binding.btnPickFolder.setOnClickListener { pickTree.launch(null) }
        binding.btnEncode.setOnClickListener { startEncode() }
        binding.btnPickParts.setOnClickListener { pickParts.launch(arrayOf("image/png")) }
        binding.btnPickOutFile.setOnClickListener { pickOutFile.launch("restored.bin") }
        binding.btnDecode.setOnClickListener { startDecode() }

        binding.txtPartSize.text = getString(R.string.part_size_value, partSize / MB)
        binding.btnEncode.isEnabled = false
        binding.btnDecode.isEnabled = false
        binding.progress.visibility = View.GONE
        binding.txtStatus.text = getString(R.string.max_part, PhotoNative.maxBytes() / MB)
    }

    private fun askPartSize() {
        val input = EditText(this)
        input.setText((partSize / MB).toString())
        input.inputType = android.text.InputType.TYPE_CLASS_NUMBER
        AlertDialog.Builder(this)
            .setTitle(R.string.part_size_title)
            .setView(input)
            .setPositiveButton(R.string.ok) { _, _ ->
                val mb = input.text.toString().toLongOrNull() ?: return@setPositiveButton
                if (mb in 1..(PhotoNative.maxBytes() / MB)) {
                    partSize = mb * MB
                    binding.txtPartSize.text = getString(R.string.part_size_value, mb)
                    if (encodeUri != null) planParts()
                } else {
                    Toast.makeText(
                        this,
                        getString(R.string.err_bad_part_size, PhotoNative.maxBytes() / MB),
                        Toast.LENGTH_LONG
                    ).show()
                }
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun onFilePicked(uri: Uri) {
        encodeUri = uri
        binding.txtEncodeFile.text = queryName(uri) ?: uri.lastPathSegment ?: uri.toString()
        planParts()
        binding.btnEncode.isEnabled = outputTree != null
    }

    private fun planParts() {
        val size = querySize(encodeUri ?: return)
        if (size < 0) {
            Toast.makeText(this, R.string.err_size_unknown, Toast.LENGTH_LONG).show()
            return
        }
        encodeSize = size
        // a zero-byte file still becomes one (empty) part so it round-trips
        val n = if (encodeSize == 0L) 1
        else ((encodeSize + partSize - 1) / partSize).toInt()
        encodeParts.clear()
        var off = 0L
        for (i in 0 until n) {
            val s = minOf(partSize, encodeSize - off)
            encodeParts.add(Part(i, off, s))
            off += s
        }
        buildEncodeTable()
    }

    private fun buildEncodeTable() {
        binding.tblParts.removeAllViews()
        binding.tableHead.visibility =
            if (encodeParts.isEmpty()) View.GONE else View.VISIBLE
        for (p in encodeParts) {
            val row = layoutInflater.inflate(
                R.layout.table_row, binding.tblParts, false
            ) as TableRow
            row.findViewById<TextView>(R.id.col_part).text =
                getString(R.string.part_n, p.index + 1)
            row.findViewById<TextView>(R.id.col_range).text =
                getString(R.string.range, p.start / MB, (p.start + p.size) / MB)
            row.findViewById<TextView>(R.id.col_status).text = getString(R.string.pending)
            binding.tblParts.addView(row)
        }
    }

    private fun showFolder(uri: Uri) {
        val doc = DocumentFile.fromTreeUri(this, uri)
        binding.txtFolder.text = doc?.name ?: uri.lastPathSegment ?: uri.toString()
    }

    private fun startEncode() {
        val uri = encodeUri ?: return
        val tree = outputTree ?: return
        if (running) return
        val base = (queryName(uri) ?: "file").substringBeforeLast('.')
        val dir = DocumentFile.fromTreeUri(this, tree) ?: return

        running = true
        setBusy(true)
        val t0 = SystemClock.elapsedRealtime()
        lifecycleScope.launch {
            val shas = mutableListOf<String>()
            var failure: String? = null
            try {
                withContext(Dispatchers.IO) {
                    encodeParts.forEachIndexed { row, part ->
                        val name = "$base.part%03d.png".format(part.index)
                        // SAF creates unique names instead of overwriting:
                        // drop leftovers from an earlier run first.
                        dir.findFile(name)?.delete()
                        val doc = dir.createFile("image/png", name)
                            ?: throw IOException("cannot create $name")
                        val inPfd = contentResolver.openFileDescriptor(uri, "r")
                            ?: throw IOException("cannot open input")
                        val outPfd = contentResolver.openFileDescriptor(doc.uri, "w")
                            ?: throw IOException("cannot open output")
                        try {
                            Os.lseek(inPfd.fileDescriptor, part.start, OsConstants.SEEK_SET)
                            val sha = PhotoNative.encodePart(inPfd.fd, part.size, outPfd.fd)
                            shas.add(sha)
                            markRow(row, getString(R.string.done_sha, sha.take(12)))
                        } finally {
                            inPfd.close()
                            outPfd.close()
                        }
                    }
                }
            } catch (e: Exception) {
                failure = e.message ?: e.javaClass.simpleName
            }
            val secs = (SystemClock.elapsedRealtime() - t0) / 1000
            setBusy(false)
            running = false
            if (failure == null) {
                showResultDialog(
                    getString(R.string.encode_done),
                    getString(R.string.encode_summary, encodeParts.size, secs) +
                        "\n\n" + shas.joinToString("\n") { "• ${it.take(16)}…" } +
                        "\n\n" + getString(R.string.keep_shas_hint)
                )
            } else {
                Toast.makeText(
                    this@MainActivity,
                    getString(R.string.encode_failed, failure),
                    Toast.LENGTH_LONG
                ).show()
            }
        }
    }

    private fun onPartsPicked(uris: List<Uri>) {
        // sort by display name so part_000 < part_001 regardless of selection order
        val sorted = uris.sortedBy { queryName(it) ?: it.lastPathSegment ?: "" }
        decodeUris = sorted
        binding.txtDecodeCount.text = getString(R.string.parts_selected, sorted.size)
        binding.btnDecode.isEnabled = outFileUri != null
    }

    private fun startDecode() {
        if (running || decodeUris.isEmpty() || outFileUri == null) return
        running = true
        setBusy(true)
        val t0 = SystemClock.elapsedRealtime()
        lifecycleScope.launch {
            val shas = mutableListOf<String>()
            var failure: String? = null
            try {
                withContext(Dispatchers.IO) {
                    val outPfd = contentResolver.openFileDescriptor(outFileUri!!, "w")
                        ?: throw IOException("cannot open output")
                    try {
                        decodeUris.forEachIndexed { i, uri ->
                            val inPfd = contentResolver.openFileDescriptor(uri, "r")
                                ?: throw IOException("cannot open part ${i + 1}")
                            try {
                                val (sha, size) = PhotoNative.decodePart(inPfd.fd, outPfd.fd)
                                shas.add(sha)
                                val n = i + 1
                                runOnUiThread {
                                    binding.txtStatus.text =
                                        getString(R.string.restored_part, n, decodeUris.size)
                                }
                            } finally {
                                inPfd.close()
                            }
                        }
                    } finally {
                        outPfd.close()
                    }
                }
            } catch (e: Exception) {
                failure = e.message ?: e.javaClass.simpleName
            }
            val secs = (SystemClock.elapsedRealtime() - t0) / 1000
            setBusy(false)
            running = false
            if (failure == null) {
                showResultDialog(
                    getString(R.string.decode_done),
                    getString(R.string.decode_summary, decodeUris.size, secs) +
                        "\n\n" + shas.joinToString("\n") { "• ${it.take(16)}…" }
                )
            } else {
                Toast.makeText(
                    this@MainActivity,
                    getString(R.string.decode_failed, failure),
                    Toast.LENGTH_LONG
                ).show()
            }
        }
    }

    private fun markRow(index: Int, text: String) {
        runOnUiThread {
            val row = binding.tblParts.getChildAt(index) as? TableRow ?: return@runOnUiThread
            row.findViewById<TextView>(R.id.col_status).text = text
        }
    }

    private fun setBusy(busy: Boolean) {
        binding.progress.visibility = if (busy) View.VISIBLE else View.GONE
        binding.btnEncode.isEnabled = !busy && encodeUri != null && outputTree != null
        binding.btnDecode.isEnabled = !busy && decodeUris.isNotEmpty() && outFileUri != null
    }

    private fun showResultDialog(title: String, message: String) {
        AlertDialog.Builder(this)
            .setTitle(title)
            .setMessage(message)
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    private fun queryName(uri: Uri): String? {
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
            ?.use { c -> if (c.moveToFirst()) return c.getString(0) }
        return null
    }

    private fun querySize(uri: Uri): Long {
        contentResolver.query(uri, arrayOf(OpenableColumns.SIZE), null, null, null)
            ?.use { c -> if (c.moveToFirst()) return c.getLong(0) }
        return try {
            contentResolver.openInputStream(uri)?.use { it.available().toLong() } ?: -1L
        } catch (e: Exception) {
            -1L
        }
    }

    companion object {
        const val MB = 1024L * 1024L
        const val DEFAULT_PART_SIZE = 500L * MB
    }
}
