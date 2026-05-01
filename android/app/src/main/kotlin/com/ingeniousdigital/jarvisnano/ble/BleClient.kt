package com.ingeniousdigital.jarvisnano.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.content.Context
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Phase 2 — BLE scan + GATT skeleton.
 *
 * What this class does today:
 *   - Scans for advertising devices whose name starts with one of [BleConstants.NAME_PREFIXES].
 *   - Connects GATT and discovers services so we can verify the firmware is exposing
 *     [BleConstants.SERVICE_UUID].
 *   - Exposes [state] for the UI to render a "BLE: discovered" pill.
 *
 * What this class does NOT do yet (Phase 2):
 *   - Subscribe to audio_in / state notifies.
 *   - Push frames to audio_out.
 *   - Send commands to control.
 *   - Reconnect on RSSI loss.
 *
 * Permissions are caller-checked. On Android 12+ ([android.os.Build.VERSION_CODES.S]),
 * `BLUETOOTH_SCAN` and `BLUETOOTH_CONNECT` must be granted at runtime before invoking
 * any method here.
 */
class BleClient(private val context: Context) {

    sealed interface State {
        data object Idle : State
        data object Scanning : State
        data class Found(val device: BluetoothDevice) : State
        data object Connecting : State
        data class Connected(val device: BluetoothDevice) : State
        data class Failed(val reason: String) : State
    }

    private val _state = MutableStateFlow<State>(State.Idle)
    val state: StateFlow<State> = _state.asStateFlow()

    private val manager: BluetoothManager? =
        context.applicationContext.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
    private val adapter: BluetoothAdapter? get() = manager?.adapter
    private var gatt: BluetoothGatt? = null

    @SuppressLint("MissingPermission")
    fun startScan() {
        val scanner = adapter?.bluetoothLeScanner
        if (scanner == null) {
            _state.value = State.Failed("BLE not available on this device")
            return
        }
        _state.value = State.Scanning
        scanner.startScan(scanCallback)
    }

    @SuppressLint("MissingPermission")
    fun stopScan() {
        adapter?.bluetoothLeScanner?.stopScan(scanCallback)
        if (_state.value is State.Scanning) _state.value = State.Idle
    }

    @SuppressLint("MissingPermission")
    fun connect(device: BluetoothDevice) {
        _state.value = State.Connecting
        gatt?.close()
        gatt = device.connectGatt(context, /* autoConnect = */ false, gattCallback)
    }

    @SuppressLint("MissingPermission")
    fun disconnect() {
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        _state.value = State.Idle
    }

    // ---- callbacks ------------------------------------------------------

    private val scanCallback = object : ScanCallback() {
        @SuppressLint("MissingPermission")
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val name = result.device?.name ?: return
            if (BleConstants.NAME_PREFIXES.none { name.startsWith(it, ignoreCase = true) }) return
            _state.value = State.Found(result.device)
            stopScan()
        }

        override fun onScanFailed(errorCode: Int) {
            _state.value = State.Failed("scan failed: $errorCode")
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    _state.value = State.Connected(g.device)
                    g.discoverServices()
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    _state.value = State.Idle
                    g.close()
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            // Phase 2 — verify [BleConstants.SERVICE_UUID] is present, then enable notifies on
            // CHAR_AUDIO_IN and CHAR_STATE. For now we just log presence.
            val present = g.getService(BleConstants.SERVICE_UUID) != null
            if (!present) {
                _state.value = State.Failed("device does not advertise the JarvisNano GATT service")
            }
        }
    }
}
