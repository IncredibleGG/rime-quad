package org.luminakey.ime.core

import android.view.inputmethod.EditorInfo
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * [HostEditorPolicy] 的兩條規則，外加**它們真的被服務呼叫**這件事。
 *
 * 這兩個缺陷（G46 全螢幕 extract、G02 游標移走）的共同形狀是：
 * **那個回呼從來沒有被覆寫過**。所以純函式全綠並不代表修好了 ——
 * 一個沒有人呼叫的正確答案與沒有答案是同一件事。底下第 3 節掃的就是那條線。
 */
class HostEditorPolicyTest {

    /* ─────────────── 1. 全螢幕 ─────────────── */

    @Test
    fun `任何情況都不進全螢幕 extract`() {
        assertFalse("預設的編輯框不該進全螢幕", HostEditorPolicy.useFullscreen(0))
        assertFalse(
            "宿主明說 IME_FLAG_NO_FULLSCREEN 時更不該進",
            HostEditorPolicy.useFullscreen(EditorInfo.IME_FLAG_NO_FULLSCREEN),
        )
        assertFalse(
            "帶著別的 imeOptions 也一樣",
            HostEditorPolicy.useFullscreen(
                EditorInfo.IME_ACTION_SEARCH or EditorInfo.IME_FLAG_NO_EXTRACT_UI
            ),
        )
    }

    @Test
    fun `讀得出宿主有沒有明說不要全螢幕`() {
        assertTrue(HostEditorPolicy.hostForbidsFullscreen(EditorInfo.IME_FLAG_NO_FULLSCREEN))
        assertTrue(
            HostEditorPolicy.hostForbidsFullscreen(
                EditorInfo.IME_ACTION_DONE or EditorInfo.IME_FLAG_NO_FULLSCREEN
            )
        )
        assertFalse(HostEditorPolicy.hostForbidsFullscreen(0))
        assertFalse(HostEditorPolicy.hostForbidsFullscreen(EditorInfo.IME_ACTION_DONE))
    }

    /* ─────────────── 2. 游標移走 ─────────────── */

    @Test
    fun `沒有在組字時什麼都不做`() {
        assertFalse(
            HostEditorPolicy.cursorLeftComposition(
                engineComposing = false,
                newSelStart = 99, newSelEnd = 99,
                candidatesStart = 0, candidatesEnd = 5,
            )
        )
    }

    @Test
    fun `游標留在組字區裡不算移走`() {
        // setComposingText(text, 1) 把游標放在末端；那是常態，每按一顆鍵都會來一次。
        assertFalse(
            "組字區末端就是我們自己放的位置",
            HostEditorPolicy.cursorLeftComposition(true, 5, 5, 0, 5),
        )
        assertFalse(
            "組字區起點也還在裡面",
            HostEditorPolicy.cursorLeftComposition(true, 0, 0, 0, 5),
        )
    }

    @Test
    fun `游標跑到組字區外面就是移走了`() {
        assertTrue("點到組字區前面", HostEditorPolicy.cursorLeftComposition(true, 2, 2, 4, 9))
        assertTrue("點到組字區後面", HostEditorPolicy.cursorLeftComposition(true, 12, 12, 4, 9))
    }

    @Test
    fun `選取一段字也算移走`() {
        assertTrue(
            "使用者拉出了選取範圍，組字這件事已經結束了",
            HostEditorPolicy.cursorLeftComposition(true, 4, 9, 4, 9),
        )
    }

    /**
     * 這一條記的是一個**差點做進去的錯誤**。
     *
     * 宿主回報「沒有組字區」（-1）看起來就像該清掉，但 `refreshFromRime` 的
     * `commitText()` 與 `setComposingText()` 之間本來就有那麼一瞬間；那一刻的
     * 座標晚一點才送到我們手上。把它當成「使用者移走了游標」的話，
     * **每一次選字都會把接下來那一段組字清掉**。
     */
    @Test
    fun `宿主沒有回報組字區時不動作`() {
        assertFalse(
            "-1 是過期座標，不是「使用者移走了游標」",
            HostEditorPolicy.cursorLeftComposition(true, 2, 2, -1, -1),
        )
    }

    /* ─────────────── 3. 服務真的呼叫了它們 ─────────────── */

    /**
     * 這兩個回呼在這一版之前**整個 repo 零命中**。所以要守的不是判斷對不對，
     * 是「有沒有人問」。判準落在**覆寫的簽章與那一行呼叫**上，不是檔案裡有沒有
     * 出現這幾個字 —— 後者會被 import、被註解、被別處的定義餵飽。
     */
    @Test
    fun `RimeInputMethodService 覆寫了那兩個回呼並照結果做事`() {
        val src = read()
        assertTrue("原始碼只讀到 ${src.length} 個字元，路徑大概錯了", src.length >= MIN_CHARS)
        val problems = wiringProblems(src)
        assertTrue("接線斷了：\n  " + problems.joinToString("\n  "), problems.isEmpty())
    }

