package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.theme.PageIndicatorKind
import org.luminakey.ime.theme.Platform
import org.luminakey.ime.theme.RepoFixtures
import org.luminakey.ime.theme.ScrollMode
import org.luminakey.ime.theme.ThemeLoader

/**
 * `core/themes/` 底下**每一份**主題都要排得下（§10 第 41 條）。
 *
 * ── 為什麼是掃目錄而不是列幾個 id ────────────────────────────────────────
 * 這個專案已經因為「清單寫死」漏檢過兩次：`LayoutEscapeTest` 的四份佈局清單
 * 讓 12 份裡的 8 份從沒被檢查、而那幾份都真的有死路；主題那份清單原封不動地
 * 犯了同一件事。所以這裡用 [RepoFixtures.themeIds]，新增一份主題就自動被納入。
 *
 * ── 為什麼是「基準情境」而不是主題自己的字級 ────────────────────────────
 * `cn-compact-*` 的 `bar.text.size` 是 22，照它自己的字級算當然排得少。
 * 但那是**使用者要的**（他選了大字的主題），不是主題悄悄加上去的開銷。
 * 這一關要擋的是後者：內距、間距、右端保留區、觸控目標下界 —— 那些使用者
 * 沒有要求、也看不見的東西。所以固定 `text.size: 20`、兩字 CJK、無序號無註解。
 *
 * ⚠ 這條檢核只驗**下界**（≥ N），不驗相等。規範 §8.6.4.1 自承不規範字形量測，
 *   四端畫得下幾個不會逐 px 相同。下界式的檢核天生比等式鬆，會漏掉
 *   「某端只差 1 dp 就少一個」的漂移 —— 這是已知的限制，不是疏忽。
 */
class ThemeDensityTest {

    private fun bar(id: String) =
        ThemeLoader.load(id, RepoFixtures.themes, Platform.ANDROID).value
            ?.candidates?.bar
            ?: error("主題 $id 載不起來")

    @Test
    fun `每一份主題在 360 dp 上都排得下 5 個`() {
        assertAtLeast(360f, 5)
    }

    @Test
    fun `每一份主題在 411 dp 上都排得下 6 個`() {
        assertAtLeast(411.43f, 6)
    }

    @Test
    fun `每一份主題在 456 dp 的 S24U 上都排得下 6 個`() {
        assertAtLeast(456.2f, 6)
    }

    private fun assertAtLeast(widthDp: Float, want: Int) {
        val bad = ArrayList<String>()
        for (id in RepoFixtures.themeIds) {
            val b = bar(id)
            val n = CandidateDensity.baselineVisible(
                screenWidthDp = widthDp,
                barPaddingH = b.paddingH,
                reservedEndDp = b.reservedEnd,
                paddingH = b.style.item.paddingH,
                spacing = b.style.item.spacing,
                minWidth = b.style.item.minWidth,
            )
            if (n < want) {
                bad += "$id：$n 個（padding_h=${b.style.item.paddingH}、" +
                    "spacing=${b.style.item.spacing}、min_width=${b.style.item.minWidth}、" +
                    "bar.padding_h=${b.paddingH}、reserved_end=${b.reservedEnd}）"
            }
        }
        assertTrue(
            "${widthDp.toInt()} dp 上排不下 $want 個的主題（基準情境：text.size 20、兩字 CJK、" +
                "無序號無註解）：\n  " + bad.joinToString("\n  "),
            bad.isEmpty(),
        )
    }

