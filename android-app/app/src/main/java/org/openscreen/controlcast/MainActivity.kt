package org.openscreen.controlcast

import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.ParcelFileDescriptor
import android.system.OsConstants
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts.RequestMultiplePermissions
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts.OpenDocument
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.ui.draw.clip
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.ui.PlayerView
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.flow.receiveAsFlow

sealed interface DebugCommand {
    data class Connect(val target: String) : DebugCommand
    data object Disconnect : DebugCommand
    data class OpenVideo(
        val uri: Uri,
        val startPlaying: Boolean,
        val startPositionMs: Long,
    ) : DebugCommand
    data object Play : DebugCommand
    data object Pause : DebugCommand
    data class SeekTo(
        val positionMs: Long?,
        val offsetsMs: List<Long>,
        val timesMs: List<Long>,
    ) : DebugCommand
    data object ResetView : DebugCommand
    data class SetLocalMirror(val enabled: Boolean) : DebugCommand
    data class SetLocalSound(val enabled: Boolean) : DebugCommand
    data class SetHwEncode(val enabled: Boolean) : DebugCommand
    data object Print : DebugCommand
}

private fun parseLongListExtra(raw: String?): List<Long> =
    raw
        ?.split(',', ';', ' ')
        ?.mapNotNull { token -> token.trim().takeIf { it.isNotEmpty() }?.toLongOrNull() }
        ?: emptyList()

private fun Intent.toDebugCommand(): DebugCommand? {
    return when (action) {
        "org.openscreen.controlcast.DEBUG_CONNECT" ->
            getStringExtra("target")?.takeIf { it.isNotBlank() }?.let(DebugCommand::Connect)
        "org.openscreen.controlcast.DEBUG_DISCONNECT" -> DebugCommand.Disconnect
        "org.openscreen.controlcast.DEBUG_OPEN_VIDEO" -> {
            val uri = data ?: getParcelableExtra(Intent.EXTRA_STREAM)
                ?: getStringExtra("uri")?.let(Uri::parse)
            uri?.let {
                DebugCommand.OpenVideo(
                    uri = it,
                    startPlaying = getBooleanExtra("start_playing", false),
                    startPositionMs = getLongExtra("start_position_ms", 0L),
                )
            }
        }
        "org.openscreen.controlcast.DEBUG_PLAY" -> DebugCommand.Play
        "org.openscreen.controlcast.DEBUG_PAUSE" -> DebugCommand.Pause
        "org.openscreen.controlcast.DEBUG_SEEK_TO" -> {
            val hasPosition = hasExtra("position_ms")
            val offsetsMs = parseLongListExtra(getStringExtra("offsets_ms"))
            val timesMs = parseLongListExtra(getStringExtra("times_ms"))
            DebugCommand.SeekTo(
                positionMs = if (hasPosition) getLongExtra("position_ms", 0L).coerceAtLeast(0L) else null,
                offsetsMs = offsetsMs,
                timesMs = timesMs,
            )
        }
        "org.openscreen.controlcast.DEBUG_RESET_VIEW" -> DebugCommand.ResetView
        "org.openscreen.controlcast.DEBUG_SET_LOCAL_MIRROR" ->
            DebugCommand.SetLocalMirror(getBooleanExtra("enabled", true))
        "org.openscreen.controlcast.DEBUG_SET_LOCAL_SOUND" ->
            DebugCommand.SetLocalSound(getBooleanExtra("enabled", true))
        "org.openscreen.controlcast.DEBUG_SET_HW_ENCODE" ->
            DebugCommand.SetHwEncode(getBooleanExtra("enabled", true))
        "org.openscreen.controlcast.DEBUG_PRINT" -> DebugCommand.Print
        else -> null
    }
}

private fun debugTargetToCastDevice(target: String): CastDevice? {
    val trimmed = target.trim()
    if (trimmed.isEmpty()) return null
    val endBracket = trimmed.lastIndexOf(']')
    return if (trimmed.startsWith("[") && endBracket > 0 && endBracket + 1 < trimmed.length &&
        trimmed[endBracket + 1] == ':'
    ) {
        val host = trimmed.substring(1, endBracket)
        val port = trimmed.substring(endBracket + 2).toIntOrNull() ?: return null
        CastDevice(trimmed, host, port)
    } else {
        val colon = trimmed.lastIndexOf(':')
        if (colon <= 0 || colon == trimmed.lastIndex) return null
        val host = trimmed.substring(0, colon)
        val port = trimmed.substring(colon + 1).toIntOrNull() ?: return null
        CastDevice(trimmed, host, port)
    }
}

private fun displayNameForUri(context: Context, uri: Uri?): String {
    if (uri == null) return "No file"
    if (uri.scheme == "file") {
        return java.io.File(uri.path.orEmpty()).name.takeIf { it.isNotBlank() } ?: "No file"
    }
    return DocumentFile.fromSingleUri(context, uri)?.name
        ?: uri.lastPathSegment?.substringAfterLast('/')
        ?: uri.toString().takeIf { it.isNotBlank() }
        ?: "No file"
}

class MainActivity : ComponentActivity() {
    // Saved across rotation via onSaveInstanceState
    var savedPosition = 0L
    var savedPlaying = false
    private val debugCommands = Channel<DebugCommand>(Channel.UNLIMITED)
    private val sharedVideoUris = Channel<Uri>(Channel.UNLIMITED)
    private val videoReadPermissionResults = Channel<Boolean>(Channel.UNLIMITED)

    fun debugCommandsFlow() = debugCommands.receiveAsFlow()
    fun sharedVideoUrisFlow() = sharedVideoUris.receiveAsFlow()
    fun videoReadPermissionResultsFlow() = videoReadPermissionResults.receiveAsFlow()

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putLong("cast_position", savedPosition)
        outState.putBoolean("cast_playing", savedPlaying)
    }

    override fun onRestoreInstanceState(savedInstanceState: Bundle) {
        super.onRestoreInstanceState(savedInstanceState)
        savedPosition = savedInstanceState.getLong("cast_position", 0L)
        savedPlaying = savedInstanceState.getBoolean("cast_playing", false)
    }

    private val permissionLauncher = registerForActivityResult(
        RequestMultiplePermissions()
    ) { /* permissions granted or denied, user retaps Calibrate */ }

    fun requestCalibrationPermissions() {
        permissionLauncher.launch(arrayOf(
            android.Manifest.permission.CAMERA,
            android.Manifest.permission.RECORD_AUDIO,
        ))
    }

    private val videoReadPermissionLauncher = registerForActivityResult(
        RequestMultiplePermissions()
    ) { results ->
        videoReadPermissionResults.trySend(results.values.all { it })
    }

    fun hasVideoReadPermission(): Boolean {
        val permission = if (Build.VERSION.SDK_INT >= 33) {
            android.Manifest.permission.READ_MEDIA_VIDEO
        } else {
            android.Manifest.permission.READ_EXTERNAL_STORAGE
        }
        return checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED
    }

    fun requestVideoReadPermission() {
        val permissions = if (Build.VERSION.SDK_INT >= 33) {
            arrayOf(android.Manifest.permission.READ_MEDIA_VIDEO)
        } else {
            arrayOf(android.Manifest.permission.READ_EXTERNAL_STORAGE)
        }
        videoReadPermissionLauncher.launch(permissions)
    }

    private fun enqueueDebugIntent(intent: Intent?) {
        intent?.toDebugCommand()?.let { debugCommands.trySend(it) }
    }

    private fun extractSharedVideoUri(intent: Intent?): Uri? {
        return when (intent?.action) {
            android.content.Intent.ACTION_SEND ->
                intent.getParcelableExtra(android.content.Intent.EXTRA_STREAM)
            android.content.Intent.ACTION_VIEW,
            "com.android.camera.action.REVIEW" -> intent.data
            else -> null
        }
    }

    private fun enqueueSharedVideoIntent(intent: Intent?) {
        extractSharedVideoUri(intent)?.let { sharedVideoUris.trySend(it) }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Bind this process to the WiFi network so native UDP sockets
        // are routed correctly.  Without this, sendto() on UDP sockets
        // created by Open Screen's native code fails with EPERM.
        bindProcessToWifi()
        val testTarget = intent?.getStringExtra("test_target")
        val testFile = intent?.getStringExtra("test_file")
        val testCalibrate = intent?.getBooleanExtra("test_calibrate", false) == true
        val testFullscreen = intent?.getBooleanExtra("test_fullscreen", false) == true
        // Handle shared video from Gallery or other apps
        val sharedUri = extractSharedVideoUri(intent)
        enableEdgeToEdge()
        setContent {
            MaterialTheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = Color(0xFF101317),
                ) {
                    ControlCastApp(testTarget, testFile, testCalibrate, sharedUri, testFullscreen)
                }
            }
        }
        enqueueDebugIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        enqueueDebugIntent(intent)
        enqueueSharedVideoIntent(intent)
    }

    private fun bindProcessToWifi() {
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        // Bind synchronously first.
        cm.activeNetwork?.let { cm.bindProcessToNetwork(it) }
        // Keep a persistent network request so the binding is maintained
        // even if the network briefly drops and reconnects.
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .build()
        cm.requestNetwork(request, object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: android.net.Network) {
                cm.bindProcessToNetwork(network)
            }
        })
    }
}

