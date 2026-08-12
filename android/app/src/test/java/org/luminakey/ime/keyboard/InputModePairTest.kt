package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.theme.LabelSource
import org.luminakey.ime.theme.Platform
import org.luminakey.ime.theme.RepoFixtures
import org.luminakey.ime.theme.ThemeLoader
import org.luminakey.ime.theme.LayoutLoader
import kotlin.math.abs
import kotlin.math.pow

/**
 * 「中/En」那顆鍵的三個缺陷，一次守完。
 *
 * ── 使用者看到的是什麼 ──────────────────────────────────────────────────
 * 系統字級調到 1.30（主題自己宣告的 `font_scale_max`）之後，`qwerty` 底列的
 * 中英切換鍵印出來是 **「中/」** —— `En` 整段不見了。實測截圖見這一輪的
 * `build/b1-shots/`（emulator-5558，1080×2400 @420dpi，default-light）。
 *
 * ── 三個環節，壞了三個 ──────────────────────────────────────────────────
 *   1. **量測沒帶字重。** 畫出來的那一段用 `inputModeFace()`，當前那一態是
 *      **Bold**；而 `fittedLabelSize()` 量的是不帶樣式的純文字。粗體比較寬，
 *      於是「量起來放得下、畫出來放不下」。這一條由 [inputModeMeasureIsBold]
 *      守著。
 *   2. **`maxLines = 1` 沒有 `overflow`。** Compose 的預設是 `Clip` ——
 *      放不下就**無聲切掉**，切在哪就是哪，於是 `中/En` 變成 `中/`。
 *      這一條由掃描 [KeyboardView] 那一段守著。
 *   3. **未選中那半的對比只有 2.84:1。** `modifier` 樣式的 `hint_color`
 *      （`$muted` #6B7484）壓在 `$key_mod`（#C4C9D4）上。WCAG 小字要 4.5:1。
 *      這一條由 [inputModePairContrast] 對**每一份主題、每一顆這種鍵**算一次。
 *
 * ── 為什麼不是只改 default-light ────────────────────────────────────────
 * 十二份主題都有 `modifier` 樣式，而 `hint_color` 在 `hint_position: none`
 * 的樣式底下**只有這顆鍵在用** —— 也就是說改它不會動到別的東西，
 * 但漏改任何一份就有一份主題上的使用者看不清楚。所以掃全部。
 */
class InputModePairTest {

    /** 小字 4.5:1（WCAG 2.1 AA，與 App 側的 ThemeContrastTest 同一個門檻）。 */
    private val threshold = 4.5

    @Test
    fun inputModePairContrast() {
        val bad = mutableListOf<String>()
        var checked = 0
        for (themeId in RepoFixtures.themeIds) {
            val theme = ThemeLoader.load(themeId, RepoFixtures.themes, Platform.ANDROID).value
                ?: error("主題 $themeId 載不起來")
            for (layoutId in RepoFixtures.layoutIds) {
                val layout = LayoutLoader.load(layoutId, RepoFixtures.layouts, Platform.ANDROID)
                    .value ?: error("佈局 $layoutId 載不起來")
                for (layer in layout.layers) {
                    for (row in layer.rows) {
                        for (key in row.keys) {
                            if (key.labelFrom != LabelSource.INPUT_MODE_PAIR) continue
                            val style = theme.keyboard.keyStyle(key.style)
                            checked++
                            // ⚠ **鎖定（active）那一組刻意不算，而且那不是省略。**
                            //   `isActiveFace` 把 INPUT_MODE_PAIR 排除在外（見那一支
                            //   的註解：鍵面已經畫出兩態了，再把整顆鍵染成 accent 色
                            //   是重複同一個訊息）。所以那組顏色**到不了這顆鍵**。
                            //   算了反而會逼著去改一組根本不會出現的配色。
                            //   前提由 activeStateIsUnreachableForThePairKey 釘住 ——
                            //   哪天有佈局在這顆鍵上寫 `active: true`，那一條會紅。
                            //
                            // 兩半用的是同一個顏色（見 inputModeFace 的註解），
                            // 所以每種底色只有一組要算。
                            for ((what, pair) in listOf(
                                "常態" to (style.foreground to style.background),
                                "按下" to (style.pressedForeground to style.pressedBackground),
                            )) {
                                val r = ratio(pair.first, pair.second)
                                if (r < threshold) {
                                    bad += "$themeId / $layoutId:${layer.id}:${key.id}" +
                                        " $what ${fmt(r)}:1"
                                }
                            }
                        }
                    }
                }
            }
            // 工具列上的同一顆（§8.6.6.1 的 input_mode_pair 項目）。
            val bar = theme.candidates.bar
            if (bar.toolbar.items.any { it.labelFrom == LabelSource.INPUT_MODE_PAIR }) {
                checked++
                val r = ratio(bar.style.text.color, bar.background)
                if (r < threshold) bad += "$themeId / 工具列 ${fmt(r)}:1"
            }
        }
        // G2：一顆都沒掃到就是掃描壞了，不是「全部合格」。
        assertTrue("一顆 input_mode_pair 都沒掃到 —— 掃描壞了", checked > 0)
        assertEquals(
            "這幾處的中/En 對比低於 $threshold:1（$checked 處掃過）：\n  " +
                bad.distinct().joinToString("\n  "),
            emptyList<String>(),
            bad.distinct(),
        )
    }

