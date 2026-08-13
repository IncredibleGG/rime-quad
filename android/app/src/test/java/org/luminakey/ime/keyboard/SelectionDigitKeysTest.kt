package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.theme.KeyAction
import org.luminakey.ime.theme.ActionVerb
import org.luminakey.ime.theme.LabelSource
import org.luminakey.ime.theme.LayoutKey
import org.luminakey.ime.theme.LayoutLayer
import org.luminakey.ime.theme.LayoutRow
import org.luminakey.ime.theme.LocalizedString
import org.luminakey.ime.theme.LayoutLoader
import org.luminakey.ime.theme.Platform
import org.luminakey.ime.theme.RepoFixtures
import org.luminakey.ime.theme.SendSpec

/**
 * ⛔ **工單 #99：九宮格數字列按下去會毀掉組字。**
 *
 * 實測（emulator-5558 / lumina_test2）：`cn-t9-pinyin-numrow` ＋ `t9_pinyin`，
 * 打 `MG GAM` 之後按數字列的 `3` → `rs_process_key` 回 consumed、
 * preedit 變成 `MGGAM3`、宿主輸入框變成 `3⋯` —— **使用者已經打好的組字沒了**。
 *
 * 這一份測的是那條路的判準：哪一顆算數字鍵、按下去做什麼。
 * 裝置端的那一半由 `scripts/verify_selection_digit.sh` 守（「按 3 上屏的
 * 就是第 3 個候選」）。
 */
class SelectionDigitKeysTest {

    private fun key(
        label: String,
        keysym: String?,
        modifiers: Int = 0,
        tap: KeyAction? = null,
        spacer: Boolean = false,
    ) = LayoutKey(
        id = label, label = label, hint = "", icon = null,
        labelFrom = LabelSource.NONE, width = 1f, style = "default",
        spacer = spacer, active = false, repeat = false,
        send = keysym?.let { SendSpec.Keysym(it, 0, modifiers) },
        tap = tap, doubleTap = null, longPress = null,
        popup = null, swipe = emptyMap(),
    )

    private fun layerOf(vararg keys: LayoutKey) = LayoutLayer(
        id = "l", label = LocalizedString.EMPTY, units = 10f,
        rows = listOf(LayoutRow(weight = 1f, keys = keys.toList())),
    )

    /* ═════════════════ 哪一顆算「選字數字鍵」 ═════════════════ */

    @Test
    fun `鍵面是數字而且送同一個數字才算`() {
        assertEquals(3, SelectionDigitKeys.digitOf(key("3", "3")))
        assertEquals(1, SelectionDigitKeys.digitOf(key("1", "1")))
        assertEquals(9, SelectionDigitKeys.digitOf(key("9", "9")))
    }

    /**
     * ⛔ **`bopomofo-dachen` 的 ㄅ 送 keysym `1`，但它不是「第 1 個」。**
     *
     * 那是大千鍵盤的實體鍵位：使用者眼裡那顆鍵是注音字母。把它當序號，
     * 就是上一輪抓到的那個誤判（畫了序號、按下去得到 ㄅ）。
     */
    @Test
    fun `送數字但鍵面不是數字的不算`() {
        assertNull(SelectionDigitKeys.digitOf(key("ㄅ", "1")))
        assertNull(SelectionDigitKeys.digitOf(key("ˇ", "3")))
        assertNull(SelectionDigitKeys.digitOf(key("abc", "2")))
    }

    @Test
    fun `鍵面是數字而送別的東西的不算`() {
        assertNull(SelectionDigitKeys.digitOf(key("3", "D")))
        assertNull(SelectionDigitKeys.digitOf(key("3", null)))
    }

    @Test
    fun `0 不算 —— page_size 是 9，沒有第 10 個`() {
        assertNull(SelectionDigitKeys.digitOf(key("0", "0")))
    }

    @Test
    fun `帶 modifier 的不算`() {
        assertNull(SelectionDigitKeys.digitOf(key("3", "3", modifiers = 1)))
    }

    @Test
    fun `有 tap 動詞的不算（tap 勝過 send,見 §9·6）`() {
        assertNull(
            SelectionDigitKeys.digitOf(
                key("3", "3", tap = KeyAction(ActionVerb.CLEAR, emptyList(), "clear"))
            )
        )
    }

    @Test
    fun `spacer 不算`() {
        assertNull(SelectionDigitKeys.digitOf(key("3", "3", spacer = true)))
    }

    /**
     * 整層要有**整排** 1–9。字母層裡孤零零一顆送 `1` 的鍵不是數字列
     * —— 那正是 `t9-pinyin/t9` 的形狀（只有 `k1` 送得出真的 `1`）。
     */
    @Test
    fun `整排 1 到 9 都在才算專用數字列`() {
        val full = layerOf(*(1..9).map { key("$it", "$it") }.toTypedArray())
        assertTrue(SelectionDigitKeys.rowActive(full))
        val one = layerOf(key("1", "1"), key("abc", "A"), key("def", "D"))
        assertEquals(false, SelectionDigitKeys.rowActive(one))
        assertEquals(false, SelectionDigitKeys.rowActive(null))
    }

    /* ═════════════════ 按下去做什麼 ═════════════════ */

