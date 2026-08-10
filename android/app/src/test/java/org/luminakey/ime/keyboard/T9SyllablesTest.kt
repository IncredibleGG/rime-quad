package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.core.RimeCandidate

/**
 * 拼音消歧欄。
 *
 * ── 這裡驗的東西，一半是「使用者摸不摸得到」──────────────────────────────
 * 這個專案抓過六個「看得到但摸不到」的缺陷，共同點是畫面完全正常、自動化全綠。
 * 消歧欄有兩個一模一樣的陷阱形狀：
 *
 *   1. **有讀音永遠捲不到。** 格位比讀音少時要靠「⋯」翻頁；翻法只要寫錯一點
 *      （例如每次都回到 0），第三個讀音就永遠不會出現。畫面正常。
 *   2. **篩掉了空白鍵會送出的那一個。** 見 [T9Syllables] 檔頭的鐵律。
 *
 * 兩條都寫成**性質**，而不是幾個例子；而且各自附一份**故意寫壞的實作**，
 * 確認這些檢查在該紅的時候真的會紅（`測試會安靜地跳過自己` 是本專案的舊帳）。
 */
class T9SyllablesTest {

    /* ── 讀音怎麼從 comment 認出來 ─────────────────────────────────────── */

    @Test
    fun `第一個音節`() {
        // librime ScriptTranslator::Spell() 用 speller/delimiter 的第一個字元串接;
        // t9_pinyin 是 " '" → 空白。實測值取自 tools/rime_console.cc。
        assertEquals("ni", T9Syllables.readingOf("ni hao"))
        assertEquals("mi", T9Syllables.readingOf("mi gao"))
        assertEquals("ni", T9Syllables.readingOf("ni"))
        // 別的方案可能用 `'`，一併認得，免得換方案就整欄消失。
        assertEquals("ni", T9Syllables.readingOf("ni'hao"))
        assertEquals("zhuang", T9Syllables.readingOf("zhuang yuan"))
    }

    @Test
    fun `認不出來的 comment 一律回 null 而不是硬切一段出來`() {
        // comment 不是只有拼寫提示會用：simplifier 的字形提示、方案自訂的
        // comment_format 都可能寫別的東西。把「〔简〕」當音節印在鍵面上比不印更糟。
        assertNull(T9Syllables.readingOf(""))
        assertNull(T9Syllables.readingOf("   "))
        assertNull(T9Syllables.readingOf("〔简〕"))
        assertNull(T9Syllables.readingOf("Ni"))          // 拼寫提示是小寫的
        assertNull(T9Syllables.readingOf("ni2"))
        assertNull(T9Syllables.readingOf("abcdefghij"))  // 太長，不可能是一個音節
    }

    @Test
    fun `讀音去重並保持 librime 的順序`() {
        // 順序就是引擎的排序：第一個讀音之所以在前面，是因為引擎認為它最可能。
        val readings = T9Syllables.readingsOf(page("你" to "ni", "米" to "mi", "迷" to "mi", "擬" to "ni"))
        assertEquals(listOf("ni", "mi"), readings)
    }

    @Test
    fun `沒有拼寫提示的方案不會長出一條空欄`() {
        assertEquals(emptyList<String>(), T9Syllables.readingsOf(page("你" to "", "好" to "")))
    }

    /* ── 釘住的讀音 ───────────────────────────────────────────────────── */

    @Test
    fun `釘住一個已經不在集合裡的讀音會被丟掉`() {
        assertEquals("ni", T9Syllables.resolvePin(listOf("ni", "mi"), "ni"))
        assertNull(T9Syllables.resolvePin(listOf("ni", "mi"), "o"))
        assertNull(T9Syllables.resolvePin(listOf("ni", "mi"), null))
    }

    /* ── 篩選 ─────────────────────────────────────────────────────────── */

    @Test
    fun `釘住讀音之後候選真的收斂`() {
        val cands = page("你" to "ni", "米" to "mi", "迷" to "mi", "擬" to "ni", "尼" to "ni")
        assertEquals(listOf(0, 3, 4), T9Syllables.visibleIndices(cands, "ni", highlighted = 0))
        assertEquals(
            "沒有釘住讀音時一個都不能少",
            listOf(0, 1, 2, 3, 4),
            T9Syllables.visibleIndices(cands, null, highlighted = 0),
        )
    }

