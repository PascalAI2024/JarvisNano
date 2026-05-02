package com.ingeniousdigital.jarvisnano.ui.screens

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import com.ingeniousdigital.jarvisnano.R
import com.ingeniousdigital.jarvisnano.data.DeviceRepository
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private const val LIVE_REFRESH_MS = 500L

@Composable
fun CameraScreen(repository: DeviceRepository) {
    val coroutineScope = rememberCoroutineScope()

    var jpegBytes by remember { mutableStateOf<ByteArray?>(null) }
    var lastError by remember { mutableStateOf<String?>(null) }
    var live by remember { mutableStateOf(false) }
    var inFlight by remember { mutableStateOf(false) }

    suspend fun snap() {
        if (inFlight) return
        inFlight = true
        try {
            val bytes = withContext(Dispatchers.IO) { repository.loadSnapshot() }
            jpegBytes = bytes
            lastError = null
        } catch (t: Throwable) {
            lastError = t.message ?: t::class.java.simpleName
        } finally {
            inFlight = false
        }
    }

    LaunchedEffect(live) {
        if (!live) return@LaunchedEffect
        while (live) {
            snap()
            delay(LIVE_REFRESH_MS)
        }
    }

    Column(
        modifier = Modifier.fillMaxSize().padding(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(
            text = stringResource(R.string.camera_title),
            style = MaterialTheme.typography.titleLarge,
        )
        Spacer(Modifier.height(12.dp))

        Box(
            modifier = Modifier
                .fillMaxWidth()
                .aspectRatio(4f / 3f)
                .background(Color.Black),
            contentAlignment = Alignment.Center,
        ) {
            val bytes = jpegBytes
            when {
                bytes != null -> {
                    val bitmap = remember(bytes) {
                        BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
                    }
                    if (bitmap != null) {
                        Image(
                            bitmap = bitmap.asImageBitmap(),
                            contentDescription = null,
                            modifier = Modifier.fillMaxSize(),
                            contentScale = ContentScale.Fit,
                        )
                    } else {
                        Text(
                            text = stringResource(R.string.camera_decode_failed),
                            color = Color.White,
                        )
                    }
                }
                inFlight -> CircularProgressIndicator(color = Color.White)
                else -> Text(
                    text = stringResource(R.string.camera_no_frame),
                    color = Color.White,
                )
            }
        }

        Spacer(Modifier.height(12.dp))

        if (lastError != null) {
            Text(
                text = lastError ?: "",
                color = MaterialTheme.colorScheme.error,
                fontFamily = FontFamily.Monospace,
                style = MaterialTheme.typography.bodySmall,
                textAlign = TextAlign.Center,
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(8.dp))
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = androidx.compose.foundation.layout.Arrangement.spacedBy(8.dp),
        ) {
            OutlinedButton(
                onClick = { coroutineScope.launch { snap() } },
                modifier = Modifier.fillMaxWidth().weight(1f),
                enabled = !live,
            ) { Text(stringResource(R.string.camera_snap_once)) }

            Button(
                onClick = { live = !live },
                modifier = Modifier.fillMaxWidth().weight(1f),
            ) {
                Text(
                    if (live) stringResource(R.string.camera_stop_live)
                    else stringResource(R.string.camera_start_live)
                )
            }
        }
    }
}