    private val page9 = (0..8).toSet()

    @Test
    fun `沒有在組字就照常送數字`() {
        assertEquals(
            SelectionDigitKeys.Act.SendDigit,
            SelectionDigitKeys.act(3, composing = false, selectableIndices = page9),
        )
    }

    /** ⚠ **頁內相對索引**：按 3 = `rs_select_candidate(2)`，不得攤平成全域序號。 */
    @Test
    fun `組字中按 3 選的是頁內第 3 個（索引 2）`() {
        assertEquals(
            SelectionDigitKeys.Act.Select(2),
            SelectionDigitKeys.act(3, composing = true, selectableIndices = page9),
        )
        assertEquals(
            SelectionDigitKeys.Act.Select(0),
            SelectionDigitKeys.act(1, composing = true, selectableIndices = page9),
        )
    }

    /**
     * ⛔ **索引超過本頁候選數 → 什麼都不做,不得毀掉組字。**
     *
     * 這一格從前是「送給引擎」,而引擎把它吃掉、把數字接到輸入串上、
     * 讓宿主輸入框變成 `3⋯`。
     */
    @Test
    fun `索引超過本頁候選數就什麼都不做`() {
        val onlyThree = setOf(0, 1, 2)
        assertEquals(
            SelectionDigitKeys.Act.Ignore,
            SelectionDigitKeys.act(4, composing = true, selectableIndices = onlyThree),
        )
        assertEquals(
            SelectionDigitKeys.Act.Ignore,
            SelectionDigitKeys.act(9, composing = true, selectableIndices = emptySet()),
        )
    }

    /**
     * 消歧欄篩掉的那幾格畫面上**沒有序號** —— 按下去選中一個看不見的候選
     * 是換一個缺陷,不是修好。
     */
    @Test
    fun `被消歧欄篩掉的那幾格按下去什麼都不做`() {
        val filtered = setOf(0, 2, 4)      // 畫面上只有 1、3、5
        assertEquals(
            SelectionDigitKeys.Act.Select(2),
            SelectionDigitKeys.act(3, composing = true, selectableIndices = filtered),
        )
        assertEquals(
            SelectionDigitKeys.Act.Ignore,
            SelectionDigitKeys.act(2, composing = true, selectableIndices = filtered),
        )
    }

    /* ═════════════════ 隨附佈局逐份掃 ═════════════════ */

    /**
     * ⛔ **把判準套在真的佈局上,不要只套在手搓的層上。**
     *
     * 手搓一個「每顆鍵都是數字」的層再問「這是不是數字列」是同義反覆,
     * 永遠不會紅。這一條掃 `core/layouts/` 每一份、每一層,把結果印出來,
     * 並釘住兩件事:
     *
     *   · `cn-t9-pinyin-numrow` 的 `t9` 層(`n1`–`n9`)**是**數字列;
     *   · `bopomofo-dachen` 的 `bopomofo` 層(ㄅ 送 keysym 1)**不是**。
     */
    @Test
    fun `隨附佈局裡誰有專用數字列`() {
        val found = LinkedHashMap<String, List<Int>>()
        for (layoutId in RepoFixtures.layoutIds) {
            val layout = LayoutLoader.load(layoutId, RepoFixtures.layouts, Platform.ANDROID).value
                ?: error("佈局 $layoutId 載不起來")
            for (layer in layout.layers) {
                if (!SelectionDigitKeys.rowActive(layer)) continue
                val digits = layer.rows.flatMap { it.keys }
                    .mapNotNull { SelectionDigitKeys.digitOf(it) }
                    .sorted()
                found["$layoutId/${layer.id}"] = digits
            }
        }
        val report = found.entries.joinToString("\n  ") { "${it.key} → ${it.value}" }
        assertTrue(
            "cn-t9-pinyin-numrow 的 t9 層必須被認出來（那是工單 #99 的那一排）。\n  $report",
            found["cn-t9-pinyin-numrow/t9"] == (1..9).toList(),
        )
        // ⛔ **注音層過得了第一關(整排 1–9 的 keysym 都在 —— 那是大千鍵位),
        //    而第二關必須把它全部擋下來:一顆選字數字鍵都不准有。**
        //    只驗第一關的話,ㄅ 就會變成「第 1 個」——那正是上一輪的誤判。
        assertEquals(
            "bopomofo-dachen 的注音層一顆選字數字鍵都不該有（ㄅ 送 keysym 1，" +
                "但使用者看到的是注音字母）。\n  $report",
            emptyList<Int>(), found["bopomofo-dachen/bopomofo"],
        )
        assertTrue(
            "注音層必須有出現在這張表裡（它整排 keysym 都在，擋它的是第二關）——" +
                "沒出現的話這一條驗的不是我以為的那件事。\n  $report",
            "bopomofo-dachen/bopomofo" in found,
        )
        // 這張表本身就是紀錄:哪幾層在組字中會把數字解讀成選字。
        // 少一層多一層都要有人看見,所以連數量一起釘。
        assertEquals(
            "有專用數字列的層數變了 —— 新增/刪掉一層數字列是產品決定，" +
                "請確認它在組字中把數字解讀成「選第 N 個」是你要的。\n  $report",
            16, found.size,
        )
    }
}
