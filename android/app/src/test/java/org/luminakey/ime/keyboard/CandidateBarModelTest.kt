package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.theme.PageIndicatorKind
import org.luminakey.ime.theme.ScrollMode
import org.luminakey.ime.theme.RepoFixtures

/**
 * 候選列的兩件事，都來自使用者拿真機測九宮格的回報。
 *
 * ⚠ **這裡刻意不寫「檔案裡有沒有這個字」那種檢查。** 本專案已經被實測拆穿過
 * 好幾次：`grep -q 名字` 掃整個檔案，名字在別處出現一次就永遠綠。
 * 下面每一條驗的都是「**給定這一份真的會被載入的佈局／主題，算出來的東西
 * 是什麼**」，而且每一條都附一段說明「它該在什麼時候紅」。
 */
class CandidateBarModelTest {

    /* ═══════════════ A-2:候選列左端不准印按鍵代碼 ═══════════════ */

    /**
     * 真機截圖上那一格印的是 `MG GAM`。
     *
     * 判準不是「t9_pinyin 就別印」，是「送出這個字元的那顆鍵，鍵面上是不是
     * 一整組字母」—— 所以這條測試讀的是**真的那份佈局**：有人把九宮格拆成
     * 一鍵一字母，`M` 就不再是代號，這一條會當場紅。
     */
    @Test
    fun `九宮格的按鍵代碼不會印在候選列左端`() {
        val faces = t9GroupCodes()
        assertTrue("送 M 的那顆鍵鍵面是 mno —— M 是一整組的代號", faces.contains('M'))
        assertFalse("九宮格上沒有任何一顆鍵送 n,`ni` 是引擎那邊來的拼音", faces.contains('n'))

        // 打「你好」= M G G A M,speller 依 delimiter 斷成 `MG GAM`。
        assertNull("整串都是按鍵代碼 → 那一格整個不要出現", InlinePreedit.forDisplay("MG GAM", faces))
        // 點了 ni 之後引擎收到的是 `niGAM`,preedit 成了 `ni GAM`。
        assertEquals("ni⋯", InlinePreedit.forDisplay("ni GAM", faces))
        // 選了字之後前半段變成漢字。漢字不是按鍵代碼,一律留著。
        assertEquals("你⋯", InlinePreedit.forDisplay("你GAM", faces))
        // 兩個音節都定了,剩最後一鍵。
        assertEquals("ni hao⋯", InlinePreedit.forDisplay("ni hao M", faces))
        // 全部定完就沒有代碼了 —— 這時候原封不動,連 ⋯ 都不加。
        assertEquals("ni hao", InlinePreedit.forDisplay("ni hao", faces))
    }

    /**
     * 掃**每一份**宣告給 `t9_pinyin` 的佈局，代碼一律印不出來。
     *
     * 代碼字串不是手寫的,是拿 [T9Syllables.t9Encode]（改寫輸入串用的同一張
     * 對照表）現算的 —— 對照表與這條規則因此不可能各走各的。
     * 新增一份九宮格佈局而鍵面用了大寫,這裡會紅。
     *
     * ⚠ 掃的是每份佈局的**第一層**（§9.1.1 的初始層,也就是九宮格那一層）。
     * 同一份佈局裡的 `en_upper` 鍵面就是大寫 `M`,`MGGAM` 站在那裡不是謊話,
     * 而且要走到它得先經過 `en` —— 切過去 `ascii_mode` 一開組字就結束了。
     */
    @Test
    fun `每一份九宮格佈局都印不出按鍵代碼`() {
        val repo = FixtureRepo()
        val t9Layouts = RepoFixtures.layoutIds
            .mapNotNull { repo.loadLayout(it).value }
            .filter { it.forSchema.contains(T9_SCHEMA) }
        assertTrue(
            "掃不到任何宣告 for_schema: $T9_SCHEMA 的佈局 —— 判準壞了,這條在空轉",
            t9Layouts.size >= 3,
        )
        val code = T9Syllables.t9Encode("ni")!! + T9Syllables.t9Encode("hao")!!
        assertEquals("MGGAM", code)
        var checked = 0
        for (layout in t9Layouts) {
            val layer = layout.layers.first()
            assertEquals("${layout.id} 的第一層不是九宮格那一層", "t9", layer.id)
            checked++
            val shown = InlinePreedit.forDisplay(code, InlinePreedit.groupCodeChars(layer))
            assertNull(
                "${layout.id}/${layer.id} 會把按鍵代碼 $code 印成「$shown」—— " +
                    "使用者按的是 mno/ghi,畫面上卻冒出 MGGAM",
                shown,
            )
        }
        assertEquals("掃到的份數對不上 —— 掃到 0 份一樣是全綠", t9Layouts.size, checked)
    }

