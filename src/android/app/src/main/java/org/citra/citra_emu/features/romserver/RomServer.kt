// Copyright CitraVR Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

package org.citra.citra_emu.features.romserver

import android.content.Context
import android.database.Cursor
import android.net.Uri
import android.provider.DocumentsContract
import android.system.Os
import androidx.preference.PreferenceManager
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import org.citra.citra_emu.model.Game
import org.citra.citra_emu.utils.FileUtil
import org.citra.citra_emu.utils.GameHelper
import org.citra.citra_emu.utils.Log
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.Inet4Address
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.net.URLDecoder
import java.security.SecureRandom
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * A small HTTP server that lets a computer on the same network list, upload and
 * delete ROMs in the games folder chosen in CitraVR.
 *
 * Every URL starts with a random token (kept across restarts so bookmarks keep
 * working), so other devices on the network cannot use it without the address
 * shown in settings. Uploads are raw PUT bodies streamed straight into the
 * games folder, so files of any size work without temporary copies.
 */
object RomServer {
    // Not 8080: on Steam Frame the Steam client listens on 8080 on the host, so
    // Lepton could not forward that port from its container to the network.
    const val PORT = 8338

    private const val PREF_TOKEN = "rom_server_token"
    private const val MAX_HEADER_BYTES = 16 * 1024
    private const val COPY_BUFFER_BYTES = 1024 * 1024
    private const val PARTIAL_SUFFIX = ".part"

    // Same depth as GameHelper's scan of the games folder.
    private const val MAX_SCAN_DEPTH = 3

    sealed class State {
        object Stopped : State()
        data class Running(val urls: List<String>) : State()
        data class Failed(val message: String) : State()
    }

    private val _state = MutableStateFlow<State>(State.Stopped)
    val state: StateFlow<State> get() = _state.asStateFlow()

    // Incremented whenever the server changes the games folder, so the game
    // list can reload.
    private val _libraryChanges = MutableStateFlow(0)
    val libraryChanges: StateFlow<Int> get() = _libraryChanges.asStateFlow()

    private lateinit var appContext: Context
    private var serverSocket: ServerSocket? = null
    private var executor: ExecutorService? = null
    private var token = ""

    // Called when an upload starts and ends, so the service can keep Wi-Fi awake.
    var onTransferActive: ((Boolean) -> Unit)? = null

    @Synchronized
    fun start(context: Context) {
        if (serverSocket != null) {
            return
        }
        appContext = context.applicationContext
        token = getOrCreateToken(appContext)
        _state.value = State.Stopped
        try {
            val socket = ServerSocket()
            socket.reuseAddress = true
            socket.bind(InetSocketAddress(PORT))
            serverSocket = socket
            val pool = Executors.newCachedThreadPool()
            executor = pool
            pool.execute { acceptLoop(socket, pool) }
            _state.value = State.Running(getUrls())
            Log.info("[RomServer] Listening on port $PORT")
        } catch (e: IOException) {
            Log.error("[RomServer] Could not start: ${e.message}")
            stop()
            _state.value = State.Failed(e.message ?: e.toString())
        }
    }

    @Synchronized
    fun stop() {
        try {
            serverSocket?.close()
        } catch (_: IOException) {
        }
        serverSocket = null
        executor?.shutdownNow()
        executor = null
        // Keep a start failure visible in settings until the next start.
        if (_state.value !is State.Failed) {
            _state.value = State.Stopped
        }
    }

    /** Re-reads the network addresses, e.g. after a Wi-Fi change. */
    fun refreshUrls() {
        val current = _state.value
        if (current is State.Running) {
            _state.value = State.Running(getUrls())
        }
    }

    private fun getOrCreateToken(context: Context): String {
        val preferences = PreferenceManager.getDefaultSharedPreferences(context)
        preferences.getString(PREF_TOKEN, null)?.let { return it }
        val alphabet = "abcdefghijkmnpqrstuvwxyz23456789"
        val random = SecureRandom()
        val newToken = (1..8).map { alphabet[random.nextInt(alphabet.length)] }.joinToString("")
        preferences.edit().putString(PREF_TOKEN, newToken).apply()
        return newToken
    }

