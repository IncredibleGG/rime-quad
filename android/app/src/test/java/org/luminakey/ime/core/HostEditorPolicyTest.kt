package org.luminakey.ime.core

import android.view.inputmethod.EditorInfo
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * [HostEditorPolicy] 的兩條規則，外加**它們真的被服務呼叫**這件事。
 *
 * 這兩個缺陷（G46 橫屏、G02 游標移走）的共同形狀是：
 * **那個回呼從來沒有被覆寫過**。所以純函式全綠並不代表修好了 ——
 * 一個沒有人呼叫的正確答案與沒有答案是同一件事。底下第 3 節掃的就是那條線。
 *
 * ⚠ 第 1 節這一輪**整組換掉了**。上一版斷言的是「任何情況都不進全螢幕」，
 *   而那個答案本身是錯的：拿掉 extract 模式等於把「橫屏螢幕不夠高」的補償
 *   拿掉而不補，實測宿主連一個輸入框都排不下（見 [HostEditorPolicy.useFullscreen]
 *   的量測）。現在斷言的是 AOSP 那條規則，而**那條輸入條由我們自己畫**。
 */
class HostEditorPolicyTest {

    /* ─────────────── 1. 全螢幕 extract ─────────────── */

    @Test
    fun `直屏一律不進全螢幕`() {
        assertFalse(HostEditorPolicy.useFullscreen(0, landscape = false))
        assertFalse(
            HostEditorPolicy.useFullscreen(EditorInfo.IME_FLAG_NO_FULLSCREEN, landscape = false),
        )
        assertFalse(
            "直屏的宿主自己排得下，extract 只會多一條",
            HostEditorPolicy.useFullscreen(EditorInfo.IME_ACTION_SEARCH, landscape = false),
        )
    }

    @Test
    fun `橫屏預設要進全螢幕 —— 那是使用者唯一看得到自己在打什麼的路`() {
        assertTrue(HostEditorPolicy.useFullscreen(0, landscape = true))
        assertTrue(
            "IME_FLAG_NO_EXTRACT_UI 不是「不要全螢幕」：AOSP 仍然進，只是把那一條藏起來，" +
                "而宿主因此不會被縮排、照樣看得見自己",
            HostEditorPolicy.useFullscreen(EditorInfo.IME_FLAG_NO_EXTRACT_UI, landscape = true),
        )
        assertTrue(
            HostEditorPolicy.useFullscreen(
                EditorInfo.IME_ACTION_SEARCH or EditorInfo.IME_FLAG_NO_ACCESSORY_ACTION,
                landscape = true,
            ),
        )
    }

