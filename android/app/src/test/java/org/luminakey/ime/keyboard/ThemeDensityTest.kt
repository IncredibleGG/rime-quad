package org.luminakey.ime.keyboard

import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.Assert.assertFalse
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
                    if (CandidateDensity.deadEnd(layout, count, morePages = !last)) {
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
            CandidateDensity.deadEnd(trapped, 3, morePages = true),
        )
        // 只關掉翻頁指示器(展開鍵留著)**不是**死路 —— 判準不得寬到把它也判紅。
        val onlyPagerOff = CandidateDensity.barLayout(
            screenWidthDp = 411.43f, barPaddingH = 4f, reservedEnd = 40f, buttonDp = 40f,
            leadingDp = 0f, widths = List(3) { 56f }, spacing = 4f,
            pageCandidateCount = 3, pageNo = 0, isLastPage = false,
            pagerKind = PageIndicatorKind.ARROWS,
            pagerShow = false, expandAvailable = true, panelOpen = false,
        )
        assertFalse(CandidateDensity.deadEnd(onlyPagerOff, 3, morePages = true))
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