    private fun getUrls(): List<String> {
        val addresses = mutableListOf<Pair<String, String>>()
        try {
            NetworkInterface.getNetworkInterfaces()?.toList()?.forEach { networkInterface ->
                if (!networkInterface.isUp || networkInterface.isLoopback) {
                    return@forEach
                }
                networkInterface.inetAddresses.toList()
                    .filterIsInstance<Inet4Address>()
                    .filter { !it.isLoopbackAddress && !it.isLinkLocalAddress }
                    .forEach { addresses.add(networkInterface.name to it.hostAddress!!) }
            }
        } catch (e: SocketException) {
            Log.error("[RomServer] Could not list network interfaces: ${e.message}")
        }
        // Wi-Fi first.
        return addresses
            .sortedBy { (name, _) -> if (name.startsWith("wlan")) 0 else 1 }
            .map { (_, address) -> "http://$address:$PORT/$token/" }
            .distinct()
    }

    private fun acceptLoop(socket: ServerSocket, pool: ExecutorService) {
        while (!socket.isClosed) {
            try {
                val client = socket.accept()
                pool.execute { handleClient(client) }
            } catch (e: IOException) {
                if (!socket.isClosed) {
                    Log.error("[RomServer] accept failed: ${e.message}")
                }
            } catch (e: java.util.concurrent.RejectedExecutionException) {
                return
            }
        }
    }

    //==================================================================================
    // HTTP

    private class Request(
        val method: String,
        val path: String,
        val query: Map<String, String>,
        val headers: Map<String, String>,
        val body: InputStream
    )

    private class HttpError(val status: Int, message: String) : Exception(message)

    private fun handleClient(client: Socket) {
        client.use {
            try {
                client.soTimeout = 60_000
                val input = BufferedInputStream(client.getInputStream(), COPY_BUFFER_BYTES)
                val output = client.getOutputStream()
                val request = readRequest(input) ?: return
                try {
                    route(request, output)
                } catch (e: HttpError) {
                    sendJson(output, e.status, JSONObject().put("error", e.message))
                }
            } catch (e: Exception) {
                Log.warning("[RomServer] Request failed: ${e.message}")
            }
        }
    }

    private fun readRequest(input: InputStream): Request? {
        val headerBytes = ByteArrayOutputStream()
        var matched = 0
        val terminator = byteArrayOf('\r'.code.toByte(), '\n'.code.toByte(), '\r'.code.toByte(), '\n'.code.toByte())
        while (matched < terminator.size) {
            val b = input.read()
            if (b < 0) {
                return null
            }
            headerBytes.write(b)
            matched = if (b.toByte() == terminator[matched]) {
                matched + 1
            } else if (b.toByte() == terminator[0]) {
                1
            } else {
                0
            }
            if (headerBytes.size() > MAX_HEADER_BYTES) {
                return null
            }
        }
        val lines = headerBytes.toString(Charsets.ISO_8859_1.name()).split("\r\n")
        val requestLine = lines.first().split(" ")
        if (requestLine.size < 2) {
            return null
        }
        val headers = lines.drop(1)
            .filter { it.contains(':') }
            .associate {
                it.substringBefore(':').trim().lowercase() to it.substringAfter(':').trim()
            }
        val target = requestLine[1]
        val query = target.substringAfter('?', "")
            .split('&')
            .filter { it.isNotEmpty() }
            .associate {
                decode(it.substringBefore('=')) to decode(it.substringAfter('=', ""))
            }
        return Request(requestLine[0].uppercase(), decode(target.substringBefore('?')), query, headers, input)
    }

    private fun decode(value: String): String = URLDecoder.decode(value, "UTF-8")

    private fun route(request: Request, output: OutputStream) {
        val prefix = "/$token"
        if (request.path == prefix) {
            sendRedirect(output, "$prefix/")
            return
        }
        if (!request.path.startsWith("$prefix/")) {
            sendText(output, 404, "text/plain", "Not found")
            return
        }
        when (request.method to request.path.removePrefix(prefix)) {
            "GET" to "/" -> sendPage(output)
            "GET" to "/api/files" -> sendJson(output, 200, listFiles())
            "DELETE" to "/api/files" -> {
                deleteFile(request.query["path"] ?: throw HttpError(400, "Missing path"))
                sendJson(output, 200, JSONObject().put("ok", true))
            }
            "PUT" to "/api/upload" -> {
                upload(request, output)
                sendJson(output, 200, JSONObject().put("ok", true))
            }
            else -> sendText(output, 404, "text/plain", "Not found")
        }
    }

    private fun sendPage(output: OutputStream) {
        val page = appContext.assets.open("rom_server/index.html").use { it.readBytes() }
        sendBytes(output, 200, "text/html; charset=utf-8", page)
    }