data class ViewportState(
    val zoom: Float = 1f,
    val offsetX: Float = 0f,
    val offsetY: Float = 0f,
)

interface CastControlBackend {
    val status: kotlinx.coroutines.flow.StateFlow<String>
    suspend fun connect(target: String): Result<Unit>
    fun disconnect()
    fun openVideo(
        context: Context,
        uri: Uri,
        mirrorLocally: Boolean,
        startPositionMs: Long = 0L,
        startPlaying: Boolean = false,
    )
    fun play()
    fun pause()
    fun seekTo(positionMs: Long)
    fun updateViewport(viewport: ViewportState)
    fun setMirrorLocally(enabled: Boolean)
}

class NativeBackedBackend : CastControlBackend {
    private val mutableStatus =
        kotlinx.coroutines.flow.MutableStateFlow("Native backend loaded.")
    override val status: kotlinx.coroutines.flow.StateFlow<String> = mutableStatus
    private val stateListeners = mutableSetOf<(Boolean, Boolean, String) -> Unit>()

    init {
        nativeInit()
        refreshStatus()
    }

    fun testCast(target: String, filePath: String) {
        nativeTestCast(target, filePath)
        refreshStatus()
    }

    override suspend fun connect(target: String): Result<Unit> {
        return if (nativeConnect(target)) {
            refreshStatus()
            Result.success(Unit)
        } else {
            refreshStatus()
            Result.failure(IllegalStateException(mutableStatus.value))
        }
    }

    override fun disconnect() {
        nativeDisconnect()
        refreshStatus()
    }

    private var openPfd1: ParcelFileDescriptor? = null
    private var openPfd2: ParcelFileDescriptor? = null

    override fun openVideo(
        context: Context,
        uri: Uri,
        mirrorLocally: Boolean,
        startPositionMs: Long,
        startPlaying: Boolean,
    ) {
        var opened = false
        try {
            openPfd1?.close()
            openPfd2?.close()
            openPfd1 = null
            openPfd2 = null

            // Prefer fd-backed access for shared/external media. Some Android
            // devices expose a readable path to Java but native ffmpeg still gets
            // EACCES on direct open under scoped storage.
            val filePath = resolveFilePath(context, uri)
            if (filePath != null) {
                nativeOpenVideoPath(
                    uri.toString(),
                    filePath,
                    mirrorLocally,
                    startPositionMs,
                    startPlaying,
                )
            } else {
                // Fallback: open two independent fds for audio/video capturers.
                openPfd1 = context.contentResolver.openFileDescriptor(uri, "r")
                openPfd2 = context.contentResolver.openFileDescriptor(uri, "r")
                val fd1 = openPfd1?.fd ?: -1
                val fd2 = openPfd2?.fd ?: -1
                nativeOpenVideo(
                    uri.toString(),
                    fd1,
                    fd2,
                    mirrorLocally,
                    startPositionMs,
                    startPlaying,
                )
            }
            opened = true
        } catch (e: SecurityException) {
            android.util.Log.e("ControlCast", "openVideo failed for $uri", e)
            mutableStatus.value = "Open failed: permission denied for $uri"
            openPfd1?.close()
            openPfd2?.close()
            openPfd1 = null
            openPfd2 = null
        }
        if (opened) {
            refreshStatus()
        }
    }

    private fun resolveFilePath(context: Context, uri: Uri): String? {
        if (uri.scheme == "file") {
            val path = uri.path ?: return null
            return path.takeIf { isNativeDirectPathSafe(context, it) }
        }
        if (uri.scheme != "content") return null
        val cursor = context.contentResolver.query(
            uri, arrayOf(android.provider.MediaStore.MediaColumns.DATA),
            null, null, null)
        cursor?.use {
            if (it.moveToFirst()) {
                val path = it.getString(0)
                if (!path.isNullOrEmpty() &&
                    java.io.File(path).canRead() &&
                    isNativeDirectPathSafe(context, path)
                ) {
                    return path
                }
            }
        }
        return null
    }

    private fun isNativeDirectPathSafe(context: Context, path: String): Boolean {
        val normalized = path.trim()
        if (normalized.isEmpty()) return false
        val appSafePrefixes = listOf(
            context.filesDir?.absolutePath,
            context.cacheDir?.absolutePath,
            context.externalCacheDir?.absolutePath,
            context.getExternalFilesDir(null)?.absolutePath,
        ).filterNotNull()
        if (appSafePrefixes.any { normalized.startsWith(it) }) {
            return true
        }
        // Shared storage paths should go through ParcelFileDescriptor.
        if (normalized.startsWith("/storage/") ||
            normalized.startsWith("/sdcard/") ||
            normalized.startsWith("/mnt/")
        ) {
            return false
        }
        return true
    }

    override fun play() {
        nativePlay()
        refreshStatus()
    }

    override fun pause() {
        nativePause()
        refreshStatus()
    }

    override fun seekTo(positionMs: Long) {
        nativeSeekTo(positionMs)
        refreshStatus()
    }

    override fun updateViewport(viewport: ViewportState) {
        nativeUpdateViewport(viewport.zoom, viewport.offsetX, viewport.offsetY)
        refreshStatus()
    }

    fun setPlayoutDelay(delayMs: Int) {
        nativeSetPlayoutDelay(delayMs)
    }

    fun setBrightness(brightness: Int) {
        nativeSetBrightness(brightness)
        refreshStatus()
    }

    fun getCastPositionMs(): Long = nativeGetPositionMs()
    fun getCastDurationMs(): Long = nativeGetDurationMs()
    fun isCastPlaying(): Boolean = nativeIsPlaying()
    fun isConnected(): Boolean = nativeIsConnected()

    fun setAvSyncOffset(offsetMs: Long) {
        nativeSetAvSyncOffset(offsetMs)
        refreshStatus()
    }

    fun setHwEncode(enabled: Boolean) {
        nativeSetHwEncode(enabled)
        refreshStatus()
    }

    fun setVideoPassthroughEnabled(enabled: Boolean) {
        nativeSetVideoPassthroughEnabled(enabled)
        refreshStatus()
    }

    fun syncStatus() {
        refreshStatus()
    }

    fun addStateListener(listener: (Boolean, Boolean, String) -> Unit) {
        stateListeners += listener
        val statusText = mutableStatus.value
        listener(
            nativeIsConnected() || statusText.startsWith("Connected to "),
            nativeIsPlaying(),
            statusText,
        )
    }

    fun removeStateListener(listener: (Boolean, Boolean, String) -> Unit) {
        stateListeners -= listener
    }

    override fun setMirrorLocally(enabled: Boolean) {
        nativeSetMirrorLocally(enabled)
        refreshStatus()
    }

