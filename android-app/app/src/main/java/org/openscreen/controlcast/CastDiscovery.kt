package org.openscreen.controlcast

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

data class CastDevice(
    val name: String,
    val host: String,
    val port: Int,
) {
    val target: String
        get() {
            val formattedHost = if (host.contains(":") && !host.startsWith("[")) {
                "[$host]"
            } else {
                host
            }
            return "$formattedHost:$port"
        }
}

// Discovers Cast receivers on the local network using Android's NsdManager
// (mDNS/DNS-SD). Looks for "_googlecast._tcp" services, the same service
// type that x11cast discovers via Open Screen's DnsSdServiceWatcher.
class CastDiscovery(
    context: Context,
) {

    private val nsdManager =
        context.getSystemService(Context.NSD_SERVICE) as NsdManager

    private val mutableDevices = MutableStateFlow<List<CastDevice>>(emptyList())
    val devices: StateFlow<List<CastDevice>> = mutableDevices

    private val deviceMap = LinkedHashMap<String, CastDevice>()
    private var listener: NsdManager.DiscoveryListener? = null

    fun startDiscovery() {
        if (listener != null) return

        val discoveryListener = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) {}

            override fun onServiceFound(serviceInfo: NsdServiceInfo) {
                nsdManager.resolveService(serviceInfo, ResolveListener())
            }

            override fun onServiceLost(serviceInfo: NsdServiceInfo) {
                synchronized(deviceMap) {
                    deviceMap.remove(serviceInfo.serviceName)
                    mutableDevices.value = deviceMap.values.toList()
                }
            }

            override fun onDiscoveryStopped(serviceType: String) {}
            override fun onStartDiscoveryFailed(serviceType: String, err: Int) {}
            override fun onStopDiscoveryFailed(serviceType: String, err: Int) {}
        }

        listener = discoveryListener
        nsdManager.discoverServices(
            SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, discoveryListener)
    }

    fun stopDiscovery() {
        listener?.let {
            try {
                nsdManager.stopServiceDiscovery(it)
            } catch (_: IllegalArgumentException) {
                // Already stopped.
            }
        }
        listener = null
    }

    private inner class ResolveListener : NsdManager.ResolveListener {
        override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) {}

        override fun onServiceResolved(serviceInfo: NsdServiceInfo) {
            // TODO: On Android 14+ use NsdServiceInfo.getHostAddresses()
            // instead of the deprecated single-host API here. That will let
            // us choose IPv4 vs IPv6 deliberately instead of taking whichever
            // one resolveService() handed back first.
            val host = serviceInfo.host?.hostAddress ?: return
            val port = serviceInfo.port
            // Cast devices advertise their friendly name in the "fn" TXT
            // record attribute (same field Open Screen's ReceiverInfo uses).
            val friendlyName = serviceInfo.attributes["fn"]
                ?.let { String(it, Charsets.UTF_8) }
                ?: serviceInfo.serviceName

            val device = CastDevice(name = friendlyName, host = host, port = port)
            synchronized(deviceMap) {
                deviceMap[serviceInfo.serviceName] = device
                mutableDevices.value = deviceMap.values.toList()
            }
        }
    }

    companion object {
        private const val SERVICE_TYPE = "_googlecast._tcp"
    }
}