    /**
     * ⚠ **這一版的招牌成果在「消歧欄不活」那一格自動歸零 —— 把它量出來,
     *    並且釘住,不要等使用者踩到。**
     *
     * 密度從 3 拉到 6,有 35.3% 來自「註解與消歧欄互斥」(§8.6.3.1):
     * 消歧欄畫了讀音,註解就不必再畫第二份。而消歧欄那一側在兩種組態下
     * **不活**,於是註解回來:
     *
     *   · 主題寫 `candidates.syllables.placement: none`
     *   · 啟動探針判定方案改寫不動輸入串(`syllableRewriteReady == false`)
     *
     * 這一關把那一格的數字**量出來並釘住**。它刻意**不是**「≥ 6」——
     * 那會逼人拿掉互斥規則或動別的東西來湊,而那一格本來就付得起
     * 一格 98.6 dp:使用者在那個組態下拿到的是讀音,不是空氣。
     * 釘的是「不得更差」＋「差多少要說得出來」。
     */
    @Test
    fun `消歧欄不活時註解回來,密度掉多少要量得出來`() {
        val 下界 = 3          // 411 dp 上的實測/模型值。掉到 2 就是新的缺陷。
        val rows = ArrayList<String>()
        val bad = ArrayList<String>()
        for (id in RepoFixtures.themeIds) {
            val b = bar(id)
            val 活 = CandidateDensity.baselineVisible(
                screenWidthDp = 411.43f, barPaddingH = b.paddingH, reservedEndDp = b.reservedEnd,
                paddingH = b.style.item.paddingH, spacing = b.style.item.spacing,
                minWidth = b.style.item.minWidth,
            )
            val 不活 = CandidateDensity.baselineVisibleWithComment(
                screenWidthDp = 411.43f, barPaddingH = b.paddingH, reservedEndDp = b.reservedEnd,
                paddingH = b.style.item.paddingH, spacing = b.style.item.spacing,
                minWidth = b.style.item.minWidth,
                commentSize = CandidateDensity.BASELINE_TEXT_SIZE * 0.6f,  // 12 sp
            )
            rows += "$id：消歧欄活 $活 個 → 不活 $不活 個"
            if (不活 < 下界) bad += "$id：$不活 個(下界 $下界)"
        }
        assertTrue(
            "消歧欄不活的那一格掉到下界以下了。\n  " + bad.joinToString("\n  ") +
                "\n全部：\n  " + rows.joinToString("\n  "),
            bad.isEmpty(),
        )
        // 反向:這一關必須真的分得出兩種組態 —— 兩邊一樣多的話它什麼都沒在守。
        val 差 = CandidateDensity.itemWidthDp(
            textChars = 2, textSize = CandidateDensity.BASELINE_TEXT_SIZE,
            labelChars = 0, labelSize = 0f,
            commentChars = CandidateDensity.BASELINE_COMMENT_CHARS, commentSize = 12f,
            paddingH = 8f, minWidth = 48f,
        ) - CandidateDensity.itemWidthDp(
            textChars = 2, textSize = CandidateDensity.BASELINE_TEXT_SIZE,
            labelChars = 0, labelSize = 0f, commentChars = 0, commentSize = 0f,
            paddingH = 8f, minWidth = 48f,
        )
        assertEquals("一格的註解成本變了 —— 上面那個下界是照 42.6 dp 訂的", 42.6f, 差, 0.05f)
    }

    /**
     * ⚠ **每一份主題都必須留著展開面板這條路。**
     *
     * §8.6.6.4 第 2 條規定「本頁還有畫不出來的候選時不得提供下一頁」，
     * 而唯一的替代出口是展開面板。主題把 `scroll` 或 `expand_button.show`
     * 關掉，就等於讓那個情境退回翻頁（[CandidateDensity.rightEnd] 的退路），
     * 也就是**讓使用者跳過他沒看見的候選** —— 或者更糟，什麼出口都沒有。
     *
     * §8.6.6.4 第 4 條：「候選列本身可以橫向捲動，但捲動不得是唯一路徑。」
     * 沒有捲軸、沒有提示，使用者不會知道右邊還有東西。
     */
    @Test
    fun `每一份主題都留著展開面板`() {
        val bad = RepoFixtures.themeIds.filter { id ->
            val b = bar(id)
            !(b.expandButton.show && b.scroll == ScrollMode.EXPANDABLE)
        }
        assertTrue(
            "這幾份主題關掉了展開面板，於是「本頁還有畫不出來的候選」時" +
                "沒有第二條路：$bad",
            bad.isEmpty(),
        )
    }

    /**
     * ⛔ **每一份主題、每一種頁況,右端都要留得住一條出口。**
     *
     * 上一條(`每一份主題都留著展開面板`)只看 `scroll` 與 `expand_button.show`,
     * **沒有看 `page_indicator`**。而右端那一顆是三選一:翻頁畫不出來時
     * 展開鍵是唯一的出口,展開鍵關掉時翻頁是唯一的出口,兩個都關掉就是
     * **右端空白、使用者鎖死在第 1 頁** —— 而畫面完全正常。
     *
     * 所以這一條不是「檢查兩個欄位」,是把 [CandidateDensity.barLayout] 逐份
     * 主題、逐種頁況跑一遍,問 [CandidateDensity.deadEnd]。判準因此永遠等於
     * 產品真的會畫出來的東西,不會因為欄位改名或多一個開關而悄悄失效。
     */
    @Test
    fun `每一份主題在每一種頁況下都留得住出口`() {
        val bad = ArrayList<String>()
        for (id in RepoFixtures.themeIds) {
            for ((page, last) in listOf(0 to false, 0 to true, 1 to false, 1 to true)) {
                // 本頁 3 個(411 dp 上一定排得下)與 9 個(一定排不下)兩種。
                for (count in listOf(3, 9)) {
                    val layout = layoutOf(id, count, page, last)
                    val panelPager =
                        CandidateDensity.panelPagerDrawable(page, last, shownCount = count)
                    if (CandidateDensity.deadEnd(layout, count, !last, panelPager)) {
                        bad += "$id：第 ${page + 1} 頁、${if (last) "" else "不"}是最後一頁、" +
                            "本頁 $count 個 → 右端 ${layout.rightEnd}"
                    }
                }
            }
        }
        assertTrue(
            "這幾格「還有候選沒看到,而右端一顆都畫不出來」—— 使用者鎖死在這一頁：\n  " +
                bad.joinToString("\n  "),
            bad.isEmpty(),
        )
    }

