package org.rimequad.ime.keyboard

import android.view.HapticFeedbackConstants
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay

private val KeyHeight = 52.dp
private val KeyGap = 4.dp
private val CandidateBarHeight = 44.dp

/**
 * 軟鍵盤主體：上方候選列 + 下方 QWERTY。
 *
 * 這一層只認 [KeyboardUiState] 與 [KeyboardEvent]，不直接碰 RimeCore，
 * 方便之後換成 YAML 驅動的佈局與主題。
 */
@Composable
fun RimeKeyboard(
    state: KeyboardUiState,
    onEvent: (KeyboardEvent) -> Unit,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier = modifier
            .fillMaxWidth()
            .background(MaterialTheme.colorScheme.background)
            .padding(horizontal = 4.dp, vertical = 4.dp),
    ) {
        CandidateBar(state = state, onEvent = onEvent)
        HorizontalDivider(color = MaterialTheme.colorScheme.surfaceVariant)
        KeyGrid(state = state, onEvent = onEvent)
    }
}

@Composable
private fun CandidateBar(state: KeyboardUiState, onEvent: (KeyboardEvent) -> Unit) {
    Column(modifier = Modifier.fillMaxWidth()) {
        // 組字串／狀態列
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = 24.dp)
                .padding(horizontal = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            val header = when {
                state.fatalMessage != null -> state.fatalMessage
                state.preedit.isNotEmpty() -> state.preedit
                state.isStub -> "⟦STUB⟧ 未接 librime，候選字為假資料"
                else -> state.schemaName
            }
            Text(
                text = header,
                fontSize = 13.sp,
                maxLines = 1,
                color = if (state.fatalMessage != null || state.isStub) {
                    MaterialTheme.colorScheme.primary
                } else {
                    MaterialTheme.colorScheme.onSurfaceVariant
                },
                modifier = Modifier.weight(1f),
            )
            if (state.candidates.isNotEmpty()) {
                Text(
                    text = "第 ${state.pageNo + 1} 頁",
                    fontSize = 11.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }

        // 候選列：橫向捲動、可點選
        LazyRow(
            modifier = Modifier
                .fillMaxWidth()
                .height(CandidateBarHeight),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            itemsIndexed(state.candidates) { index, candidate ->
                CandidateChip(
                    index = index,
                    text = candidate.text,
                    comment = candidate.comment,
                    label = candidate.label,
                    highlighted = index == state.highlighted,
                    onClick = { onEvent(KeyboardEvent.Candidate(index)) },
                    onLongClick = { onEvent(KeyboardEvent.CandidateLongPress(index)) },
                )
            }
        }
    }
}

@Composable
private fun CandidateChip(
    index: Int,
    text: String,
    comment: String,
    label: String,
    highlighted: Boolean,
    onClick: () -> Unit,
    onLongClick: () -> Unit,
) {
    val view = LocalView.current
    var pressed by remember { mutableStateOf(false) }
    val bg = when {
        pressed -> MaterialTheme.colorScheme.primary
        highlighted -> MaterialTheme.colorScheme.surfaceVariant
        else -> Color.Transparent
    }
    val fg = if (pressed) {
        MaterialTheme.colorScheme.onPrimary
    } else {
        MaterialTheme.colorScheme.onSurface
    }

    Row(
        modifier = Modifier
            .clip(RoundedCornerShape(6.dp))
            .background(bg)
            .padding(horizontal = 10.dp, vertical = 6.dp)
            .pointerInput(index, text) {
                detectTapGestures(
                    onPress = {
                        pressed = true
                        tryAwaitRelease()
                        pressed = false
                    },
                    onTap = {
                        view.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
                        onClick()
                    },
                    onLongPress = { onLongClick() },
                )
            },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (label.isNotEmpty()) {
            Text(
                text = label,
                fontSize = 11.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(end = 3.dp),
            )
        }
        Text(
            text = text,
            fontSize = 19.sp,
            fontWeight = if (highlighted) FontWeight.SemiBold else FontWeight.Normal,
            color = fg,
        )
        if (comment.isNotEmpty()) {
            Text(
                text = comment,
                fontSize = 11.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = 3.dp),
            )
        }
    }
}

@Composable
private fun KeyGrid(state: KeyboardUiState, onEvent: (KeyboardEvent) -> Unit) {
    val layout = layoutFor(state.layer)
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(top = 4.dp),
        verticalArrangement = Arrangement.spacedBy(KeyGap),
    ) {
        for (row in layout.rows) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(KeyGap),
            ) {
                for (key in row.keys) {
                    KeyButton(
                        key = key,
                        state = state,
                        onEvent = onEvent,
                        modifier = Modifier.weight(key.weight),
                    )
                }
            }
        }
    }
}

@Composable
private fun KeyButton(
    key: KeyDef,
    state: KeyboardUiState,
    onEvent: (KeyboardEvent) -> Unit,
    modifier: Modifier = Modifier,
) {
    val view = LocalView.current
    var pressed by remember { mutableStateOf(false) }

    val isModifierKey = key.type != KeyType.NORMAL && key.type != KeyType.SPACE
    val active = (key.type == KeyType.SHIFT && state.shift) ||
        (key.type == KeyType.LANGUAGE && state.asciiMode)

    val bg = when {
        pressed -> MaterialTheme.colorScheme.primary
        active -> MaterialTheme.colorScheme.primary.copy(alpha = 0.35f)
        isModifierKey -> MaterialTheme.colorScheme.surfaceVariant
        else -> MaterialTheme.colorScheme.surface
    }
    val fg = if (pressed) {
        MaterialTheme.colorScheme.onPrimary
    } else {
        MaterialTheme.colorScheme.onSurface
    }

    fun fire() {
        view.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
        when (key.type) {
            KeyType.SHIFT -> onEvent(KeyboardEvent.ToggleShift)
            KeyType.LAYER -> onEvent(KeyboardEvent.ToggleLayer)
            KeyType.LANGUAGE -> onEvent(KeyboardEvent.ToggleLanguage)
            else -> onEvent(KeyboardEvent.Key(key))
        }
    }

    // 退格支援長按連發。
    if (key.type == KeyType.BACKSPACE && pressed) {
        androidx.compose.runtime.LaunchedEffect(Unit) {
            delay(400)
            while (true) {
                onEvent(KeyboardEvent.Key(key))
                delay(60)
            }
        }
    }

    Box(
        modifier = modifier
            .height(KeyHeight)
            .clip(RoundedCornerShape(7.dp))
            .background(bg)
            .pointerInput(key, state.shift) {
                detectTapGestures(
                    onPress = {
                        pressed = true
                        // 鍵盤習慣：按下就出字，不等放開。
                        fire()
                        tryAwaitRelease()
                        pressed = false
                    },
                )
            },
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = key.labelFor(state.shift),
            fontSize = if (key.label.length > 2) 13.sp else 18.sp,
            color = fg,
            maxLines = 1,
            textAlign = TextAlign.Center,
        )
    }
}
