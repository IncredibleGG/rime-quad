package org.rimequad.ime.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

/**
 * **設定畫面**（MainActivity）用的 Material 主題。
 *
 * ⚠ 軟鍵盤**不吃**這裡的顏色。鍵盤的每一個顏色與尺寸都來自 core/themes
 * 目錄的 yaml，由 `theme.ThemeParser` 解析、`keyboard.KeyboardView` 直接消費，
 * 中間不經過 Material 的 ColorScheme。這裡只剩設定畫面在用。
 */

private val DarkColors = darkColorScheme(
    primary = Color(0xFF7FB2FF),
    onPrimary = Color(0xFF00325C),
    surface = Color(0xFF1B1C1E),
    onSurface = Color(0xFFE3E2E6),
    surfaceVariant = Color(0xFF2A2C2F),
    onSurfaceVariant = Color(0xFFC5C6CA),
    background = Color(0xFF121316),
    onBackground = Color(0xFFE3E2E6),
)

private val LightColors = lightColorScheme(
    primary = Color(0xFF00639B),
    onPrimary = Color(0xFFFFFFFF),
    surface = Color(0xFFFAF9FD),
    onSurface = Color(0xFF1A1C1E),
    surfaceVariant = Color(0xFFE7E8EC),
    onSurfaceVariant = Color(0xFF43474E),
    background = Color(0xFFF1F2F6),
    onBackground = Color(0xFF1A1C1E),
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