    /**
     * 反向測試:把 `page_indicator` 與展開鍵**同時**關掉,上一條必須紅。
     *
     * 沒有它,上一條有可能只是因為隨附主題碰巧沒有人關過翻頁指示器 ——
     * 而「一個什麼都分不出來的判準,長得跟通過一模一樣」正是本專案栽過的跟頭。
     */
    @Test
    fun `兩條出口同時關掉時那一關會紅`() {
        val trapped = CandidateDensity.barLayout(
            screenWidthDp = 411.43f, barPaddingH = 4f, reservedEnd = 40f, buttonDp = 40f,
            leadingDp = 0f, widths = List(3) { 56f }, spacing = 4f,
            pageCandidateCount = 3, pageNo = 0, isLastPage = false,
            pagerKind = PageIndicatorKind.ARROWS,
            pagerShow = false,          // page_indicator.show: false
            expandAvailable = false,    // expand_button.show: false
            panelOpen = false,
        )
        assertTrue(
            "兩條出口都關掉了而 deadEnd() 說沒事 —— 那上面那一關什麼都沒在守",
            CandidateDensity.deadEnd(
                trapped, 3, morePages = true,
                panelPagerDrawable = CandidateDensity.panelPagerDrawable(0, false, 3),
            ),
        )
        // 只關掉翻頁指示器(展開鍵留著)**不是**死路 —— 但這句話只有在
        // **面板自己的翻頁列還畫得出來**的時候才成立。上一版這裡是
        //     assertFalse(deadEnd(onlyPagerOff, 3, morePages = true))
        // 註解寫著「這不是死路」,而實測是:右端有 `∨` → 面板打開 →
        // 底部翻頁鍵一顆都沒有 → 第 2 頁永遠進不去。那一行把缺陷釘成了綠燈。
        val onlyPagerOff = CandidateDensity.barLayout(
            screenWidthDp = 411.43f, barPaddingH = 4f, reservedEnd = 40f, buttonDp = 40f,
            leadingDp = 0f, widths = List(3) { 56f }, spacing = 4f,
            pageCandidateCount = 3, pageNo = 0, isLastPage = false,
            pagerKind = PageIndicatorKind.ARROWS,
            pagerShow = false, expandAvailable = true, panelOpen = false,
        )
        assertEquals(CandidateDensity.RightEnd.EXPAND, onlyPagerOff.rightEnd)
        // ⛔ **先斷言那條路真的通得到第 2 頁**,再說它不是死路。
        assertTrue(
            "候選列的 page_indicator 關掉了,而展開面板的翻頁列**必須**照畫 ——" +
                "面板是這條路唯一的下一頁入口",
            CandidateDensity.panelPagerDrawable(0, isLastPage = false, shownCount = 3),
        )
        assertFalse(
            CandidateDensity.deadEnd(
                onlyPagerOff, 3, morePages = true,
                panelPagerDrawable = CandidateDensity.panelPagerDrawable(0, false, 3),
            ),
        )
    }

