package org.luminakey.ime.home

import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import kotlin.math.pow

/**
 * 「我現在選的是哪一個」必須看得見（WCAG 2.1 §1.4.11,非文字對比 3:1）。
 *
 * ── 走查 A4 量到的數字 ──────────────────────────────────────────────────
 * 分段控制的選中膠囊 vs 底槽:
 *   淺色 `surface #FFFFFF` on `surfaceVariant #F2F5F4` = **1.10:1**
 *   深色 `surface #171B1D` on `surfaceVariant #121618` = **1.05:1**
 * 而它同時是「改了設定有沒有生效」的**唯一**回饋 —— 連拍九張截圖,
 * 位元組完全相同。使用者看不見自己剛剛做了什麼。
 *
 * ── 這條測試守的兩件事 ──────────────────────────────────────────────────
 * ① **底色本身救不回來**,而這一點必須是可驗證的事實而不是一句斷言:
 *    底下那條測試會證明 `surface` 與 `primaryContainer` 兩個候選都不到 3:1。
 *    沒有它,下一個人會想「換個底色就好了吧」然後換出一個 1.12:1。
 * ② 現在扛著這個訊號的是**外框的顏色**,而它在**兩種模式**下都要達標。
 *    只算淺色是這個專案在 `ThemeContrastTest` 檔頭記著的那個錯。
 *
 * 讀原始碼而不是讀 ColorScheme:改了顏色卻沒重算,這裡會當場紅。
 */
class SegmentedContrastTest {

    /* ─────────────── 1. 訊號本身 ─────────────── */

    @Test
    fun `選取外框在深淺兩種模式下都達到非文字對比門檻`() {
        for ((mode, table) in listOf("淺色" to light, "深色" to dark)) {
            val r = ratio(table.need("primary"), table.need("surfaceVariant"))
            assertTrue(
                "$mode 的選取外框對底槽只有 ${fmt(r)}:1，低於 $NON_TEXT:1。" +
                    "「我現在選的是哪一個」是這個 app 裡唯一的設定回饋。",
                r >= NON_TEXT,
            )
        }
    }

    /**
     * **反向**:上一版那個訊號(膠囊底色)在兩種模式下都不合格。
     *
     * 沒有這一條,上面那條全綠就不代表任何事。
     */
    @Test
    fun `上一版的膠囊底色確實不合格`() {
        for ((mode, table) in listOf("淺色" to light, "深色" to dark)) {
            val r = ratio(table.need("surface"), table.need("surfaceVariant"))
            assertTrue(
                "$mode 的膠囊底色對底槽有 ${fmt(r)}:1 —— 走查量到的是 1.10 / 1.05，" +
                    "對不上了就表示這條測試已經在守一個不存在的東西。",
                r < NON_TEXT,
            )
        }
    }

    /**
     * 「換個底色就好」這條路走不通,而且是**算出來**的。
     *
     * `primaryContainer` 是最直覺的替代品(Colours 那一頁的綠底語言),
     * 但它在淺色下仍然只有 1.1x:1。留下這條測試,下一個人不必再算一次。
     */
    @Test
    fun `換成 primaryContainer 也救不回來`() {
        val r = ratio(light.need("primaryContainer"), light.need("surfaceVariant"))
        assertTrue(
            "primaryContainer 對底槽居然有 ${fmt(r)}:1（≥ $NON_TEXT）—— " +
                "色票換過了，Dimens.selectedBorder 的註解要跟著重寫。",
            r < NON_TEXT,
        )
    }

    /* ─────────────── 2. 接線 ─────────────── */

    /**
     * `Segmented` 那一格真的畫了外框,而且用的是 `primary`。
     *
     * 限定在 `Segmented(` 的函式體裡找,不整檔 grep:`border` 這個字在
     * 這個檔案的別處也出現得了。
     */
    @Test
    fun `Segmented 真的把外框接上了`() {
        val src = File("src/main/java/org/luminakey/ime/home/Ui.kt").readText()
        val at = src.indexOf("fun <T> Segmented(")
        assertTrue("Ui.kt 裡找不到 Segmented() —— 這條測試已經對不上實作", at >= 0)
        val open = src.indexOf('{', at)
        var depth = 0
        var end = -1
        for (i in open until src.length) {
            when (src[i]) {
                '{' -> depth++
                '}' -> {
                    depth--
                    if (depth == 0) {
                        end = i
                        break
                    }
                }
            }
        }
        assertTrue("Segmented() 的大括號沒有配對成功", end > open)
        val body = src.substring(open, end)
            .lineSequence().joinToString("\n") { line ->
                val i = line.indexOf("//")
                if (i >= 0 && line.take(i).isBlank()) "" else line
            }

        assertTrue(
            "Segmented() 沒有畫選取外框 —— 選中的訊號又只剩 1.10:1 的底色:\n$body",
            body.contains("Modifier.border("),
        )
        assertTrue(
            "外框沒有用 Dimens.selectedBorder（那裡記著為什麼是 2dp）:\n$body",
            body.contains("width = Dimens.selectedBorder"),
        )
        assertTrue(
            "外框沒有用 primary —— 換成別的顏色就要重新算對比:\n$body",
            body.contains("color = MaterialTheme.colorScheme.primary"),
        )
    }

    private companion object {
        /** WCAG 2.1 §1.4.11,使用者介面元件與狀態指示。 */
        const val NON_TEXT = 3.0

        val src: String by lazy {
            File("src/main/java/org/luminakey/ime/ui/Theme.kt").readText()
        }

        val light: Map<String, Int> by lazy { palette("LightColors") }
        val dark: Map<String, Int> by lazy { palette("DarkColors") }

        /** `primary = Color(0xFF1F6F63),` → "primary" to 0xFF1F6F63。 */
        fun palette(table: String): Map<String, Int> {
            val at = src.indexOf("private val $table")
            check(at >= 0) { "Theme.kt 裡找不到 $table" }
            val end = src.indexOf("\n)", at)
            check(end > at) { "$table 的結尾找不到" }
            val block = src.substring(at, end)
            val out = LinkedHashMap<String, Int>()
            for (m in Regex("""(\w+)\s*=\s*Color\(0x([0-9A-Fa-f]{8})\)""").findAll(block)) {
                out[m.groupValues[1]] = m.groupValues[2].toLong(16).toInt()
            }
            // G2：抓到空表然後全綠，是這個專案踩過的失效方式。
            check(out.size >= 10) { "$table 只剖析到 ${out.size} 個色票，剖析壞了" }
            return out
        }

        fun Map<String, Int>.need(role: String): Int =
            this[role] ?: throw AssertionError("色票表裡沒有 $role")

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
            return (maxOf(la, lb) + 0.05) / (minOf(la, lb) + 0.05)
        }

        fun fmt(r: Double) = String.format("%.2f", r)
    }
}
