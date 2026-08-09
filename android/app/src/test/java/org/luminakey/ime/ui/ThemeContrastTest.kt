package org.luminakey.ime.ui

import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import kotlin.math.abs
import kotlin.math.pow

/**
 * App 側色票的對比度（`docs/ui-design.md` §3.4.1、檢核表 F3）。
 *
 * ── 為什麼要有這一條，以及它記著一個什麼錯 ──────────────────────────────
 * 規範算過 11 組配對，**只有分隔線不合格**：舊值淺色 `#E6EAE9` 對卡片只有
 * 1.21:1、深色 `#242A2C` 只有 1.19:1。1.2:1 在便宜的 LCD 上、在陽光下、
 * 在把亮度調低的夜裡，是**看不見**的。
 *
 * 但真正值得寫成測試的是**第一版怎麼錯的**：
 * 同一個 `outline` 既畫在卡片裡（列與列之間），也畫在畫面底上（區塊之間的
 * hairline）。第一版只算了「對卡片」就宣告過關，選了 `#D0D8D7`
 * （對卡片 1.45:1，合格），**而它對畫面底只有 1.34:1，仍然不合格**。
 *
 * 所以這一條測試**每一個分隔線都算兩個底**。這正是「只驗一半就宣告過關」
 * 的形狀，而本專案在別的地方已經因為它付過學費（六項全錯而守門 6/6 全綠）。
 *
 * ── 為什麼讀原始碼而不是讀 ColorScheme ──────────────────────────────────
 * 讀原始碼的話，**改了顏色卻沒重算**這件事會當場被抓到，
 * 而且 JVM 單元測試不必把 Material3 的 ColorScheme 建起來。
 * 代價是要自己剖析 —— 但那段剖析壞掉時，第一個斷言會以「找不到色票」失敗，
 * 不會靜默通過（見 [`色票表讀得到而且兩份的鍵名集合相同`]）。
 */
class ThemeContrastTest {

    /* ─────────────── 1. 真的算一次 ─────────────── */

    /**
     * F4 的一半：**深淺兩份色票的鍵名集合必須完全相同**。
     *
     * 這一條同時也是這支測試的 G2：剖析壞掉、抓到空表的時候它會先紅，
     * 底下那些「全部合格」才不會變成「什麼都沒算」。
     */
    @Test
    fun `色票表讀得到而且兩份的鍵名集合相同`() {
        assertTrue("淺色色票只讀到 ${light.size} 個，剖析大概壞了", light.size >= MIN_ROLES)
        assertTrue("深色色票只讀到 ${dark.size} 個，剖析大概壞了", dark.size >= MIN_ROLES)
        assertTrue(
            "深淺兩份的鍵名集合不同 —— 深色不是另一份設計，只換色票（§3.5 規則 3）。" +
                "只有一邊有的：${light.keys.symmetricDifference(dark.keys)}",
            light.keys == dark.keys,
        )
    }

    @Test
    fun `分隔線在卡片上與畫面底上都看得見`() {
        // ⚠ 兩個底都要算。只算一個就是規範第一版犯的那個錯。
        for ((name, c) in listOf("淺色" to light, "深色" to dark)) {
            for (backdrop in listOf("surface", "background")) {
                val r = ratio(c.need("outline"), c.need(backdrop))
                assertTrue(
                    "$name 的分隔線對 $backdrop 只有 ${fmt(r)}:1，低於門檻 $DIVIDER。" +
                        "在便宜螢幕上、陽光下、低亮度時它會是看不見的。",
                    r >= DIVIDER,
                )
            }
        }
    }

    @Test
    fun `文字對它的底都達到 4_5比1`() {
        val pairs = listOf(
            "onSurface" to "surface",
            "onSurfaceVariant" to "surface",
            "onSurfaceVariant" to "surfaceVariant",
            "onSurface" to "background",
            "onPrimary" to "primary",
            "onPrimaryContainer" to "primaryContainer",
            "error" to "surface",
            "onErrorContainer" to "errorContainer",
        )
        val bad = mutableListOf<String>()
        for ((name, c) in listOf("淺色" to light, "深色" to dark)) {
            for ((fg, bg) in pairs) {
                val r = ratio(c.need(fg), c.need(bg))
                if (r < TEXT) bad += "$name $fg / $bg = ${fmt(r)}:1"
            }
        }
        assertTrue("這幾組文字對比不足 $TEXT:1：\n  " + bad.joinToString("\n  "), bad.isEmpty())
    }

    /* ─────────────── 2. 反向測試（G1）─────────────── */