    /**
     * 反過來的那一半：**別的方案那一格是有意義的，不准跟著消失**。
     *
     * 使用者原話說的是「九宮格那一格沒意義」，不是「候選列不要印組字串」。
     * 把整個功能拿掉一樣能讓上面那幾條變綠 —— 這一條就是攔那個做法的。
     */
    @Test
    fun `全拼與注音的組字串原封不動`() {
        val repo = FixtureRepo()
        val qwerty = repo.loadLayout("qwerty").value!!
        val qwertyFaces = InlinePreedit.groupCodeChars(qwerty.layers.first())
        assertTrue("全鍵盤一鍵一字母,沒有任何「一組的代號」", qwertyFaces.isEmpty())
        assertEquals("nihao", InlinePreedit.forDisplay("nihao", qwertyFaces))
        assertEquals("ni hao", InlinePreedit.forDisplay("ni hao", qwertyFaces))

        val bopomofo = repo.loadLayout("bopomofo-dachen").value!!
        val bpmfFaces = InlinePreedit.groupCodeChars(bopomofo.layers.first())
        assertEquals("ㄋㄧˇㄏㄠˇ", InlinePreedit.forDisplay("ㄋㄧˇㄏㄠˇ", bpmfFaces))
        assertEquals("你ㄏㄠˇ", InlinePreedit.forDisplay("你ㄏㄠˇ", bpmfFaces))
    }

    /**
     * 佈局還沒載進來時**不准藏東西**。
     *
     * 「沒有資訊」與「確定沒有意義」是兩回事。混為一談的話，鍵盤剛彈出來的
     * 那幾十毫秒（佈局還在載）組字串會整個不見，而那是最像 bug 的一種閃爍。
     */
    @Test
    fun `不知道鍵面是什麼的時候一律照印`() {
        assertEquals("MG GAM", InlinePreedit.forDisplay("MG GAM", emptySet()))
        assertNull(InlinePreedit.forDisplay("", t9GroupCodes()))
        assertEquals(emptySet<Char>(), InlinePreedit.groupCodeChars(null))
    }

    /** 砍完只剩分隔符時要收乾淨，不留一個孤零零的 `'` 或空白。 */
    @Test
    fun `砍掉代碼之後不留懸空的分隔符`() {
        val faces = t9GroupCodes()
        assertNull(InlinePreedit.forDisplay("MG'GAM", faces))
        assertEquals("ni⋯", InlinePreedit.forDisplay("ni'GAM", faces))
        assertEquals("ni⋯", InlinePreedit.forDisplay("ni  GAM  ", faces))
    }

    /* ═══════════════ A-4:候選翻頁 ═══════════════ */

    /**
     * 真機回報:「候選詞只有 5 個,下一頁就沒了」。
     * 引擎那一側是好的（`rs_change_page` 實測翻得到第 4 頁），缺的是入口。
     */
    @Test
    fun `還有下一頁時下一頁鍵是亮的`() {
        val s = Pager.state(PageIndicatorKind.ARROWS, true, pageNo = 0, isLastPage = false, candidateCount = 5)
        assertTrue("第一頁而且不是最後一頁 —— 這正是使用者卡住的那一格", s.show)
        assertFalse("第一頁沒有上一頁", s.prevEnabled)
        assertTrue("還有下一頁", s.nextEnabled)
    }

    @Test
    fun `中間的頁兩顆都亮`() {
        val s = Pager.state(PageIndicatorKind.ARROWS, true, pageNo = 2, isLastPage = false, candidateCount = 5)
        assertTrue(s.show)
        assertTrue(s.prevEnabled)
        assertTrue(s.nextEnabled)
    }

    /**
     * 最後一頁時下一頁鍵**變灰但留在原地**。
     *
     * 藏起來的話整條候選列會在翻到最後一頁的瞬間橫向位移一次，
     * 使用者剛瞄準的那個候選就跑掉了。規範 §8.6.5 給 `disabled_color`
     * 就是為了這個。
     */
    @Test
    fun `最後一頁下一頁鍵變灰而不是消失`() {
        val s = Pager.state(PageIndicatorKind.ARROWS, true, pageNo = 3, isLastPage = true, candidateCount = 5)
        assertTrue("翻到最後一頁,兩顆鍵不可以就這樣不見", s.show)
        assertTrue(s.prevEnabled)
        assertFalse(s.nextEnabled)
    }

