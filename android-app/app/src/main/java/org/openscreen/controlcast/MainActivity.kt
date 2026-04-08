package org.openscreen.controlcast

import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
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
import androidx.activity.result.contract.ActivityResultContracts.GetContent
import androidx.activity.result.contract.ActivityResultContracts.OpenDocument
import androidx.activity.result.contract.ActivityResultContracts.PickVisualMedia
import androidx.activity.result.PickVisualMediaRequest
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
import androidx.media3.common.PlaybackException
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
    data class Connect(val target: String, val timeoutMs: Int?) : DebugCommand
    data object Disconnect : DebugCommand
    data class OpenVideo(
        val uri: Uri?,
        val filePath: String?,
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
    data class SetVideoPassthrough(val enabled: Boolean) : DebugCommand
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
            getStringExtra("target")?.takeIf { it.isNotBlank() }?.let {
                DebugCommand.Connect(
                    target = it,
                    timeoutMs = if (hasExtra("timeout_ms")) getIntExtra("timeout_ms", 0) else null,
                )
            }
        "org.openscreen.controlcast.DEBUG_DISCONNECT" -> DebugCommand.Disconnect
        "org.openscreen.controlcast.DEBUG_OPEN_VIDEO" -> {
            val filePath = getStringExtra("file_path")?.trim()?.takeIf { it.isNotEmpty() }
            val uri = data ?: getParcelableExtra(Intent.EXTRA_STREAM)
                ?: getStringExtra("uri")?.let(Uri::parse)
            if (uri != null || filePath != null) {
                DebugCommand.OpenVideo(
                    uri = uri,
                    filePath = filePath,
                    startPlaying = getBooleanExtra("start_playing", false),
                    startPositionMs = getLongExtra("start_position_ms", 0L),
                )
            } else {
                null
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
        "org.openscreen.controlcast.DEBUG_SET_VIDEO_PASSTHROUGH" ->
            DebugCommand.SetVideoPassthrough(getBooleanExtra("enabled", false))
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

private enum class VideoPickerMode {
    DOCUMENTS,
    GALLERY,
    MODERN,
}

private fun videoPickerModeFromPref(raw: String?): VideoPickerMode {
    return VideoPickerMode.entries.firstOrNull { it.name == raw } ?: VideoPickerMode.DOCUMENTS
}

private fun loadInitialPlayoutDelayMs(prefs: SharedPreferences): Int {
    val stored = prefs.getInt("playout_delay_ms", 400)
    val customized = prefs.getBoolean("playout_delay_ms_customized", false)
    if (!customized && stored == 800) {
        prefs.edit().putInt("playout_delay_ms", 400).apply()
        android.util.Log.i(
            "ControlCast",
            "Migrated legacy default playout delay from 800ms to 400ms",
        )
        return 400
    }
    return stored
}

class MainActivity : ComponentActivity() {
    // Saved across rotation via onSaveInstanceState
    var savedPosition = 0L
    var savedPlaying = false
    private val debugCommands = Channel<DebugCommand>(Channel.UNLIMITED)
    private val sharedVideoUris = Channel<Uri>(Channel.UNLIMITED)
    private val videoReadPermissionResults = Channel<Boolean>(Channel.UNLIMITED)
    private var wifiBindingRegistered = false

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
        refreshWifiBinding("onCreate")
        val testTarget = intent?.getStringExtra("test_target")
        val testFile = intent?.getStringExtra("test_file")
        val testCalibrate = intent?.getBooleanExtra("test_calibrate", false) == true
        val testFullscreen = intent?.getBooleanExtra("test_fullscreen", false) == true
        enableEdgeToEdge()
        setContent {
            MaterialTheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = Color(0xFF101317),
                ) {
                    ControlCastApp(testTarget, testFile, testCalibrate, testFullscreen)
                }
            }
        }
        enqueueDebugIntent(intent)
        enqueueSharedVideoIntent(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        enqueueDebugIntent(intent)
        enqueueSharedVideoIntent(intent)
    }

    fun refreshWifiBinding(reason: String) {
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val wifiNetwork = cm.allNetworks.firstOrNull { network ->
            val caps = cm.getNetworkCapabilities(network)
            caps?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true &&
                caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
        }
        if (wifiNetwork != null) {
            cm.bindProcessToNetwork(wifiNetwork)
            android.util.Log.i(
                "ControlCast",
                "refreshWifiBinding reason=$reason boundNetwork=$wifiNetwork",
            )
        } else {
            android.util.Log.w(
                "ControlCast",
                "refreshWifiBinding reason=$reason found no active Wi-Fi network",
            )
        }
        if (wifiBindingRegistered) {
            return
        }
        wifiBindingRegistered = true
        // Keep a persistent network request so the binding is maintained
        // even if the network briefly drops and reconnects.
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .build()
        cm.requestNetwork(request, object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: android.net.Network) {
                cm.bindProcessToNetwork(network)
                android.util.Log.i(
                    "ControlCast",
                    "refreshWifiBinding callback=onAvailable boundNetwork=$network",
                )
            }

            override fun onCapabilitiesChanged(
                network: android.net.Network,
                networkCapabilities: NetworkCapabilities,
            ) {
                if (networkCapabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) {
                    cm.bindProcessToNetwork(network)
                    android.util.Log.i(
                        "ControlCast",
                        "refreshWifiBinding callback=onCapabilitiesChanged boundNetwork=$network",
                    )
                }
            }

            override fun onLost(network: android.net.Network) {
                android.util.Log.w(
                    "ControlCast",
                    "refreshWifiBinding callback=onLost lostNetwork=$network",
                )
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
    fun openVideoDebugPath(
        context: Context,
        filePath: String,
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
        openVideoInternal(
            context,
            uri,
            mirrorLocally,
            startPositionMs,
            startPlaying,
            debugDirectPath = null,
        )
    }

    override fun openVideoDebugPath(
        context: Context,
        filePath: String,
        mirrorLocally: Boolean,
        startPositionMs: Long,
        startPlaying: Boolean,
    ) {
        val debugUri = Uri.fromFile(java.io.File(filePath))
        openVideoInternal(
            context,
            debugUri,
            mirrorLocally,
            startPositionMs,
            startPlaying,
            debugDirectPath = filePath,
        )
    }

    private fun openVideoInternal(
        context: Context,
        uri: Uri,
        mirrorLocally: Boolean,
        startPositionMs: Long,
        startPlaying: Boolean,
        debugDirectPath: String?,
    ) {
        var opened = false
        try {
            openPfd1?.close()
            openPfd2?.close()
            openPfd1 = null
            openPfd2 = null

            // Only file:// URIs are safe to pass through as raw native paths.
            // Shared media and picker/share intents normally arrive as
            // content:// URIs and should stay on the fd-backed path.
            // Debug intents may also provide an explicit raw path to avoid
            // content:// URI grant issues when launched from adb.
            val filePath = debugDirectPath ?: resolveFilePath(context, uri)
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

    fun setConnectTimeoutMs(timeoutMs: Int) {
        nativeSetConnectTimeoutMs(timeoutMs)
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
    private external fun nativeSetConnectTimeoutMs(timeoutMs: Int)
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
    private var expectedRestartUntilRealtimeMs = 0L

    private fun isExpectingRestart(): Boolean =
        target != null && android.os.SystemClock.elapsedRealtime() < expectedRestartUntilRealtimeMs

    private val nativeStateListener: (Boolean, Boolean, String) -> Unit =
        { connected, _, statusText ->
            if (connected) {
                state = State.CONNECTED
                lastError = 0
                expectedRestartUntilRealtimeMs = 0L
                if (target == null) {
                    parseConnectedTarget(statusText)?.let { recoveredTarget ->
                        target = debugTargetToCastDevice(recoveredTarget)
                    }
                }
            } else if (statusText.startsWith("Not connected.")) {
                if (isExpectingRestart()) {
                    state = State.CONNECTING
                } else {
                    state = State.DISCONNECTED
                    if (target != null) {
                        lastError = OsConstants.ENOTCONN
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
                    expectedRestartUntilRealtimeMs = 0L
                    if (target == null) {
                        parseConnectedTarget(backend.status.value)?.let { recoveredTarget ->
                            target = debugTargetToCastDevice(recoveredTarget)
                        }
                    }
                    continue
                }
                if (backend.status.value.startsWith("Not connected.")) {
                    if (isExpectingRestart()) {
                        state = State.CONNECTING
                    } else {
                        state = State.DISCONNECTED
                        if (target != null) {
                            lastError = OsConstants.ENOTCONN
                        }
                    }
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

    fun beginConnect(device: CastDevice): Boolean {
        if (device.target.isBlank()) {
            state = State.DISCONNECTED
            lastError = OsConstants.EINVAL
            return false
        }
        if (target?.target == device.target && state != State.DISCONNECTED) {
            backend.syncStatus()
            if (backend.status.value.startsWith("Connected to ")) {
                state = State.CONNECTED
                lastError = 0
            }
            return false
        }
        target = device
        state = State.CONNECTING
        lastError = 0
        return true
    }

    suspend fun connect(device: CastDevice): Result<Unit> {
        if (device.target.isBlank()) {
            state = State.DISCONNECTED
            lastError = OsConstants.EINVAL
            return Result.failure(IllegalArgumentException("Blank cast target"))
        }
        if (target?.target != device.target || state == State.DISCONNECTED) {
            beginConnect(device)
        } else if (state != State.CONNECTING) {
            backend.syncStatus()
            if (backend.status.value.startsWith("Connected to ")) {
                state = State.CONNECTED
                lastError = 0
            }
            return Result.success(Unit)
        }
        android.util.Log.i("ControlCast", "connect(): starting backend connect target=${device.target}")
        val result = backend.connect(device.target)
        backend.syncStatus()
        if (backend.status.value.startsWith("Connected to ")) {
            state = State.CONNECTED
            lastError = 0
            android.util.Log.i("ControlCast", "connect(): backend connected target=${device.target}")
        } else {
            android.util.Log.w(
                "ControlCast",
                "connect(): backend connect incomplete target=${device.target} status=${backend.status.value}",
            )
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
        if (target != null) {
            expectedRestartUntilRealtimeMs =
                android.os.SystemClock.elapsedRealtime() + 3000L
            state = State.CONNECTING
        }
    }

    fun openVideoDebugPath(
        context: Context,
        filePath: String,
        mirrorLocally: Boolean,
        startPositionMs: Long,
        startPlaying: Boolean,
    ) {
        backend.openVideoDebugPath(
            context,
            filePath,
            mirrorLocally,
            startPositionMs,
            startPlaying,
        )
        if (target != null) {
            expectedRestartUntilRealtimeMs =
                android.os.SystemClock.elapsedRealtime() + 3000L
            state = State.CONNECTING
        }
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

    fun setConnectTimeoutMs(timeoutMs: Int) {
        backend.setConnectTimeoutMs(timeoutMs)
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
private fun ControlCastApp(testTarget: String? = null, testFile: String? = null, testCalibrate: Boolean = false, testFullscreen: Boolean = false) {
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

    var selectedUri by remember { mutableStateOf<Uri?>(null) }
    var localMirrorEnabled by rememberSaveable { mutableStateOf(true) }
    var localSoundEnabled by rememberSaveable { mutableStateOf(false) }
    var hwEncodeEnabled by rememberSaveable { mutableStateOf(true) }
    var videoPassthroughEnabled by rememberSaveable {
        mutableStateOf(prefs.getBoolean("video_passthrough_enabled", false))
    }
    var playoutDelayMs by rememberSaveable {
        mutableIntStateOf(loadInitialPlayoutDelayMs(prefs))
    }
    var connectTimeoutMs by rememberSaveable {
        mutableIntStateOf(prefs.getInt("connect_timeout_ms", 8000))
    }
    var videoPickerMode by rememberSaveable {
        mutableStateOf(videoPickerModeFromPref(prefs.getString("video_picker_mode", null)))
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
    var castOpenedUri by remember { mutableStateOf<String?>(null) }
    var castOpenRestartPending by remember { mutableStateOf(false) }
    var lastCastOpenRequestRealtimeMs by remember { mutableLongStateOf(0L) }
    var reconnectResumeArmed by remember { mutableStateOf(false) }
    var reconnectResumeUri by remember { mutableStateOf<String?>(null) }
    var pendingSharedUri by remember { mutableStateOf<Uri?>(null) }
    var pendingExternalPlayUri by remember { mutableStateOf<String?>(null) }
    var latestSeekTargetMs by remember { mutableLongStateOf(-1L) }
    var latestSeekRealtimeMs by remember { mutableLongStateOf(0L) }
    var lastCastSeekDispatchRealtimeMs by remember { mutableLongStateOf(0L) }
    var pendingCastSeekTargetMs by remember { mutableLongStateOf(-1L) }
    var pendingCastSeekJob by remember { mutableStateOf<Job?>(null) }
    var lastObservedCastPlaying by remember { mutableStateOf(false) }
    var lastObservedCastPosForPlayConfirm by remember { mutableLongStateOf(0L) }
    var pendingCastPlayConfirmation by remember { mutableStateOf(false) }
    var pendingCastPlayBaselineMs by remember { mutableLongStateOf(-1L) }
    var lastPickerLaunchMode by remember { mutableStateOf<VideoPickerMode?>(null) }
    var lastPickerLaunchRealtimeMs by remember { mutableLongStateOf(0L) }
    var lastNoRouteStatus by remember { mutableStateOf<String?>(null) }
    val connectionState = connection.state
    val connectedDevice = connection.target
    val isConnected = connectionState == Connection.State.CONNECTED
    val isConnecting = connectionState == Connection.State.CONNECTING
    val hasLiveCastSession =
        isConnected &&
            backend.isConnected() &&
            !backendStatus.startsWith("Not connected.") &&
            !backendStatus.contains("| no video selected")
    val connectionStatusText = when (connectionState) {
        Connection.State.DISCONNECTED ->
            connectedDevice?.target?.let { "Not connected. Target: $it" } ?: "Not connected."
        Connection.State.CONNECTING ->
            "Connecting to ${connectedDevice?.target ?: connectedDevice?.name ?: "device"}"
        Connection.State.CONNECTED ->
            "Connected to ${connectedDevice?.target ?: connectedDevice?.name ?: "device"}"
    }

    LaunchedEffect(connectTimeoutMs) {
        connection.setConnectTimeoutMs(connectTimeoutMs)
    }

    LaunchedEffect(playoutDelayMs) {
        backend.setPlayoutDelay(playoutDelayMs)
    }

    fun pushSelectedVideoToCastSession(
        uri: Uri,
        startPlaying: Boolean,
        startPositionMs: Long = 0L,
        debugFilePath: String? = null,
    ): Boolean {
        if (connectedDevice == null) return false
        castOpenedUri = uri.toString()
        castOpenRestartPending = true
        lastCastOpenRequestRealtimeMs = android.os.SystemClock.elapsedRealtime()
        if (debugFilePath != null) {
            connection.openVideoDebugPath(
                context,
                debugFilePath,
                localMirrorEnabled,
                startPositionMs,
                startPlaying,
            )
        } else {
            connection.openVideo(
                context,
                uri,
                localMirrorEnabled,
                startPositionMs,
                startPlaying,
            )
        }
        return true
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
        if (startPlaying) {
            pendingCastPlayConfirmation = true
            pendingCastPlayBaselineMs = connection.getCastPositionMs().coerceAtLeast(0L)
        } else {
            pendingCastPlayConfirmation = false
            pendingCastPlayBaselineMs = -1L
        }
        pushSelectedVideoToCastSession(uri, startPlaying, startPositionMs)
    }

    // Connect to a device and optionally send the current video.
    fun connectToDevice(device: CastDevice) {
        activity?.refreshWifiBinding("connectToDevice")
        if (!connection.beginConnect(device)) {
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

    fun setConnectTimeoutMs(timeoutMs: Int) {
        backend.setConnectTimeoutMs(timeoutMs)
    }

    fun setVideoPassthroughEnabled(enabled: Boolean) {
        videoPassthroughEnabled = enabled
        prefs.edit().putBoolean("video_passthrough_enabled", enabled).apply()
        backend.setVideoPassthroughEnabled(enabled)
    }

    fun persistConnectTimeoutMs(timeoutMs: Int) {
        val clamped = timeoutMs.coerceAtLeast(1000)
        connectTimeoutMs = clamped
        prefs.edit().putInt("connect_timeout_ms", clamped).apply()
        connection.setConnectTimeoutMs(clamped)
    }

    fun persistPlayoutDelayMs(delayMs: Int) {
        val clamped = delayMs.coerceAtLeast(100)
        playoutDelayMs = clamped
        prefs.edit()
            .putInt("playout_delay_ms", clamped)
            .putBoolean("playout_delay_ms_customized", true)
            .apply()
        backend.setPlayoutDelay(clamped)
    }

    fun setBrightnessLevel(level: Int) {
        val clamped = level.coerceIn(-200, 200)
        brightnessLevel = clamped
        backend.setBrightness(clamped)
    }

    fun pauseBoth() {
        pendingExternalPlayUri = null
        pendingCastPlayConfirmation = false
        pendingCastPlayBaselineMs = -1L
        exoPlayer.pause()
        connection.pause()
        isPlaying = false
        reconnectResumeArmed = false
    }

    fun playBoth() {
        pendingExternalPlayUri = null
        connection.play()
        if (connectionState == Connection.State.CONNECTED) {
            // While Cast is connected, native playback state is authoritative.
            // Do not start local preview optimistically or it can run alone if
            // the receiver stays paused at EOF.
            pendingCastPlayConfirmation = true
            pendingCastPlayBaselineMs = connection.getCastPositionMs().coerceAtLeast(0L)
            exoPlayer.pause()
            isPlaying = false
        } else {
            if (localMirrorEnabled) {
                exoPlayer.play()
            } else {
                exoPlayer.pause()
            }
            isPlaying = true
        }
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
        val shouldRemainPaused = !isPlaying
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
        if (shouldRemainPaused) {
            exoPlayer.pause()
            isPlaying = false
        }
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
        val liveConnectionState = connection.state
        val liveConnectedDevice = connection.target
        val liveCastConnected =
            backend.isConnected() &&
                !backend.status.value.startsWith("Not connected.") &&
                !backend.status.value.contains("| no video selected")
        val exoHasMedia = exoPlayer.mediaItemCount > 0
        val exoState = exoPlayer.playbackState
        val exoPlayWhenReady = exoPlayer.playWhenReady
        val pendingPlayConfirm = pendingCastPlayConfirmation
        val pendingPlayBaseline = pendingCastPlayBaselineMs
        android.util.Log.i(
            "ControlCast",
            buildString {
                append("DEBUG_PRINT reason=").append(reason)
                append(" connection=").append(liveConnectionState)
                append(" target=").append(liveConnectedDevice?.target ?: "")
                append(" selectedUri=").append(selectedUri ?: "")
                append(" isPlaying=").append(isPlaying)
                append(" restored=").append(restored)
                append(" exoHasMedia=").append(exoHasMedia)
                append(" exoState=").append(exoState)
                append(" exoPlayWhenReady=").append(exoPlayWhenReady)
                append(" exoPos=").append(exoPos)
                append(" exoDur=").append(exoPlayer.duration.coerceAtLeast(0L))
                append(" castPos=").append(castPos)
                append(" castDur=").append(connection.getCastDurationMs())
                append(" castPlaying=").append(connection.isCastPlaying())
                append(" liveCastConnected=").append(liveCastConnected)
                append(" pendingCastPlayConfirmation=").append(pendingPlayConfirm)
                append(" pendingCastPlayBaselineMs=").append(pendingPlayBaseline)
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
        uri: Uri?,
        filePath: String?,
        startPlaying: Boolean,
        startPositionMs: Long,
    ) {
        val effectiveUri = uri ?: filePath?.let { Uri.fromFile(java.io.File(it)) } ?: return
        val stagingReconnect = connectedDevice != null && !hasLiveCastSession
        pendingExternalPlayUri = effectiveUri.toString().takeIf { startPlaying }
        reconnectResumeArmed = false
        reconnectResumeUri = effectiveUri.toString()
        selectedUri = effectiveUri
        val mediaItem = MediaItem.fromUri(effectiveUri)
        exoPlayer.setMediaItem(mediaItem)
        exoPlayer.prepare()
        exoPlayer.seekTo(startPositionMs.coerceAtLeast(0L))
        if (localMirrorEnabled && startPlaying && !stagingReconnect) {
            exoPlayer.play()
        } else {
            exoPlayer.pause()
        }
        isPlaying = startPlaying && !stagingReconnect
        positionMs = startPositionMs.coerceAtLeast(0L)
        sliderValue = if (durationMs > 0L) {
            positionMs.toFloat() / durationMs.toFloat()
        } else {
            0f
        }
        if (connectionState == Connection.State.CONNECTED) {
            if (filePath != null) {
                pushSelectedVideoToCastSession(
                    effectiveUri,
                    startPlaying,
                    positionMs,
                    debugFilePath = filePath,
                )
            } else {
                openSelectedVideoOnCast(effectiveUri, startPlaying, positionMs)
            }
        } else if (connectedDevice != null) {
            if (filePath != null) {
                pushSelectedVideoToCastSession(
                    effectiveUri,
                    startPlaying,
                    positionMs,
                    debugFilePath = filePath,
                )
            } else {
                pushSelectedVideoToCastSession(effectiveUri, startPlaying, positionMs)
            }
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
        val shouldStartPlaying = true
        // Keep local paused until Cast is connected/opened so the app does not
        // race ahead from a cold start.
        pendingExternalPlayUri = uri.toString()
        reconnectResumeArmed =
            shouldStartPlaying && connectionState != Connection.State.CONNECTED
        reconnectResumeUri = uri.toString()
        selectedUri = uri
        val mediaItem = MediaItem.fromUri(uri)
        exoPlayer.setMediaItem(mediaItem)
        exoPlayer.prepare()
        // Shared opens must not let local preview run ahead of the TV. Keep it
        // paused until the Cast session becomes authoritative.
        exoPlayer.pause()
        isPlaying = false
        pendingCastPlayConfirmation = false
        pendingCastPlayBaselineMs = -1L
        if (connectionState == Connection.State.CONNECTED) {
            openSelectedVideoOnCast(uri, shouldStartPlaying, startPositionMs = 0L)
            pendingExternalPlayUri = null
        } else if (connectedDevice != null) {
            pushSelectedVideoToCastSession(uri, shouldStartPlaying, startPositionMs = 0L)
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
        val listener = object : Player.Listener {
            override fun onPlaybackStateChanged(playbackState: Int) {
                android.util.Log.i(
                    "ControlCast",
                    "exo onPlaybackStateChanged state=$playbackState playWhenReady=${exoPlayer.playWhenReady} isPlaying=${exoPlayer.isPlaying} pos=${exoPlayer.currentPosition.coerceAtLeast(0L)} uri=${exoPlayer.currentMediaItem?.localConfiguration?.uri}",
                )
            }

            override fun onIsPlayingChanged(isPlaying: Boolean) {
                android.util.Log.i(
                    "ControlCast",
                    "exo onIsPlayingChanged isPlaying=$isPlaying playWhenReady=${exoPlayer.playWhenReady} state=${exoPlayer.playbackState} pos=${exoPlayer.currentPosition.coerceAtLeast(0L)} uri=${exoPlayer.currentMediaItem?.localConfiguration?.uri}",
                )
            }

            override fun onPlayerError(error: PlaybackException) {
                android.util.Log.e(
                    "ControlCast",
                    "exo onPlayerError code=${error.errorCode} name=${error.errorCodeName} message=${error.message} cause=${error.cause} uri=${exoPlayer.currentMediaItem?.localConfiguration?.uri}",
                )
            }
        }
        exoPlayer.addListener(listener)
        onDispose {
            exoPlayer.removeListener(listener)
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
            val recentOpenRestart =
                lastCastOpenRequestRealtimeMs > 0L &&
                    android.os.SystemClock.elapsedRealtime() - lastCastOpenRequestRealtimeMs < 3000L
            android.util.Log.i(
                "ControlCast",
                "connectionState DISCONNECTED: castOpenedUri=$castOpenedUri pendingRestart=$castOpenRestartPending recentOpenRestart=$recentOpenRestart selectedUri=$selectedUri isPlaying=$isPlaying",
            )
            if (!castOpenRestartPending && !recentOpenRestart) {
                castOpenedUri = null
            }
            val currentUri = selectedUri?.toString()
            if (isPlaying && currentUri != null) {
                reconnectResumeArmed = true
                reconnectResumeUri = currentUri
                exoPlayer.pause()
                isPlaying = false
            } else if (currentUri == null) {
                castOpenRestartPending = false
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
                val shouldPlayPendingExternal = pendingExternalPlayUri == uriString
                android.util.Log.i(
                    "ControlCast",
                    "connected effect uri=$uriString castOpenedUri=$castOpenedUri shouldResumeAfterReconnect=$shouldResumeAfterReconnect shouldPlayPendingExternal=$shouldPlayPendingExternal exoPlaying=${exoPlayer.isPlaying} reconnectResumeArmed=$reconnectResumeArmed",
                )
                if (castOpenedUri == uriString) {
                    castOpenRestartPending = false
                    if (shouldPlayPendingExternal) {
                        pendingExternalPlayUri = null
                    }
                }
                if (castOpenedUri != uriString) {
                    openSelectedVideoOnCast(
                        uri,
                        shouldPlayPendingExternal || shouldResumeAfterReconnect || exoPlayer.isPlaying,
                        if (shouldPlayPendingExternal) 0L else exoPlayer.currentPosition.coerceAtLeast(0L),
                    )
                    if (shouldPlayPendingExternal) {
                        pendingExternalPlayUri = null
                    }
                }
                if (shouldResumeAfterReconnect && !localMirrorEnabled) {
                    reconnectResumeArmed = false
                }
            }
        }
    }

    LaunchedEffect(activity) {
        val debugFlow = activity?.debugCommandsFlow() ?: return@LaunchedEffect
        debugFlow.collect { command ->
            when (command) {
                is DebugCommand.Connect -> {
                    command.timeoutMs?.let { persistConnectTimeoutMs(it) }
                    activity.refreshWifiBinding("debugConnect")
                    val device = discoveredDevices.firstOrNull { it.target == command.target }
                    if (device != null) {
                        connection.connect(device)
                    } else {
                        debugTargetToCastDevice(command.target)?.let { parsed ->
                            connection.connect(parsed)
                        }
                    }
                }
                DebugCommand.Disconnect -> connection.disconnect()
                is DebugCommand.OpenVideo -> openVideoFromCommand(
                    command.uri,
                    command.filePath,
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
                is DebugCommand.SetVideoPassthrough -> setVideoPassthroughEnabled(command.enabled)
                DebugCommand.Print -> printDebugState("intent")
            }
        }
    }

    LaunchedEffect(activity, backendStatus) {
        val currentActivity = activity ?: return@LaunchedEffect
        if (backendStatus.contains("No route to host")) {
            if (lastNoRouteStatus != backendStatus) {
                lastNoRouteStatus = backendStatus
                currentActivity.refreshWifiBinding("no_route_to_host")
            }
        } else {
            lastNoRouteStatus = null
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
        android.util.Log.i("ControlCast", "poll loop started")
        while (true) {
            try {
                val castConnected = connectionState == Connection.State.CONNECTED
                val liveCastConnected =
                    backend.isConnected() &&
                        !backend.status.value.startsWith("Not connected.") &&
                        !backend.status.value.contains("| no video selected")
                val castPos = connection.getCastPositionMs().coerceAtLeast(0L)
                val castDur = connection.getCastDurationMs().coerceAtLeast(0L)
                val castPlaying = connection.isCastPlaying()
                val exoPos = exoPlayer.currentPosition.coerceAtLeast(0L)
                val exoHasMedia = exoPlayer.mediaItemCount > 0
                val inferredCastPlaying =
                    pendingCastPlayConfirmation &&
                        castPos > pendingCastPlayBaselineMs + 250L &&
                        castPos > lastObservedCastPosForPlayConfirm
                val effectiveCastPlaying = castPlaying || inferredCastPlaying
                if (connectedDevice != null && !liveCastConnected && exoHasMedia) {
                    exoPlayer.pause()
                }
                durationMs = if (castConnected && castDur > 0L) {
                    castDur
                } else if (exoHasMedia) {
                    exoPlayer.duration.coerceAtLeast(0L)
                } else {
                    castDur
                }
                if (castConnected && exoHasMedia && restored) {
                    if (localMirrorEnabled && !exoPlayer.isPlaying && castPos > 0L) {
                        android.util.Log.i(
                            "ControlCast",
                            "poll local-idle castConnected=$castConnected liveCastConnected=$liveCastConnected restored=$restored exoHasMedia=$exoHasMedia castPlaying=$castPlaying inferredCastPlaying=$inferredCastPlaying effectiveCastPlaying=$effectiveCastPlaying playWhenReady=${exoPlayer.playWhenReady} exoState=${exoPlayer.playbackState} exoPos=$exoPos castPos=$castPos selectedUri=$selectedUri",
                        )
                    }
                    val castStartedThisTick = effectiveCastPlaying && !lastObservedCastPlaying
                    if (castStartedThisTick && castPos > 0L) {
                        exoPlayer.seekTo(castPos)
                    }
                    if (effectiveCastPlaying &&
                        reconnectResumeArmed &&
                        reconnectResumeUri == selectedUri?.toString()) {
                        reconnectResumeArmed = false
                    }
                    if (effectiveCastPlaying) {
                        pendingCastPlayConfirmation = false
                        pendingCastPlayBaselineMs = -1L
                    }
                    val driftMs = kotlin.math.abs(exoPos - castPos)
                    val recentSeek = latestSeekRealtimeMs > 0L &&
                        android.os.SystemClock.elapsedRealtime() - latestSeekRealtimeMs < 2500L
                    val syncThresholdMs = if (recentSeek) 150L else 300L
                    if (!sliderDragging && castPos > 0L && driftMs > syncThresholdMs) {
                        exoPlayer.seekTo(castPos)
                    }
                    if (effectiveCastPlaying) {
                        if (localMirrorEnabled) {
                            exoPlayer.play()
                        } else {
                            exoPlayer.pause()
                        }
                    } else {
                        exoPlayer.pause()
                    }
                }
                lastObservedCastPlaying = effectiveCastPlaying
                lastObservedCastPosForPlayConfirm = castPos
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
                    liveCastConnected && effectiveCastPlaying
                } else {
                    if (exoHasMedia) exoPlayer.isPlaying else effectiveCastPlaying
                }
            } catch (t: Throwable) {
                android.util.Log.e("ControlCast", "poll loop failed", t)
                throw t
            }
            delay(200)
        }
    }

    fun handlePickedVideo(uri: Uri?, persistable: Boolean) {
        val launchMode = lastPickerLaunchMode
        val launchAgeMs =
            if (lastPickerLaunchRealtimeMs > 0L) {
                android.os.SystemClock.elapsedRealtime() - lastPickerLaunchRealtimeMs
            } else {
                -1L
            }
        android.util.Log.i(
            "ControlCast",
            "picker result mode=${launchMode ?: "unknown"} ageMs=$launchAgeMs uri=${uri ?: "null"} persistable=$persistable",
        )
        if (uri != null) {
            val shouldStartPlaying = true
            val openingWhileConnected = hasLiveCastSession
            val stagingReconnect = connectedDevice != null && !hasLiveCastSession
            pendingExternalPlayUri = uri.toString()
            reconnectResumeArmed = false
            reconnectResumeUri = uri.toString()
            if (persistable) {
                try {
                    context.contentResolver.takePersistableUriPermission(
                        uri,
                        android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION,
                    )
                } catch (_: SecurityException) {
                } catch (_: IllegalArgumentException) {
                }
            }
            selectedUri = uri
            val mediaItem = MediaItem.fromUri(uri)
            exoPlayer.setMediaItem(mediaItem)
            exoPlayer.prepare()
            if (localMirrorEnabled && shouldStartPlaying &&
                !openingWhileConnected && !stagingReconnect) {
                exoPlayer.play()
            } else {
                exoPlayer.pause()
            }
            // During an in-place Cast reopen, keep local preview paused until the
            // new Cast session becomes authoritative again.
            isPlaying = shouldStartPlaying && !openingWhileConnected && !stagingReconnect
            if (openingWhileConnected) {
                openSelectedVideoOnCast(uri, shouldStartPlaying)
            } else if (connectedDevice != null) {
                pushSelectedVideoToCastSession(uri, shouldStartPlaying)
            }
        }
        lastPickerLaunchMode = null
        lastPickerLaunchRealtimeMs = 0L
    }

    val openDocumentVideoLauncher = rememberLauncherForActivityResult(OpenDocument()) { uri ->
        handlePickedVideo(uri, persistable = true)
    }

    val getContentVideoLauncher = rememberLauncherForActivityResult(GetContent()) { uri ->
        handlePickedVideo(uri, persistable = false)
    }

    val pickVisualMediaLauncher = rememberLauncherForActivityResult(
        PickVisualMedia(),
    ) { uri ->
        handlePickedVideo(uri, persistable = false)
    }

    fun launchVideoPicker() {
        lastPickerLaunchMode = videoPickerMode
        lastPickerLaunchRealtimeMs = android.os.SystemClock.elapsedRealtime()
        android.util.Log.i(
            "ControlCast",
            "launchVideoPicker requestedMode=$videoPickerMode",
        )
        when (videoPickerMode) {
            VideoPickerMode.DOCUMENTS -> openDocumentVideoLauncher.launch(arrayOf("video/*"))
            VideoPickerMode.GALLERY -> getContentVideoLauncher.launch("video/*")
            VideoPickerMode.MODERN -> {
                android.util.Log.w(
                    "ControlCast",
                    "launchVideoPicker: MODERN requested but using GALLERY fallback due to photopicker result-delivery instability",
                )
                getContentVideoLauncher.launch("video/*")
            }
        }
    }

    fun setVideoPickerMode(mode: VideoPickerMode) {
        videoPickerMode = mode
        prefs.edit().putString("video_picker_mode", mode.name).apply()
    }

    if (isFullscreen) {
        FullscreenPlayer(
            exoPlayer = exoPlayer,
            viewport = viewport,
            onViewportChange = { viewport = it; connection.updateViewport(it) },
            isPlaying = isPlaying,
            onPlayPause = {
                try {
                    pendingExternalPlayUri = null
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
                Button(onClick = { launchVideoPicker() }) {
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
                    Text(
                        text = "Picker",
                        color = Color(0xFFD9E2EC),
                        modifier = Modifier.width(120.dp),
                    )
                    Button(
                        onClick = {
                            setVideoPickerMode(
                                when (videoPickerMode) {
                                    VideoPickerMode.DOCUMENTS -> VideoPickerMode.GALLERY
                                    VideoPickerMode.GALLERY -> VideoPickerMode.MODERN
                                    VideoPickerMode.MODERN -> VideoPickerMode.DOCUMENTS
                                },
                            )
                        },
                    ) {
                        Text(
                            when (videoPickerMode) {
                                VideoPickerMode.DOCUMENTS -> "Documents"
                                VideoPickerMode.GALLERY -> "Gallery"
                                VideoPickerMode.MODERN -> "Modern"
                            },
                        )
                    }
                    Text(
                        text = when (videoPickerMode) {
                            VideoPickerMode.DOCUMENTS -> "Persistable"
                            VideoPickerMode.GALLERY -> "GET_CONTENT"
                            VideoPickerMode.MODERN -> "Photo Picker"
                        },
                        color = Color(0xFF9CB0C3),
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
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
                    playoutDelayMs,
                    ::persistPlayoutDelayMs,
                    connectTimeoutMs,
                    ::persistConnectTimeoutMs,
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
    playoutDelayMs: Int,
    onPlayoutDelayChange: (Int) -> Unit,
    connectTimeoutMs: Int,
    onConnectTimeoutChange: (Int) -> Unit,
) {
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            var bufferText by rememberSaveable(playoutDelayMs) {
                mutableStateOf(playoutDelayMs.toString())
            }
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text("Buffer", color = Color(0xFF6B7F8E), style = MaterialTheme.typography.labelSmall)
                androidx.compose.material3.OutlinedTextField(
                    value = bufferText,
                    onValueChange = { new ->
                        bufferText = new.filter { it.isDigit() }
                        bufferText.toIntOrNull()?.let { onPlayoutDelayChange(it) }
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
            var timeoutText by rememberSaveable(connectTimeoutMs) {
                mutableStateOf(connectTimeoutMs.toString())
            }
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text("Connect", color = Color(0xFF6B7F8E), style = MaterialTheme.typography.labelSmall)
                androidx.compose.material3.OutlinedTextField(
                    value = timeoutText,
                    onValueChange = { new ->
                        timeoutText = new.filter { it.isDigit() }
                        timeoutText.toIntOrNull()?.let { onConnectTimeoutChange(it) }
                    },
                    modifier = Modifier.width(88.dp),
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