    private fun refreshStatus() {
        val statusText = nativeGetStatus()
        mutableStatus.value = statusText
        val connected = nativeIsConnected() || statusText.startsWith("Connected to ")
        val playing = nativeIsPlaying()
        stateListeners.toList().forEach { it(connected, playing, statusText) }
    }

    private external fun nativeInit()
    private external fun nativeConnect(target: String): Boolean
    private external fun nativeDisconnect()
    private external fun nativeOpenVideo(
        uri: String,
        fd1: Int,
        fd2: Int,
        mirrorLocally: Boolean,
        startPositionMs: Long,
        startPlaying: Boolean,
    )
    private external fun nativeOpenVideoPath(
        uri: String,
        filePath: String,
        mirrorLocally: Boolean,
        startPositionMs: Long,
        startPlaying: Boolean,
    )
    private external fun nativePlay()
    private external fun nativePause()
    private external fun nativeSeekTo(positionMs: Long)
    private external fun nativeUpdateViewport(zoom: Float, offsetX: Float, offsetY: Float)
    private external fun nativeSetMirrorLocally(enabled: Boolean)
    private external fun nativeGetStatus(): String
    private external fun nativeSetPlayoutDelay(delayMs: Int)
    private external fun nativeGetPositionMs(): Long
    private external fun nativeGetDurationMs(): Long
    private external fun nativeIsPlaying(): Boolean
    private external fun nativeIsConnected(): Boolean
    private external fun nativeSetAvSyncOffset(offsetMs: Long)
    private external fun nativeSetHwEncode(enabled: Boolean)
    private external fun nativeSetVideoPassthroughEnabled(enabled: Boolean)
    private external fun nativeSetBrightness(brightness: Int)
    private external fun nativeTestCast(target: String, filePath: String)

    companion object {
        init {
            System.loadLibrary("controlcast")
        }
    }
}

class Connection(private val backend: NativeBackedBackend) {
    enum class State {
        DISCONNECTED,
        CONNECTING,
        CONNECTED,
    }

    var state by mutableStateOf(State.DISCONNECTED)
        private set

    var target: CastDevice? by mutableStateOf(null)
        private set

    // 0 means clean state / intentional shutdown. Non-zero means the last
    // disconnect was due to an actual failure and may be surfaced to the UI.
    var lastError by mutableIntStateOf(0)
        private set

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var monitorJob: Job? = null
    private val nativeStateListener: (Boolean, Boolean, String) -> Unit =
        { connected, _, statusText ->
            if (connected) {
                state = State.CONNECTED
                lastError = 0
                if (target == null) {
                    parseConnectedTarget(statusText)?.let { recoveredTarget ->
                        target = debugTargetToCastDevice(recoveredTarget)
                    }
                }
            } else if (target != null && state == State.CONNECTING) {
                state = State.CONNECTING
            } else if (target == null) {
                state = State.DISCONNECTED
            }
        }

    init {
        backend.addStateListener(nativeStateListener)
        startMonitoring()
    }

    private fun startMonitoring() {
        monitorJob?.cancel()
        monitorJob = scope.launch {
            while (isActive) {
                delay(500)
                backend.syncStatus()
                val connected =
                    backend.isConnected() || backend.status.value.startsWith("Connected to ")
                if (connected) {
                    state = State.CONNECTED
                    lastError = 0
                    continue
                }
                if (target != null) {
                    state = State.CONNECTING
                    continue
                }
                state = State.DISCONNECTED
            }
        }
    }

    suspend fun connect(device: CastDevice): Result<Unit> {
        if (device.target.isBlank()) {
            state = State.DISCONNECTED
            lastError = OsConstants.EINVAL
            return Result.failure(IllegalArgumentException("Blank cast target"))
        }
        target = device
        state = State.CONNECTING
        lastError = 0
        val result = backend.connect(device.target)
        backend.syncStatus()
        if (backend.status.value.startsWith("Connected to ")) {
            state = State.CONNECTED
            lastError = 0
        }
        return result
    }

    fun disconnect() {
        monitorJob?.cancel()
        monitorJob = null
        backend.disconnect()
        state = State.DISCONNECTED
        target = null
        lastError = 0
    }

    fun dispose() {
        monitorJob?.cancel()
        backend.removeStateListener(nativeStateListener)
        scope.cancel()
    }

    fun openVideo(
        context: Context,
        uri: Uri,
        mirrorLocally: Boolean,
        startPositionMs: Long,
        startPlaying: Boolean,
    ) {
        backend.openVideo(
            context,
            uri,
            mirrorLocally,
            startPositionMs,
            startPlaying,
        )
    }

    fun play() {
        backend.play()
    }

    fun pause() {
        backend.pause()
    }

    fun seekTo(positionMs: Long) {
        backend.seekTo(positionMs)
    }

    fun updateViewport(viewport: ViewportState) {
        backend.updateViewport(viewport)
    }

    fun getCastPositionMs(): Long = backend.getCastPositionMs()

    fun getCastDurationMs(): Long = backend.getCastDurationMs()

    fun isCastPlaying(): Boolean = backend.isCastPlaying()

    val status: kotlinx.coroutines.flow.StateFlow<String>
        get() = backend.status

    private fun parseConnectedTarget(statusText: String): String? {
        val prefix = "Connected to "
        if (!statusText.startsWith(prefix)) return null
        return statusText.removePrefix(prefix).substringBefore(" |").ifBlank { null }
    }
}

private fun getAutoReconnectTargets(context: Context): Set<String> {
    val prefs = context.getSharedPreferences("cast_devices", Context.MODE_PRIVATE)
    return prefs.getStringSet("auto_reconnect", emptySet()) ?: emptySet()
}

