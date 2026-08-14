package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.theme.ActionVerb
import java.io.File

/**
 * 引擎還沒好的那十幾秒,工具列上**不准出現按下去不會有事的鍵**。
 *
 * ── 這條測試是修 #105 時差點自己做出來的缺陷 ────────────────────────────
 * 把「鍵盤還沒好」那句話從候選列搬到鍵區之後,工具列整排回來了 ——
 * 那是對的(走查抱怨過它整排消失,使用者連換輸入法的 🌐 都沒有)。
 * 但其中「中/En」與「繁」在那段時間裡是**死的**:沒有 session,
 * `RimeCore.setOption` 是空轉,按下去有按壓色、有震動,什麼都不會發生。
 *
 * 那正是本專案抓過六次的形狀,而且這一次是**修另一個缺陷時長出來的**。
 */
class NotReadyToolbarTest {

    /* ─────────────── 判準 ─────────────── */

    @Test
    fun `會改引擎狀態的動詞都算需要引擎`() {
        for (v in listOf(
            ActionVerb.TOGGLE_OPTION,
            ActionVerb.SET_OPTION,
            ActionVerb.INPUT_MODE_TOGGLE,
            ActionVerb.SCHEMA_PICKER,
            ActionVerb.SCHEMA_NEXT,
            ActionVerb.CANDIDATE_SELECT,
            ActionVerb.CLEAR,
        )) {
            assertTrue("$v 需要 session,引擎不在時不該畫出來", VerbSupport.needsEngine(v))
        }
    }

    /**
     * 🌐 與 ⚙ **必須**留著。
     *
     * 它們是使用者在那十幾秒裡唯二的出路:換一個輸入法先用著、或者去設定
     * 看看發生什麼事。把它們一起濾掉,就變成「一個什麼都按不動的鍵盤」。
     */
    @Test
    fun `出路不需要引擎`() {
        for (v in listOf(
            ActionVerb.SETTINGS,
            ActionVerb.HIDE_KEYBOARD,
            ActionVerb.LAYER,
            ActionVerb.SWITCH_LAYOUT,
            ActionVerb.CURSOR_LEFT,
            ActionVerb.NOOP,
        )) {
            assertFalse("$v 不碰引擎,不該被濾掉", VerbSupport.needsEngine(v))
        }
    }

    /**
     * `when` 必須窮盡 —— 新增一個動詞時編譯器會逼人回答「它需不需要引擎」。
     * 這一條只是把那件事寫下來:每一個動詞都問得出答案,沒有漏網的。
     */
    @Test
    fun `每一個動詞都有答案`() {
        for (v in ActionVerb.entries) {
            VerbSupport.needsEngine(v)
        }
        assertTrue("動詞表只有 ${ActionVerb.entries.size} 個,大概抓錯了", ActionVerb.entries.size >= 20)
    }

    /* ─────────────── 接線 ─────────────── */

    /**
     * `Toolbar()` 真的用了這個判準,而且**與 engineReady 綁在一起**。
     *
     * 驗的是那一段的接線形態(`state.engineReady || !VerbSupport.needsEngine`),
     * 不是「檔案裡有沒有 needsEngine 這個字」—— 那個字在 import 與別處也有。
     */
    @Test
    fun `Toolbar 接上了判準`() {
        val src = File("src/main/java/org/luminakey/ime/keyboard/KeyboardView.kt").readText()
        val at = src.indexOf("private fun Toolbar(")
        assertTrue("KeyboardView 裡找不到 Toolbar() —— 這條測試已經對不上實作", at >= 0)
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
        assertTrue("Toolbar() 的大括號沒有配對成功", end > open)
        val body = src.substring(open, end)
            .lineSequence().joinToString("\n") { line ->
                val i = line.indexOf("//")
                if (i >= 0 && line.take(i).isBlank()) "" else line
            }
        assertTrue(
            "Toolbar 沒有把「引擎不在」納入過濾 —— 那兩顆死鍵又回來了:\n$body",
            body.contains("state.engineReady || !VerbSupport.needsEngine(it.tap.verb)"),
        )
        assertTrue(
            "原本那條「還沒實作的動詞不畫」不見了 —— 不要為了加一條而弄丟另一條",
            body.contains("VerbSupport.isImplemented(it.tap.verb)"),
        )
    }

    /** 反向:上一版那一行只有一條過濾,接線斷言必須看得出差別。 */
    @Test
    fun `上一版只有一條過濾`() {
        val old = "val items = toolbar.items.filter { VerbSupport.isImplemented(it.tap.verb) }"
        assertEquals(false, old.contains("needsEngine"))
    }
}