    @Test
    fun `只有一頁或沒有候選時不畫翻頁鍵`() {
        assertFalse(
            "總共就這幾個字,兩顆按不動的灰鍵只是佔位置",
            Pager.state(PageIndicatorKind.ARROWS, true, 0, isLastPage = true, candidateCount = 3).show,
        )
        assertFalse(
            "沒有候選時候選列畫的是工具列",
            Pager.state(PageIndicatorKind.ARROWS, true, 0, isLastPage = false, candidateCount = 0).show,
        )
        assertFalse(
            "主題關掉 page_indicator 就不畫",
            Pager.state(PageIndicatorKind.ARROWS, false, 0, isLastPage = false, candidateCount = 5).show,
        )
        assertFalse(
            "style: none 就不畫",
            Pager.state(PageIndicatorKind.NONE, true, 0, isLastPage = false, candidateCount = 5).show,
        )
    }

    /**
     * **性質**:一直按下一頁，每一頁都到得了。
     *
     * 附一份故意寫壞的實作（「只有翻過頁之後才准再翻」），確認這條檢查
     * 在該紅的時候真的會紅 —— 「測試安靜地跳過自己」是本專案的舊帳。
     */
    @Test
    fun `一路按下去走得完每一頁`() {
        assertReachable(pages = 4) { pageNo, isLast ->
            Pager.state(PageIndicatorKind.ARROWS, true, pageNo, isLast, 5).nextEnabled
        }
        var caught = false
        try {
            assertReachable(pages = 4) { pageNo, _ -> pageNo > 0 }   // 故意寫壞
        } catch (e: AssertionError) {
            caught = true
        }
        assertTrue("寫壞的翻頁判斷沒有被這條性質擋下來 —— 那這條檢查沒有在守", caught)
    }

    private fun assertReachable(pages: Int, nextEnabled: (Int, Boolean) -> Boolean) {
        var page = 0
        var steps = 0
        while (page < pages - 1) {
            assertTrue(
                "停在第 ${page + 1} 頁就按不動了,後面 ${pages - page - 1} 頁使用者永遠看不到",
                nextEnabled(page, false),
            )
            page++
            steps++
            assertTrue("翻頁走不完", steps <= pages)
        }
        assertFalse("最後一頁還說有下一頁", nextEnabled(page, true))
    }

    /**
     * 隨附的每一份主題都真的會把箭頭畫出來。
     *
     * 純函式全綠而主題把 `page_indicator.show` 關掉的話，使用者看到的仍然是
     * 「下一頁就沒了」—— 這一條讀的是 `core/themes` 的真檔案。
     */
    @Test
    fun `隨附主題的候選列都留著翻頁鍵`() {
        val repo = FixtureRepo()
        var checked = 0
        for (id in RepoFixtures.themeIds) {
            val theme = repo.loadTheme(id).value
            assertNotNull("主題 $id 載不起來", theme)
            val pi = theme!!.candidates.bar.style.pageIndicator
            assertTrue(
                "主題 $id 的候選列關掉了翻頁鍵（show=${pi.show} style=${pi.kind}）—— " +
                    "使用者又會看到「下一頁就沒了」",
                pi.show && pi.kind != PageIndicatorKind.NONE,
            )
            checked++
        }
        assertTrue("一份主題都沒掃到,這條在空轉", checked >= 8)
    }


    /* ═══════════════ 候選列展開:不可以重新編號 ═══════════════ */

    /**
     * **這一條守的是「點了第 6 個卻上屏第 1 個」。**
     *
     * `rs_select_candidate` 吃的是頁內索引(實測:第 2 頁 select(0) 上屏的是
     * 第 2 頁的第 0 個)。展開面板只是把同一頁換個排法,收進去的引擎索引
     * 必須原封不動地吐回來 —— 順序、值、個數都不准變。
     *
     * 什麼時候紅:任何人把 `rows` 改成「回傳畫面位置」(例如 `indices.indices`
     * 或 `chunked` 之後重新編號),或是為了排版順手排序／去重。
     */
    @Test
    fun `展開之後的候選索引還是引擎索引`() {
        val engineIndices = listOf(0, 1, 2, 3, 4, 5, 6, 7, 8)
        val rows = Expander.rows(engineIndices, 4)
        assertEquals("9 個排 4 欄 = 3 列", 3, rows.size)
        assertEquals("攤平回來必須與輸入逐項相同", engineIndices, rows.flatten())

        // 消歧欄釘住讀音之後,畫面上只留下引擎的第 3/4/7 個 ——
        // 那時候「畫面位置」與「引擎索引」本來就不一樣,這一條才有意義。
        val filtered = listOf(3, 4, 7)
        val filteredRows = Expander.rows(filtered, 2)
        assertEquals(listOf(listOf(3, 4), listOf(7)), filteredRows)
        assertEquals(
            "被篩選過的清單更不可以重新編號 —— 重編就是選到隔壁的字",
            filtered,
            filteredRows.flatten(),
        )
    }

