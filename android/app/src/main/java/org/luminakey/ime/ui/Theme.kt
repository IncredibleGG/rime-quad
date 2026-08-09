package org.luminakey.ime.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

/**
 * **App 畫面**（引導、設定、市集）用的 Material 主題。
 *
 * ⚠ 軟鍵盤**不吃**這裡的顏色。鍵盤的每一個顏色與尺寸都來自 core/themes
 * 目錄的 yaml，由 `theme.ThemeParser` 解析、`keyboard.KeyboardView` 直接消費，
 * 中間不經過 Material 的 ColorScheme。這裡只有 App 這一側在用。
 *
 * ── 為什麼是這一組色 ────────────────────────────────────────────────────
 * 墨黑 `#14181A` 打底 + 一個重點色（青瓷綠 `#1F6F63`）。刻意避開所有競品的
 * 識別色（語燕橘紫、三星藍、Gboard 藍），也刻意只有一個重點色 —— 畫面上
 * 「該點哪裡」必須不必讀字就看得出來，多一個彩色就少一分這個能力。
 *
 * 深色不是另一份設計：版面、間距、字級、層級完全不動，只換這幾個色票。
 * 兩條額外規則：
 *   1. 深色時重點色**提亮、降飽和**（`#1F6F63` → `#63C3AC`），
 *      實心按鈕上的字改成近黑 —— 亮青底配白字對比不足。
 *   2. 卡片與底之間只差一階（`#171B1D` vs `#0D1012`），靠 1px 分隔線切開，
 *      不靠陰影，避免深色下常見的「一坨黑」。
 *
 * ── 分隔線為什麼是這兩個值（docs/ui-design.md §3.4.1）──────────────────
 * 這一組色票裡有 11 組文字／底的配對，用 WCAG 2.1 的相對亮度公式算過之後，
 * **只有分隔線不合格**：舊值淺色 `#E6EAE9` 對卡片只有 1.21:1、深色 `#242A2C`
 * 只有 1.19:1。1.2:1 在便宜的 LCD 上、在陽光下、在把亮度調低的夜裡是**看不見**的，
 * 而這一版的分組同時靠間距表達（§3.1 規則 1），所以分隔線只需要「看得見但很安靜」。
 *
 * 現值 `#C4CDCC` / `#363E40` 的由來，以及一個**只驗一半**的教訓：
 * 同一個 `outline` 既畫在卡片裡（列與列之間），也畫在畫面底上（區塊之間的
 * hairline）。規範的第一版只算了對卡片的值，選了 `#D0D8D7`（對卡片 1.45:1 合格），
 * **但它對畫面底只有 1.34:1，仍然不合格**。所以這兩個值是**同時**滿足兩個底的
 * 最淺值 —— 淺色 1.62:1（對卡片）／1.49:1（對畫面底），深色 1.59:1／1.75:1。
 * `ThemeContrastTest` 兩個底都算，就是為了不讓那個錯誤再犯一次。
 *
 * ⚠ 門檻 1.4:1 是規範提出的判斷，**沒有在任何一台實體螢幕上驗證過**。
 * 反駁方式：拿一台便宜螢幕、亮度調到 30%，新舊值各截一張圖對照。
 */

private val DarkColors = darkColorScheme(
    primary = Color(0xFF63C3AC),
    onPrimary = Color(0xFF08110F),
    primaryContainer = Color(0xFF17322D),
    onPrimaryContainer = Color(0xFFBDE8DD),
    secondaryContainer = Color(0xFF17322D),
    onSecondaryContainer = Color(0xFFBDE8DD),
    surface = Color(0xFF171B1D),
    onSurface = Color(0xFFECEFEE),
    surfaceVariant = Color(0xFF121618),
    onSurfaceVariant = Color(0xFFB9C3C2),
    // ⚠ 分隔線是這一組色票裡唯一一個**實際算出來不合格**的東西，見下方註解。
    outline = Color(0xFF363E40),
    outlineVariant = Color(0xFF363E40),
    background = Color(0xFF0D1012),
    onBackground = Color(0xFFECEFEE),
    error = Color(0xFFFF8A80),
    errorContainer = Color(0xFF3A1512),
    onErrorContainer = Color(0xFFFFDAD5),
)

private val LightColors = lightColorScheme(
    primary = Color(0xFF1F6F63),
    onPrimary = Color(0xFFFFFFFF),
    primaryContainer = Color(0xFFD7EDE7),
    onPrimaryContainer = Color(0xFF06231E),
    secondaryContainer = Color(0xFFD7EDE7),
    onSecondaryContainer = Color(0xFF06231E),
    surface = Color(0xFFFFFFFF),
    onSurface = Color(0xFF14181A),
    surfaceVariant = Color(0xFFF2F5F4),
    onSurfaceVariant = Color(0xFF454F51),
    outline = Color(0xFFC4CDCC),
    outlineVariant = Color(0xFFC4CDCC),
    background = Color(0xFFF4F6F5),
    onBackground = Color(0xFF14181A),
    error = Color(0xFFB3261E),
    errorContainer = Color(0xFFF9DEDC),
    onErrorContainer = Color(0xFF410E0B),
)

@Composable
fun RimeTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    content: @Composable () -> Unit,
) {
    MaterialTheme(
        colorScheme = if (darkTheme) DarkColors else LightColors,
        content = content,
    )
}
