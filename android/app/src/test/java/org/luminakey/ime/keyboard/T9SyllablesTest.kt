package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.core.RimeCandidate
import org.luminakey.ime.theme.LayoutKey
import org.luminakey.ime.theme.RepoFixtures
import java.io.File

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
        assertEquals(listOf(reading("ni"), reading("mi"), T9Syllables.Cell.Original), cells)
        assertEquals("塞得下時 offset 不該漂移", 0, T9Syllables.nextOffset(listOf("ni", "mi"), 3, 0))
    }

    /**
     * ⚠ **task #78 的紅線:沒有用到的格位要還原成它原本的鍵。**
     *
     * 實機截圖(`build/look/android/04-t9-composing-ime.png`)上,九宮格左欄
     * 原本是「，。？!@#」,打了 PGM 之後變成「qin / pin / □ / !@#」——
     * 第三格的「？」整顆消失,留下一個空的灰色方塊。使用者看到的是鍵盤
     * 破了一個洞,而那個洞按下去什麼都不會發生。
     *
     * 空洞比「一欄兩種意思」更糟:標點還在的話,最壞情況是使用者多看一眼;
     * 空洞則是**把一顆本來就在的鍵拿走**,而且沒有任何東西說它去哪了。
     */
    @Test
    fun `讀音比格位少時沒用到的格位還原成原本的鍵`() {
        val layer = FixtureRepo().loadLayout(T9_LAYOUT).value!!.layer("t9")!!
        val keys = layer.rows.flatMap { it.keys }
        val slotIds = layer.syllableSlots
        assertEquals("這條測試假設三格", 3, slotIds.size)

        // 兩個讀音配三格 —— 正是實機截圖那一幕(qin / pin / ?)。
        val cells = T9Syllables.cells(listOf("qin", "pin"), slots = slotIds.size, offset = 0)
        val third = keys.first { it.id == slotIds[2] }
        val shown = T9Syllables.slotKey(third, cells[2], pinned = false)

        assertEquals("第三格的鍵面要還是「${third.label}」,不是空的", third.label, shown.label)
        assertFalse("第三格變成 spacer = 畫面上一個灰色空洞", shown.spacer)
        assertTrue("還原的格位必須還按得出標點", shown.hasTapBehavior)
        assertEquals("還原的格位連長按盤都要留著", third.popup, shown.popup)
    }

    /**
     * **格數恆等於宣告的格位數。**
     *
     * 呼叫端是 `slotIds.zip(cells(...))`：短一格就是短一個 key id 沒被接管，
     * 而那個 id 會靜靜地照佈局畫 —— 沒有任何東西會叫。
     *
     * ⚠ 填不滿時補的是 [T9Syllables.Cell.Original]（原本那顆鍵),不是空洞。
     * 反過來做過一版,使用者在實機上看到的是鍵盤破了一個洞(task #78)。
     */
    @Test
    fun `消歧欄的格數恆等於宣告的格位數`() {
        for (n in 0..6) {
            val readings = (1..n).map { "s$it" }
            for (slots in T9Syllables.MIN_SLOTS..4) {
                val cells = T9Syllables.cells(readings, slots, offset = 0)
                assertEquals("n=$n slots=$slots 的格數不對", slots, cells.size)
            }
        }
        // ⚠ 一個讀音都沒有時**也**是一格對一格 —— 每一格都是 Original(照佈局畫)。
        // 這裡一度斷言「回空清單」,而 [T9Syllables.cells] 的 kdoc 寫的是
        // 「長度恆等於 slots」:文件與碼不符。改的是碼那一邊,因為呼叫端真的在
        // zip,而 zip 遇到短清單不會叫,只會少接管幾個 key id。
        assertEquals(
            "沒有讀音時仍然要一格對一格(每一格都是 Original = 照佈局畫)",
            List(3) { T9Syllables.Cell.Original },
            T9Syllables.cells(emptyList(), 3, 0),
        )
    }

    /**
     * 沒用到的格位是**原鍵本人**,不是複製品 —— 少一個欄位就少一種行為,
     * 而少掉的那一種(長按盤、朗讀名、`send`)在畫面上都看不出來。
     */
    @Test
    fun `沒用到的格位退回的是原鍵本人`() {
        val layout = FixtureRepo().loadLayout(T9_LAYOUT).value!!
        for (id in listOf("pu_comma", "pu_period", "pu_question")) {
            val original = layout.layer("t9")!!.rows.flatMap { it.keys }.first { it.id == id }
            val kept = T9Syllables.slotKey(original, T9Syllables.Cell.Original, pinned = false)
            assertEquals("$id 沒有原封不動退回來", original, kept)
            assertFalse("$id 變成 spacer = 畫面上一個灰色空洞", kept.spacer)
            assertTrue("$id 必須還按得出標點", kept.hasTapBehavior)
        }
    }

    /**
     * 反向:被真正接管的那幾格**不可以**是 spacer。
     *
     * 上面那條只說「沒用到的要還原」,一份把每一格都無條件 return original 的
     * 實作照樣過 —— 那樣消歧欄整條不會出現。這一條把另一半釘住。
     */
    @Test
    fun `被接管的格位一定畫得出鍵面`() {
        val layout = FixtureRepo().loadLayout(T9_LAYOUT).value!!
        val original = layout.layer("t9")!!.rows.flatMap { it.keys }.first { it.id == "pu_question" }
        for (cell in listOf(reading("ni"), T9Syllables.Cell.More)) {
            val slot = T9Syllables.slotKey(original, cell, pinned = false)
            assertFalse("$cell 被畫成 spacer,消歧欄會整條看不見", slot.spacer)
            assertNotEquals("$cell 沒有換掉鍵面", original.label, slot.label)
            assertTrue("$cell 必須點得下去", slot.hasTapBehavior)
        }
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
     * ⚠ 這裡的佈局清單原本是**寫死的** `listOf("cn-t9-pinyin",
     * "cn-t9-pinyin-numrow")`,而白名單搬進 YAML 的**理由**正是「新增一份
     * 九宮格佈局不必再改 Kotlin」（見 [T9Syllables] 的 kdoc）—— 寫死的清單
     * 把那個好處原封還了回去:新佈局把 key id 宣告錯了,這條測試掃不到它,
     * 使用者拿到的是一份沒有消歧欄的九宮格,而畫面只是照常顯示標點。
     * 所以改成從 `core/layouts` 掃（[RepoFixtures.layoutIds] 本身也是掃的）。
     *
     * ⚠ 掃出來的東西要有**下界**:掃到 0 份必須是紅。否則有人把 YAML 欄位
     * 改名、或解析器不再讀它,這條測試會在「一份都沒掃到」的情況下全綠 ——
     * 那正是本專案的舊帳「測試安靜地跳過自己」。
     */
    @Test
    fun `每一個宣告的格位都真的存在於它宣告的那一層`() {
        val repo = FixtureRepo()
        val declaring = ArrayList<Pair<String, org.luminakey.ime.theme.LayoutLayer>>()
        for (layoutId in RepoFixtures.layoutIds) {
            val layout = repo.loadLayout(layoutId).value
            assertNotNull("佈局 $layoutId 載不起來", layout)
            for (layer in layout!!.layers) {
                if (layer.syllableSlots.isNotEmpty()) declaring += layoutId to layer
            }
        }
        assertTrue(
            "掃遍 core/layouts（${RepoFixtures.layoutIds.size} 份）只有 ${declaring.size} 層" +
                "宣告了 syllable_slots，少於下界 $MIN_DECLARING_LAYERS —— " +
                "九宮格少了消歧欄，而畫面上只是照常顯示標點，不會有任何東西叫",
            declaring.size >= MIN_DECLARING_LAYERS,
        )
        for ((layoutId, layer) in declaring) {
            val where = "$layoutId/${layer.id}"
            val keyIds = layer.syllableSlots
            assertTrue(
                "$where 只宣告了 ${keyIds.size} 格，" +
                    "少於 ${T9Syllables.MIN_SLOTS} 格就翻不了頁，會有讀音摸不到",
                keyIds.size >= T9Syllables.MIN_SLOTS,
            )
            assertEquals("$where 宣告了重複的 key id", keyIds.size, keyIds.distinct().size)
            val rows = layer.rows
            val rowOf = HashMap<String, Int>()
            rows.forEachIndexed { i, r -> r.keys.forEach { k -> k.id?.let { rowOf[it] = i } } }
            for (id in keyIds) {
                assertTrue(
                    "$where 裡沒有 id 為 $id 的鍵 —— " +
                        "消歧欄會整欄消失，而畫面上只是照常顯示標點",
                    rowOf.containsKey(id),
                )
            }
            assertEquals(
                "$where 的格位必須落在不同列上（那是一整條直欄）",
                keyIds.size,
                keyIds.mapNotNull { rowOf[it] }.distinct().size,
            )
            assertFalse(
                "$where 的消歧欄吃到了**底列**。底列是導覽列（!@# 是 " +
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

    /* ── 啟動探針:這個方案接不接得了精確拼音 ──────────────────────────── */

    /**
     * 兩組數字都是**模擬器上實測**出來的,不是想像的（`rime_console`,
     * emulator-5554,`./rime_console <shared> <user> niG 1 t9_pinyin`）:
     *
     *     雙編碼 alphabet: 'abc…xyzADGJMPTW'
     *         n/i/G 全部消費,preedit="ni G"
     *         候選 你好 # ni hao／你會 # ni hui／你還 # ni hai…
     *
     *     單編碼 alphabet: 'ADGJMPTW'（collect_data.sh 沒跑過的裝置）
     *         'n' -> 未消費、'i' -> 未消費,preedit="G"
     *         候選 和 # he／好 # hao／還 # hai…
     *
     * 兩個訊號各擋一種:未消費擋的是**字元集**,comment 擋的是**切分**。
     */
    @Test
    fun `探針分得出雙編碼與單編碼`() {
        val dual = page(
            "你好" to "ni hao", "你會" to "ni hui", "你還" to "ni hai",
            "妳好" to "ni hao", "你個" to "ni ge",
        )
        assertTrue(T9Syllables.probeAccepted(allKeysConsumed = true, candidates = dual))

        // 單編碼:小寫根本進不去,speller 放行了 n 與 i。
        val single = page("和" to "he", "好" to "hao", "還" to "hai", "個" to "ge", "會" to "hui")
        assertFalse(
            "n、i 未被消費就代表 alphabet 沒有小寫 —— 改寫一定會產生垃圾",
            T9Syllables.probeAccepted(allKeysConsumed = false, candidates = single),
        )
        // 就算按鍵全被別的處理器吃掉了,切不出 ni 這個音節一樣不算過。
        assertFalse(
            "字元集收下了不等於切得出音節",
            T9Syllables.probeAccepted(allKeysConsumed = true, candidates = single),
        )
    }

    @Test
    fun `探針的音節要在第一個位置`() {
        // `hao ni` 有 ni,但不在第一個音節 —— 那代表引擎切的與我們送的對不上。
        assertFalse(
            T9Syllables.probeAccepted(true, page("好你" to "hao ni")),
        )
        // 探針送的尾巴是模糊碼,所以 comment 必定是多音節的;單音節也要認。
        assertTrue(T9Syllables.probeAccepted(true, page("你" to "ni")))
        assertFalse("一個候選都沒有就是沒聽懂", T9Syllables.probeAccepted(true, emptyList()))
    }

    /* ── 一次改寫算不算成立 ───────────────────────────────────────────── */

    /**
     * **這一條就是真機回報的那個 bug。**
     *
     * 使用者打 `PGM`、點 `pin`,輸入串改寫成 `pin`。引擎完全聽懂了 ——
     * 實測（`rime_console shared user pin 1 t9_pinyin`）:
     *
     *     preedit="pin"  候選 1.品 2.拼 3.浜 4.頻 5.貧    ← **一個 comment 都沒有**
     *
     * `spelling_hints` 只在拼寫與輸入**不同**時才給 comment,而改寫成 `pin`
     * 之後輸入已經是精確拼音。舊的驗收判準問的正是「comment 有沒有以 pin
     * 開頭」,於是把這個**最成功**的結果判成失敗、把輸入串還原 ——
     * 使用者點下 `pin`,候選一個都沒變。改寫得越徹底,它越確定失敗。
     *
     * 新的判準不看 comment,所以這裡必須是 OK。
     */
    @Test
    fun `改寫成功時候選沒有 comment 也算成功`() {
        val afterPin = page("品" to "", "拼" to "", "浜" to "", "頻" to "", "貧" to "")
        assertEquals(
            T9Syllables.Rewrite.OK,
            T9Syllables.rewriteOutcome(
                schemaCanRewrite = true,
                setInputReturned = true,
                inputAfterRewrite = "pin",
                wantedInput = "pin",
                candidateCount = afterPin.size,
            ),
        )
        // 對照組:同一頁候選拿去餵探針的判準會說「沒聽懂」——
        // 那個判準本身沒壞,壞的是把它用在每一次選字上。
        assertFalse(
            "comment 判準在改寫成功時就是會說 false,所以它不能當選字的驗收",
            T9Syllables.probeAccepted(true, afterPin),
        )
    }

    /**
     * 多音節那一條要照舊走:`MGGAM` 點 `ni` → `niGAM`,尾巴還是模糊碼,
     * 所以 comment 仍然在（實測 你好 # ni hao …）。判準換了以後它一樣是 OK。
     */
    @Test
    fun `多音節改寫仍然成立`() {
        assertEquals(
            T9Syllables.Rewrite.OK,
            T9Syllables.rewriteOutcome(true, true, "niGAM", "niGAM", 5),
        )
    }

    /**
     * 三種失敗要有三個名字,而且每一種都要真的被判出來。
     *
     * `SCHEMA_CANNOT` 擋的是實測過的那一種:裝置上放一份舊的單編碼
     * `t9_pinyin`,`rs_set_input("niGAM")` 回的是 **true**（標頭說會回 false,
     * 實測不會）,引擎把 `ni` 當成翻不出東西的原文、只替 `GAM` 出候選,
     * 使用者點第一個就上屏 **「ni好」**。所以「方案能不能改寫」必須在寫進去
     * 之前就決定,不能靠回傳值。
     */
    @Test
    fun `每一種失敗都有自己的名字`() {
        assertEquals(
            "方案過不了探針時,連 set_input 的回傳值都不該被採信",
            T9Syllables.Rewrite.SCHEMA_CANNOT,
            T9Syllables.rewriteOutcome(false, true, "niGAM", "niGAM", 5),
        )
        assertEquals(
            T9Syllables.Rewrite.ENGINE_REFUSED,
            T9Syllables.rewriteOutcome(true, false, "MGGAM", "niGAM", 5),
        )
        assertEquals(
            "引擎手上不是我們寫進去的那一串",
            T9Syllables.Rewrite.ENGINE_DROPPED,
            T9Syllables.rewriteOutcome(true, true, "MGGAM", "niGAM", 5),
        )
        assertEquals(
            "收下了卻翻不出候選 —— 使用者會看到一條空的候選列",
            T9Syllables.Rewrite.EMPTY_RESULT,
            T9Syllables.rewriteOutcome(true, true, "niGAM", "niGAM", 0),
        )
    }

    /**
     * 判準的順序也是規格:方案過不了探針時,**其他三個訊號一律不算數**。
     * 少了這一條,有人把 `!schemaCanRewrite` 挪到最後面,單編碼方案上就會
     * 因為「set_input 回 true、輸入串也對」而一路走到 OK —— 而那正是
     * 「ni好」上屏的那條路。
     */
    @Test
    fun `方案過不了探針時其他訊號一律不算數`() {
        for (setInputOk in listOf(true, false)) {
            for (held in listOf("niGAM", "MGGAM")) {
                for (count in listOf(0, 5)) {
                    assertEquals(
                        "schemaCanRewrite=false 必須是最先判的",
                        T9Syllables.Rewrite.SCHEMA_CANNOT,
                        T9Syllables.rewriteOutcome(false, setInputOk, held, "niGAM", count),
                    )
                }
            }
        }
    }

    /* ── 那一格**按下去**會怎樣（不是它畫成什麼樣）───────────────────── */

    /**
     * ⚠ **這一節守的是 task #78 的孿生兄弟,而它一度一條守門都沒有。**
     *
     * 上面每一條都只看得到**鍵面與幾何**。把 `KeyboardView.kt` 裡
     * 「Cell.Original 折回原鍵那一邊」的那一步拿掉、[T9Syllables.slotKey] 的
     * 修正完整保留,604 條測試一條都不紅 —— 而使用者拿到的是一顆**畫得對、
     * 按下去什麼都不做、長按盤也開不出來**的標點鍵:
     *
     *   · 鍵面對:`slotKey` 對 [T9Syllables.Cell.Original] 原封退回原鍵;
     *   · 點擊死:點擊被導進 `onSlot`,而 `onSlot` 對 Cell.Original 是 `Unit`;
     *   · 長按死:`if (cell != null) null else key.popup` 把盤關掉。
     *
     * 那正是這個專案抓過七次的形狀:**本來摸得到,現在只剩看得到**。
     * 決定因此收進純函式 [T9Syllables.renderSlot],這一節直接驗**行為**。
     */
    @Test
    fun `沒用到的格位,鍵面、點擊、長按盤、朗讀名四件全部走原鍵`() {
        val layer = FixtureRepo().loadLayout(T9_LAYOUT).value!!.layer("t9")!!
        val keys = layer.rows.flatMap { it.keys }
        val slotIds = layer.syllableSlots
        assertTrue("夾具前提:這一層要有格位", slotIds.isNotEmpty())
        for (id in slotIds) {
            val original = keys.first { it.id == id }
            assertNotNull(
                "夾具前提:「${original.label}」本來就有長按盤,否則這條測試驗不到長按",
                original.popup,
            )
            assertUntouched(
                "$id/讀音不夠用",
                original,
                T9Syllables.renderSlot(original, T9Syllables.Cell.Original, null),
            )
            assertUntouched(
                "$id/根本不是格位",
                original,
                T9Syllables.renderSlot(original, null, null),
            )
            assertUntouched(
                "$id/別人被釘住了",
                original,
                T9Syllables.renderSlot(original, T9Syllables.Cell.Original, "ni"),
            )
            val holed = original.copy(spacer = true)
            assertUntouched("$id/spacer", holed, T9Syllables.renderSlot(holed, reading("ni"), "ni"))
        }
    }

    /**
     * 反向:被真正接管的那幾格,四件事也要**同時**換過去。
     *
     * 少了這一條,一份把每一格都無條件當成「不歸消歧欄管」的實作照樣全綠 ——
     * 那樣消歧欄整條不會有反應,而鍵盤看起來完全正常。
     */
    @Test
    fun `被接管的格位,點擊交給消歧欄、長按盤收起來、朗讀名換成讀音`() {
        val layer = FixtureRepo().loadLayout(T9_LAYOUT).value!!.layer("t9")!!
        val original = layer.rows.flatMap { it.keys }.first { it.id == "pu_question" }
        for (cell in listOf(reading("ni"), T9Syllables.Cell.More)) {
            val slot = T9Syllables.renderSlot(original, cell, pinnedSyllable = null)
            assertTrue("$cell 沒有被接管", slot.takenOver)
            assertEquals("$cell 點下去要把 cell 交給 onSlot", cell, slot.tapCell)
            assertNull(
                "「${original.label}」的長按盤在組字中不該還開得出來",
                slot.popup,
            )
            assertEquals("$cell 的朗讀名要念讀音,不是念「${original.label}」", cell, slot.speaks)
            assertNotEquals("$cell 沒有換掉鍵面", original.label, slot.key.label)
            assertTrue("$cell 必須點得下去", slot.key.hasTapBehavior)
            assertEquals("幾何一格都不能動", original.width, slot.key.width, 0.0001f)
        }
    }

    @Test
    fun `釘住的那一格看得出來,而且只有那一格`() {
        val layer = FixtureRepo().loadLayout(T9_LAYOUT).value!!.layer("t9")!!
        val original = layer.rows.flatMap { it.keys }.first { it.id == "pu_comma" }
        val pinned = T9Syllables.renderSlot(original, reading("ni"), pinnedSyllable = "ni")
        assertTrue("釘住的那一格要看得出來", pinned.pinned)
        assertTrue("釘住的那一格鍵面要是 active", pinned.key.active)
        val other = T9Syllables.renderSlot(original, reading("mi"), pinnedSyllable = "ni")
        assertFalse("沒被釘住的格位也亮起來 = 兩格同時反白", other.pinned)
        assertFalse("沒被釘住的格位也亮起來 = 兩格同時反白", other.key.active)
        val more = T9Syllables.renderSlot(original, T9Syllables.Cell.More, pinnedSyllable = "ni")
        assertFalse("翻頁鍵不是讀音,不該被當成釘住的那一個", more.pinned)
    }

    /**
     * **性質,不是例子**:掃遍 `core/layouts` 每一份宣告了 `syllable_slots`
     * 的層 × 每一種讀音數 × 每一個 offset,每一格的行為都要與它的 cell 一致。
     *
     * 「一致」對 [T9Syllables.Cell.Original] 的意思是**四件事全部走原鍵**,
     * 而那正是實機截圖（task #78）指著的那一格。
     */
    @Test
    fun `整層掃一遍,沒用到的格位在任何一種讀音數下都還按得動`() {
        assertSlotsBehave { key, declared, pin -> T9Syllables.renderSlot(key, declared, pin) }
    }

    /**
     * 同一條檢查餵一份**故意寫壞的實作**必須失敗,否則它什麼都沒在守。
     *
     * 這份壞掉的實作就是植入 M3 的形狀:少了「Cell.Original 折回原鍵那一邊」
     * 那一步,其餘完整保留 —— 鍵面照樣正確,點擊與長按盤卻交給了消歧欄。
     */
    @Test
    fun `少了「Original 折回原鍵」那一步會被同一條檢查擋下`() {
        var caught = false
        try {
            assertSlotsBehave { key, declared, pin ->
                if (declared == null) {
                    T9Syllables.SlotRender(key, null, key.popup, null, false)
                } else {
                    val pinned =
                        declared is T9Syllables.Cell.Reading && declared.syllable == pin
                    val face = T9Syllables.slotKey(key, declared, pinned)
                    T9Syllables.SlotRender(face, declared, face.popup, declared, pinned)
                }
            }
        } catch (e: AssertionError) {
            caught = true
        }
        assertTrue(
            "『沒用到的格位還按得動』這條檢查在該紅的時候沒有紅 —— " +
                "那顆標點鍵可以變成按下去什麼都不做,而沒有任何東西會叫",
            caught,
        )
    }

    private fun assertSlotsBehave(
        render: (LayoutKey, T9Syllables.Cell?, String?) -> T9Syllables.SlotRender,
    ) {
        val repo = FixtureRepo()
        var untouched = 0
        var taken = 0
        for (layoutId in RepoFixtures.layoutIds) {
            val layout = repo.loadLayout(layoutId).value ?: continue
            for (layer in layout.layers) {
                val slotIds = layer.syllableSlots
                if (slotIds.isEmpty()) continue
                val keys = layer.rows.flatMap { it.keys }.associateBy { it.id }
                for (n in 0..6) {
                    val readings = (1..n).map { "s$it" }
                    for (offset in 0 until maxOf(n, 1)) {
                        val cells = T9Syllables.cells(readings, slotIds.size, offset)
                        assertEquals(
                            "$layoutId/${layer.id} n=$n:格數與宣告的格位數對不上",
                            slotIds.size,
                            cells.size,
                        )
                        for ((i, id) in slotIds.withIndex()) {
                            // 「宣告的 id 真的存在」由另一條測試守,這裡不重複。
                            val original = keys[id] ?: continue
                            val where = "$layoutId/${layer.id}/$id n=$n offset=$offset"
                            val slot = render(original, cells[i], readings.firstOrNull())
                            if (cells[i] == T9Syllables.Cell.Original) {
                                untouched++
                                assertUntouched(where, original, slot)
                            } else {
                                taken++
                                assertTrue("$where 該被消歧欄接管卻沒有", slot.takenOver)
                            }
                        }
                    }
                }
            }
        }
        // 下界:一格都沒掃到的話,上面每一條斷言都沒跑過,而這條測試會全綠。
        assertTrue("一個「沒用到的格位」都沒掃到,這條檢查是空轉的", untouched > 0)
        assertTrue("一個「被接管的格位」都沒掃到,這條檢查是空轉的", taken > 0)
    }

    /** 「這一格不歸消歧欄管」的完整意思:四件事全部走原鍵那一份。 */
    private fun assertUntouched(
        where: String,
        original: LayoutKey,
        slot: T9Syllables.SlotRender,
    ) {
        assertSame("$where 的鍵面不是原鍵本人", original, slot.key)
        assertNull(
            "$where 的點擊被交給了消歧欄 —— onSlot 對 Cell.Original 是 Unit," +
                "使用者按下這顆標點鍵什麼都不會發生(畫面完全正常)",
            slot.tapCell,
        )
        assertSame(
            "$where 的長按盤被關掉了 —— 那顆鍵的長按盤在組字中應該還開得出來",
            original.popup,
            slot.popup,
        )
        assertNull("$where 的朗讀名被換掉了,TalkBack 會把標點念成讀音", slot.speaks)
        assertFalse("$where 不是使用者釘住的讀音,不該亮起來", slot.pinned)
    }

    /**
     * **KeyGrid 只准照著做。**
     *
     * [T9Syllables.renderSlot] 是純函式,上面那幾條測試摸得到它;但
     * 「KeyGrid 有沒有真的照著做」摸不到 —— 本模組沒有 Robolectric、
     * 也沒有 androidTest,`@Composable` 一行都執行不到。植入 M3 之所以全綠,
     * 缺的就是這一段:專門為側欄寫的測試走的是自己的 ProbeProc,不是產品碼。
     *
     * 所以這裡掃原始碼,守**一件事**:那四個決定不准在 KeyGrid 裡重新做一次。
     * 把 `slotCells` 直接拿去判斷（M3 的形狀）、自己合成鍵面、或從原鍵拿
     * 長按盤,三種都會在這裡紅。
     */
    @Test
    fun `KeyGrid 不自己決定那一格的行為`() {
        val src = KEYBOARD_VIEW.readText(Charsets.UTF_8)
        assertTrue("讀不到 ${KEYBOARD_VIEW.path}（工作目錄變了?）", src.length > 1000)
        val grid = src.substringAfter("private fun KeyGrid(", "")
            .substringBefore("private fun syllableDescription")
        assertTrue("在 KeyboardView.kt 裡找不到 KeyGrid 的本體", grid.length in 500..40000)

        assertEquals(
            "KeyGrid 只准查一次 slotCells,而且要把查到的東西整個交給 renderSlot",
            1,
            Regex("""slotCells\[""").findAll(grid).count(),
        )
        assertTrue(
            "那一格的四個決定必須由 T9Syllables.renderSlot 一次算完 —— " +
                "在 KeyGrid 裡自己判斷 Cell.Original,少同步一件就是一顆" +
                "畫得對、按下去什麼都不做的標點鍵",
            Regex("""T9Syllables\.renderSlot\(\s*key,\s*slotCells\[key\.id],\s*pinnedSyllable\s*\)""")
                .containsMatchIn(grid),
        )
        assertFalse(
            "鍵面不准在 KeyGrid 自己合成:slotKey 只該由 renderSlot 呼叫," +
                "否則鍵面與行為又變成兩個判斷點",
            src.contains("T9Syllables.slotKey("),
        )
        assertEquals(
            "長按盤必須走 slot.popup。在 KeyGrid 裡從原鍵拿 key.popup," +
                "就是把「這一格有沒有被接管」又判斷了一次,而它可以與鍵面分岔",
            0,
            Regex("""\bkey\.popup\b""").findAll(grid).count(),
        )
        for (needle in listOf("slot.key", "slot.tapCell", "slot.speaks", "slot.pinned", "slot.popup")) {
            assertTrue("KeyGrid 沒有用到 $needle —— 那個決定跑到別的地方去了", grid.contains(needle))
        }
    }

    private fun reading(s: String) = T9Syllables.Cell.Reading(s)

    private fun page(vararg pairs: Pair<String, String>): List<RimeCandidate> =
        pairs.mapIndexed { i, (text, comment) ->
            RimeCandidate(text = text, comment = comment, label = (i + 1).toString())
        }

    private companion object {
        const val T9_LAYOUT = "cn-t9-pinyin"

        /**
         * 「有幾層宣告了 syllable_slots」的**下界**,不是清單。
         *
         * 目前是 `cn-t9-pinyin` 與 `cn-t9-pinyin-numrow` 各一層。新增九宮格
         * 佈局時**不必**動它（那正是把宣告搬進 YAML 的用意）;它只擋一件事:
         * 掃到 0 份時整條檢查空轉全綠。
         */
        const val MIN_DECLARING_LAYERS = 2

        /** 工作目錄在 Gradle 下是模組目錄（android/app）。 */
        val KEYBOARD_VIEW =
            File("src/main/java/org/luminakey/ime/keyboard/KeyboardView.kt")
    }
}
