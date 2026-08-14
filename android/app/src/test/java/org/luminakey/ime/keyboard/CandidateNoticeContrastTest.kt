package org.luminakey.ime.keyboard

import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.theme.Platform
import org.luminakey.ime.theme.RepoFixtures
import org.luminakey.ime.theme.Theme
import org.luminakey.ime.theme.ThemeLoader
import java.io.File
import kotlin.math.pow

/**
 * 「鍵盤現在不會出字」那一句話,在**每一份隨附主題**上都要讀得到。
 *
 * ── 這條測試守的是什麼(工單 #105 走查)──────────────────────────────────
 * 首次部署的那十幾秒,候選列上那一行寫著「鍵盤還沒好」。走查量到它的對比是
 * **4.48:1**,低於 WCAG AA 內文的 4.5:1 —— 而它是那個畫面上最要緊的一句話:
 * 使用者讀不到它,就會照常打字。
 *
 * 成因不是有人挑錯顏色,是**借錯了色票**:那一行用的是
 * `candidates.<...>.label.color`,也就是候選**序號**的顏色。序號是次要資訊,
 * 隨附主題給它 `$muted` 是對的;拿它去寫一句警告就不對。
 *
 * 所以這條測試同時釘住兩件事:
 *   ① 那一行用的色票對候選列底色 ≥ 4.5:1;
 *   ② **`KeyboardView` 真的用了那個色票** —— 否則主題再合格也沒有用。
 *
 * ⚠ 這裡刻意逐一走 `RepoFixtures.themeIds`(掃目錄)而不是寫死幾個 id:
 * 這個專案有過「十二份主題只驗到四份」的紀錄。
 */
class CandidateNoticeContrastTest {

    private fun load(id: String): Theme {
        val r = ThemeLoader.load(id, RepoFixtures.themes, Platform.ANDROID)
        assertNotNull("$id 載不起來:${RepoFixtures.describe(r.diagnostics)}", r.value)
        return r.value!!
    }

    /** G2:範圍非空。掃到零份主題然後全綠,是這個專案踩過的失效方式。 */
    @Test
    fun `每一份隨附主題都真的被算過`() {
        assertTrue(
            "只掃到 ${RepoFixtures.themeIds.size} 份主題,路徑大概錯了",
            RepoFixtures.themeIds.size >= 10,
        )
    }

    @Test
    fun `提示文字在每一份主題上都達到 WCAG AA`() {
        val bad = mutableListOf<String>()
        for (id in RepoFixtures.themeIds) {
            val bar = load(id).candidates.bar
            val r = ratio(bar.style.text.color, bar.background)
            if (r < AA_BODY) bad += "$id ${fmt(r)}:1"
        }
        assertTrue(
            "這幾份主題的候選列提示讀不到（門檻 $AA_BODY:1）：$bad\n" +
                "那一行講的是「鍵盤現在不會出字」——讀不到它，使用者就會照常打字。",
            bad.isEmpty(),
        )
    }

    /**
     * **反向**:上一版借的那個色票(`label.color`)拿到今天的隨附主題上,
     * 至少有一份會不合格。
     *
     * 沒有這一條,上面那條全綠就不代表任何事 —— 它可能只是因為兩個色票剛好
     * 都合格。走查量到的 4.48:1 必須在這裡重現得出來。
     */
    @Test
    fun `上一版借的序號色票確實不合格`() {
        val bad = mutableListOf<String>()
        for (id in RepoFixtures.themeIds) {
            val bar = load(id).candidates.bar
            val r = ratio(bar.style.label.color, bar.background)
            if (r < AA_BODY) bad += "$id ${fmt(r)}:1"
        }
        assertTrue(
            "序號色票在每一份主題上都合格 —— 那這條測試就沒有守住任何東西，" +
                "請重新確認 4.48:1 那個量測是怎麼來的。",
            bad.isNotEmpty(),
        )
    }

    /**
     * ② 接線:`KeyboardView` 那一行真的讀 `style.text.color`。
     *
     * 驗的是**那一段範圍內**的接線形態,不是整檔 grep：`style.text.color`
     * 在候選格那邊也出現，整檔掃會被它餵飽。
     */
    @Test
    fun `KeyboardView 的提示那一行接的是候選文字色`() {
        val src = File("src/main/java/org/luminakey/ime/keyboard/KeyboardView.kt").readText()
        val at = src.indexOf("if (notice != null) {")
        assertTrue("KeyboardView 裡找不到提示那一段 —— 這條測試已經對不上實作", at >= 0)
        val end = src.indexOf("return@Row", at)
        assertTrue("提示那一段的結尾找不到", end > at)
        // ⚠ 註解要挖掉:底下那個「不可以再接 style.label.color」的斷言,
        // 會被**解釋為什麼不用它**的那句註解自己餵飽。
        val block = src.substring(at, end)
            .lineSequence().joinToString("\n") { line ->
                val i = line.indexOf("//")
                if (i >= 0 && line.take(i).isBlank()) "" else line
            }

        assertTrue(
            "提示那一行沒有接 style.text.color:\n$block",
            block.contains("color = Color(style.text.color)"),
        )
        assertTrue(
            "提示那一行還接著 style.label.color（4.48:1 的那一個）:\n$block",
            !block.contains("style.label.color"),
        )
    }

    private companion object {
        /** WCAG 2.1 AA,內文。 */
        const val AA_BODY = 4.5

        fun channel(v: Int): Double {
            val c = v / 255.0
            return if (c <= 0.03928) c / 12.92 else ((c + 0.055) / 1.055).pow(2.4)
        }

        fun luminance(argb: Int): Double =
            0.2126 * channel((argb shr 16) and 0xFF) +
                0.7152 * channel((argb shr 8) and 0xFF) +
                0.0722 * channel(argb and 0xFF)

        fun ratio(a: Int, b: Int): Double {
            val la = luminance(a)
            val lb = luminance(b)
            val hi = maxOf(la, lb)
            val lo = minOf(la, lb)
            return (hi + 0.05) / (lo + 0.05)
        }

        fun fmt(r: Double) = String.format("%.2f", r)
    }
}