    /**
     * ⚠ 本檔最重要的一條。
     *
     * 空白鍵上屏的是**引擎目前高亮的候選**，而 Android 這一端還沒有
     * `rs_highlight_candidate` 的 JNI 綁定，前端搬不動那個高亮。把它篩掉，
     * 使用者就會按下空白、拿到一個他從頭到尾沒看過的字 —— 畫面完全正常。
     */
    @Test
    fun `篩選永遠留著引擎高亮的那一個`() {
        val cands = page("你" to "ni", "米" to "mi", "迷" to "mi")
        val shown = T9Syllables.visibleIndices(cands, "mi", highlighted = 0)
        assertTrue("高亮的 0（你 / ni）被篩掉了：空白鍵會送出畫面上沒有的字", shown.contains(0))
        assertEquals(listOf(0, 1, 2), shown)
        // 一份**故意寫壞**的篩選（老老實實只留同讀音的）必須被同一條斷言擋下。
        val broken = cands.indices.filter { T9Syllables.readingOf(cands[it].comment) == "mi" }
        assertFalse("這條檢查沒有在該紅的時候紅", broken.contains(0))
    }

    @Test
    fun `篩選結果依引擎索引遞增且不重複`() {
        val cands = page("你" to "ni", "米" to "mi", "迷" to "mi", "擬" to "ni")
        for (pin in listOf(null, "ni", "mi")) {
            for (hi in listOf(-1, 0, 1, 3)) {
                val shown = T9Syllables.visibleIndices(cands, pin, hi)
                assertEquals("$pin/$hi 有重複", shown.distinct(), shown)
                assertEquals("$pin/$hi 沒有依引擎排序", shown.sorted(), shown)
            }
        }
    }

    @Test
    fun `解析過的釘選保證篩不出空清單`() {
        val cands = page("你" to "ni", "米" to "mi")
        val readings = T9Syllables.readingsOf(cands)
        for (r in readings) {
            val pin = T9Syllables.resolvePin(readings, r)
            assertTrue(
                "釘 $r 之後候選列是空的 —— 使用者會看到一條什麼都沒有的候選列",
                T9Syllables.visibleIndices(cands, pin, highlighted = -1).isNotEmpty(),
            )
        }
    }

    /* ── 格位與翻頁 ───────────────────────────────────────────────────── */

    @Test
    fun `讀音塞得下就不出現翻頁鍵`() {
        val cells = T9Syllables.cells(listOf("ni", "mi"), slots = 3, offset = 0)
        assertEquals(listOf(reading("ni"), reading("mi"), T9Syllables.Cell.Empty), cells)
        assertEquals("塞得下時 offset 不該漂移", 0, T9Syllables.nextOffset(listOf("ni", "mi"), 3, 0))
    }

    /**
     * **一欄只能有一種意思。**
     *
     * 第一版讓多出來的格位維持原本的標點，實機截圖立刻看出問題：讀音只有
     * ni / mi 時，整欄是「ni / mi / ？」—— 使用者沒有理由知道第三個不是第三個
     * 讀音。所以 [T9Syllables.cells] 的長度恆等於格位數，多的是 [Cell.Empty]。
     */
    @Test
    fun `消歧欄是全有或全無,不會半邊讀音半邊標點`() {
        for (n in 1..6) {
            val readings = (1..n).map { "s$it" }
            for (slots in T9Syllables.MIN_SLOTS..4) {
                val cells = T9Syllables.cells(readings, slots, offset = 0)
                assertEquals("n=$n slots=$slots 的格數不對", slots, cells.size)
            }
        }
        assertEquals("沒有讀音就整欄不接管", emptyList<T9Syllables.Cell>(), T9Syllables.cells(emptyList(), 3, 0))
    }

    /** 空格必須是 §9.6 的 spacer，不是一顆沒有字、按下去沒反應的鍵。 */
    @Test
    fun `多出來的格位是空的,不是一顆啞鍵`() {
        val layout = FixtureRepo().loadLayout(T9_LAYOUT).value!!
        val original = layout.layer("t9")!!.rows.flatMap { it.keys }.first { it.id == "pu_question" }
        val empty = T9Syllables.slotKey(original, T9Syllables.Cell.Empty, pinned = false)
        assertTrue("空格要走 spacer,否則 TalkBack 會停在一顆念作「按鈕」的鍵上", empty.spacer)
        assertFalse("空格不可以是按得下去的鍵", empty.hasTapBehavior)
        assertEquals("寬度仍要保留,否則整列會重排", original.width, empty.width, 0.0001f)
    }

    @Test
    fun `讀音塞不下時最後一格讓給翻頁鍵`() {
        val readings = listOf("ni", "mi", "o", "m")
        val cells = T9Syllables.cells(readings, slots = 3, offset = 0)
        assertEquals(listOf(reading("ni"), reading("mi"), T9Syllables.Cell.More), cells)
    }

    /**
     * **每一個讀音都摸得到。** 這是性質，不是靠人數格子。
     */
    @Test
    fun `連續按翻頁鍵可以走到每一個讀音`() {
        for (n in 1..12) {
            val readings = (1..n).map { "s$it" }
            for (slots in T9Syllables.MIN_SLOTS..4) {
                assertReachable(readings, slots) { r, s, o -> T9Syllables.nextOffset(r, s, o) }
            }
        }
    }