    /**
     * ⛔ **面板的翻頁列不得跟著候選列的 `page_indicator` 一起被關掉。**
     *
     * 這一關釘的是 [Pager.panelState] 的**型別**:它沒有、也不准有
     * `kind` / `show` 參數 —— 主題傳不進來,就不可能把它關掉。
     * 舊規則(面板吃 `style.pageIndicator`)在這裡逐格重演一次,證明它真的是死路。
     */
    @Test
    fun `舊規則(面板吃候選列的 page_indicator)在每一種關法上都是死路`() {
        val 關法 = listOf(
            "show: false" to Pair(false, PageIndicatorKind.ARROWS),
            "style: none" to Pair(true, PageIndicatorKind.NONE),
            "兩個都關" to Pair(false, PageIndicatorKind.NONE),
        )
        for ((名字, 設定) in 關法) {
            val (show, kind) = 設定
            // 舊規則:面板的翻頁列吃候選列那一份設定。
            val 舊 = Pager.state(
                kind = kind, show = show,
                pageNo = 0, isLastPage = false, candidateCount = 3,
            )
            assertFalse(
                "舊規則在「$名字」之下畫得出翻頁列 —— 那這一關重演的不是當初那個缺陷",
                舊.show,
            )
            // 新規則:面板自己說了算。
            assertTrue(
                "「$名字」之下面板的翻頁列必須照畫",
                CandidateDensity.panelPagerDrawable(0, isLastPage = false, shownCount = 3),
            )
        }
    }

    /**
     * ⛔ **使用者能到達的每一種頁況 × 每一種主題開關組合,逐格回答
     *    「他要怎麼看到下一個候選」。**
     *
     * 這條死路修過三次,每一次都只補了被指出的那一格:
     *
     *   第一次  候選密度(一列排得下幾個)
     *   第二次  右端不得空白 → 補了 `pagerDrawable`
     *   第三次  死路搬進面板裡 → 面板的翻頁列被同一個開關關掉
     *
     * 每一次都是「修好被指出的那一格」,所以每一次都留下了下一格。
     * 這一關改成**列舉**:七個維度全展開,每一格算出使用者的出路,
     * 然後斷言「走不出去的格子」**恰好**等於「主題把兩條出口都關掉」那一組。
     * 不是「沒有死路」(那會被一個過寬的判準騙過),是**等於** ——
     * 多一格少一格都紅。
     *
     * 不可到達的組合明著排除並說出理由,不是默默不列。
     */
    @Test
    fun `每一種頁況乘每一種主題開關,逐格算得出出路`() {
        data class Cell(
            val pageNo: Int,
            val isLastPage: Boolean,
            val pageCount: Int,
            val panelOpen: Boolean,
            val pagerShow: Boolean,
            val pagerKind: PageIndicatorKind,
            val expandAvailable: Boolean,
        )

        val cells = ArrayList<Cell>()
        for (pageNo in listOf(0, 1)) {
            for (isLastPage in listOf(false, true)) {
                // 3 = 411 dp 上一定排得下(visible == count)
                // 9 = 一定排不下(visible < count,本頁就有沒看到的)
                for (pageCount in listOf(3, 9)) {
                    for (pagerShow in listOf(true, false)) {
                        for (pagerKind in listOf(PageIndicatorKind.ARROWS, PageIndicatorKind.NONE)) {
                            for (expandAvailable in listOf(true, false)) {
                                for (panelOpen in listOf(false, true)) {
                                    // ⛔ 不可到達:面板只能從展開鍵打開。展開鍵不存在時
                                    //    `panelOpen` 這一格在產品裡不存在,列進來只會
                                    //    讓這張表多一格假的答案。
                                    if (panelOpen && !expandAvailable) continue
                                    cells += Cell(
                                        pageNo, isLastPage, pageCount, panelOpen,
                                        pagerShow, pagerKind, expandAvailable,
                                    )
                                }
                            }
                        }
                    }
                }
            }
        }
        // 2(頁) × 2(末頁) × 2(本頁排不排得下) × 2(show) × 2(kind) × 3(展開×面板)
        assertEquals("列舉少了格子 —— 這張表就不是「每一種」了", 96, cells.size)

        val table = StringBuilder()
        val dead = ArrayList<String>()
        val expectedDead = ArrayList<String>()
        for (c in cells) {
            val layout = CandidateDensity.barLayout(
                screenWidthDp = 411.43f, barPaddingH = 4f, reservedEnd = 40f, buttonDp = 40f,
                leadingDp = 0f, widths = List(c.pageCount) { 56f }, spacing = 4f,
                pageCandidateCount = c.pageCount, pageNo = c.pageNo, isLastPage = c.isLastPage,
                pagerKind = c.pagerKind, pagerShow = c.pagerShow,
                expandAvailable = c.expandAvailable, panelOpen = c.panelOpen,
            )
            val panelPager =
                CandidateDensity.panelPagerDrawable(c.pageNo, c.isLastPage, c.pageCount)
            val morePages = !c.isLastPage
            val unseenHere = layout.visible < c.pageCount
            val unseen = unseenHere || morePages
            val isDead = CandidateDensity.deadEnd(layout, c.pageCount, morePages, panelPager)

            // 「翻頁那一組真的畫得出來嗎」—— 與 barLayout 內部同一條判準。
            val barPager = Pager.state(
                kind = c.pagerKind, show = c.pagerShow, pageNo = c.pageNo,
                isLastPage = c.isLastPage, candidateCount = c.pageCount,
            )
            val pagerDrawable = barPager.show && (barPager.prevEnabled || barPager.nextEnabled)

            val route = when {
                !unseen -> "不必 —— 候選全在畫面上"
                layout.rightEnd == CandidateDensity.RightEnd.PAGER -> "按 › 翻頁"
                layout.rightEnd == CandidateDensity.RightEnd.EXPAND && c.panelOpen ->
                    if (morePages && !panelPager) "✗ 面板開著而面板內翻不了頁"
                    else "面板已開:本頁全在面板裡" + if (morePages) ",下一頁按面板底部的 ›" else ""
                layout.rightEnd == CandidateDensity.RightEnd.EXPAND ->
                    if (morePages && !panelPager) "✗ 面板打得開,但面板內翻不了頁"
                    else "按 ∨ 開面板:本頁全在裡面" + if (morePages) ",下一頁按面板底部的 ›" else ""
                else -> "✗ 右端一顆都畫不出來"
            }
            val key = "頁${c.pageNo + 1}/${if (c.isLastPage) "末頁" else "還有頁"}" +
                "/本頁${c.pageCount}個/${if (c.panelOpen) "面板開" else "面板關"}" +
                "/show=${c.pagerShow}/kind=${c.pagerKind}/展開=${c.expandAvailable}"
            table.append("  ").append(key).append(" → ").append(layout.rightEnd)
                .append(" | ").append(route).append('\n')

            if (isDead) dead += key
            // **應該**走不出去的,恰好是「還有東西沒看到,而兩條出口都畫不出來」。
            if (unseen && !pagerDrawable && !c.expandAvailable) expectedDead += key
        }

        assertEquals(
            "「走不出去的格子」與「兩條出口都關掉的格子」對不上。\n" +
                "多出來的(判準太寬):${dead - expectedDead.toSet()}\n" +
                "漏掉的(判準太窄,使用者真的鎖死了):${expectedDead - dead.toSet()}\n" +
                "全表:\n$table",
            expectedDead.toSet(), dead.toSet(),
        )
        // 死路必須真的存在幾格 —— 一個永遠算不出死路的判準,長得跟通過一模一樣。
        assertTrue("這張表一格死路都算不出來 —— 判準在空轉", dead.isNotEmpty())
    }

