package com.ingeniousdigital.jarvisnano

import android.app.Application
import com.ingeniousdigital.jarvisnano.data.DeviceClient
import com.ingeniousdigital.jarvisnano.data.DeviceRepository
import com.ingeniousdigital.jarvisnano.discovery.MdnsDiscovery

/**
 * Manual DI container.
 *
 * The companion app is small enough that introducing Koin or Hilt would cost
 * more in build time and ceremony than it saves. Singletons are constructed
 * lazily here and pulled out via [App.deviceRepository] from composables.
 */
class App : Application() {

    val deviceClient: DeviceClient by lazy { DeviceClient() }
    val mdnsDiscovery: MdnsDiscovery by lazy { MdnsDiscovery(this) }
    val deviceRepository: DeviceRepository by lazy {
        DeviceRepository(client = deviceClient, discovery = mdnsDiscovery)
    }

    override fun onCreate() {
        super.onCreate()
        instance = this
    }

    companion object {
        @Volatile private var instance: App? = null
        fun get(): App = instance ?: error("App.get() called before Application.onCreate()")
    }
}
