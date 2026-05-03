package com.ingeniousdigital.jarvisnano

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Surface
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import com.ingeniousdigital.jarvisnano.ui.nav.JarvisNanoNavHost
import com.ingeniousdigital.jarvisnano.ui.theme.JarvisNanoTheme
import java.util.UUID

class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        setContent {
            // One chat session ID per Activity instance — matches the dashboard's model.
            val sessionId = remember { UUID.randomUUID().toString() }
            val app = remember { App.get() }

            JarvisNanoTheme {
                Surface(modifier = Modifier.fillMaxSize()) {
                    JarvisNanoNavHost(
                        repository = app.deviceRepository,
                        bleClient = app.bleClient,
                        sessionId = sessionId,
                    )
                }
            }
        }
    }
}