    private fun sendRedirect(output: OutputStream, location: String) {
        output.write(
            ("HTTP/1.1 302 Found\r\nLocation: $location\r\nContent-Length: 0\r\n" +
                "Connection: close\r\n\r\n").toByteArray(Charsets.UTF_8)
        )
        output.flush()
    }

    private fun sendJson(output: OutputStream, status: Int, json: Any) {
        sendText(output, status, "application/json; charset=utf-8", json.toString())
    }

    private fun sendText(output: OutputStream, status: Int, contentType: String, text: String) {
        sendBytes(output, status, contentType, text.toByteArray(Charsets.UTF_8))
    }

    private fun sendBytes(output: OutputStream, status: Int, contentType: String, body: ByteArray) {
        val reason = when (status) {
            200 -> "OK"
            400 -> "Bad Request"
            404 -> "Not Found"
            409 -> "Conflict"
            411 -> "Length Required"
            else -> "Error"
        }
        output.write(
            ("HTTP/1.1 $status $reason\r\nContent-Type: $contentType\r\n" +
                "Content-Length: ${body.size}\r\nCache-Control: no-store\r\n" +
                "Connection: close\r\n\r\n").toByteArray(Charsets.UTF_8)
        )
        output.write(body)
        output.flush()
    }

    //==================================================================================
    // Games folder

    private class Entry(
        val name: String,
        val documentId: String,
        val mimeType: String,
        val size: Long,
        val lastModified: Long
    ) {
        val isDirectory get() = mimeType == DocumentsContract.Document.MIME_TYPE_DIR
    }

    private fun gamesTreeUri(): Uri {
        val gamesPath = PreferenceManager.getDefaultSharedPreferences(appContext)
            .getString(GameHelper.KEY_GAME_PATH, "")
        if (gamesPath.isNullOrEmpty()) {
            throw HttpError(409, "No games folder is set. Choose one in CitraVR first.")
        }
        return Uri.parse(gamesPath)
    }

    private fun rootDocumentId(treeUri: Uri): String = DocumentsContract.getTreeDocumentId(treeUri)

