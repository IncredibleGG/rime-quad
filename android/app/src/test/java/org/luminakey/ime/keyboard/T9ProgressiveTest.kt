package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import org.luminakey.ime.core.RimeCandidate

/**
 * 逐個音節選下去（輸入串改寫）。
 *
 * ── 這裡守的是使用者最在意的那一件事 ──────────────────────────────────
 * 使用者的原話是「我選擇了 ni,接下來他要讓我選擇下一個」。所以這一組測試的
 * 主軸不是「畫面上有沒有東西」,而是**選了第一個之後,第二個問得出來嗎**。
 *
 * ⚠ 改寫出來的字串必須整串落在方案的 alphabet 裡（雙編碼之後是
 * `a-z` + `ADGJMPTW`）。`rs_set_input()` 對不合法的字串回 false,而前端若把
 * false 當成功,就會畫出一個與引擎狀態不符的畫面 —— 那是這個專案抓過七次的
 * 那個形狀。這裡用 [T9Syllables.rewriteInput] 回 null 來擋住同一件事。
 */
class T9ProgressiveTest {

    /** 「你好」= ni + hao → M G ＋ G A M。這是方案 xlit 的反向,兩邊必須一致。 */
    @Test
    fun `t9 編碼與方案的 xlit 一致`() {
        assertEquals("MG", T9Syllables.t9Encode("ni"))
        assertEquals("GAM", T9Syllables.t9Encode("hao"))
        assertEquals("MG", T9Syllables.t9Encode("mi"))
        // 非拼音字母不編碼,而不是硬塞一個代表字母進去。
        assertNull(T9Syllables.t9Encode("nü"))
        assertNull(T9Syllables.t9Encode(""))
    }

    /**
     * 第一步:全模糊 `MGGAM`,點 ni → `niGAM`。
     *
     * 這正是協調端用 `tools/rime_console.cc` 實測過解得出「你好」的那一串:
     * 第一個音節精確、後面仍然模糊。
     */
    @Test
    fun `點第一個音節之後輸入串變成精確加模糊`() {
        assertEquals("niGAM", T9Syllables.rewriteInput("MGGAM", emptyList(), "ni"))
        assertEquals("miGAM", T9Syllables.rewriteInput("MGGAM", emptyList(), "mi"))
    }

    /** 第二步:已確定 ni,再點 hao → 整串精確。 */
    @Test
    fun `第二個音節接在已確定的那一個後面`() {
        assertEquals("nihao", T9Syllables.rewriteInput("niGAM", listOf("ni"), "hao"))
    }

    /**
     * ⚠ 前綴對不上就**什麼都不做**。
     *
     * 使用者按了刪除鍵,引擎的輸入串已經變短;若照樣把「已確定」的前綴接回去,
     * 會憑空生出一段使用者沒有打過的輸入。
     */
    @Test
    fun `已確定的前綴不在輸入串裡就拒絕改寫`() {
        assertNull(T9Syllables.rewriteInput("MGGAM", listOf("ni"), "hao"))
        assertNull(T9Syllables.rewriteInput("ni", listOf("ni"), "hao"))
    }

    /** 認不出這個音節吃掉幾鍵時回 null —— 猜一個數字會把後面打的東西吃掉。 */
    @Test
    fun `對不上的音節不猜`() {
        assertNull(T9Syllables.consumedCodes("GAM", "ni"))
        assertNull(T9Syllables.rewriteInput("GAM", emptyList(), "ni"))
    }

    /** 超級簡拼:`abbrev` 讓一個音節可能只按了一鍵。 */
    @Test
    fun `簡拼的一鍵也認得`() {
        // 完整長度對不上（MAM 不是 MG 開頭）,但首鍵對得上 → 吃一鍵。
        assertEquals(1, T9Syllables.consumedCodes("MAM", "ni"))
        assertEquals("niAM", T9Syllables.rewriteInput("MAM", emptyList(), "ni"))
    }

    /**
     * 已確定的音節必須跟著引擎走。
     *
     * 留著過期的前綴,消歧欄會從第 3 個音節開始問,而畫面上根本沒有前兩個。
     */
    @Test
    fun `輸入串變了之後已確定的音節跟著失效`() {
        assertEquals(
            listOf("ni"),
            T9Syllables.syncConfirmed("niGAM", listOf("ni", "hao")),
        )
        assertEquals(
            emptyList<String>(),
            T9Syllables.syncConfirmed("MGGAM", listOf("ni")),
        )
        assertEquals(
            listOf("ni", "hao"),
            T9Syllables.syncConfirmed("nihaoW", listOf("ni", "hao")),
        )
    }

    /**
     * **這一條就是「選了第一個之後,第二個問得出來」的單元測試版本。**
     *
     * 改寫之後引擎給的候選是「你好 / 你搞 / 你敢」,它們的 comment 第 1 個音節
     * 分別是 hao / gao / gan —— 那就是第二欄該出現的東西。
     */
    @Test
    fun `第二個音節的讀音取自候選的第二個音節`() {
        val after = listOf(
            RimeCandidate("你好", "ni hao"),
            RimeCandidate("你搞", "ni gao"),
            RimeCandidate("你敢", "ni gan"),
            // 只涵蓋一個音節的候選對「下一個音節」沒有意見,必須被跳過,
            // 而不是讓它把 null 或空字串擠進讀音清單。
            RimeCandidate("你", "ni"),
        )
        assertEquals(listOf("hao", "gao", "gan"), T9Syllables.readingsAt(after, 1))
        // 第 0 個音節這時只剩一種可能 —— 它已經被使用者確定了。
        assertEquals(listOf("ni"), T9Syllables.readingsAt(after, 0))
        // 沒有第三個音節。
        assertEquals(emptyList<String>(), T9Syllables.readingsAt(after, 2))
    }

    /** 走完一整輪:MGGAM → ni → hao,每一步都問得出下一批讀音。 */
    @Test
    fun `一整輪兩個音節都選得下去`() {
        val first = listOf(
            RimeCandidate("你", "ni"),
            RimeCandidate("米", "mi"),
            RimeCandidate("你好", "ni hao"),
        )
        assertEquals(listOf("ni", "mi"), T9Syllables.readingsAt(first, 0))

        val afterFirst = T9Syllables.rewriteInput("MGGAM", emptyList(), "ni")
        assertEquals("niGAM", afterFirst)

        val second = listOf(RimeCandidate("你好", "ni hao"), RimeCandidate("你搞", "ni gao"))
        val confirmed = listOf("ni")
        assertEquals(listOf("hao", "gao"), T9Syllables.readingsAt(second, confirmed.size))

        assertEquals("nihao", T9Syllables.rewriteInput(afterFirst!!, confirmed, "hao"))
    }
}
