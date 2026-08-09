package org.luminakey.ime.keyboard

import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource
import org.luminakey.ime.R
import org.luminakey.ime.core.RimeStatus
import org.luminakey.ime.theme.KeyAction
import org.luminakey.ime.theme.LabelSource
import org.luminakey.ime.theme.LayoutKey

/*
 * 朗讀名的**資源那一半**。規則在 [KeyA11y],這裡只把代號翻成當地語言的字。
 *
 * 切開的理由與 PrefLevels／PrefLabels 一樣:規則要能被 JVM 單元測試掃過
 * 十二份佈局的每一顆鍵,而那個測試不能扛 Robolectric。
 */

@Composable
internal fun KeyName.text(): String = when (this) {
    is KeyName.Face -> text
    is KeyName.Named ->
        if (arg != null) stringResource(label.resId(), arg) else stringResource(label.resId())
}

@Composable
internal fun keyDescription(
    key: LayoutKey,
    layerLabels: Map<String, String>,
): String = KeyA11y.nameOf(key, layerLabels).text()

@Composable
internal fun toolbarItemDescription(
    icon: String?,
    label: String,
    labelFrom: LabelSource,
    tap: KeyAction,
): String = KeyA11y.toolbarNameOf(icon, label, labelFrom, tap).text()

@Composable
internal fun a11yStateText(labelFrom: LabelSource, status: RimeStatus): String? =
    KeyA11y.stateOf(labelFrom, status)?.let { stringResource(it.resId()) }

private fun A11yLabel.resId(): Int = when (this) {
    A11yLabel.BACKSPACE -> R.string.a11y_backspace
    A11yLabel.ENTER -> R.string.a11y_enter
    A11yLabel.SHIFT -> R.string.a11y_shift
    A11yLabel.SHIFT_LOCK -> R.string.a11y_shift_lock
    A11yLabel.SPACE -> R.string.a11y_space
    A11yLabel.GLOBE -> R.string.a11y_globe
    A11yLabel.KEYBOARD_HIDE -> R.string.a11y_keyboard_hide
    A11yLabel.SETTINGS -> R.string.a11y_settings
    A11yLabel.EMOJI -> R.string.a11y_emoji
    A11yLabel.SEARCH -> R.string.a11y_search
    A11yLabel.GO -> R.string.a11y_go
    A11yLabel.DONE -> R.string.a11y_done
    A11yLabel.NEXT -> R.string.a11y_next
    A11yLabel.CLIPBOARD -> R.string.a11y_clipboard
    A11yLabel.UNDO -> R.string.a11y_undo
    A11yLabel.MIC -> R.string.a11y_mic
    A11yLabel.ARROW_LEFT -> R.string.a11y_arrow_left
    A11yLabel.ARROW_RIGHT -> R.string.a11y_arrow_right
    A11yLabel.ARROW_UP -> R.string.a11y_arrow_up
    A11yLabel.ARROW_DOWN -> R.string.a11y_arrow_down
    A11yLabel.CLEAR -> R.string.a11y_clear
    A11yLabel.CURSOR_LEFT -> R.string.a11y_cursor_left
    A11yLabel.CURSOR_RIGHT -> R.string.a11y_cursor_right
    A11yLabel.CURSOR_HOME -> R.string.a11y_cursor_home
    A11yLabel.CURSOR_END -> R.string.a11y_cursor_end
    A11yLabel.CANDIDATE_NEXT_PAGE -> R.string.a11y_candidate_next_page
    A11yLabel.CANDIDATE_PREV_PAGE -> R.string.a11y_candidate_prev_page
    A11yLabel.SCHEMA_PICKER -> R.string.a11y_schema_picker
    A11yLabel.SCHEMA_NEXT -> R.string.a11y_schema_next
    A11yLabel.SCHEMA_PREV -> R.string.a11y_schema_prev
    A11yLabel.INPUT_MODE -> R.string.a11y_input_mode
    A11yLabel.VARIANT -> R.string.a11y_variant
    A11yLabel.SHAPE -> R.string.a11y_shape
    A11yLabel.LAYER_SWITCH -> R.string.a11y_layer_switch
    A11yLabel.LAYOUT_PRIMARY -> R.string.a11y_layout_primary
    A11yLabel.LAYOUT_PREVIOUS -> R.string.a11y_layout_previous
    A11yLabel.LAYOUT_SWITCH -> R.string.a11y_layout_switch
}

private fun A11yState.resId(): Int = when (this) {
    A11yState.CJK -> R.string.a11y_input_mode_cjk
    A11yState.LATIN -> R.string.a11y_input_mode_latin
    A11yState.TRADITIONAL -> R.string.a11y_variant_trad
    A11yState.SIMPLIFIED -> R.string.a11y_variant_simp
    A11yState.FULL_WIDTH -> R.string.a11y_shape_full
    A11yState.HALF_WIDTH -> R.string.a11y_shape_half
}
