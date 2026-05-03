package com.ingeniousdigital.jarvisnano.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback.SCAN_FAILED_ALREADY_STARTED
import android.bluetooth.le.ScanCallback.SCAN_FAILED_APPLICATION_REGISTRATION_FAILED
import android.bluetooth.le.ScanCallback.SCAN_FAILED_FEATURE_UNSUPPORTED
import android.bluetooth.le.ScanCallback.SCAN_FAILED_INTERNAL_ERROR
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Phase 2 — BLE scan + GATT skeleton.
 *
 * What this class does today:
 *   - Scans for the Jarvis service UUID when supported, then falls back to
 *     devices whose name starts with one of [BleConstants.NAME_PREFIXES].
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
        data class Found(
            val device: BluetoothDevice,
            val name: String,
            val address: String,
        ) : State
        data object Connecting : State
        data class Connected(
            val device: BluetoothDevice,
            val name: String,
            val address: String,
            val jarvisServicePresent: Boolean? = null,
        ) : State
        data class Failed(val reason: String) : State
    }

    private val _state = MutableStateFlow<State>(State.Idle)
    val state: StateFlow<State> = _state.asStateFlow()

    private val manager: BluetoothManager? =
        context.applicationContext.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
    private val adapter: BluetoothAdapter? get() = manager?.adapter
    private val handler = Handler(Looper.getMainLooper())
    private var gatt: BluetoothGatt? = null
    private var scanTimeout: Runnable? = null
    private var scanFallback: Runnable? = null
    private var connectTimeout: Runnable? = null
    private var activeScanMode: ScanMode = ScanMode.None

    private enum class ScanMode {
        None,
        ServiceUuid,
        NamePrefix,
    }

    @SuppressLint("MissingPermission")
    fun startScan() {
        val bluetoothAdapter = adapter
        val scanner = bluetoothAdapter?.bluetoothLeScanner
        if (scanner == null) {
            _state.value = State.Failed("BLE not available on this device")
            return
        }
        if (!bluetoothAdapter.isEnabled) {
            _state.value = State.Failed("Bluetooth is disabled")
            return
        }
        cancelScanTimers()
        stopActiveScan()
        _state.value = State.Scanning
        val startedWithServiceFilter = bluetoothAdapter.isOffloadedFilteringSupported &&
            startActiveScan(ScanMode.ServiceUuid, reportFailure = false)
        if (!startedWithServiceFilter && !startActiveScan(ScanMode.NamePrefix)) return
        if (startedWithServiceFilter) {
            scanFallback = Runnable {
                if (_state.value is State.Scanning && activeScanMode == ScanMode.ServiceUuid) {
                    stopActiveScan()
                    startActiveScan(ScanMode.NamePrefix)
                }
            }.also { handler.postDelayed(it, SERVICE_FILTER_WINDOW_MS) }
        }
        scanTimeout = Runnable {
            if (_state.value is State.Scanning) {
                stopActiveScan()
                _state.value = State.Failed(
                    "scan timed out after ${SCAN_TIMEOUT_MS / 1_000}s; checked Jarvis service UUID and name prefixes ${
                        BleConstants.NAME_PREFIXES.joinToString("/")
                    }",
                )
            }
        }.also { handler.postDelayed(it, SCAN_TIMEOUT_MS) }
    }

    @SuppressLint("MissingPermission")
    fun stopScan() {
        cancelScanTimers()
        stopActiveScan()
        if (_state.value is State.Scanning) _state.value = State.Idle
    }

    @SuppressLint("MissingPermission")
    fun connect(device: BluetoothDevice) {
        val bluetoothAdapter = adapter
        if (bluetoothAdapter == null) {
            _state.value = State.Failed("BLE not available on this device")
            return
        }
        if (!bluetoothAdapter.isEnabled) {
            _state.value = State.Failed("Bluetooth is disabled")
            return
        }
        stopScan()
        cancelConnectTimer()
        _state.value = State.Connecting
        gatt?.close()
        gatt = try {
            device.connectGatt(
                context,
                /* autoConnect = */ false,
                gattCallback,
                BluetoothDevice.TRANSPORT_LE,
            )
        } catch (error: SecurityException) {
            _state.value = State.Failed("missing Bluetooth connect permission")
            null
        }
        if (gatt == null) {
            _state.value = State.Failed("GATT connection did not start")
            return
        }
        connectTimeout = Runnable {
            val timedOutGatt = gatt ?: return@Runnable
            failAndClose(timedOutGatt, "GATT connect/service discovery timed out after ${CONNECT_TIMEOUT_MS / 1_000}s")
        }.also { handler.postDelayed(it, CONNECT_TIMEOUT_MS) }
    }

    @SuppressLint("MissingPermission")
    fun disconnect() {
        cancelScanTimers()
        cancelConnectTimer()
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        _state.value = State.Idle
    }

    // ---- callbacks ------------------------------------------------------

    private val scanCallback = object : ScanCallback() {
        @SuppressLint("MissingPermission")
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device = result.device ?: return
            val advertisedServices = result.scanRecord?.serviceUuids.orEmpty()
            val serviceMatches = advertisedServices.any { it.uuid == BleConstants.SERVICE_UUID }
            val name = result.scanRecord?.deviceName ?: device.name
            val nameMatches = name?.let { deviceName ->
                BleConstants.NAME_PREFIXES.any { deviceName.startsWith(it, ignoreCase = true) }
            } == true
            if (!serviceMatches && !nameMatches) return
            _state.value = State.Found(
                device = device,
                name = name ?: "BLE device",
                address = device.address ?: "",
            )
            stopScan()
        }

        override fun onScanFailed(errorCode: Int) {
            cancelScanTimers()
            stopActiveScan()
            _state.value = State.Failed("scan failed: ${scanErrorName(errorCode)}")
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (g != gatt) {
                g.close()
                return
            }
            if (status != BluetoothGatt.GATT_SUCCESS) {
                failAndClose(g, "GATT state change failed: ${gattStatusName(status)}")
                return
            }
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    _state.value = State.Connected(
                        device = g.device,
                        name = g.device.name ?: "BLE device",
                        address = g.device.address ?: "",
                    )
                    if (!g.discoverServices()) {
                        failAndClose(g, "GATT service discovery did not start")
                    }
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    cancelConnectTimer()
                    closeGatt(g)
                    _state.value = State.Failed("GATT disconnected")
                }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (g != gatt) {
                g.close()
                return
            }
            cancelConnectTimer()
            if (status != BluetoothGatt.GATT_SUCCESS) {
                failAndClose(g, "GATT service discovery failed: ${gattStatusName(status)}")
                return
            }
            // Phase 2 readiness — report whether firmware exposes the Jarvis service.
            // Characteristic notify/write plumbing lands with the firmware GATT service.
            val present = g.getService(BleConstants.SERVICE_UUID) != null
            _state.value = State.Connected(
                device = g.device,
                name = g.device.name ?: "BLE device",
                address = g.device.address ?: "",
                jarvisServicePresent = present,
            )
        }
    }

    @SuppressLint("MissingPermission")
    private fun startActiveScan(mode: ScanMode, reportFailure: Boolean = true): Boolean {
        val scanner = adapter?.bluetoothLeScanner ?: return false
        val filters = when (mode) {
            ScanMode.ServiceUuid -> listOf(
                ScanFilter.Builder()
                    .setServiceUuid(ParcelUuid(BleConstants.SERVICE_UUID))
                    .build(),
            )
            ScanMode.NamePrefix -> emptyList()
            ScanMode.None -> return false
        }
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        return try {
            scanner.startScan(filters, settings, scanCallback)
            activeScanMode = mode
            true
        } catch (error: SecurityException) {
            if (reportFailure) _state.value = State.Failed("missing Bluetooth scan permission")
            false
        } catch (error: IllegalArgumentException) {
            if (reportFailure) {
                _state.value = State.Failed("scan could not start: ${error.message ?: "invalid scan request"}")
            }
            false
        } catch (error: IllegalStateException) {
            if (reportFailure) {
                _state.value = State.Failed("scan could not start: ${error.message ?: "Bluetooth scanner unavailable"}")
            }
            false
        }
    }

    @SuppressLint("MissingPermission")
    private fun stopActiveScan() {
        if (activeScanMode == ScanMode.None) return
        runCatching { adapter?.bluetoothLeScanner?.stopScan(scanCallback) }
        activeScanMode = ScanMode.None
    }

    private fun cancelScanTimers() {
        scanTimeout?.let(handler::removeCallbacks)
        scanFallback?.let(handler::removeCallbacks)
        scanTimeout = null
        scanFallback = null
    }

    private fun cancelConnectTimer() {
        connectTimeout?.let(handler::removeCallbacks)
        connectTimeout = null
    }

    private fun failAndClose(g: BluetoothGatt, reason: String) {
        cancelConnectTimer()
        closeGatt(g)
        _state.value = State.Failed(reason)
    }

    @SuppressLint("MissingPermission")
    private fun closeGatt(g: BluetoothGatt) {
        if (g == gatt) gatt = null
        runCatching { g.close() }
    }

    private fun scanErrorName(errorCode: Int): String = when (errorCode) {
        SCAN_FAILED_ALREADY_STARTED -> "already started ($errorCode)"
        SCAN_FAILED_APPLICATION_REGISTRATION_FAILED -> "app registration failed ($errorCode)"
        SCAN_FAILED_FEATURE_UNSUPPORTED -> "feature unsupported ($errorCode)"
        SCAN_FAILED_INTERNAL_ERROR -> "internal error ($errorCode)"
        else -> "error $errorCode"
    }

    private fun gattStatusName(status: Int): String = when (status) {
        BluetoothGatt.GATT_SUCCESS -> "success ($status)"
        BluetoothGatt.GATT_READ_NOT_PERMITTED -> "read not permitted ($status)"
        BluetoothGatt.GATT_WRITE_NOT_PERMITTED -> "write not permitted ($status)"
        BluetoothGatt.GATT_INSUFFICIENT_AUTHENTICATION -> "insufficient authentication ($status)"
        BluetoothGatt.GATT_REQUEST_NOT_SUPPORTED -> "request not supported ($status)"
        BluetoothGatt.GATT_INVALID_OFFSET -> "invalid offset ($status)"
        BluetoothGatt.GATT_INVALID_ATTRIBUTE_LENGTH -> "invalid attribute length ($status)"
        BluetoothGatt.GATT_INSUFFICIENT_ENCRYPTION -> "insufficient encryption ($status)"
        BluetoothGatt.GATT_CONNECTION_CONGESTED -> "connection congested ($status)"
        BluetoothGatt.GATT_FAILURE -> "failure ($status)"
        8 -> "connection timeout ($status)"
        19 -> "remote disconnect ($status)"
        22 -> "local disconnect ($status)"
        62 -> "connection failed to establish ($status)"
        133 -> "generic GATT error ($status)"
        else -> "status $status"
    }

    private companion object {
        const val SCAN_TIMEOUT_MS = 10_000L
        const val SERVICE_FILTER_WINDOW_MS = 3_000L
        const val CONNECT_TIMEOUT_MS = 15_000L
    }
}