private fun setAutoReconnect(context: Context, target: String, enabled: Boolean) {
    val prefs = context.getSharedPreferences("cast_devices", Context.MODE_PRIVATE)
    val current = prefs.getStringSet("auto_reconnect", emptySet())?.toMutableSet()
        ?: mutableSetOf()
    if (enabled) current.add(target) else current.remove(target)
    prefs.edit().putStringSet("auto_reconnect", current).apply()
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun ControlCastApp(testTarget: String? = null, testFile: String? = null, testCalibrate: Boolean = false, sharedUri: Uri? = null, testFullscreen: Boolean = false) {
    val context = LocalContext.current
    val backend = remember { NativeBackedBackend() }
    val connection = remember(backend) { Connection(backend) }

    // Auto-cast when launched with test extras via adb:
    // adb shell am start -n org.openscreen.controlcast/.MainActivity
    //   --es test_target "192.168.1.189:8009"
    //   --es test_file "/sdcard/DCIM/Camera/video.mp4"
    // Auto-cast and optional auto-calibrate via adb intent extras:
    //   --es test_target "192.168.1.189:8009"
    //   --es test_file "/data/local/tmp/test.mp4"
    //   --ez test_calibrate true
    val activity = context as? MainActivity
    LaunchedEffect(testTarget, testFile, testCalibrate) {
        if (!testTarget.isNullOrEmpty() && !testFile.isNullOrEmpty()) {
            delay(1000)
            backend.testCast(testTarget, testFile)
        }
        if (testCalibrate && activity != null) {
            // Extract and cast bundled sync test if no file specified
            if (testFile.isNullOrEmpty() && !testTarget.isNullOrEmpty()) {
                val syncFile = java.io.File(context.cacheDir, "sync_test.mp4")
                if (!syncFile.exists()) {
                    context.resources.openRawResource(R.raw.sync_test).use { input ->
                        syncFile.outputStream().use { output -> input.copyTo(output) }
                    }
                }
                delay(1000)
                backend.testCast(testTarget, syncFile.absolutePath)
            }
            val calibrator = AvSyncCalibrator(context)
            if (!calibrator.hasPermissions()) {
                activity.requestCalibrationPermissions()
            }
            delay(5000)
            if (AvSyncCalibrator(context).hasPermissions()) {
                android.util.Log.i("ControlCast", "Starting calibration...")
                val result = calibrator.calibrate(activity)
                android.util.Log.i("ControlCast", "Calibration: ${result.message}")
                if (result.numSamples > 0) {
                    backend.setAvSyncOffset(result.offsetMs)
                }
            } else {
                android.util.Log.e("ControlCast", "Calibration: permissions not granted")
            }
        }
    }
    val backendStatus by connection.status.collectAsStateWithLifecycle()
    val discovery = remember { CastDiscovery(context) }
    val discoveredDevices by discovery.devices.collectAsStateWithLifecycle()
    val coroutineScope = rememberCoroutineScope()
    val exoPlayer = remember(context) {
        ExoPlayer.Builder(context).build().apply {
            repeatMode = Player.REPEAT_MODE_OFF
            volume = 0f  // Muted by default — sound goes to Cast receiver.
        }
    }
    val prefs = remember { context.getSharedPreferences("cast_ui", Context.MODE_PRIVATE) }

    var selectedUri by rememberSaveable { mutableStateOf<Uri?>(null) }
    var localMirrorEnabled by rememberSaveable { mutableStateOf(true) }
    var localSoundEnabled by rememberSaveable { mutableStateOf(false) }
    var hwEncodeEnabled by rememberSaveable { mutableStateOf(true) }
    var videoPassthroughEnabled by rememberSaveable {
        mutableStateOf(prefs.getBoolean("video_passthrough_enabled", false))
    }
    var brightnessLevel by rememberSaveable { mutableIntStateOf(0) }
    var isPlaying by rememberSaveable { mutableStateOf(false) }
    var durationMs by rememberSaveable { mutableLongStateOf(0L) }
    var positionMs by rememberSaveable { mutableLongStateOf(0L) }
    var viewport by remember { mutableStateOf(ViewportState()) }
    var sliderValue by rememberSaveable {
        mutableFloatStateOf(if (durationMs > 0L) positionMs.toFloat() / durationMs.toFloat() else 0f)
    }
    var sliderDragging by remember { mutableStateOf(false) }
    // Only block polling during restore if we have a saved position to restore.
    // On fresh launch (positionMs=0), no restore needed — start polling immediately.
    var restored by remember { mutableStateOf(positionMs == 0L) }
    var isFullscreen by rememberSaveable {
        mutableStateOf(testFullscreen || prefs.getBoolean("fullscreen", false))
    }
    var showAdvancedControls by rememberSaveable { mutableStateOf(false) }
    var autoReconnectTargets by remember {
        mutableStateOf(getAutoReconnectTargets(context))
    }
    var castOpenedUri by rememberSaveable { mutableStateOf<String?>(null) }
    var reconnectResumeArmed by rememberSaveable { mutableStateOf(false) }
    var reconnectResumeUri by rememberSaveable { mutableStateOf<String?>(null) }
    var pendingSharedUri by rememberSaveable { mutableStateOf<Uri?>(null) }
    var latestSeekTargetMs by rememberSaveable { mutableLongStateOf(-1L) }
    var latestSeekRealtimeMs by rememberSaveable { mutableLongStateOf(0L) }
    var lastCastSeekDispatchRealtimeMs by remember { mutableLongStateOf(0L) }
    var pendingCastSeekTargetMs by remember { mutableLongStateOf(-1L) }
    var pendingCastSeekJob by remember { mutableStateOf<Job?>(null) }
    var lastObservedCastPlaying by remember { mutableStateOf(false) }
    val connectionState = connection.state
    val connectedDevice = connection.target
    val isConnected = connectionState == Connection.State.CONNECTED
    val isConnecting = connectionState == Connection.State.CONNECTING
    val connectionStatusText = when (connectionState) {
        Connection.State.DISCONNECTED ->
            connectedDevice?.target?.let { "Not connected. Target: $it" } ?: "Not connected."
        Connection.State.CONNECTING ->
            "Connecting to ${connectedDevice?.target ?: connectedDevice?.name ?: "device"}"
        Connection.State.CONNECTED ->
            "Connected to ${connectedDevice?.target ?: connectedDevice?.name ?: "device"}"
    }

    fun openSelectedVideoOnCast(uri: Uri, startPlaying: Boolean, startPositionMs: Long = 0L) {
        if (connectedDevice == null || connectionState != Connection.State.CONNECTED) return
        // Sync policy:
        // 1. Initial handoff after connect/open/seek/resume-from-paused is local -> cast.
        //    We seed Cast from the current local/Exo position because the TV has
        //    not established visible playback yet.
        // 2. Once Cast is actively running, steady-state correction becomes
        //    cast -> local in the polling loop below.
        android.util.Log.i(
            "ControlCast",
            "openSelectedVideoOnCast uri=$uri startPlaying=$startPlaying startPositionMs=$startPositionMs castOpenedUri=$castOpenedUri connectionState=$connectionState",
        )
        connection.openVideo(
            context,
            uri,
            localMirrorEnabled,
            startPositionMs,
            startPlaying,
        )
        castOpenedUri = uri.toString()
    }

    // Connect to a device and optionally send the current video.
    fun connectToDevice(device: CastDevice) {
        if (connectedDevice?.target == device.target &&
            connectionState != Connection.State.DISCONNECTED) {
            return
        }
        coroutineScope.launch {
            connection.connect(device)
        }
    }

    fun setLocalMirrorEnabled(enabled: Boolean) {
        localMirrorEnabled = enabled
        backend.setMirrorLocally(enabled)
        if (!enabled) {
            exoPlayer.pause()
        }
    }

    fun setLocalSoundEnabled(enabled: Boolean) {
        localSoundEnabled = enabled
        exoPlayer.volume = if (enabled) 1f else 0f
    }

    fun setHwEncodeEnabled(enabled: Boolean) {
        hwEncodeEnabled = enabled
        backend.setHwEncode(enabled)
    }

    fun setVideoPassthroughEnabled(enabled: Boolean) {
        if (selectedUri != null) {
            pauseBoth()
        }
        videoPassthroughEnabled = enabled
        prefs.edit().putBoolean("video_passthrough_enabled", enabled).apply()
        backend.setVideoPassthroughEnabled(enabled)
    }

    fun setBrightnessLevel(level: Int) {
        val clamped = level.coerceIn(-200, 200)
        brightnessLevel = clamped
        backend.setBrightness(clamped)
    }

    fun pauseBoth() {
        exoPlayer.pause()
        connection.pause()
        isPlaying = false
        reconnectResumeArmed = false
    }

    fun playBoth() {
        connection.play()
        if (localMirrorEnabled) {
            exoPlayer.play()
        } else {
            exoPlayer.pause()
        }
        isPlaying = true
        reconnectResumeArmed = false
    }

    fun scheduleCastSeek(targetPositionMs: Long, forceImmediate: Boolean = false) {
        val target = targetPositionMs.coerceAtLeast(0L)
        pendingCastSeekTargetMs = target
        pendingCastSeekJob?.cancel()
        pendingCastSeekJob = coroutineScope.launch {
            val now = android.os.SystemClock.elapsedRealtime()
            val minGapMs = if (forceImmediate) 0L else 250L
            val waitMs = (lastCastSeekDispatchRealtimeMs + minGapMs - now).coerceAtLeast(0L)
            if (waitMs > 0L) {
                delay(waitMs)
            }
            val dispatchTarget = pendingCastSeekTargetMs
            pendingCastSeekTargetMs = -1L
            connection.seekTo(dispatchTarget)
            lastCastSeekDispatchRealtimeMs = android.os.SystemClock.elapsedRealtime()
        }
    }

    fun seekBoth(targetPositionMs: Long, forceCastSeek: Boolean = false) {
        reconnectResumeArmed = false
        latestSeekTargetMs = targetPositionMs.coerceAtLeast(0L)
        latestSeekRealtimeMs = android.os.SystemClock.elapsedRealtime()
        positionMs = targetPositionMs.coerceAtLeast(0L)
        sliderValue = if (durationMs > 0L) {
            positionMs.toFloat() / durationMs.toFloat()
        } else {
            0f
        }
        exoPlayer.seekTo(positionMs)
        scheduleCastSeek(positionMs, forceCastSeek)
    }

    suspend fun runSeekSequence(command: DebugCommand.SeekTo) {
        val basePosition = command.positionMs ?: positionMs
        if (command.offsetsMs.isNotEmpty()) {
            val times = if (command.timesMs.isNotEmpty()) command.timesMs else List(command.offsetsMs.size) { it * 200L }
            var lastTime = 0L
            command.offsetsMs.forEachIndexed { index, offset ->
                val targetTime = times.getOrElse(index) { times.lastOrNull() ?: 0L }.coerceAtLeast(lastTime)
                val delayMs = (targetTime - lastTime).coerceAtLeast(0L)
                if (delayMs > 0L) delay(delayMs)
                seekBoth((basePosition + offset).coerceAtLeast(0L))
                lastTime = targetTime
            }
        } else if (command.positionMs != null) {
            seekBoth(command.positionMs, forceCastSeek = true)
        }
    }

    fun printDebugState(reason: String) {
        backend.syncStatus()
        val castPos = connection.getCastPositionMs()
        val exoPos = exoPlayer.currentPosition.coerceAtLeast(0L)
        val now = android.os.SystemClock.elapsedRealtime()
        android.util.Log.i(
            "ControlCast",
            buildString {
                append("DEBUG_PRINT reason=").append(reason)
                append(" connection=").append(connectionState)
                append(" target=").append(connectedDevice?.target ?: "")
                append(" selectedUri=").append(selectedUri ?: "")
                append(" isPlaying=").append(isPlaying)
                append(" exoPos=").append(exoPos)
                append(" exoDur=").append(exoPlayer.duration.coerceAtLeast(0L))
                append(" castPos=").append(castPos)
                append(" castDur=").append(connection.getCastDurationMs())
                append(" castPlaying=").append(connection.isCastPlaying())
                append(" exoMinusCast=").append(exoPos - castPos)
                append(" latestSeekTarget=").append(latestSeekTargetMs)
                append(" latestSeekAgeMs=")
                    .append(if (latestSeekRealtimeMs > 0L) now - latestSeekRealtimeMs else -1L)
                append(" castMinusLatestSeek=")
                    .append(if (latestSeekTargetMs >= 0L) castPos - latestSeekTargetMs else Long.MIN_VALUE)
                append(" localMirror=").append(localMirrorEnabled)
                append(" localSound=").append(localSoundEnabled)
                append(" hwEncode=").append(hwEncodeEnabled)
                append(" ptEnabled=").append(videoPassthroughEnabled)
                append(" viewport=").append(viewport.zoom).append(',').append(viewport.offsetX).append(',').append(viewport.offsetY)
                append(" backendStatus=").append(backend.status.value)
            },
        )
    }

    suspend fun openVideoFromCommand(
        uri: Uri,
        startPlaying: Boolean,
        startPositionMs: Long,
    ) {
        reconnectResumeArmed = false
        reconnectResumeUri = uri.toString()
        selectedUri = uri
        val mediaItem = MediaItem.fromUri(uri)
        exoPlayer.setMediaItem(mediaItem)
        exoPlayer.prepare()
        exoPlayer.seekTo(startPositionMs.coerceAtLeast(0L))
        if (localMirrorEnabled && startPlaying) {
            exoPlayer.play()
        } else {
            exoPlayer.pause()
        }
        isPlaying = startPlaying
        positionMs = startPositionMs.coerceAtLeast(0L)
        sliderValue = if (durationMs > 0L) {
            positionMs.toFloat() / durationMs.toFloat()
        } else {
            0f
        }
        if (connectionState == Connection.State.CONNECTED) {
            openSelectedVideoOnCast(uri, startPlaying, positionMs)
        }
    }

    suspend fun loadIncomingSharedVideo(uri: Uri) {
        val activityContext = context as? MainActivity
        if (uri.scheme == "content" &&
            activityContext != null &&
            !activityContext.hasVideoReadPermission()
        ) {
            pendingSharedUri = uri
            Toast.makeText(
                context,
                "Allow video access to open the selected clip.",
                Toast.LENGTH_SHORT,
            ).show()
            activityContext.requestVideoReadPermission()
            return
        }
        pendingSharedUri = null
        val shouldStartPlaying = if (connectionState == Connection.State.CONNECTED) {
            isPlaying
        } else {
            localMirrorEnabled
        }
        // Keep local paused until Cast is connected/opened so the app does not
        // race ahead from a cold start.
        reconnectResumeArmed =
            shouldStartPlaying && connectionState != Connection.State.CONNECTED
        reconnectResumeUri = uri.toString()
        selectedUri = uri
        val mediaItem = MediaItem.fromUri(uri)
        exoPlayer.setMediaItem(mediaItem)
        exoPlayer.prepare()
        // Intent-driven opens must not let local preview run ahead of Cast.
        // Keep local paused and wait for the Cast session to become authoritative.
        exoPlayer.pause()
        isPlaying = false
        if (connectionState == Connection.State.CONNECTED) {
            openSelectedVideoOnCast(uri, shouldStartPlaying, startPositionMs = 0L)
        }
    }

    // Load test file into ExoPlayer for local preview + slider
    LaunchedEffect(testFile, exoPlayer) {
        if (!testFile.isNullOrEmpty() && exoPlayer.mediaItemCount == 0) {
            // Copy to cache dir — ExoPlayer can't access /data/local/tmp/
            val src = java.io.File(testFile)
            val cached = java.io.File(context.cacheDir, src.name)
            if (src.canRead()) {
                src.inputStream().use { i -> cached.outputStream().use { o -> i.copyTo(o) } }
            }
            val fileUri = Uri.fromFile(if (cached.exists()) cached else src)
            selectedUri = fileUri
            exoPlayer.setMediaItem(MediaItem.fromUri(fileUri))
            exoPlayer.prepare()
            exoPlayer.volume = if (localSoundEnabled) 1f else 0f
            exoPlayer.play()
            isPlaying = true
        }
    }

    DisposableEffect(exoPlayer) {
        onDispose {
            // Save to activity fields — these survive between
            // onSaveInstanceState and recreation, unlike rememberSaveable
            // which is captured before onDispose runs.
            (context as? MainActivity)?.let {
                it.savedPlaying = exoPlayer.isPlaying
                it.savedPosition = exoPlayer.currentPosition.coerceAtLeast(0L)
            }
            exoPlayer.release()
        }
    }

    DisposableEffect(discovery) {
        discovery.startDiscovery()
        onDispose { discovery.stopDiscovery() }
    }

    LaunchedEffect(connectionState) {
        if (connectionState == Connection.State.DISCONNECTED) {
            android.util.Log.i(
                "ControlCast",
                "connectionState DISCONNECTED: clearing castOpenedUri old=$castOpenedUri selectedUri=$selectedUri isPlaying=$isPlaying",
            )
            castOpenedUri = null
            val currentUri = selectedUri?.toString()
            if (isPlaying && currentUri != null) {
                reconnectResumeArmed = true
                reconnectResumeUri = currentUri
                exoPlayer.pause()
                isPlaying = false
            } else if (currentUri == null) {
                reconnectResumeArmed = false
                reconnectResumeUri = null
            }
        }
    }

    DisposableEffect(connection) {
        onDispose {
            connection.dispose()
        }
    }

    // Auto-load video shared from Gallery or other apps
    LaunchedEffect(sharedUri) {
        sharedUri?.let { loadIncomingSharedVideo(it) }
    }

    LaunchedEffect(activity) {
        val sharedFlow = activity?.sharedVideoUrisFlow() ?: return@LaunchedEffect
        sharedFlow.collect { uri ->
            loadIncomingSharedVideo(uri)
        }
    }

    LaunchedEffect(activity, pendingSharedUri) {
        val permissionFlow = activity?.videoReadPermissionResultsFlow()
            ?: return@LaunchedEffect
        permissionFlow.collect { granted ->
            val pending = pendingSharedUri
            if (granted && pending != null) {
                loadIncomingSharedVideo(pending)
            } else if (!granted && pending != null) {
                pendingSharedUri = null
                Toast.makeText(
                    context,
                    "Video access was denied.",
                    Toast.LENGTH_SHORT,
                ).show()
            }
        }
    }

    // Auto-reconnect: when a device marked for auto-reconnect appears
    // and nothing is connected yet, connect automatically.
    LaunchedEffect(discoveredDevices, connectionState) {
        if (connectionState != Connection.State.DISCONNECTED) return@LaunchedEffect
        val targets = getAutoReconnectTargets(context)
        val match = discoveredDevices.firstOrNull { it.target in targets }
        if (match != null) {
            connectToDevice(match)
        }
    }

    LaunchedEffect(connectionState, connectedDevice, selectedUri) {
        if (connectedDevice != null && connectionState == Connection.State.CONNECTED) {
            selectedUri?.let { uri ->
                val uriString = uri.toString()
                val shouldResumeAfterReconnect =
                    reconnectResumeArmed && reconnectResumeUri == uriString
                android.util.Log.i(
                    "ControlCast",
                    "connected effect uri=$uriString castOpenedUri=$castOpenedUri shouldResumeAfterReconnect=$shouldResumeAfterReconnect exoPlaying=${exoPlayer.isPlaying} reconnectResumeArmed=$reconnectResumeArmed",
                )
                if (castOpenedUri != uriString) {
                    openSelectedVideoOnCast(
                        uri,
                        shouldResumeAfterReconnect || exoPlayer.isPlaying,
                        exoPlayer.currentPosition.coerceAtLeast(0L),
                    )
                }
                if (shouldResumeAfterReconnect && localMirrorEnabled) {
                    exoPlayer.play()
                    isPlaying = true
                    reconnectResumeArmed = false
                }
            }
        }
    }

    LaunchedEffect(activity, discoveredDevices, connectionState, connectedDevice, selectedUri) {
        val debugFlow = activity?.debugCommandsFlow() ?: return@LaunchedEffect
        debugFlow.collect { command ->
            when (command) {
                is DebugCommand.Connect -> {
                    val device = discoveredDevices.firstOrNull { it.target == command.target }
                    if (device != null) {
                        connectToDevice(device)
                    } else {
                        debugTargetToCastDevice(command.target)?.let { parsed ->
                            coroutineScope.launch {
                                connection.connect(parsed)
                            }
                        }
                    }
                }
                DebugCommand.Disconnect -> connection.disconnect()
                is DebugCommand.OpenVideo -> openVideoFromCommand(
                    command.uri,
                    command.startPlaying,
                    command.startPositionMs,
                )
                DebugCommand.Play -> playBoth()
                DebugCommand.Pause -> pauseBoth()
                is DebugCommand.SeekTo -> runSeekSequence(command)
                DebugCommand.ResetView -> {
                    viewport = ViewportState()
                    backend.updateViewport(viewport)
                }
                is DebugCommand.SetLocalMirror -> setLocalMirrorEnabled(command.enabled)
                is DebugCommand.SetLocalSound -> setLocalSoundEnabled(command.enabled)
                is DebugCommand.SetHwEncode -> setHwEncodeEnabled(command.enabled)
                DebugCommand.Print -> printDebugState("intent")
            }
        }
    }

    // Restore video after rotation (ExoPlayer is recreated but URI/position are saved)
    LaunchedEffect(exoPlayer, selectedUri) {
        if (selectedUri == null) {
            restored = true
        }
        selectedUri?.let { uri ->
            if (exoPlayer.mediaItemCount == 0) {
                exoPlayer.setMediaItem(MediaItem.fromUri(uri))
                exoPlayer.prepare()
                while (exoPlayer.playbackState != Player.STATE_READY &&
                       exoPlayer.playbackState != Player.STATE_ENDED) {
                    delay(50)
                }
                durationMs = exoPlayer.duration.coerceAtLeast(0L)
                // Get position from Cast if running, else use saved
                val castPos = connection.getCastPositionMs()
                if (castPos > 0) positionMs = castPos
                exoPlayer.seekTo(positionMs)
                sliderValue = if (durationMs > 0L) positionMs.toFloat() / durationMs.toFloat() else 0f
                exoPlayer.volume = if (localSoundEnabled) 1f else 0f
                // Query Cast backend for play state — it survives rotation
                val castPlaying = connection.isCastPlaying()
                if (castPlaying) {
                    exoPlayer.play()
                    isPlaying = true
                }
                restored = true
            }
        }
    }

    LaunchedEffect(exoPlayer) {
        while (true) {
            val castConnected = connectionState == Connection.State.CONNECTED
            val castPos = connection.getCastPositionMs().coerceAtLeast(0L)
            val castDur = connection.getCastDurationMs().coerceAtLeast(0L)
            val castPlaying = connection.isCastPlaying()
            val exoPos = exoPlayer.currentPosition.coerceAtLeast(0L)
            val exoHasMedia = exoPlayer.mediaItemCount > 0
            durationMs = if (castConnected && castDur > 0L) {
                castDur
            } else if (exoHasMedia) {
                exoPlayer.duration.coerceAtLeast(0L)
            } else {
                castDur
            }
            if (castConnected && exoHasMedia && restored) {
                val castStartedThisTick = castPlaying && !lastObservedCastPlaying
                if (castStartedThisTick && castPos > 0L) {
                    exoPlayer.seekTo(castPos)
                }
                // After Cast is actively running, Cast becomes the authority for
                // steady-state correction. This keeps local preview from drifting
                // away from the real TV playback path during fallback/re-entry.
                val driftMs = kotlin.math.abs(exoPos - castPos)
                val recentSeek = latestSeekRealtimeMs > 0L &&
                    android.os.SystemClock.elapsedRealtime() - latestSeekRealtimeMs < 2500L
                val syncThresholdMs = if (recentSeek) 150L else 300L
                if (!sliderDragging && castPos > 0L && driftMs > syncThresholdMs) {
                    exoPlayer.seekTo(castPos)
                }
                if (castPlaying) {
                    if (localMirrorEnabled) {
                        exoPlayer.play()
                    } else {
                        exoPlayer.pause()
                    }
                } else {
                    exoPlayer.pause()
                }
            }
            lastObservedCastPlaying = castPlaying
            if (!sliderDragging && restored) {
                positionMs = if (castConnected && castPos > 0L) {
                    castPos
                } else if (exoHasMedia) {
                    exoPlayer.currentPosition.coerceAtLeast(0L)
                } else {
                    castPos
                }
                sliderValue = if (durationMs > 0L) {
                    positionMs.toFloat() / durationMs.toFloat()
                } else {
                    0f
                }
            }
            isPlaying = if (castConnected) {
                castPlaying
            } else if (exoHasMedia) {
                exoPlayer.isPlaying
            } else {
                castPlaying
            }
            delay(200)
        }
    }

    LaunchedEffect(videoPassthroughEnabled) {
        backend.setVideoPassthroughEnabled(videoPassthroughEnabled)
    }

    val openVideoLauncher = rememberLauncherForActivityResult(OpenDocument()) { uri ->
        if (uri != null) {
            val shouldStartPlaying = if (connectionState == Connection.State.CONNECTED) {
                isPlaying
            } else {
                localMirrorEnabled
            }
            reconnectResumeArmed = false
            reconnectResumeUri = uri.toString()
            context.contentResolver.takePersistableUriPermission(
                uri,
                android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION,
            )
            selectedUri = uri
            val mediaItem = MediaItem.fromUri(uri)
            exoPlayer.setMediaItem(mediaItem)
            exoPlayer.prepare()
            if (localMirrorEnabled && shouldStartPlaying) {
                exoPlayer.play()
            } else {
                exoPlayer.pause()
            }
            isPlaying = shouldStartPlaying
            if (connectionState == Connection.State.CONNECTED) {
                openSelectedVideoOnCast(uri, shouldStartPlaying)
            }
        }
    }

    if (isFullscreen) {
        FullscreenPlayer(
            exoPlayer = exoPlayer,
            viewport = viewport,
            onViewportChange = { viewport = it; connection.updateViewport(it) },
            isPlaying = isPlaying,
            onPlayPause = {
                try {
                    if (isPlaying) { exoPlayer.pause(); connection.pause() }
                    else { exoPlayer.play(); connection.play() }
                    isPlaying = !isPlaying
                } catch (_: Exception) {}
            },
            sliderValue = sliderValue,
            onSliderChange = {
                sliderDragging = true; sliderValue = it; positionMs = (durationMs * it).toLong()
                seekBoth(positionMs)
            },
            onSliderFinished = { sliderDragging = false; seekBoth(positionMs, forceCastSeek = true) },
            sliderEnabled = durationMs > 0L,
            positionMs = positionMs,
            durationMs = durationMs,
            onExitFullscreen = {
                isFullscreen = false
                prefs.edit().putBoolean("fullscreen", false).apply()
            },
        )
        return
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        // Device discovery section
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Text(
                text = "Cast devices",
                color = Color(0xFFD9E2EC),
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.weight(1f),
            )
            Button(onClick = {
                discovery.stopDiscovery()
                discovery.startDiscovery()
            }) {
                Text("Refresh")
            }
        }

        if (isConnected || isConnecting) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(10.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = if (isConnecting) {
                        "Connecting: ${connectedDevice?.name ?: "device"}"
                    } else {
                        "Connected: ${connectedDevice?.name ?: "device"}"
                    },
                    color = Color(0xFF9CB0C3),
                    style = MaterialTheme.typography.bodyMedium,
                    modifier = Modifier.weight(1f),
                )
                Button(onClick = {
                    connection.disconnect()
                }) {
                    Text("Disconnect")
                }
            }
        }

        if (discoveredDevices.isEmpty()) {
            Text(
                text = "Searching for Cast receivers...",
                color = Color(0xFF6B7F8E),
                style = MaterialTheme.typography.bodySmall,
            )
        } else {
            for (device in discoveredDevices) {
                val isCurrentTarget = device == connectedDevice
                val isAutoReconnect = device.target in autoReconnectTargets
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(
                            if (isConnected && isCurrentTarget) Color(0xFF27486E)
                            else Color(0xFF182028),
                            RoundedCornerShape(8.dp),
                        )
                        .clickable { connectToDevice(device) }
                        .padding(12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text(
                            text = device.name,
                            color = Color(0xFFD9E2EC),
                            style = MaterialTheme.typography.bodyMedium,
                        )
                        Text(
                            text = device.target,
                            color = Color(0xFF6B7F8E),
                            style = MaterialTheme.typography.bodySmall,
                        )
                    }
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Switch(
                            checked = isAutoReconnect,
                            onCheckedChange = { enabled ->
                                setAutoReconnect(context, device.target, enabled)
                                autoReconnectTargets =
                                    getAutoReconnectTargets(context)
                            },
                            modifier = Modifier.size(40.dp),
                        )
                        Text(
                            text = "Auto",
                            color = Color(0xFF6B7F8E),
                            style = MaterialTheme.typography.labelSmall,
                        )
                    }
                }
            }
        }

        // Video controls
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(10.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Button(onClick = { openVideoLauncher.launch(arrayOf("video/*")) }) {
                    Text("Open Video")
                }
                Button(
                    onClick = {
                        if (isPlaying) pauseBoth() else playBoth()
                    },
                    enabled = selectedUri != null,
                ) {
                    Text(if (isPlaying) "Pause" else "Play")
                }
                Button(
                    onClick = { showAdvancedControls = !showAdvancedControls },
                    contentPadding = androidx.compose.foundation.layout.PaddingValues(
                        horizontal = 12.dp,
                        vertical = 8.dp,
                    ),
                ) {
                    Text(if (showAdvancedControls) "−" else "+")
                }
            }
            Text(
                text = displayNameForUri(context, selectedUri),
                modifier = Modifier.fillMaxWidth(),
                color = Color(0xFFD9E2EC),
            )
        }

        if (showAdvancedControls) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(Color(0xFF182028), RoundedCornerShape(12.dp))
                    .padding(12.dp),
                verticalArrangement = Arrangement.spacedBy(10.dp),
            ) {
                Text(
                    text = "Build ${BuildConfig.GIT_HEAD} ${BuildConfig.GIT_SUBJECT}",
                    color = Color(0xFF7F93A7),
                    style = MaterialTheme.typography.bodySmall,
                )
                Text(
                    text = connectionStatusText,
                    color = Color(0xFF9CB0C3),
                    style = MaterialTheme.typography.bodyMedium,
                )
                Text(
                    text = "Native: $backendStatus",
                    color = Color(0xFF6B7F8E),
                    style = MaterialTheme.typography.bodySmall,
                )
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    Switch(
                        checked = localMirrorEnabled,
                        onCheckedChange = {
                            setLocalMirrorEnabled(it)
                        },
                    )
                    Text("Local mirror", color = Color(0xFFD9E2EC))
                    Spacer(modifier = Modifier.width(12.dp))
                    Switch(
                        checked = hwEncodeEnabled,
                        onCheckedChange = {
                            setHwEncodeEnabled(it)
                        },
                    )
                    Text("HW enc", color = Color(0xFFD9E2EC))
                }
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    Switch(
                        checked = localSoundEnabled,
                        onCheckedChange = {
                            setLocalSoundEnabled(it)
                        },
                    )
                    Text("Local sound", color = Color(0xFFD9E2EC))
                    Spacer(modifier = Modifier.width(12.dp))
                    Switch(
                        checked = videoPassthroughEnabled,
                        onCheckedChange = {
                            setVideoPassthroughEnabled(it)
                        },
                    )
                    Text("Video PT", color = Color(0xFFD9E2EC))
                }
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    Button(
                        onClick = {
                            viewport = ViewportState()
                            backend.updateViewport(viewport)
                        },
                    ) {
                        Text("Reset View")
                    }
                    Text(
                        text = if (videoPassthroughEnabled) {
                            "Passthrough enabled"
                        } else {
                            "Passthrough off by default"
                        },
                        color = Color(0xFF9CB0C3),
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
                SettingsRow(
                    context,
                    backend,
                    connectedDevice,
                    coroutineScope,
                    brightnessLevel,
                    ::setBrightnessLevel,
                )
            }
        } else {
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(
                    text = "Brightness ${if (brightnessLevel > 0) "+" else ""}$brightnessLevel",
                    color = Color(0xFFD9E2EC),
                    style = MaterialTheme.typography.labelMedium,
                )
                Slider(
                    value = brightnessLevel.toFloat(),
                    onValueChange = { setBrightnessLevel(it.toInt()) },
                    valueRange = -200f..200f,
                )
            }
        }

        if (localMirrorEnabled) {
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(320.dp)
                    .clip(RoundedCornerShape(24.dp))
                    .background(Color.Black)
                    .pointerInput(Unit) {
                        detectTransformGestures { _, pan, zoom, _ ->
                            val newZoom = (viewport.zoom * zoom).coerceIn(1f, 8f)
                            val zoomRatio =
                                if (newZoom == 0f) 1f else newZoom / viewport.zoom
                            val nextViewport = viewport.copy(
                                zoom = newZoom,
                                offsetX = (viewport.offsetX + pan.x * zoomRatio)
                                    .coerceIn(-1200f, 1200f),
                                offsetY = (viewport.offsetY + pan.y * zoomRatio)
                                    .coerceIn(-1200f, 1200f),
                            )
                            viewport = nextViewport
                            connection.updateViewport(nextViewport)
                        }
                    },
                contentAlignment = Alignment.Center,
            ) {
                AndroidView(
                    modifier = Modifier
                        .fillMaxSize()
                        .graphicsLayer {
                            scaleX = viewport.zoom
                            scaleY = viewport.zoom
                            translationX = viewport.offsetX
                            translationY = viewport.offsetY
                            clip = true
                        },
                    factory = { androidContext ->
                        PlayerView(androidContext).apply {
                            player = exoPlayer
                            useController = false
                            layoutParams = android.view.ViewGroup.LayoutParams(
                                MATCH_PARENT, MATCH_PARENT)
                        }
                    },
                    update = { it.player = exoPlayer },
                )
                // Fullscreen button overlay
                Button(
                    onClick = {
                        isFullscreen = true
                        prefs.edit().putBoolean("fullscreen", true).apply()
                    },
                    modifier = Modifier
                        .align(Alignment.TopEnd)
                        .padding(top = 8.dp, end = 72.dp)
                        .size(36.dp),
                    contentPadding = androidx.compose.foundation.layout.PaddingValues(0.dp),
                    colors = androidx.compose.material3.ButtonDefaults.buttonColors(
                        containerColor = Color.Black.copy(alpha = 0.5f)),
                ) {
                    Text("[ ]", color = Color.White, fontSize = 12.sp)
                }
            }
        } else {
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(320.dp)
                    .background(Color(0xFF182028), RoundedCornerShape(24.dp)),
                contentAlignment = Alignment.Center,
            ) {
                Text("Local mirror disabled", color = Color(0xFF9CB0C3))
            }
        }

        Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Slider(
                value = sliderValue,
                onValueChange = {
                    sliderDragging = true
                    sliderValue = it
                    positionMs = (durationMs * it).toLong()
                    seekBoth(positionMs)
                },
                onValueChangeFinished = {
                    sliderDragging = false
                    seekBoth(positionMs, forceCastSeek = true)
                },
                enabled = durationMs > 0L,
            )
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
            ) {
                Text(formatTime(positionMs), color = Color(0xFFD9E2EC))
                Text(formatTime(durationMs), color = Color(0xFFD9E2EC))
            }
        }
    }
}