    /**
     * 上面那條略過「鎖定」那一組的**前提**：這顆鍵到不了 active 狀態。
     *
     * 少了這一條，「略過」就變成一個沒有人查得到的假設 —— 而假設會過期：
     * 哪天有佈局在中／En 鍵上寫 `active: true`，使用者就會在一個從來沒被
     * 算過對比的配色上看不清楚那兩個字。
     */
    @Test
    fun activeStateIsUnreachableForThePairKey() {
        var seen = 0
        for (layoutId in RepoFixtures.layoutIds) {
            val layout = LayoutLoader.load(layoutId, RepoFixtures.layouts, Platform.ANDROID)
                .value ?: error("佈局 $layoutId 載不起來")
            for (layer in layout.layers) {
                for (row in layer.rows) {
                    for (key in row.keys) {
                        if (key.labelFrom != LabelSource.INPUT_MODE_PAIR) continue
                        seen++
                        assertTrue(
                            "$layoutId:${layer.id}:${key.id} 宣告了 active —— " +
                                "那組配色從來沒被算過對比，請一起放進 inputModePairContrast",
                            !isActiveFace(key.active, key.labelFrom, org.luminakey.ime.core.RimeStatus()),
                        )
                    }
                }
            }
        }
        assertTrue("一顆都沒掃到 —— 掃描壞了", seen > 0)
    }

    /** 公式對得上 WCAG 的已知值，而且 alpha 真的有被合成掉。 */
    @Test
    fun contrastFormula() {
        assertTrue(abs(ratio(0xFF000000.toInt(), 0xFFFFFFFF.toInt()) - 21.0) < 0.01)
        assertTrue(abs(ratio(0xFF808080.toInt(), 0xFF808080.toInt()) - 1.0) < 0.001)
        // 全透明的前景 = 底色本身 → 1:1。沒有合成的話會算成黑對白 = 21:1。
        assertTrue(abs(ratio(0x00000000, 0xFFFFFFFF.toInt()) - 1.0) < 0.001)
    }

    /**
     * 反向測試：把改動前的那一組值放回去，這條檢查必須是紅的。
     *
     * `$muted` #6B7484 壓在 `$key_mod` #C4C9D4 上 = 2.84:1 —— 那正是回報裡
     * 那個數字，一模一樣。算得出同一個數字，才證明這條檢查量的是同一件事。
     */
    @Test
    fun oldValueIsRejected() {
        val r = ratio(0xFF6B7484.toInt(), 0xFFC4C9D4.toInt())
        assertEquals("改動前的實測值", 2.84, r, 0.01)
        assertTrue("2.84:1 居然過關了 —— 那這條檢查沒有在檢查", r < threshold)
    }

    /**
     * 未選中那半**不可以**再被單獨染成另一個顏色。
     *
     * 上一條測的是「那個顏色不合格」，這一條測的是「那條路已經拆掉了」——
     * 兩者缺一不可：只留前者的話，有人重新加一個 idle 顏色參數、從別的欄位
     * 餵一個一樣淡的值進來，上一條照樣是綠的（它算的是 foreground）。
     */
    @Test
    fun idleHalfHasNoSeparateColour() {
        val src = read("KeyboardView.kt")
        val problems = mutableListOf<String>()
        if (src.contains("idleColor")) {
            problems += "inputModeFace 又出現了 idleColor —— 未選中那半被單獨染色了"
        }
        if (!Regex("fun inputModeFace\\(\\s*asciiMode: Boolean,\\s*color: Int,\\s*\\)")
                .containsMatchIn(src)
        ) {
            problems += "inputModeFace 的簽章不是單一顏色"
        }
        if (!src.contains("INPUT_MODE_IDLE_SCALE")) {
            problems += "沒有字級這個線索 —— 兩半只剩字重分得開，太弱"
        }
        assertEquals("中/En 的兩態線索沒接好", emptyList<String>(), problems)
    }