    /** 用某一份主題真正的欄位值算出一格頁況的版面。 */
    private fun layoutOf(id: String, pageCount: Int, pageNo: Int, isLastPage: Boolean) =
        bar(id).let { b ->
            CandidateDensity.barLayout(
                screenWidthDp = 411.43f,
                barPaddingH = b.paddingH,
                reservedEnd = b.reservedEnd,
                buttonDp = 40f,
                leadingDp = 0f,
                widths = List(pageCount) {
                    CandidateDensity.itemWidthDp(
                        textChars = 2,
                        textSize = CandidateDensity.BASELINE_TEXT_SIZE,
                        labelChars = 0, labelSize = 0f, commentChars = 0, commentSize = 0f,
                        paddingH = b.style.item.paddingH,
                        minWidth = b.style.item.minWidth,
                    )
                },
                spacing = b.style.item.spacing,
                pageCandidateCount = pageCount,
                pageNo = pageNo,
                isLastPage = isLastPage,
                pagerKind = b.style.pageIndicator.kind,
                pagerShow = b.style.pageIndicator.show,
                expandAvailable = b.expandButton.show && b.scroll == ScrollMode.EXPANDABLE,
                panelOpen = false,
            )
        }

    /**
     * 反向測試：把基準情境換成**改動前**的那組數，這一關必須紅。
     *
     * 沒有它，上面三條有可能只是因為門檻訂得太鬆 —— 而「一個什麼都分不出來的
     * 判準，長得跟通過一模一樣」正是本專案栽過的跟頭。
     */
    @Test
    fun `改動前的那組數過不了這一關`() {
        val before = CandidateDensity.baselineVisible(
            screenWidthDp = 411.43f,
            barPaddingH = 4f,
            reservedEndDp = 80f,
            paddingH = 10f,
            spacing = 4f,
            minWidth = 0f,
        )
        assertTrue(
            "改動前的值也通過 411 dp ≥ 6 —— 那這一關什麼都沒在守",
            before < 6,
        )
    }
}
