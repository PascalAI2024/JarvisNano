package com.ingeniousdigital.jarvisnano.discovery

import android.content.Context
import android.net.wifi.WifiManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import javax.jmdns.JmDNS
import javax.jmdns.ServiceEvent
import javax.jmdns.ServiceListener
import kotlin.coroutines.resume

/**
 * Resolves the JarvisNano device on the LAN via mDNS.
 *
 * The firmware advertises itself as `esp-claw.local` under `_http._tcp.local.`.
 * We scan that service type and return the first host whose name starts with
 * `esp-claw` (case-insensitive). Multicast lock is grabbed for the duration so
 * the radio actually delivers the packets to userspace.
 */
class MdnsDiscovery(private val context: Context) {

    /**
     * Returns the resolved host (IP literal or hostname) suitable for HTTP calls.
     * Throws on timeout. Callers should wrap with runCatching {} if they want a
     * recoverable failure path.
     */
    suspend fun findEspClaw(timeoutMs: Long = 8_000L): String = withContext(Dispatchers.IO) {
        val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        val lock = wifi.createMulticastLock(LOCK_TAG).apply {
            setReferenceCounted(false)
            acquire()
        }

        try {
            withTimeout(timeoutMs) {
                val dns = JmDNS.create()
                try {
                    suspendCancellableCoroutine { cont ->
                        val listener = object : ServiceListener {
                            override fun serviceAdded(event: ServiceEvent) {
                                // Resolution is async — request the full record.
                                dns.requestServiceInfo(event.type, event.name, true)
                            }

                            override fun serviceRemoved(event: ServiceEvent) = Unit

                            override fun serviceResolved(event: ServiceEvent) {
                                if (!event.name.startsWith(NAME_PREFIX, ignoreCase = true)) return
                                val info = event.info ?: return
                                val host = info.inet4Addresses.firstOrNull()?.hostAddress
                                    ?: info.hostAddresses.firstOrNull()
                                    ?: return
                                if (cont.isActive) cont.resume(host)
                            }
                        }
                        dns.addServiceListener(SERVICE_TYPE, listener)
                        cont.invokeOnCancellation { runCatching { dns.removeServiceListener(SERVICE_TYPE, listener) } }
                    }
                } finally {
                    runCatching { dns.close() }
                }
            }
        } finally {
            runCatching { lock.release() }
        }
    }

    companion object {
        private const val SERVICE_TYPE = "_http._tcp.local."
        private const val NAME_PREFIX = "esp-claw"
        private const val LOCK_TAG = "jarvisnano-mdns"
    }
}