    /** 同一條斷言餵一份寫壞的翻頁（永遠回到 0）必須失敗，否則它什麼都沒在守。 */
    @Test
    fun `寫壞的翻頁會被同一條檢查擋下`() {
        val readings = listOf("a", "b", "c", "d", "e")
        var caught = false
        try {
            assertReachable(readings, slots = 3) { _, _, _ -> 0 }
        } catch (e: AssertionError) {
            caught = true
        }
        assertTrue("『每個讀音都摸得到』這條檢查在該紅的時候沒有紅", caught)
    }

    private fun assertReachable(
        readings: List<String>,
        slots: Int,
        next: (List<String>, Int, Int) -> Int,
    ) {
        val seen = LinkedHashSet<String>()
        var offset = 0
        // 走 readings.size + 1 圈綽綽有餘；走不完就是走不到。
        repeat(readings.size + 1) {
            for (c in T9Syllables.cells(readings, slots, offset)) {
                if (c is T9Syllables.Cell.Reading) seen += c.syllable
            }
            offset = next(readings, slots, offset)
        }
        assertEquals(
            "n=${readings.size} slots=$slots：這些讀音永遠捲不到 —— " +
                "使用者看得到那顆「⋯」，按下去卻永遠等不到它們",
            readings.toSet(),
            seen,
        )
    }

    /* ── 合成出來的鍵 ─────────────────────────────────────────────────── */

    @Test
    fun `消歧欄的鍵保留原本的幾何,只換鍵面與行為`() {
        val layout = FixtureRepo().loadLayout(T9_LAYOUT).value!!
        val original = layout.layer("t9")!!.rows.flatMap { it.keys }.first { it.id == "pu_comma" }
        val slot = T9Syllables.slotKey(original, reading("ni"), pinned = true)

        assertEquals("寬度一變整列就重排,使用者會看到一個自己跳動的鍵盤", original.width, slot.width, 0.0001f)
        assertEquals(original.id, slot.id)
        assertEquals(original.style, slot.style)
        assertEquals("ni", slot.label)
        assertTrue("釘住的那一格要看得出來", slot.active)
        assertNull("消歧欄不送鍵給引擎", slot.send)
        assertNull("「，」的長按盤在組字中不該還開得出來", slot.popup)
        assertTrue(
            "沒有 tap 的鍵拿不到無障礙的 ACTION_CLICK —— TalkBack 使用者按下去不會有反應",
            slot.hasTapBehavior,
        )
    }

    /* ── 格位宣告本身 ─────────────────────────────────────────────────── */

    /**
     * 宣告已經搬進佈局 YAML（layer 上的 `syllable_slots:`）。這條測試是
     * 「它不會靜靜地失效」的唯一保證：有人把 `pu_comma` 改名，消歧欄會整欄
     * 消失，而畫面上只是照常顯示標點 —— 沒有任何東西會叫。
     *
     * ⚠ 清單寫死在這裡是**故意**的：它就是「哪幾份佈局應該有消歧欄」的規格。
     * 新增一份九宮格佈局時要**同時**加進這裡，漏了就等於漏了驗證。
     */
    @Test
    fun `每一個宣告的格位都真的存在於它宣告的那一層`() {
        val repo = FixtureRepo()
        val declaring = listOf("cn-t9-pinyin", "cn-t9-pinyin-numrow")
        for (layoutId in declaring) {
            val layout = repo.loadLayout(layoutId).value
            assertNotNull("佈局 $layoutId 載不起來", layout)
            val layer = layout!!.layer("t9")
            assertNotNull("佈局 $layoutId 沒有 t9 層", layer)
            val keyIds = layer!!.syllableSlots
            assertTrue(
                "$layoutId/t9 沒有宣告 syllable_slots —— 九宮格少了消歧欄，" +
                    "而畫面上只是照常顯示標點，不會有任何東西叫",
                keyIds.isNotEmpty(),
            )
            assertTrue(
                "$layoutId/t9 只宣告了 ${keyIds.size} 格，" +
                    "少於 ${T9Syllables.MIN_SLOTS} 格就翻不了頁，會有讀音摸不到",
                keyIds.size >= T9Syllables.MIN_SLOTS,
            )
            val rows = layer.rows
            val rowOf = HashMap<String, Int>()
            rows.forEachIndexed { i, r -> r.keys.forEach { k -> k.id?.let { rowOf[it] = i } } }
            for (id in keyIds) {
                assertTrue(
                    "$layoutId/t9 裡沒有 id 為 $id 的鍵 —— " +
                        "消歧欄會整欄消失，而畫面上只是照常顯示標點",
                    rowOf.containsKey(id),
                )
            }
            assertEquals(
                "$layoutId 的格位必須落在不同列上（那是一整條直欄）",
                keyIds.size,
                keyIds.mapNotNull { rowOf[it] }.distinct().size,
            )
            assertFalse(
                "$layoutId 的消歧欄吃到了**底列**。底列是導覽列（!@# 是 " +
                    "switch_layout），而 LayoutEscape 走的是佈局檔的靜態內容、" +
                    "看不見執行期替換 —— 在那裡動手就是在死路檢查上開一個測不到的洞。",
                keyIds.any { rowOf[it] == rows.lastIndex },
            )
        }
    }