    /**
     * 量測那一段必須看得到字重，而且鍵面必須允許省略號。
     *
     * 判準落在**呼叫位置**：`fittedLabelSize` 收的是那一份帶樣式的字串
     * （`AnnotatedString`），而不是拆掉樣式的純文字。
     */
    @Test
    fun inputModeMeasureIsBold() {
        val src = read("KeyboardView.kt")
        assertTrue("KeyboardView.kt 只讀到 ${src.length} 個字元，路徑大概錯了", src.length > 30000)
        val problems = mutableListOf<String>()
        if (!src.contains("fittedLabelSize(measuredFace,")) {
            problems += "鍵面的 fittedLabelSize 收的不是 measuredFace —— 量測會看不到 Bold"
        }
        if (!Regex("""fun fittedLabelSize\(\s*text: AnnotatedString""").containsMatchIn(src)) {
            problems += "fittedLabelSize 仍然吃純文字 —— 樣式（字重）在簽章上就被丟掉了"
        }
        if (!src.contains("inputModeMeasureFace(")) {
            problems += "沒有 inputModeMeasureFace() —— 量測用的那一份不存在"
        }
        // 鍵面那一段的 Text 要有 overflow：Compose 的預設是 Clip（無聲切一半）。
        val keyFace = section(src, "fontSize = fittedLabelSize(measuredFace,")
        if (keyFace == null || !keyFace.contains("overflow = TextOverflow.Ellipsis")) {
            problems += "鍵面的 Text 沒有 overflow = Ellipsis —— 放不下時會被無聲切掉"
        }
        // 量測用 softWrap = false，畫的時候也必須是 false，否則量到
        // 「一行放得下」而畫出來斷成兩行、被 maxLines=1 丟掉後半 ——
        // 那正是「中/En 變成中/」的真正機制。
        if (keyFace == null || !keyFace.contains("softWrap = false")) {
            problems += "鍵面的 Text 沒有 softWrap = false —— 會斷行，然後第二行被丟掉"
        }
        assertEquals("中/En 的量測與截斷沒接好", emptyList<String>(), problems)
    }

    /* ─────────────── 工具 ─────────────── */

    private fun fmt(v: Double) = String.format("%.2f", v)

    /** 前景可能帶 alpha（`$accent@30%` 這種），要先合成到底色上。 */
    private fun composite(fg: Int, bg: Int): Int {
        val a = ((fg ushr 24) and 0xFF) / 255.0
        if (a >= 1.0) return fg or (0xFF shl 24)
        fun mix(shift: Int): Int {
            val f = (fg ushr shift) and 0xFF
            val b = (bg ushr shift) and 0xFF
            return (f * a + b * (1 - a)).toInt().coerceIn(0, 255)
        }
        return (0xFF shl 24) or (mix(16) shl 16) or (mix(8) shl 8) or mix(0)
    }

    private fun luminance(argb: Int): Double {
        fun channel(v: Int): Double {
            val s = v / 255.0
            return if (s <= 0.03928) s / 12.92 else ((s + 0.055) / 1.055).pow(2.4)
        }
        return 0.2126 * channel((argb shr 16) and 0xFF) +
            0.7152 * channel((argb shr 8) and 0xFF) +
            0.0722 * channel(argb and 0xFF)
    }

    private fun ratio(fg: Int, bgRaw: Int): Double {
        // 底色自己也可能是半透明的（`transparent` 的 item.background 就是）；
        // 那時它落在鍵盤底色上。這裡取鍵盤底色當最後一層。
        val bg = composite(bgRaw, 0xFFFFFFFF.toInt())
        val la = luminance(composite(fg, bg))
        val lb = luminance(bg)
        val hi = maxOf(la, lb)
        val lo = minOf(la, lb)
        return (hi + 0.05) / (lo + 0.05)
    }

    private fun read(name: String): String {
        val f = java.io.File("src/main/java/org/luminakey/ime/keyboard/$name")
        return f.takeIf { it.isFile }?.readText(Charsets.UTF_8)
            ?: error("找不到 ${f.path} —— 單元測試的工作目錄應該是 android/app")
    }

    /** 取 [anchor] 之後 40 行，讓判準限定在那一段 Text 的範圍內。 */
    private fun section(src: String, anchor: String): String? {
        val i = src.indexOf(anchor)
        if (i < 0) return null
        return src.substring(i).lineSequence().take(40).joinToString("\n")
    }
}
