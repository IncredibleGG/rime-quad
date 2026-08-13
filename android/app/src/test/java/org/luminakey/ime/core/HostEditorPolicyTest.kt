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

    /**
     * 兩台真的量過的裝置，判準的兩個端點。數字的出處見
     * [HostEditorPolicy.useFullscreen] 的檔頭。
     */
    private object Measured {
        /** 手機橫屏：1080×2400 @420dpi → 2400×1080 px @2.625 = 914×411 dp。 */
        const val PHONE_LAND_H = 411f
        const val PHONE_LAND_IME = 324f

        /** 平板橫屏：`wm size 1600x2560` + density 240 → 2560×1600 px @1.5。 */
        const val TABLET_LAND_H = 1066f
        const val TABLET_LAND_IME = 337f
    }

    private fun phone(imeOptions: Int, landscape: Boolean) = HostEditorPolicy.useFullscreen(
        imeOptions, landscape, Measured.PHONE_LAND_H, Measured.PHONE_LAND_IME,
    )

    @Test
    fun `直屏一律不進全螢幕`() {
        assertFalse(phone(0, landscape = false))
        assertFalse(phone(EditorInfo.IME_FLAG_NO_FULLSCREEN, landscape = false))
        assertFalse(
            "直屏的宿主自己排得下，extract 只會多一條",
            phone(EditorInfo.IME_ACTION_SEARCH, landscape = false),
        )
        // 直屏就是直屏,螢幕再大也一樣 —— 尺寸那一項不可以把方向那一項蓋掉。
        assertFalse(
            HostEditorPolicy.useFullscreen(
                0, landscape = false, Measured.PHONE_LAND_H, Measured.PHONE_LAND_IME,
            ),
        )
    }

    @Test
    fun `橫屏預設要進全螢幕 —— 那是使用者唯一看得到自己在打什麼的路`() {
        assertTrue(phone(0, landscape = true))
        assertTrue(
            "IME_FLAG_NO_EXTRACT_UI 不是「不要全螢幕」：AOSP 仍然進，只是把那一條藏起來，" +
                "而宿主因此不會被縮排、照樣看得見自己",
            phone(EditorInfo.IME_FLAG_NO_EXTRACT_UI, landscape = true),
        )
        assertTrue(
            phone(
                EditorInfo.IME_ACTION_SEARCH or EditorInfo.IME_FLAG_NO_ACCESSORY_ACTION,
                landscape = true,
            ),
        )
    }

    @Test
    fun `宿主明說不要全螢幕時就不進`() {
        assertFalse(phone(EditorInfo.IME_FLAG_NO_FULLSCREEN, landscape = true))
        assertFalse(
            "跟別的 imeOptions 一起帶也一樣",
            phone(
                EditorInfo.IME_ACTION_DONE or EditorInfo.IME_FLAG_NO_FULLSCREEN,
                landscape = true,
            ),
        )
    }

    /* ── 尺寸:大螢幕上宿主排得下,就不要把它整片換掉 ── */

    /**
     * **這一條就是那個缺陷的墓碑。**
     *
     * 判準原本只有「橫向 ＋ 宿主沒帶 NO_FULLSCREEN」,沒有任何一項在看尺寸,
     * 而它挑這條路的理由是一個手機上的量測。實測平板橫屏
     * (`wm size 1600x2560` / density 240)照樣 `mIsFullscreen=true`,宿主被
     * 整片換掉,而鍵盤只用掉 31 % 的高度、中間留下約 700 dp 空白。
     * 截圖:`build/ship-evidence/before-tablet-land.png`
     *
     * 把尺寸那一項拿掉就會紅。
     */
    @Test
    fun `大螢幕橫屏不進全螢幕 —— 宿主自己排得下`() {
        assertFalse(
            "平板橫屏扣掉鍵盤還剩 ${Measured.TABLET_LAND_H - Measured.TABLET_LAND_IME} dp," +
                "宿主排得下自己的輸入框,不該把它整片換成一條 extract",
            HostEditorPolicy.useFullscreen(
                0, landscape = true, Measured.TABLET_LAND_H, Measured.TABLET_LAND_IME,
            ),
        )
    }

    /**
     * 判準是「宿主還剩不剩得下」,不是「螢幕大不大」——
     * 所以鍵盤被調高到吃掉大螢幕時,一樣要進全螢幕。
     */
    @Test
    fun `大螢幕但鍵盤被調得很高時仍然進全螢幕`() {
        assertTrue(
            HostEditorPolicy.useFullscreen(
                0, landscape = true, Measured.TABLET_LAND_H, Measured.TABLET_LAND_H - 100f,
            ),
        )
    }

    @Test
    fun `剩不剩得下是純函式,兩個實測值各在門檻的一邊`() {
        assertTrue(
            "手機橫屏只剩 ${Measured.PHONE_LAND_H - Measured.PHONE_LAND_IME} dp",
            HostEditorPolicy.hostCannotFitEditor(Measured.PHONE_LAND_H, Measured.PHONE_LAND_IME),
        )
        assertFalse(
            "平板橫屏剩 ${Measured.TABLET_LAND_H - Measured.TABLET_LAND_IME} dp",
            HostEditorPolicy.hostCannotFitEditor(Measured.TABLET_LAND_H, Measured.TABLET_LAND_IME),
        )
        // 門檻本身:剛好等於不算「排不下」,少一點才算。
        val t = HostEditorPolicy.MIN_HOST_HEIGHT_DP
        assertFalse(HostEditorPolicy.hostCannotFitEditor(t + 100f, 100f))
        assertTrue(HostEditorPolicy.hostCannotFitEditor(t + 100f - 1f, 100f))
    }

    /**
     * 還不知道的時候走「看得見自己在打什麼」那一邊。
     *
     * 主題還沒載入、`Configuration` 還沒到手時高度是 0。猜錯的兩個代價不對稱:
     * 多進一次全螢幕只是版面不好看,少進一次是使用者**完全看不到自己在打什麼**。
     */
    @Test
    fun `高度還不知道的時候寧可進全螢幕`() {
        assertTrue(HostEditorPolicy.hostCannotFitEditor(0f, 0f))
        assertTrue(HostEditorPolicy.hostCannotFitEditor(1066f, 0f))
        assertTrue(HostEditorPolicy.hostCannotFitEditor(0f, 337f))
        assertTrue(
            HostEditorPolicy.useFullscreen(0, landscape = true, 0f, 0f),
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
                    "landscape = config.orientation ==\n" +
                        "                Configuration.ORIENTATION_LANDSCAPE,",
                    "landscape = false,",
                ),
            // ⚠ 這一條是**批 1 交出去的那一版**：判準只看方向，沒有任何尺寸項。
            //   實測平板橫屏照樣進全螢幕，宿主被整片換掉而鍵盤只用掉三成高度。
            "把尺寸那一項拿掉（批 1 的做法，平板上宿主會被整片換掉）" to
                src.replace(
                    "            screenHeightDp = config.screenHeightDp.toFloat(),\n" +
                        "            imeHeightDp = imeHeightDp(),\n",
                    "",
                ),
            // 尺寸有傳，但鍵盤高是一個從手機量來的常數 —— 那正是這一輪修的
            // 缺陷的來源形狀（「一個裝置上的量測被當成普遍事實」）。
            "鍵盤高寫死成手機上量到的 324dp" to
                src.replace(
                    "    private fun imeHeightDp(): Float {",
                    "    private fun imeHeightDp(): Float {\n        return 324f\n    }\n" +
                        "    private fun imeHeightDpDead(): Float {",
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
                // ⚠ 同一個形狀的第二次:判準補上尺寸之後,「問了但尺寸寫死」
                //   一樣會讓平板橫屏整片被換掉。所以守的是**兩個尺寸都真的
                //   從當下量進去**,不是「有沒有傳參數」。
                if (!fullscreen.contains("config.screenHeightDp")) {
                    out += "onEvaluateFullscreenMode() 沒有把螢幕高讀進去 —— " +
                        "沒有尺寸那一項的話,平板／摺疊機展開後照樣進全螢幕," +
                        "宿主被整片換掉而鍵盤只用掉三成高度"
                }
                if (!fullscreen.contains("imeHeightDp()")) {
                    out += "onEvaluateFullscreenMode() 沒有把鍵盤高讀進去 —— " +
                        "判準是「宿主還剩不剩得下」,不是「螢幕大不大」;" +
                        "少了這一項,使用者把鍵盤拉得很高時就不會進全螢幕了"
                }
            }

            // 鍵盤高必須是**當下算出來的**,不是一個常數。
            val imeHeight = bodyOf(src, "private fun imeHeightDp(): Float {")
            if (imeHeight == null) {
                out += "沒有 imeHeightDp() —— 全螢幕判準拿不到鍵盤高度"
            } else if (!imeHeight.contains("geometry.budget(")) {
                out += "imeHeightDp() 沒有走 KeyGeometry.budget() —— " +
                    "寫死一個高度的話,換主題與使用者拖曳調高度都不會反映在判準上"
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
