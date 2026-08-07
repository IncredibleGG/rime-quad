package org.rimequad.ime.keyboard

import org.rimequad.ime.core.RimeKeysym

/**
 * ⚠ 暫時性的寫死佈局。
 *
 * 真正的佈局定義會來自 core/layouts 目錄的 yaml（由另一條線定義格式並提供
 * Kotlin 解析器）。這裡刻意只做到「能用」，資料模型也刻意做得像 YAML
 * 會展開成的樣子（rows → keys，每個 key 帶 label 與 keysym），
 * 之後接上解析器時只要換掉 [QwertyLayout] 這個資料來源即可，
 * [KeyboardView] 不需要改。
 */

enum class KeyType {
    NORMAL,
    SHIFT,
    BACKSPACE,
    ENTER,
    SPACE,
    /** 切換到符號／字母層。 */
    LAYER,
    /** 中英切換（ascii_mode）。 */
    LANGUAGE,
}

data class KeyDef(
    val label: String,
    val keysym: Int,
    val type: KeyType = KeyType.NORMAL,
    /** 同一列內的寬度比重。 */
    val weight: Float = 1f,
    /** Shift 按下時的替代字面與 keysym；null 代表沿用主字面。 */
    val shiftLabel: String? = null,
    val shiftKeysym: Int? = null,
) {
    fun labelFor(shift: Boolean): String = if (shift) shiftLabel ?: label else label
    fun keysymFor(shift: Boolean): Int = if (shift) shiftKeysym ?: keysym else keysym
}

data class KeyRow(val keys: List<KeyDef>)

data class KeyboardLayout(val rows: List<KeyRow>)

enum class KeyLayer { LETTERS, SYMBOLS }

private fun letter(c: Char): KeyDef = KeyDef(
    label = c.toString(),
    keysym = RimeKeysym.ofAsciiChar(c),
    shiftLabel = c.uppercaseChar().toString(),
    shiftKeysym = RimeKeysym.ofAsciiChar(c.uppercaseChar()),
)

private fun sym(s: String): KeyDef =
    KeyDef(label = s, keysym = RimeKeysym.ofAsciiChar(s[0]))

private fun row(vararg keys: KeyDef) = KeyRow(keys.toList())

private val functionRow4 = { layerLabel: String ->
    row(
        KeyDef(layerLabel, 0, KeyType.LAYER, weight = 1.4f),
        KeyDef("中/英", 0, KeyType.LANGUAGE, weight = 1.4f),
        sym(","),
        KeyDef("空格", RimeKeysym.SPACE, KeyType.SPACE, weight = 4f),
        sym("."),
        KeyDef("↵", RimeKeysym.RETURN, KeyType.ENTER, weight = 1.6f),
    )
}

val QwertyLayout = KeyboardLayout(
    rows = listOf(
        row(*"qwertyuiop".map(::letter).toTypedArray()),
        row(*"asdfghjkl".map(::letter).toTypedArray()),
        row(
            KeyDef("⇧", RimeKeysym.SHIFT_L, KeyType.SHIFT, weight = 1.4f),
            *"zxcvbnm".map(::letter).toTypedArray(),
            KeyDef("⌫", RimeKeysym.BACKSPACE, KeyType.BACKSPACE, weight = 1.4f),
        ),
        functionRow4("?123"),
    ),
)

val SymbolLayout = KeyboardLayout(
    rows = listOf(
        row(*"1234567890".map { sym(it.toString()) }.toTypedArray()),
        row(*listOf("-", "/", ":", ";", "(", ")", "$", "&", "@").map(::sym).toTypedArray()),
        row(
            KeyDef("=\\<", 0, KeyType.LAYER, weight = 1.4f),
            *listOf("!", "?", "'", "\"", "*", "#", "+").map(::sym).toTypedArray(),
            KeyDef("⌫", RimeKeysym.BACKSPACE, KeyType.BACKSPACE, weight = 1.4f),
        ),
        functionRow4("ABC"),
    ),
)

fun layoutFor(layer: KeyLayer): KeyboardLayout =
    when (layer) {
        KeyLayer.LETTERS -> QwertyLayout
        KeyLayer.SYMBOLS -> SymbolLayout
    }