    @Test
    fun `宿主明說不要全螢幕時就不進`() {
        assertFalse(
            HostEditorPolicy.useFullscreen(EditorInfo.IME_FLAG_NO_FULLSCREEN, landscape = true),
        )
        assertFalse(
            "跟別的 imeOptions 一起帶也一樣",
            HostEditorPolicy.useFullscreen(
                EditorInfo.IME_ACTION_DONE or EditorInfo.IME_FLAG_NO_FULLSCREEN,
                landscape = true,
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
    fun `RimeInputMethodService 覆寫了那幾個回呼並照結果做事`() {
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
            "onEvaluateFullscreenMode 還在，但不再問 HostEditorPolicy" to
                src.replace(
                    "HostEditorPolicy.useFullscreen(",
                    "org.luminakey.ime.core.NeverFullscreen.useFullscreen(",
                ),
            // ⚠ 這一條就是**上一版交出去的東西**：橫屏一律 false。
            //   它讓宿主的輸入框在橫屏被鍵盤 100 % 蓋住。
            "改回一律 false（上一版的做法，橫屏就看不到自己在打什麼）" to
                src.replace(
                    "landscape = resources.configuration.orientation ==\n" +
                        "                Configuration.ORIENTATION_LANDSCAPE,",
                    "landscape = false,",
                ),
            "刪掉 onCreateExtractTextView 的覆寫（那一條會變回系統畫的）" to
                src.replace(
                    "    override fun onCreateExtractTextView(): View {",
                    "    private fun neverCalledExtract(): View {",
                ),
            "覆寫還在，但交出去的不是我們畫的那一條" to
                src.replace(
                    "val v = ThemedExtractView(this)",
                    "val v = android.widget.LinearLayout(this)",
                ),
            "extract 那一條建出來時沒有套主題" to
                src.replace("v.applyTheme(effectiveTheme())", "Unit"),
            "換主題時 extract 那一條不跟著換" to
                src.replace("        extractView?.applyTheme(effectiveTheme())\n", ""),
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
                out += "沒有覆寫 onEvaluateFullscreenMode()"
            } else {
                if (!fullscreen.contains("HostEditorPolicy.useFullscreen(")) {
                    out += "onEvaluateFullscreenMode() 沒有問 HostEditorPolicy.useFullscreen()"
                }
                // ⚠ 上一版的缺陷就是「問了，但答案寫死」。所以要守的不是「有沒有問」，
                //   是**橫向與否真的被讀進去**。
                if (!fullscreen.contains("Configuration.ORIENTATION_LANDSCAPE")) {
                    out += "onEvaluateFullscreenMode() 沒有把「現在是不是橫屏」讀進去 —— " +
                        "答案寫死的話橫屏就沒有 extract，宿主的輸入框會被鍵盤蓋掉 100%"
                }
            }

            // extract 那一條必須是我們畫的（G46 抱怨的正是「那一條吃不到主題」），
            // 而且建出來就要套、換主題也要跟著換。
            val extract = bodyOf(src, "override fun onCreateExtractTextView(): View {")
            if (extract == null) {
                out += "沒有覆寫 onCreateExtractTextView() —— 橫屏那一條會是系統畫的，" +
                    "帶著原生 Done 鈕、吃不到 core/themes"
            } else {
                if (!extract.contains("ThemedExtractView(")) {
                    out += "onCreateExtractTextView() 交出去的不是 ThemedExtractView"
                }
                if (!extract.contains("applyTheme(")) {
                    out += "onCreateExtractTextView() 建出來沒有套主題"
                }
            }
            val sync = bodyOf(src, "private fun syncConfigToUi()")
            if (sync == null) {
                out += "找不到 syncConfigToUi()"
            } else if (!sync.contains("extractView?.applyTheme(")) {
                out += "換主題時沒有同步 extract 那一條 —— 它不在 Compose 樹裡，不會自己跟上"
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

            // 我們自己的 `cursor:*` 動詞同樣繞過引擎（直接送 DPAD 給宿主）。
            //
            // ⚠ 這幾個動詞目前**使用者按不到**：佈局裡它們全部掛在 `swipe:` 底下，
            //   而 Android 端沒有實作 swipe 分派（§9.6 說 swipe 是 OPTIONAL）。
            //   守在這裡是因為 G31 接上 swipe 的那一天它必須已經是對的 ——
            //   「按得到的那天才發現繞過引擎」是本專案吃過的形狀。
            //   那條路本身按不按得到，由 LayoutSwipeReachabilityTest 明著記著。
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
         * 取 [anchor] 之後那一段（到下一個同縮排的成員為止 —— 另一個 `fun`，
         * 或下一個成員的 KDoc）。
         *
         * 限定範圍是刻意的：不限定的話，三百行外的另一個函式就能把判準餵飽。
         * 停在 KDoc 上也是刻意的：註解裡提到某個名字不算「接上了線」。
         */
        fun bodyOf(src: String, anchor: String): String? {
            val start = src.indexOf(anchor)
            if (start < 0) return null
            val rest = src.substring(start + anchor.length)
            val next = Regex("""\n    (?:/\*\*|(?:override|private|internal) fun )""").find(rest)
            return rest.substring(0, next?.range?.first ?: rest.length)
        }
    }
}