    /**
     * 把舊值放回去，確認這條檢查真的會紅。
     *
     * 沒有這一條，「全部合格」只證明我算了一個永遠成立的式子。
     */
    @Test
    fun `舊的分隔線值會被判不合格`() {
        val oldLight = 0xE6EAE9
        val oldDark = 0x242A2C
        val lr = ratio(oldLight, light.need("surface"))
        val dr = ratio(oldDark, dark.need("surface"))
        assertTrue("舊的淺色分隔線居然過關了（${fmt(lr)}:1）—— 那這條檢查沒有在檢查", lr < DIVIDER)
        assertTrue("舊的深色分隔線居然過關了（${fmt(dr)}:1）", dr < DIVIDER)

        // 規範第一版選過的 #D0D8D7：對卡片合格，對畫面底不合格。
        // 這一條把「只驗一半」釘死成一個會失敗的例子。
        val halfChecked = 0xD0D8D7
        assertTrue(
            "#D0D8D7 對卡片本來就該合格",
            ratio(halfChecked, light.need("surface")) >= DIVIDER,
        )
        assertTrue(
            "#D0D8D7 對畫面底應該是不合格的 —— 這正是「只驗一半就宣告過關」的形狀",
            ratio(halfChecked, light.need("background")) < DIVIDER,
        )
    }

    /** 公式本身對得上 WCAG 的已知值：純黑對純白是 21:1，同色是 1:1。 */
    @Test
    fun `對比度公式算得對`() {
        assertTrue(abs(ratio(0x000000, 0xFFFFFF) - 21.0) < 0.01)
        assertTrue(abs(ratio(0x808080, 0x808080) - 1.0) < 0.001)
    }

    /* ─────────────── 剖析與公式 ─────────────── */

    private fun Map<String, Int>.need(role: String): Int =
        this[role] ?: error("色票表裡沒有 $role —— Theme.kt 的剖析壞了，或角色被改名了")

    private fun <T> Set<T>.symmetricDifference(other: Set<T>) = (this - other) + (other - this)

    private fun fmt(v: Double) = String.format("%.2f", v)

    /** WCAG 2.1 的相對亮度。 */
    private fun luminance(rgb: Int): Double {
        fun channel(v: Int): Double {
            val s = v / 255.0
            return if (s <= 0.03928) s / 12.92 else ((s + 0.055) / 1.055).pow(2.4)
        }
        return 0.2126 * channel((rgb shr 16) and 0xFF) +
            0.7152 * channel((rgb shr 8) and 0xFF) +
            0.0722 * channel(rgb and 0xFF)
    }

    private fun ratio(a: Int, b: Int): Double {
        val la = luminance(a)
        val lb = luminance(b)
        val hi = maxOf(la, lb)
        val lo = minOf(la, lb)
        return (hi + 0.05) / (lo + 0.05)
    }

    companion object {
        /** 小字 ≥ 4.5:1（§3.4.1）。 */
        private const val TEXT = 4.5

        /**
         * 分隔線 ≥ 1.4:1。
         *
         * ⚠ 這個門檻是規範提出的判斷，**沒有在任何一台實體螢幕上驗證過**。
         * WCAG 的 3:1 不適用（分隔線在 WCAG 底下屬於裝飾性元素，因為分組同時
         * 由間距表達），而硬拉到 3:1 會讓每一張卡片看起來像表格。
         * 反駁方式：拿一台便宜螢幕、亮度 30%，新舊值各截一張圖對照。
         */
        private const val DIVIDER = 1.4

        private const val MIN_ROLES = 12

        private val themeFile = File("src/main/java/org/luminakey/ime/ui/Theme.kt")

        private val light: Map<String, Int> by lazy { parse("LightColors") }
        private val dark: Map<String, Int> by lazy { parse("DarkColors") }

        /** 從 `val <name> = xxxColorScheme(...)` 那一段抓出 `role = Color(0xFFRRGGBB)`。 */
        private fun parse(name: String): Map<String, Int> {
            val src = themeFile.takeIf { it.isFile }?.readText()
                ?: error("找不到 ${themeFile.path} —— 單元測試的工作目錄應該是 android/app")
            val start = src.indexOf("val $name")
            require(start >= 0) { "Theme.kt 裡找不到 $name" }
            val open = src.indexOf('(', start)
            var depth = 0
            var end = -1
            for (i in open until src.length) {
                when (src[i]) {
                    '(' -> depth++
                    ')' -> { depth--; if (depth == 0) { end = i; break } }
                }
            }
            require(end > open) { "$name 的括號沒有配對" }
            return Regex("""(\w+)\s*=\s*Color\(0x[fF]{2}([0-9a-fA-F]{6})\)""")
                .findAll(src.substring(open, end))
                .associate { it.groupValues[1] to it.groupValues[2].toInt(16) }
        }
    }
}