    @Test
    fun `只有宣告過的那一層會被接管`() {
        val repo = FixtureRepo()
        val t9 = repo.loadLayout(T9_LAYOUT).value!!
        assertEquals(
            listOf("pu_comma", "pu_period", "pu_question"),
            T9Syllables.slotKeys(t9, "t9"),
        )
        assertEquals("數字層仍然是它自己", emptyList<String>(), T9Syllables.slotKeys(t9, "num"))
        assertEquals("英數層仍然是它自己", emptyList<String>(), T9Syllables.slotKeys(t9, "en"))
        val qwerty = repo.loadLayout("qwerty").value!!
        assertEquals(
            "全鍵盤沒有消歧欄（走的是退化規則:上方橫排,不是什麼都不畫）",
            emptyList<String>(),
            T9Syllables.slotKeys(qwerty, "default"),
        )
        assertEquals(emptyList<String>(), T9Syllables.slotKeys(null, "t9"))
    }

    /* ── 改寫之後,引擎到底有沒有聽懂 ─────────────────────────────────── */

    /**
     * 這一組數字是**模擬器上實測**出來的，不是想像的。
     *
     * 裝置上放一份舊的單編碼 `t9_pinyin`（`alphabet: 'ADGJMPTW'`），
     * 送 `MGGAM` 再 `rs_set_input("niGAM")`：
     *
     *     rs_set_input 回傳 = **true**（標頭說會回 false,實測不會）
     *     preedit = "niGAM"
     *     候選    = 好#hao／號#hao／高#gao／搞#gao／汗#han
     *     選第一個 → COMMIT "ni好"
     *
     * 使用者拿到的就是那個 —— 真機回報的「我選擇 ni 他就直接給我輸入了」。
     * 前端不能靠 `rs_set_input` 的回傳值，只能回頭問候選。
     */
    @Test
    fun `引擎沒把那段當拼音時要認得出來`() {
        val staleSchemaPage = page(
            "好" to "hao", "號" to "hao", "高" to "gao", "搞" to "gao", "汗" to "han",
        )
        assertFalse(
            "第二音節的單字被當成「ni 被接受了」—— 使用者會上屏 ni好",
            T9Syllables.rewriteAccepted(staleSchemaPage, listOf("ni")),
        )

        // 雙編碼方案上同一次改寫的實測結果:候選是 ni 開頭的**詞**。
        val goodPage = page(
            "你好" to "ni hao", "妳好" to "ni hao", "你敢" to "ni gan",
            "你搞" to "ni gao", "擬稿" to "ni gao",
        )
        assertTrue(T9Syllables.rewriteAccepted(goodPage, listOf("ni")))
        assertTrue("兩個音節都定了也要認得", T9Syllables.rewriteAccepted(goodPage, listOf("ni", "hao")))
        assertFalse(
            "第二個音節對不上就是沒聽懂",
            T9Syllables.rewriteAccepted(goodPage, listOf("ni", "mao")),
        )
    }

    @Test
    fun `沒有候選就是沒聽懂,不是沒意見`() {
        assertFalse(T9Syllables.rewriteAccepted(emptyList(), listOf("ni")))
        // 一個音節都還沒定的時候本來就沒有東西要驗收。
        assertTrue(T9Syllables.rewriteAccepted(emptyList(), emptyList()))
    }

    /**
     * 沒有 `spelling_hints` 的方案 comment 是空的 —— 那種方案連消歧欄都不會
     * 出現（[readingsOf] 回空清單），所以走不到驗收。萬一走到了，答案必須是
     * 「沒聽懂」而不是「算你過」:寧可這一下沒反應,也不能讓使用者上屏一段
     * 他沒看過的字。
     */
    @Test
    fun `comment 是空的一律當成沒聽懂`() {
        assertFalse(T9Syllables.rewriteAccepted(page("你好" to "", "米高" to ""), listOf("ni")))
    }

    private fun reading(s: String) = T9Syllables.Cell.Reading(s)

    private fun page(vararg pairs: Pair<String, String>): List<RimeCandidate> =
        pairs.mapIndexed { i, (text, comment) ->
            RimeCandidate(text = text, comment = comment, label = (i + 1).toString())
        }

    private companion object {
        const val T9_LAYOUT = "cn-t9-pinyin"
    }
}