    /**
     * 邊界:一列 0 欄不可以讓候選整批消失。
     *
     * 什麼時候紅:有人把 `perRow` 直接拿去 `chunked` 而沒有夾下限 ——
     * `chunked(0)` 會丟例外,畫面上就是鍵盤整個掛掉。
     */
    @Test
    fun `一列算出零欄時仍然畫得出候選`() {
        assertEquals(listOf(listOf(1), listOf(2)), Expander.rows(listOf(1, 2), 0))
        assertEquals(listOf(listOf(1), listOf(2)), Expander.rows(listOf(1, 2), -3))
        assertTrue("沒有候選就沒有列", Expander.rows(emptyList(), 4).isEmpty())
        assertEquals("量不到寬度時至少排一欄", 1, Expander.perRow(0f, 40f))
        assertEquals("項寬是 0 時不可以除以零", 1, Expander.perRow(400f, 0f))
        assertEquals("再寬也有上限", 5, Expander.perRow(4000f, 40f))
        assertEquals("411dp 的螢幕、40dp 的項 → 5 欄", 5, Expander.perRow(411f, 40f))
        assertEquals("項變寬就少一欄", 3, Expander.perRow(411f, 120f))
    }

    /**
     * 展開鍵什麼時候該在。
     *
     * 什麼時候紅:候選變空(選完字)之後面板還開著 —— 那是一片蓋住鍵盤、
     * 裡面卻沒有東西可點的浮層,使用者只能退出輸入框。
     */
    @Test
    fun `候選變空時展開面板一定跟著收起來`() {
        val open = Expander.state(ScrollMode.EXPANDABLE, showButton = true, candidateCount = 9, wanted = true)
        assertTrue(open.show)
        assertTrue(open.expanded)

        val emptied = Expander.state(ScrollMode.EXPANDABLE, showButton = true, candidateCount = 0, wanted = true)
        assertFalse("沒有候選時展開鍵不該擠進工具列", emptied.show)
        assertFalse("使用者上一刻按開的面板必須自己收掉", emptied.expanded)

        assertFalse(
            "主題把 scroll 設成 horizontal 就不畫展開鍵",
            Expander.state(ScrollMode.HORIZONTAL, true, 9, true).show,
        )
        assertFalse(
            "主題把 expand_button.show 關掉就不畫",
            Expander.state(ScrollMode.EXPANDABLE, false, 9, true).show,
        )
        assertFalse(
            "沒按開就不展開",
            Expander.state(ScrollMode.EXPANDABLE, true, 9, false).expanded,
        )
    }

    /**
     * 隨附主題不可以把展開這條路關掉 —— 關掉之後直式就真的只剩 3 個,
     * 而 `menu/page_size` 調到 9 完全看不出來。
     *
     * 什麼時候紅:有人在某一份主題裡把 `scroll` 設成 `none`/`horizontal`,
     * 或把 `expand_button.show` 設成 false。
     */
    @Test
    fun `隨附主題的候選列都展得開`() {
        val repo = FixtureRepo()
        var checked = 0
        for (id in RepoFixtures.themeIds) {
            val theme = repo.loadTheme(id).value
            assertNotNull("主題 $id 載不起來", theme)
            val bar = theme!!.candidates.bar
            assertTrue(
                "主題 $id 的候選列展不開(scroll=${bar.scroll} " +
                    "expand_button.show=${bar.expandButton.show}) —— " +
                    "直式一列只畫得下 3 個,展不開就等於候選只有 3 個",
                bar.scroll == ScrollMode.EXPANDABLE && bar.expandButton.show,
            )
            checked++
        }
        assertTrue("一份主題都沒掃到,這條在空轉", checked >= 8)
    }

    private fun t9GroupCodes(): Set<Char> {
        val layout = FixtureRepo().loadLayout(T9_LAYOUT).value!!
        val layer = layout.layer("t9")
        assertNotNull("$T9_LAYOUT 沒有 t9 層", layer)
        return InlinePreedit.groupCodeChars(layer)
    }

    private companion object {
        const val T9_LAYOUT = "cn-t9-pinyin"
        const val T9_SCHEMA = "t9_pinyin"
    }
}