@Composable
private fun SettingsRow(
    context: Context,
    backend: NativeBackedBackend,
    connectedDevice: CastDevice?,
    coroutineScope: kotlinx.coroutines.CoroutineScope,
    brightnessLevel: Int,
    onBrightnessChange: (Int) -> Unit,
) {
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            var bufferText by rememberSaveable { mutableStateOf("400") }
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text("Buffer", color = Color(0xFF6B7F8E), style = MaterialTheme.typography.labelSmall)
                androidx.compose.material3.OutlinedTextField(
                    value = bufferText,
                    onValueChange = { new ->
                        bufferText = new.filter { it.isDigit() }
                        bufferText.toIntOrNull()?.let { backend.setPlayoutDelay(it) }
                    },
                    modifier = Modifier.width(80.dp),
                    textStyle = androidx.compose.ui.text.TextStyle(
                        color = Color(0xFFD9E2EC),
                        fontSize = 14.sp,
                    ),
                    singleLine = true,
                )
                Text("ms", color = Color(0xFF6B7F8E), style = MaterialTheme.typography.labelSmall)
            }
            var offsetText by rememberSaveable { mutableStateOf("0") }
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text("A/V sync", color = Color(0xFF6B7F8E), style = MaterialTheme.typography.labelSmall)
                androidx.compose.material3.OutlinedTextField(
                    value = offsetText,
                    onValueChange = { new ->
                        offsetText = new.filter { it.isDigit() || it == '-' }
                        offsetText.toLongOrNull()?.let { backend.setAvSyncOffset(it) }
                    },
                    modifier = Modifier.width(80.dp),
                    textStyle = androidx.compose.ui.text.TextStyle(
                        color = Color(0xFFD9E2EC),
                        fontSize = 14.sp,
                    ),
                    singleLine = true,
                )
                Text("ms", color = Color(0xFF6B7F8E), style = MaterialTheme.typography.labelSmall)
            }
            val activity = context as? MainActivity
            var calibrating by remember { mutableStateOf(false) }
            Button(
                onClick = {
                    if (activity == null) return@Button
                    val calibrator = AvSyncCalibrator(context)
                    if (!calibrator.hasPermissions()) {
                        activity.requestCalibrationPermissions()
                        return@Button
                    }
                    calibrating = true
                    val syncFile = java.io.File(context.cacheDir, "sync_test.mp4")
                    if (!syncFile.exists()) {
                        context.resources.openRawResource(R.raw.sync_test).use { input ->
                            syncFile.outputStream().use { output -> input.copyTo(output) }
                        }
                    }
                    val target = connectedDevice?.target ?: ""
                    backend.testCast(target, syncFile.absolutePath)
                    coroutineScope.launch {
                        delay(3000)
                        val result = calibrator.calibrate(activity)
                        calibrating = false
                        Toast.makeText(context, result.message, Toast.LENGTH_LONG).show()
                        if (result.numSamples > 0) {
                            offsetText = result.offsetMs.toString()
                            backend.setAvSyncOffset(result.offsetMs)
                        }
                    }
                },
                enabled = !calibrating,
            ) {
                Text(if (calibrating) "..." else "Cal")
            }
        }
        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(
                text = "Brightness ${if (brightnessLevel > 0) "+" else ""}$brightnessLevel",
                color = Color(0xFFD9E2EC),
                style = MaterialTheme.typography.labelMedium,
            )
            Slider(
                value = brightnessLevel.toFloat(),
                onValueChange = { onBrightnessChange(it.toInt()) },
                valueRange = -200f..200f,
            )
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun FullscreenPlayer(
    exoPlayer: ExoPlayer,
    viewport: ViewportState,
    onViewportChange: (ViewportState) -> Unit,
    isPlaying: Boolean,
    onPlayPause: () -> Unit,
    sliderValue: Float,
    onSliderChange: (Float) -> Unit,
    onSliderFinished: () -> Unit,
    sliderEnabled: Boolean,
    positionMs: Long,
    durationMs: Long,
    onExitFullscreen: () -> Unit,
) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
            .pointerInput(Unit) {
                detectTransformGestures { _, pan, zoom, _ ->
                    val newZoom = (viewport.zoom * zoom).coerceIn(1f, 8f)
                    val zoomRatio = if (newZoom == 0f) 1f else newZoom / viewport.zoom
                    onViewportChange(viewport.copy(
                        zoom = newZoom,
                        offsetX = (viewport.offsetX + pan.x * zoomRatio).coerceIn(-2000f, 2000f),
                        offsetY = (viewport.offsetY + pan.y * zoomRatio).coerceIn(-2000f, 2000f),
                    ))
                }
            },
    ) {
        AndroidView(
            modifier = Modifier
                .fillMaxSize()
                .graphicsLayer {
                    scaleX = viewport.zoom
                    scaleY = viewport.zoom
                    translationX = viewport.offsetX
                    translationY = viewport.offsetY
                },
            factory = { ctx ->
                PlayerView(ctx).apply {
                    player = exoPlayer
                    useController = false
                    layoutParams = android.view.ViewGroup.LayoutParams(MATCH_PARENT, MATCH_PARENT)
                }
            },
            update = { it.player = exoPlayer },
        )
        // Overlay controls at bottom, above navigation bar
        Column(
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .fillMaxWidth()
                .background(Color.Black.copy(alpha = 0.5f))
                .navigationBarsPadding()
                .padding(horizontal = 16.dp, vertical = 8.dp),
        ) {
            Slider(
                value = sliderValue,
                onValueChange = onSliderChange,
                onValueChangeFinished = onSliderFinished,
                enabled = sliderEnabled,
                colors = androidx.compose.material3.SliderDefaults.colors(
                    thumbColor = Color.White,
                    activeTrackColor = Color.White,
                ),
            )
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(formatTime(positionMs), color = Color.White)
                Button(onClick = onPlayPause) {
                    Text(if (isPlaying) "Pause" else "Play")
                }
                Button(onClick = onExitFullscreen) {
                    Text("Exit")
                }
                Text(formatTime(durationMs), color = Color.White)
            }
        }
    }
}

private fun formatTime(valueMs: Long): String {
    val totalSeconds = (valueMs / 1000).coerceAtLeast(0L)
    val hours = totalSeconds / 3600
    val minutes = (totalSeconds % 3600) / 60
    val seconds = totalSeconds % 60
    return if (hours > 0) {
        "%d:%02d:%02d".format(hours, minutes, seconds)
    } else {
        "%02d:%02d".format(minutes, seconds)
    }
}
