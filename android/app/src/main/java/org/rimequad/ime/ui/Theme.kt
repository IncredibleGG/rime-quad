package org.rimequad.ime.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

/**
 * 暫用的極簡主題。
 *
 * ⚠ 不要在這裡長出一套主題格式。真正的鍵盤主題由 core/themes 目錄的 yaml
 * 定義（另一條線負責），接上之後這裡只負責把解析結果轉成 ColorScheme。
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