    private fun listChildren(treeUri: Uri, documentId: String): List<Entry> {
        val columns = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
            DocumentsContract.Document.COLUMN_LAST_MODIFIED
        )
        val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, documentId)
        val entries = mutableListOf<Entry>()
        var cursor: Cursor? = null
        try {
            cursor = appContext.contentResolver.query(childrenUri, columns, null, null, null)
                ?: return entries
            while (cursor.moveToNext()) {
                entries.add(
                    Entry(
                        cursor.getString(1),
                        cursor.getString(0),
                        cursor.getString(2) ?: "",
                        if (cursor.isNull(3)) 0L else cursor.getLong(3),
                        if (cursor.isNull(4)) 0L else cursor.getLong(4)
                    )
                )
            }
        } finally {
            cursor?.close()
        }
        return entries
    }

    private fun isRom(name: String): Boolean =
        Game.extensions.contains(name.substringAfterLast('.', "").lowercase())

    private fun listFiles(): JSONObject {
        val treeUri = gamesTreeUri()
        val files = JSONArray()
        fun scan(documentId: String, relativeDir: String, depth: Int) {
            if (depth <= 0) {
                return
            }
            listChildren(treeUri, documentId).sortedBy { it.name.lowercase() }.forEach {
                val relativePath = if (relativeDir.isEmpty()) it.name else "$relativeDir/${it.name}"
                if (it.isDirectory) {
                    scan(it.documentId, relativePath, depth - 1)
                } else if (isRom(it.name)) {
                    files.put(
                        JSONObject()
                            .put("path", relativePath)
                            .put("size", it.size)
                            .put("modified", it.lastModified)
                    )
                }
            }
        }
        val rootId = rootDocumentId(treeUri)
        scan(rootId, "", MAX_SCAN_DEPTH)
        val rootUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, rootId)
        return JSONObject()
            .put("folder", FileUtil.getFilename(rootUri))
            .put("freeBytes", getFreeBytes(rootUri))
            .put("extensions", JSONArray(Game.extensions.sorted()))
            .put("files", files)
    }

    private fun getFreeBytes(documentUri: Uri): Long = try {
        appContext.contentResolver.openFileDescriptor(documentUri, "r")!!.use {
            val stats = Os.fstatvfs(it.fileDescriptor)
            stats.f_bavail * stats.f_bsize
        }
    } catch (e: Exception) {
        -1L
    }

    private fun findEntry(treeUri: Uri, relativePath: String): Entry? {
        val parts = relativePath.split('/').filter { it.isNotEmpty() }
        if (parts.isEmpty() || parts.size > MAX_SCAN_DEPTH || parts.any { it == "." || it == ".." }) {
            return null
        }
        var documentId = rootDocumentId(treeUri)
        var entry: Entry? = null
        for ((index, part) in parts.withIndex()) {
            entry = listChildren(treeUri, documentId).firstOrNull { it.name == part } ?: return null
            if (index < parts.size - 1) {
                if (!entry.isDirectory) {
                    return null
                }
                documentId = entry.documentId
            }
        }
        return entry
    }

    private fun deleteFile(relativePath: String) {
        val treeUri = gamesTreeUri()
        val entry = findEntry(treeUri, relativePath)
        if (entry == null || entry.isDirectory || !isRom(entry.name)) {
            throw HttpError(404, "No ROM named $relativePath")
        }
        val uri = DocumentsContract.buildDocumentUriUsingTree(treeUri, entry.documentId)
        if (!DocumentsContract.deleteDocument(appContext.contentResolver, uri)) {
            throw HttpError(500, "Could not delete $relativePath")
        }
        Log.info("[RomServer] Deleted $relativePath")
        _libraryChanges.update { it + 1 }
    }

    private fun upload(request: Request, output: OutputStream) {
        val name = request.query["name"]?.trim() ?: throw HttpError(400, "Missing name")
        if (name.isEmpty() || name.contains('/') || name.contains('\\') || name.startsWith(".")) {
            throw HttpError(400, "Invalid file name")
        }
        if (!isRom(name)) {
            throw HttpError(
                400,
                "Unsupported file type. Supported: ${Game.extensions.sorted().joinToString(", ")}"
            )
        }
        val length = request.headers["content-length"]?.toLongOrNull()
            ?: throw HttpError(411, "Content-Length is required")

        val treeUri = gamesTreeUri()
        val rootId = rootDocumentId(treeUri)
        val existing = listChildren(treeUri, rootId)
        val overwrite = request.query["overwrite"] == "1"
        existing.firstOrNull { it.name == name }?.let {
            if (!overwrite) {
                throw HttpError(409, "$name already exists")
            }
        }
        // Leftovers from an interrupted upload of the same file.
        existing.filter { it.name == name + PARTIAL_SUFFIX }.forEach {
            DocumentsContract.deleteDocument(
                appContext.contentResolver,
                DocumentsContract.buildDocumentUriUsingTree(treeUri, it.documentId)
            )
        }

        if (request.headers["expect"]?.equals("100-continue", ignoreCase = true) == true) {
            output.write("HTTP/1.1 100 Continue\r\n\r\n".toByteArray(Charsets.UTF_8))
            output.flush()
        }

        val rootUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, rootId)
        // Written under a temporary name, so the game list never shows half a ROM.
        val partialUri = DocumentsContract.createDocument(
            appContext.contentResolver,
            rootUri,
            "application/octet-stream",
            name + PARTIAL_SUFFIX
        ) ?: throw HttpError(500, "Could not create $name")

        onTransferActive?.invoke(true)
        try {
            appContext.contentResolver.openOutputStream(partialUri, "w").use { fileOutput ->
                if (fileOutput == null) {
                    throw HttpError(500, "Could not open $name for writing")
                }
                val buffer = ByteArray(COPY_BUFFER_BYTES)
                var remaining = length
                while (remaining > 0) {
                    val read = request.body.read(buffer, 0, minOf(buffer.size.toLong(), remaining).toInt())
                    if (read < 0) {
                        throw IOException("Connection closed after ${length - remaining} of $length bytes")
                    }
                    fileOutput.write(buffer, 0, read)
                    remaining -= read
                }
            }
            if (overwrite) {
                listChildren(treeUri, rootId).firstOrNull { it.name == name }?.let {
                    DocumentsContract.deleteDocument(
                        appContext.contentResolver,
                        DocumentsContract.buildDocumentUriUsingTree(treeUri, it.documentId)
                    )
                }
            }
            DocumentsContract.renameDocument(appContext.contentResolver, partialUri, name)
                ?: throw HttpError(500, "Could not rename the uploaded file to $name")
        } catch (e: Exception) {
            try {
                DocumentsContract.deleteDocument(appContext.contentResolver, partialUri)
            } catch (_: Exception) {
            }
            throw e
        } finally {
            onTransferActive?.invoke(false)
        }
        Log.info("[RomServer] Uploaded $name ($length bytes)")
        _libraryChanges.update { it + 1 }
    }
}
