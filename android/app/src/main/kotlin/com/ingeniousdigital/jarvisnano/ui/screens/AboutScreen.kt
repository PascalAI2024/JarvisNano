package com.ingeniousdigital.jarvisnano.ui.screens

import android.content.Intent
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.core.net.toUri
import com.ingeniousdigital.jarvisnano.R

@Composable
fun AboutScreen() {
    val context = LocalContext.current
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 24.dp, vertical = 24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Image(
            painter = painterResource(id = R.drawable.mascot),
            contentDescription = null,
            contentScale = ContentScale.FillWidth,
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(20.dp)),
        )

        Text(
            text = stringResource(R.string.about_version),
            style = MaterialTheme.typography.displayLarge,
        )

        Text(
            text = stringResource(R.string.about_tagline),
            style = MaterialTheme.typography.titleMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )

        Text(
            text = stringResource(R.string.about_description),
            style = MaterialTheme.typography.bodyLarge,
        )

        Spacer(Modifier.height(8.dp))

        OutlinedButton(
            onClick = { context.startActivity(Intent(Intent.ACTION_VIEW, GITHUB_URL.toUri())) },
            modifier = Modifier.fillMaxWidth(),
        ) { Text(stringResource(R.string.about_link_github)) }

        OutlinedButton(
            onClick = { context.startActivity(Intent(Intent.ACTION_VIEW, IGD_URL.toUri())) },
            modifier = Modifier.fillMaxWidth(),
        ) { Text(stringResource(R.string.about_link_igd)) }
    }
}

private const val GITHUB_URL = "https://github.com/PascalAI2024/JarvisNano"
private const val IGD_URL = "https://ingeniousdigital.com"