    /**
     * 反向測試：把接線逐條拆掉，每一種拆法都必須被上面那一條抓到。
     *
     * 沒做過這一步的守門一律當作沒有 —— 本專案已經吃過四次
     * 「守門全綠而線根本沒接」的虧。
     */
    @Test
    fun `把接線拆掉會被抓到`() {
        val src = read()
        val mutations = listOf(
            "刪掉 onEvaluateFullscreenMode 的覆寫" to
                Regex("""override fun onEvaluateFullscreenMode\(\)""")
                    .replace(src, "private fun neverCalledFullscreen()"),
            "覆寫還在，但改成回 super（AOSP 的橫屏規則）" to
                src.replace(
                    "HostEditorPolicy.useFullscreen(currentInputEditorInfo?.imeOptions ?: 0)",
                    "super.onEvaluateFullscreenMode()",
                ),
            "刪掉 onUpdateSelection 的覆寫" to
                Regex("""override fun onUpdateSelection\(""")
                    .replace(src, "private fun neverCalledSelection("),
            "覆寫還在，但不再問 HostEditorPolicy" to
                src.replace(
                    "HostEditorPolicy.cursorLeftComposition(",
                    "org.luminakey.ime.core.NeverTrue.cursorLeftComposition(",
                ),
            "問了，但游標移走時什麼都不做" to
                src.replace("finishAndForgetComposition()", "Unit"),
            "空白鍵左右滑改回直接送 DPAD（繞過引擎）" to
                src.replace(
                    "ActionVerb.CURSOR_LEFT -> moveHostCursor(",
                    "ActionVerb.CURSOR_LEFT -> sendHostKey(",
                ),
            "moveHostCursor 還在，但不再沖組字" to
                src.replace(
                    "        flushCompositionForLiteralText()\n        sendHostKey(keyCode)",
                    "        sendHostKey(keyCode)",
                ),
        )
        for ((name, mutated) in mutations) {
            assertTrue(
                "植入失敗（錨點對不上）：$name —— 這一條等於沒驗到",
                mutated != src,
            )
            assertTrue(
                "拆法「$name」居然沒有被抓到 —— 那條守門是假的",
                wiringProblems(mutated).isNotEmpty(),
            )
        }
    }

    private companion object {
        const val MIN_CHARS = 20000

        fun read(): String {
            val f = File("src/main/java/org/luminakey/ime/RimeInputMethodService.kt")
            return f.takeIf { it.isFile }?.readText(Charsets.UTF_8)
                ?: error("找不到 ${f.path} —— 單元測試的工作目錄應該是 android/app")
        }

        /** 每一條都是「呼叫位置或資料流」，不是「字串在不在」。 */
        fun wiringProblems(src: String): List<String> {
            val out = mutableListOf<String>()
            val fullscreen = bodyOf(src, "override fun onEvaluateFullscreenMode()")
            if (fullscreen == null) {
                out += "沒有覆寫 onEvaluateFullscreenMode() —— 橫屏會掉回 AOSP 的全螢幕 extract"
            } else if (!fullscreen.contains("HostEditorPolicy.useFullscreen(")) {
                out += "onEvaluateFullscreenMode() 沒有問 HostEditorPolicy.useFullscreen()"
            }

            val selection = bodyOf(src, "override fun onUpdateSelection(")
            if (selection == null) {
                out += "沒有覆寫 onUpdateSelection() —— 沒有任何路徑會告訴引擎游標被移走了"
            } else {
                if (!selection.contains("HostEditorPolicy.cursorLeftComposition(")) {
                    out += "onUpdateSelection() 沒有問 HostEditorPolicy.cursorLeftComposition()"
                }
                if (!selection.contains("finishAndForgetComposition()")) {
                    out += "onUpdateSelection() 問了，但沒有結束組字、也沒有清掉引擎"
                }
            }

            // 我們自己的空白鍵左右滑同樣繞過引擎（直接送 DPAD 給宿主）。
            // 那條路必須自己先把組字沖出去，不能指望上面那個回呼替它收尾 ——
            // 回呼那一支刻意不理會「宿主沒有組字區」的過期座標，接不住它。
            for (verb in listOf("CURSOR_LEFT", "CURSOR_RIGHT", "CURSOR_HOME", "CURSOR_END")) {
                if (!Regex("""ActionVerb\.$verb -> moveHostCursor\(""").containsMatchIn(src)) {
                    out += "$verb 沒有走 moveHostCursor() —— 它會直接送 DPAD，繞過引擎"
                }
            }
            val move = bodyOf(src, "private fun moveHostCursor(")
            if (move == null) {
                out += "沒有 moveHostCursor()"
            } else {
                if (!move.contains("flushCompositionForLiteralText()")) {
                    out += "moveHostCursor() 沒有先把組字沖出去"
                }
                if (!move.contains("sendHostKey(")) {
                    out += "moveHostCursor() 沒有真的把鍵送給宿主 —— 游標不會動"
                }
            }
            return out
        }

        /**
         * 取 [anchor] 之後那一段（到下一個同縮排的 `override fun` / `private fun` 為止）。
         *
         * 限定範圍是刻意的：不限定的話，三百行外的另一個函式就能把判準餵飽。
         */
        fun bodyOf(src: String, anchor: String): String? {
            val start = src.indexOf(anchor)
            if (start < 0) return null
            val rest = src.substring(start + anchor.length)
            val next = Regex("""\n    (?:override|private|internal) fun """).find(rest)
            return rest.substring(0, next?.range?.first ?: rest.length)
        }
    }
}
