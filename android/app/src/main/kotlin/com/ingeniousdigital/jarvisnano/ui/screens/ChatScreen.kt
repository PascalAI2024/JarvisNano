package com.ingeniousdigital.jarvisnano.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import com.ingeniousdigital.jarvisnano.R
import com.ingeniousdigital.jarvisnano.data.ChatMessage
import com.ingeniousdigital.jarvisnano.data.ConnectionState
import com.ingeniousdigital.jarvisnano.data.DeviceRepository
import kotlinx.coroutines.launch
import java.util.concurrent.atomic.AtomicLong

@Composable
fun ChatScreen(repository: DeviceRepository, sessionId: String) {
    val connection by repository.connection.collectAsState()
    val messages = remember { mutableStateListOf<ChatMessage>() }
    val ids = remember { AtomicLong(0L) }
    var input by remember { mutableStateOf("") }
    val scope = rememberCoroutineScope()
    val listState = rememberLazyListState()

    // Subscribe to /ws/webim once we're connected.
    LaunchedEffect(connection) {
        if (connection !is ConnectionState.Connected) return@LaunchedEffect
        runCatching {
            repository.openChatSocket().collect { event ->
                if (event.text.isBlank()) return@collect
                val author = when (event.source) {
                    "user" -> ChatMessage.Author.USER
                    "agent", "assistant" -> ChatMessage.Author.AGENT
                    else -> ChatMessage.Author.SYSTEM
                }
                messages.add(
                    ChatMessage(
                        id = ids.incrementAndGet(),
                        author = author,
                        text = event.text,
                    )
                )
            }
        }
    }

    LaunchedEffect(messages.size) {
        if (messages.isNotEmpty()) listState.animateScrollToItem(messages.size - 1)
    }

    Column(modifier = Modifier.fillMaxSize().padding(horizontal = 16.dp, vertical = 12.dp)) {
        LazyColumn(
            modifier = Modifier.weight(1f).fillMaxWidth(),
            state = listState,
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            items(messages, key = { it.id }) { msg -> Bubble(msg) }
        }

        Spacer(Modifier.size(8.dp))

        Row(verticalAlignment = Alignment.CenterVertically) {
            OutlinedTextField(
                value = input,
                onValueChange = { input = it },
                placeholder = { Text(stringResource(R.string.chat_input_placeholder)) },
                singleLine = false,
                maxLines = 3,
                modifier = Modifier.weight(1f),
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send),
                keyboardActions = KeyboardActions(onSend = { sendIfNotEmpty(input, ids, messages, repository, sessionId, scope) { input = "" } }),
            )
            Spacer(Modifier.size(8.dp))
            Button(
                enabled = input.isNotBlank() && connection is ConnectionState.Connected,
                onClick = { sendIfNotEmpty(input, ids, messages, repository, sessionId, scope) { input = "" } },
            ) { Text(stringResource(R.string.chat_send)) }
        }
    }
}

private fun sendIfNotEmpty(
    text: String,
    ids: AtomicLong,
    messages: androidx.compose.runtime.snapshots.SnapshotStateList<ChatMessage>,
    repository: DeviceRepository,
    sessionId: String,
    scope: kotlinx.coroutines.CoroutineScope,
    onSent: () -> Unit,
) {
    val trimmed = text.trim()
    if (trimmed.isEmpty()) return
    messages.add(ChatMessage(id = ids.incrementAndGet(), author = ChatMessage.Author.USER, text = trimmed))
    onSent()
    scope.launch {
        runCatching { repository.sendChat(sessionId, trimmed) }
            .onFailure {
                messages.add(
                    ChatMessage(
                        id = ids.incrementAndGet(),
                        author = ChatMessage.Author.SYSTEM,
                        text = "Could not deliver: ${it.message ?: "unknown error"}",
                    )
                )
            }
    }
}

@Composable
private fun Bubble(msg: ChatMessage) {
    val alignment = if (msg.author == ChatMessage.Author.USER) Alignment.CenterEnd else Alignment.CenterStart
    val bg = when (msg.author) {
        ChatMessage.Author.USER -> MaterialTheme.colorScheme.primary
        ChatMessage.Author.AGENT -> MaterialTheme.colorScheme.surface
        ChatMessage.Author.SYSTEM -> MaterialTheme.colorScheme.surfaceVariant
    }
    val fg = when (msg.author) {
        ChatMessage.Author.USER -> MaterialTheme.colorScheme.onPrimary
        else -> MaterialTheme.colorScheme.onSurface
    }
    Box(modifier = Modifier.fillMaxWidth(), contentAlignment = alignment) {
        Surface(
            color = bg,
            contentColor = fg,
            shape = RoundedCornerShape(14.dp),
            modifier = Modifier.widthIn(max = 320.dp),
        ) {
            Text(
                text = msg.text,
                style = MaterialTheme.typography.bodyLarge,
                modifier = Modifier.padding(horizontal = 14.dp, vertical = 10.dp),
            )
        }
    }
}
