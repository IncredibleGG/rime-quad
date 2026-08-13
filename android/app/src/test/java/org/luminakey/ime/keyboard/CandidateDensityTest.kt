package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.core.RimeCandidate
import org.luminakey.ime.theme.CommentStyle
import org.luminakey.ime.theme.CommentPosition
import org.luminakey.ime.theme.HighlightStyle
import org.luminakey.ime.theme.ItemStyle
import org.luminakey.ime.theme.LabelSource
import org.luminakey.ime.theme.LabelStyle
import org.luminakey.ime.theme.LayoutKey
import org.luminakey.ime.theme.LayoutLayer
import org.luminakey.ime.theme.LayoutRow
import org.luminakey.ime.theme.LocalizedString
import org.luminakey.ime.theme.PageIndicatorKind
import org.luminakey.ime.theme.Popup
import org.luminakey.ime.theme.PopupLayout
import org.luminakey.ime.theme.SendSpec
import org.luminakey.ime.theme.SubKey
import org.luminakey.ime.theme.SwipeDirection
import org.luminakey.ime.theme.TextStyle

/**
 * 候選列的密度與資訊架構（§8.6.1.1 / §8.6.3.1 / §8.6.4.2 / §8.6.4.3 / §8.6.6.4，以及 §10 的第 40、42、43、44、45 條）。
 *
 * ── 為什麼這一組非有不可 ────────────────────────────────────────────────
 * 使用者的原話是「**我們的候選詞太少了 因為空間被壓縮了**」，以及對我們上一輪
 * 盤點的批評：「這個之前都注意麼?那產品經理在調研什麼」。那句批評是成立的 ——
 * 上一輪量到了「候選列只排得下 3–4 個」，卻**從來沒問「為什麼這麼少」**。
 *
 * 「為什麼這麼少」的答案是一條算式，而算式測得到。這一組把它釘住：
 * 360 dp 的機器上排不下 5 個、411 dp 排不下 6 個，就是**設計錯誤**，
 * 在建置期紅，不是等使用者拿真機比對三星才發現。
 */
class CandidateDensityTest {

    /* ═══════════ §10 第 40 條：密度下界 ═══════════ */

    /**
     * 基準情境的一格寬 = 兩字 CJK 40 dp ＋ 左右內距 8×2 = **56 dp**，
     * 節距 = 56 ＋ `spacing` 4 = **60 dp**。
     *
     * 這個 60 不是抄來的：它是「兩字墨跡 40 dp ＋ 兩個候選之間看得出是兩個
     * 東西的空白 20 dp（ui-design §3.1 的 s6，恰為半個字）」算出來的。
     * 對照組驗算：語燕同機實測節距 59.5 dp（差 0.8%）。
     */
    private val BAR_PADDING_H = 4f
    private val RESERVED_END = 40f
    private val PADDING_H = 8f
    private val SPACING = 4f
    private val MIN_WIDTH = 48f

    private fun baseline(width: Float) = CandidateDensity.baselineVisible(
        screenWidthDp = width,
        barPaddingH = BAR_PADDING_H,
        reservedEndDp = RESERVED_END,
        paddingH = PADDING_H,
        spacing = SPACING,
        minWidth = MIN_WIDTH,
    )

    @Test
    fun `基準情境的一格是 56 dp`() {
        assertEquals(
            56f,
            CandidateDensity.itemWidthDp(
                textChars = 2, textSize = 20f,
                labelChars = 0, labelSize = 0f,
                commentChars = 0, commentSize = 0f,
                paddingH = PADDING_H, minWidth = MIN_WIDTH,
            ),
            0.001f,
        )
    }

    @Test
    fun `360 dp 上至少 5 個`() {
        assertEquals(5, baseline(360f))
    }

    @Test
    fun `411 dp 的實測機上至少 6 個`() {
        // emulator-5558：1080×2400 @420dpi = 411.43 dp。使用者截圖裡是 3 個。
        assertEquals(6, baseline(411.43f))
    }

    @Test
    fun `456 dp 的 S24U 上至少 6 個`() {
        assertEquals(6, baseline(456.2f))
    }

    /**
     * **改動前的那組數在同一條下界上是紅的。**
     *
     * 這一條是上面三條的反向測試：沒有它，「≥ 6」有可能只是因為公式算得太寬鬆，
     * 而不是因為我們真的改了什麼。舊值 = `padding_h: 10`、`min_width: 0`、
     * 右端保留兩顆（80 dp）。
     */
    @Test
    fun `改動前的主題值在 411 dp 上不到 6 個`() {
        val before = CandidateDensity.baselineVisible(
            screenWidthDp = 411.43f,
            barPaddingH = 4f,
            reservedEndDp = 80f,   // 翻頁 ＋ 展開，兩顆
            paddingH = 10f,
            spacing = 4f,
            minWidth = 0f,
        )
        assertTrue("舊值竟然也排得下 6 個 —— 那這一輪的下界沒有在守任何東西", before < 6)
        assertEquals(5, before)
    }

    /** 右端從兩顆砍到一顆，在 411 dp 上**恰好值一個候選**。 */
    @Test
    fun `右端少一顆鍵就多一個候選`() {
        val twoButtons = CandidateDensity.baselineVisible(
            411.43f, BAR_PADDING_H, 80f, PADDING_H, SPACING, MIN_WIDTH
        )
        val oneButton = baseline(411.43f)
        assertEquals(5, twoButtons)
        assertEquals(6, oneButton)
    }

    /** 空白全放進 `spacing`（照抄三星外觀的做法）反而更少 —— 而且點不到。 */
    @Test
    fun `空白放 spacing 不放 padding 會更少`() {
        // 方案丙：padding_h 0、spacing 20、min_width 48（非套不可，否則
        // 一字候選的觸控寬度只有 20 dp）。節距 48+20 = 68 > 60。
        val spacingStyle = CandidateDensity.baselineVisible(
            screenWidthDp = 411.43f,
            barPaddingH = BAR_PADDING_H,
            reservedEndDp = RESERVED_END,
            paddingH = 0f,
            spacing = 20f,
            minWidth = 48f,
        )
        assertTrue(
            "把格間空白放進 spacing 竟然沒有變差 —— 那 §8.6.4.2 的那條警告是空話",
            spacingStyle < baseline(411.43f),
        )
    }

    /** `min_width` 真的被套用：一字候選的量測寬不得小於觸控目標下界。 */
    @Test
    fun `一字候選被 min_width 撐到 48`() {
        val w = CandidateDensity.itemWidthDp(
            textChars = 1, textSize = 20f,
            labelChars = 0, labelSize = 0f,
            commentChars = 0, commentSize = 0f,
            paddingH = PADDING_H, minWidth = MIN_WIDTH,
        )
        // 內容只有 20 + 16 = 36 dp，低於 ui-design §3.6 的 48。
        assertEquals(48f, w, 0.001f)
    }

    /** `visibleCount` 只數**完整**畫得出來的 —— 多算一個就是讓使用者跳過它。 */
    @Test
    fun `畫一半的那一個不算`() {
        // 節距 60：兩個要 116 dp。差 1 dp 就只有一個。
        assertEquals(1, CandidateDensity.visibleCount(115f, listOf(56f, 56f, 56f), 4f))
        assertEquals(2, CandidateDensity.visibleCount(116f, listOf(56f, 56f, 56f), 4f))
        assertEquals(2, CandidateDensity.visibleCount(175f, listOf(56f, 56f, 56f), 4f))
        assertEquals(3, CandidateDensity.visibleCount(176f, listOf(56f, 56f, 56f), 4f))
    }

    /* ═══════════ §10 第 42 條：註解與消歧欄互斥 ═══════════ */

    @Test
    fun `t9 的讀音 comment 讓註解不畫`() {
        val cands = listOf(
            RimeCandidate("你好", "ni hao"),
            RimeCandidate("妳好", "ni hao"),
            RimeCandidate("你敢", "ni gan"),
        )
        val readings = T9Syllables.readingsOf(cands)
        assertEquals(listOf("ni"), readings)
        assertFalse(
            "comment 是讀音而註解照畫 —— 同一份讀音被畫了兩次，而只有註解要付寬度",
            CandidateDensity.commentVisible(
                commentShow = true, readings = readings, syllableSideAlive = true,
            ),
        )
    }

    /**
     * **門檻刻意不同：註解是 ≥1 就關，消歧欄是 ≥2 才開。**
     *
     * 相同的話，讀音收斂到只剩一個時消歧欄收起來、註解同時跑出來，
     * 每一格寬度一起改變 —— 使用者正在看著候選挑字，整列在他眼前重排。
     */
    @Test
    fun `只剩一個讀音時註解也不會跑出來`() {
        val readings = listOf("ni")
        assertTrue("這個情境的前提是消歧欄不畫（門檻 2）", readings.size < 2)
        assertFalse(
            "消歧欄收起來的同時註解跑出來了 —— 整列會在使用者眼前重排",
            CandidateDensity.commentVisible(
                commentShow = true, readings = readings, syllableSideAlive = true,
            ),
        )
    }

    @Test
    fun `帶聲調的拼寫解析不出讀音，註解照畫`() {
        // terra_pinyin 的 spelling_hints 帶調（nǐ），readingOf 要求全部落在 a–z。
        // 那個方案存在的**全部理由**就是「候選標註完整帶調拼音」。
        val cands = listOf(RimeCandidate("你", "nǐ"), RimeCandidate("擬", "nǐ"))
        val readings = T9Syllables.readingsOf(cands)
        assertEquals(emptyList<String>(), readings)
        assertTrue(
            "把 terra_pinyin 的帶調拼音也關掉 = 手機看不到聲調、電腦看得到，" +
                "同一個方案兩端顯示不同資訊",
            CandidateDensity.commentVisible(
                commentShow = true, readings = readings, syllableSideAlive = true,
            ),
        )
    }

    @Test
    fun `simplifier 的字形提示不是讀音，註解照畫`() {
        val cands = listOf(RimeCandidate("简", "〔简〕"))
        assertEquals(emptyList<String>(), T9Syllables.readingsOf(cands))
        assertTrue(
            CandidateDensity.commentVisible(
                true, T9Syllables.readingsOf(cands), syllableSideAlive = true,
            ),
        )
    }

    @Test
    fun `主題自己關掉 comment 時仍然不畫`() {
        assertFalse(
            CandidateDensity.commentVisible(
                commentShow = false, readings = emptyList(), syllableSideAlive = true,
            )
        )
    }

    /* ── ⛔ 消歧欄畫不出來時,不得把註解壓掉 ────────────────────────────
     *
     * 壓掉註解的正當性只有一條:同一份讀音消歧欄已經畫過了。消歧欄那一側
     * 根本不會畫的時候壓掉註解,是**把資訊憑空刪掉** —— 使用者哪裡都看不到
     * 讀音,而畫面完全正常。
     *
     * 三個會發生的狀態(前兩個是缺陷,第三個是刻意的取捨):
     *   (a) 主題寫 `candidates.syllables.placement: none`
     *   (b) `syllableRewriteReady == false`(啟動探針還沒回答,或裝置上是
     *       舊的單編碼 t9_pinyin —— 引擎接不住改寫)
     *   (c) 只有一個相異讀音(消歧欄門檻是 2)—— **保留**,理由見
     *       `只剩一個讀音時註解也不會跑出來`。
     */
    @Test
    fun `主題關掉消歧欄時註解照畫`() {
        assertTrue(
            "placement: none —— 消歧欄一格都不畫,而註解被壓掉了。" +
                "使用者哪裡都看不到讀音",
            CandidateDensity.commentVisible(
                commentShow = true,
                readings = listOf("ni"),
                syllableSideAlive = false,
            ),
        )
    }

    @Test
    fun `引擎改寫不了讀音時註解照畫`() {
        // syllableRewriteReady == false:消歧欄的退化規則(三)讓它整條不出現,
        // 那時候 comment 是**唯一**還畫得出讀音的地方。
        assertTrue(
            CandidateDensity.commentVisible(
                commentShow = true,
                readings = listOf("ni", "nian", "niang"),
                syllableSideAlive = false,
            ),
        )
    }

    @Test
    fun `消歧欄活著而且有讀音時才壓掉註解`() {
        // 這是唯一該壓的那一格 —— 也就是本條規則存在的理由。
        assertFalse(
            CandidateDensity.commentVisible(
                commentShow = true, readings = listOf("ni"), syllableSideAlive = true,
            ),
        )
        // 消歧欄活著但這一輪解析不出讀音(terra_pinyin 的帶調拼寫)→ 照畫。
        assertTrue(
            CandidateDensity.commentVisible(
                commentShow = true, readings = emptyList(), syllableSideAlive = true,
            ),
        )
    }

    /* ═══════════ §10 第 43 條：序號按得到才畫（fail-closed）═══════════ */

    private fun layer(id: String, vararg keysyms: String): LayoutLayer = LayoutLayer(
        id = id,
        label = LocalizedString.EMPTY,
        units = 10f,
        rows = listOf(
            LayoutRow(
                weight = 1f,
                keys = keysyms.map { name ->
                    LayoutKey(
                        id = name, label = name, hint = "", icon = null,
                        labelFrom = LabelSource.NONE, width = 1f, style = "default",
                        spacer = false, active = false, repeat = false,
                        send = SendSpec.Keysym(name, 0, 0),
                        tap = null, doubleTap = null, longPress = null,
                        popup = null, swipe = emptyMap(),
                    )
                },
            )
        ),
    )

    private val NUMROW = layer("numrow", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0")

    @Test
    fun `沒有數字鍵的層不畫序號`() {
        val t9 = layer("t9", "A", "D", "G", "J", "M", "P", "T", "W", "0")
        assertFalse(
            "這一層送得出的只有代表字母與 0 —— 序號 1 2 3 在上面按不到",
            CandidateDensity.layerHasSelectionDigitRow(t9),
        )
    }

    /**
     * **只有一顆數字鍵不算「按得到 1..9」。**
     *
     * `t9-pinyin/t9` 就是這個形狀：`k1` 送得出真的 `1`，`k2`…`k9` 只有 hint。
     * 上一版的判準是「有沒有**任何一顆** send 落在 1..9」，於是那一層被判成
     * 「畫序號」，畫出 1…6 而只有 `1` 送得出去。
     */
    @Test
    fun `只有一顆數字鍵的層不算整排`() {
        val onlyOne = layer("t9", "1", "A", "D", "G", "J", "M", "P", "T", "W")
        assertFalse(
            "一顆數字鍵就判成「按得到 1..9」—— 畫面上會出現 2..6 這幾個按不到的序號",
            CandidateDensity.layerHasSelectionDigitRow(onlyOne),
        )
    }

    /**
     * ⛔ **有整排數字鍵**還不夠，那只是必要條件。
     *
     * `cn-t9-pinyin-numrow` 的數字列是真的（`send.keysym: "3"`），實測按 3 之後
     * 輸入框變成 `3⋯`、原本打好的組字被毀掉 —— 數字被 `recognizer` 的
     * `uppercase` 樣式收走了。所以第二個條件是「這一格量過而且是 yes」。
     */
    @Test
    fun `有整排數字但這一格沒量過就不畫`() {
        SelectionDigits.setForTest(emptyList())
        assertTrue(CandidateDensity.layerHasSelectionDigitRow(NUMROW))
        assertFalse(
            "沒量過的 (佈局, 方案) 竟然畫了序號 —— fail-closed 是「證不出來就不畫」",
            CandidateDensity.selectionDigitUsable(NUMROW, "cn-t9-pinyin-numrow", "t9_pinyin"),
        )
    }

    @Test
    fun `量過而且按得到才畫`() {
        SelectionDigits.setForTest(listOf("cn-qwerty-numrow" to "luna_pinyin_tw"))
        assertTrue(
            CandidateDensity.selectionDigitUsable(NUMROW, "cn-qwerty-numrow", "luna_pinyin_tw"),
        )
        assertFalse(
            "換一個方案就換一個答案 —— 判準不是 per-layout,也不是 per-schema",
            CandidateDensity.selectionDigitUsable(NUMROW, "cn-qwerty-numrow", "t9_pinyin"),
        )
        assertFalse(
            "這一格量到可用,但這一層根本沒有整排數字鍵",
            CandidateDensity.selectionDigitUsable(
                layer("lower", "q", "w", "e"), "cn-qwerty-numrow", "luna_pinyin_tw"
            ),
        )
    }

    /** 方案還沒回報（剛 attach、部署中）時不畫 —— fail-closed。 */
    @Test
    fun `方案還不知道是哪一個時不畫`() {
        SelectionDigits.setForTest(listOf("cn-qwerty-numrow" to "luna_pinyin_tw"))
        assertFalse(CandidateDensity.selectionDigitUsable(NUMROW, "cn-qwerty-numrow", ""))
        assertFalse(CandidateDensity.selectionDigitUsable(NUMROW, null, "luna_pinyin_tw"))
    }

    /**
     * ⚠ **`hint` 不算。** 角落小字不送出任何東西；拿它當判準就會做出
     * 「畫面上有數字、按下去沒有數字」的序號 —— 而那正是本專案點名過七次
     * 的那一類。
     */
    @Test
    fun `只有 hint 沒有 send 的數字不算數`() {
        val hintOnly = LayoutLayer(
            id = "en",
            label = LocalizedString.EMPTY,
            units = 10f,
            rows = listOf(
                LayoutRow(
                    weight = 1f,
                    keys = "1234567890".map { d ->
                        LayoutKey(
                            id = "e_$d", label = "q", hint = d.toString(), icon = null,
                            labelFrom = LabelSource.NONE, width = 1f, style = "default",
                            spacer = false, active = false, repeat = false,
                            send = SendSpec.Keysym("q", 0, 0),
                            tap = null, doubleTap = null, longPress = null,
                            popup = null, swipe = emptyMap(),
                        )
                    },
                )
            ),
        )
        assertFalse(
            "把 hint 當成「按得到數字」—— 那是看得到卻打不出來的那一類",
            CandidateDensity.layerHasSelectionDigitRow(hintOnly),
        )
    }

    /**
     * **`popup` 裡的數字不算，`swipe` 更不算。**
     *
     * `qwerty/lower` 的 `q..p` 有 `swipe.up` 與 `popup` 都送得出 1..0。
     * `swipe` 在本端**根本沒有實作**（`KeyboardView` 從頭到尾沒有讀 `key.swipe`），
     * 照它判就是做出一個「上滑卻沒反應」的序號；`popup` 是活的，但序號 `3`
     * 承諾的是「按 3」，不是「長按某顆鍵再從盤裡挑 3」。
     */
    @Test
    fun `popup 與 swipe 裡的數字不算數`() {
        val sub = SubKey(label = "1", hint = "", style = null, send = SendSpec.Keysym("1", 0, 0), tap = null)
        val viaPopup = LayoutLayer(
            id = "lower",
            label = LocalizedString.EMPTY,
            units = 10f,
            rows = listOf(
                LayoutRow(
                    weight = 1f,
                    keys = "123456789".map { d ->
                        LayoutKey(
                            id = "k_$d", label = "q", hint = d.toString(), icon = null,
                            labelFrom = LabelSource.NONE, width = 1f, style = "default",
                            spacer = false, active = false, repeat = false,
                            send = SendSpec.Keysym("q", 0, 0),
                            tap = null, doubleTap = null, longPress = null,
                            popup = Popup(PopupLayout.GRID, 1, listOf(sub)),
                            swipe = mapOf(SwipeDirection.UP to sub),
                        )
                    },
                )
            ),
        )
        assertFalse(
            "popup／swipe 也算成「按得到數字」—— swipe 在本端根本沒有實作",
            CandidateDensity.layerHasSelectionDigitRow(viaPopup),
        )
    }

    @Test
    fun `佈局還沒載進來時不畫序號`() {
        assertFalse(CandidateDensity.layerHasSelectionDigitRow(null))
        assertFalse(CandidateDensity.selectionDigitUsable(null, "x", "y"))
    }

    /* ═══════════ §10 第 45 條：右端最多一顆 ═══════════ */

    @Test
    fun `本頁還有畫不出來的候選時右端是展開鍵，不是翻頁`() {
        val end = CandidateDensity.rightEnd(
            visible = 6, pageCandidateCount = 9, expandAvailable = true,
            panelOpen = false, pagerDrawable = true, morePages = true,
        )
        assertEquals(
            "看得到 6 個、其實有 9 個，右端卻給了翻頁 —— 按下去就是跳過 3 個沒看過的候選",
            CandidateDensity.RightEnd.EXPAND,
            end,
        )
    }

    @Test
    fun `本頁全部畫得出來時右端才是翻頁`() {
        assertEquals(
            CandidateDensity.RightEnd.PAGER,
            CandidateDensity.rightEnd(
                visible = 6, pageCandidateCount = 4, expandAvailable = true,
                panelOpen = false, pagerDrawable = true, morePages = true,
            ),
        )
        assertEquals(
            CandidateDensity.RightEnd.PAGER,
            CandidateDensity.rightEnd(
                visible = 9, pageCandidateCount = 9, expandAvailable = true,
                panelOpen = false, pagerDrawable = true, morePages = true,
            ),
        )
    }

    @Test
    fun `沒有候選時右端一顆都不畫`() {
        assertEquals(
            CandidateDensity.RightEnd.NONE,
            CandidateDensity.rightEnd(
                visible = 0, pageCandidateCount = 0, expandAvailable = true,
                panelOpen = false, pagerDrawable = true, morePages = false,
            ),
        )
    }

    /**
     * 面板開著的時候那一顆**必須**是收合鍵。
     *
     * 翻到一頁候選比較少的頁面之後 `visible >= 本頁數`，照上面那條就會換成
     * 翻頁鍵 —— 而展開面板自己沒有關閉鍵，於是一片蓋住鍵盤的浮層留在畫面上
     * 而沒有出口。這是「每個面板的出口必須結構性存在」那條規矩。
     */
    @Test
    fun `面板開著時右端一定是收合鍵`() {
        assertEquals(
            CandidateDensity.RightEnd.EXPAND,
            CandidateDensity.rightEnd(
                visible = 6, pageCandidateCount = 3, expandAvailable = true, panelOpen = true,
                pagerDrawable = true, morePages = true,
            ),
        )
    }

    /**
     * **兩顆同時出現是表達不出來的。**
     *
     * 這一條看起來像廢話，它守的是把 `RightEnd` 換回兩個 boolean 的那一天：
     * 兩個 boolean 允許「都是 true」，而那正是改動前的現況。
     */
    @Test
    fun `右端只有三種狀態`() {
        assertEquals(3, CandidateDensity.RightEnd.values().size)
    }

    /* ═══════════ §10 第 45 條：量測扣掉的 == 畫出來的（單一真相）═══════════ */

    /**
     * `bar.reserved_end` 只在**真的畫得出東西**時才付出去。
     */
    @Test
    fun `一顆都不畫時右端不佔寬度`() {
        assertEquals(0f, CandidateDensity.reservedDp(40f, 40f, 0), 0.001f)
        assertEquals(40f, CandidateDensity.reservedDp(40f, 40f, 1), 0.001f)
        assertEquals(80f, CandidateDensity.reservedDp(40f, 40f, 2), 0.001f)
    }

    /**
     * ⛔ **量測扣掉的寬度必須就是實際會畫出來的那幾顆。**
     *
     * 上一版把它拆成 `reservedForMeasure()`（量測）與 `rightEnd()` ＋
     * `Pager.state()`（繪製）兩半，而兩半在 11 種頁況裡有 **5 種**對不上，
     * 全部是「量測扣得比畫出來的多 40 dp」。這一條把 5 種全部釘住：
     * 每一格都同時斷言「右端畫什麼」與「量測扣多少」，任何一邊漂掉都會紅。
     *
     * 實測對照（emulator-5558，1080×2400 @420dpi，主題 default-light）：
     * 一格 56 dp、節距 60 dp、`reserved_end` 40、按鍵 40。
     */
    @Test
    fun `量測扣掉的寬度就是畫出來的那幾顆`() {
        fun layout(
            pageNo: Int,
            isLastPage: Boolean,
            pageCount: Int,
            expandAvailable: Boolean = true,
            panelOpen: Boolean = false,
            pagerShow: Boolean = true,
        ) = CandidateDensity.barLayout(
            screenWidthDp = 411.43f,
            barPaddingH = BAR_PADDING_H,
            reservedEnd = RESERVED_END,
            buttonDp = 40f,
            leadingDp = 0f,
            widths = List(pageCount) { 56f },
            spacing = SPACING,
            pageCandidateCount = pageCount,
            pageNo = pageNo,
            isLastPage = isLastPage,
            pagerKind = PageIndicatorKind.ARROWS,
            pagerShow = pagerShow,
            expandAvailable = expandAvailable,
            panelOpen = panelOpen,
        )

        // ── 上一版對不上的五格 ────────────────────────────────────────────
        // 1) 第 1 頁又是最後一頁：翻頁整組不畫（兩顆都是死的）→ 一顆都不畫。
        //    上一版量測扣 40、實際畫 0。
        val onlyPage = layout(pageNo = 0, isLastPage = true, pageCount = 4)
        // ⚠ 這一格上一版回的是 `PAGER` 而 `Pager.State.show == false`(畫出來
        //   什麼都沒有)。現在回的是 `NONE`:右端該畫什麼的判準是**真的會被
        //   畫出來的那一顆**,「回一個畫不出來的答案」是這一版消滅掉的形狀。
        //   後面沒有候選、本頁也看得完 —— 什麼都不畫是**正確**的,不是死路。
        assertEquals(CandidateDensity.RightEnd.NONE, onlyPage.rightEnd)
        assertEquals("翻頁整組不畫,右端就不該佔寬度", 0f, onlyPage.reservedDp, 0.001f)
        assertNull("右端不畫翻頁時不該還交出一份 Pager.State", onlyPage.pager)
        assertFalse(
            "沒有更多候選,右端空著不是死路",
            CandidateDensity.deadEnd(
                onlyPage, pageCandidateCount = 4, morePages = false,
                panelPagerDrawable = CandidateDensity.panelPagerDrawable(0, true, 4),
            ),
        )

        // 2) 第 2 頁、本頁看不完 → 右端是展開鍵**一顆**。上一版量測扣 80。
        val p2Expand = layout(pageNo = 1, isLastPage = false, pageCount = 9)
        assertEquals(CandidateDensity.RightEnd.EXPAND, p2Expand.rightEnd)
        assertEquals(40f, p2Expand.reservedDp, 0.001f)

        // 3) 第 2 頁又是最後一頁、本頁看不完 → 一樣是展開鍵一顆。
        val p2Last = layout(pageNo = 1, isLastPage = true, pageCount = 9)
        assertEquals(CandidateDensity.RightEnd.EXPAND, p2Last.rightEnd)
        assertEquals(40f, p2Last.reservedDp, 0.001f)

        // 4) 第 2 頁又是最後一頁、本頁看得完 → 只有「上一頁」一顆。
        val p2LastFew = layout(pageNo = 1, isLastPage = true, pageCount = 3)
        assertEquals(CandidateDensity.RightEnd.PAGER, p2LastFew.rightEnd)
        assertEquals(40f, p2LastFew.reservedDp, 0.001f)
        assertEquals(true, p2LastFew.pager!!.prevEnabled)
        assertEquals(false, p2LastFew.pager!!.nextEnabled)

        // 5) 面板開著、第 2 頁 → 右端必定是收合鍵一顆。上一版量測扣 80。
        val panel = layout(pageNo = 1, isLastPage = false, pageCount = 9, panelOpen = true)
        assertEquals(CandidateDensity.RightEnd.EXPAND, panel.rightEnd)
        assertEquals(40f, panel.reservedDp, 0.001f)

        // ── 本來就對得上的那幾格,一併釘住 ────────────────────────────────
        val p1Expand = layout(pageNo = 0, isLastPage = false, pageCount = 9)
        assertEquals(CandidateDensity.RightEnd.EXPAND, p1Expand.rightEnd)
        assertEquals(40f, p1Expand.reservedDp, 0.001f)
        assertEquals("411 dp 上扣一顆之後畫得出 6 個", 6, p1Expand.visible)

        val p2Both = layout(pageNo = 1, isLastPage = false, pageCount = 3)
        assertEquals(CandidateDensity.RightEnd.PAGER, p2Both.rightEnd)
        assertEquals("上一頁＋下一頁兩顆", 80f, p2Both.reservedDp, 0.001f)

        val none = layout(pageNo = 0, isLastPage = false, pageCount = 0)
        assertEquals(CandidateDensity.RightEnd.NONE, none.rightEnd)
        assertEquals(0f, none.reservedDp, 0.001f)

        // ⛔ 主題關掉翻頁指示器,而本頁排得下、後面還有頁。
        //    上一版在這裡回 `PAGER` ＋ 0 dp —— 而 `PageArrows` 在 `show == false`
        //    時直接 return,於是**右端整片空白、使用者鎖死在第 1 頁**,
        //    畫面完全正常、沒有任何東西會叫。出口必須換成展開鍵。
        val noPager = layout(pageNo = 1, isLastPage = false, pageCount = 3, pagerShow = false)
        assertEquals(CandidateDensity.RightEnd.EXPAND, noPager.rightEnd)
        assertEquals(40f, noPager.reservedDp, 0.001f)
        assertFalse(
            CandidateDensity.deadEnd(
                noPager, pageCandidateCount = 3, morePages = true,
                panelPagerDrawable = CandidateDensity.panelPagerDrawable(1, false, 3),
            ),
        )

        // ⛔ 兩條出口同時關掉 = 死路。這不是一種要優雅處理的頁況,是設計錯誤,
        //    而 `deadEnd()` 就是讓建置期看得見它的那一支。
        val trapped = layout(
            pageNo = 0, isLastPage = false, pageCount = 3,
            expandAvailable = false, pagerShow = false,
        )
        assertEquals(CandidateDensity.RightEnd.NONE, trapped.rightEnd)
        assertTrue(
            "翻頁與展開都畫不出來而後面還有候選 —— 這就是死路,deadEnd() 沒抓到",
            CandidateDensity.deadEnd(
                trapped, pageCandidateCount = 3, morePages = true,
                panelPagerDrawable = CandidateDensity.panelPagerDrawable(0, false, 3),
            ),
        )

        // 翻頁關掉、展開留著,而本頁**看不完** —— 出口一樣是展開鍵(這一格
        // 上一版就是對的,一併釘住,免得修法把它一起換掉)。
        val noPagerLong = layout(pageNo = 0, isLastPage = false, pageCount = 9, pagerShow = false)
        assertEquals(CandidateDensity.RightEnd.EXPAND, noPagerLong.rightEnd)
        assertEquals(40f, noPagerLong.reservedDp, 0.001f)

        // 展開關掉、翻頁留著,本頁看不完 —— 退回翻頁(刻意的取捨:鎖死在
        // 第 1 頁比跳過幾個候選更糟)。這一格在隨附主題上不該出現。
        val noExpand = layout(pageNo = 0, isLastPage = false, pageCount = 9, expandAvailable = false)
        assertEquals(CandidateDensity.RightEnd.PAGER, noExpand.rightEnd)
        assertEquals(40f, noExpand.reservedDp, 0.001f)
    }

    /**
     * ⛔ 本頁還有畫不出來的候選時，**不得**出現翻頁鍵 —— 這一條在
     * 「翻頁那一組剛好一顆都不畫」時最容易漏掉：翻頁扣 0 dp，候選拿到最多的
     * 寬度，於是更容易「看得完」。這一格要的是**真的看得完**才給翻頁。
     */
    @Test
    fun `扣掉翻頁那幾顆之後仍然看得完才給翻頁`() {
        fun at(pageCount: Int, isLastPage: Boolean = true) = CandidateDensity.barLayout(
            screenWidthDp = 411.43f, barPaddingH = BAR_PADDING_H, reservedEnd = RESERVED_END,
            buttonDp = 40f, leadingDp = 0f,
            widths = List(pageCount) { 56f }, spacing = SPACING,
            pageCandidateCount = pageCount, pageNo = 0, isLastPage = isLastPage,
            pagerKind = PageIndicatorKind.ARROWS, pagerShow = true,
            expandAvailable = true, panelOpen = false,
        )
        // 扣 0 dp 時 411.43 − 8 = 403.43 dp,節距 60 → 畫得出 6 個。
        assertEquals(6, at(6).visible)
        // 第 1 頁又是最後一頁:翻頁那一組**一顆都畫不出來**,而且後面也真的
        // 沒有東西了 → 右端 `NONE`。上一版在這裡回 `PAGER` ＋ `show=false`,
        // 畫出來的結果一樣,但那是一個「畫不出來的答案」(見 rightEnd 的 KDoc)。
        assertEquals(CandidateDensity.RightEnd.NONE, at(6).rightEnd)
        assertFalse(
            CandidateDensity.deadEnd(
                at(6), 6, morePages = false,
                panelPagerDrawable = CandidateDensity.panelPagerDrawable(0, true, 6),
            ),
        )
        // 7 個就看不完了 → 出口是展開面板,不是翻頁。
        assertEquals(CandidateDensity.RightEnd.EXPAND, at(7).rightEnd)

        // 同一組數字,但後面**真的**還有頁 → 翻頁鍵畫得出來,右端才是翻頁。
        // 這一格是本測試名字講的那件事:扣掉那一顆(40 dp)之後仍然看得完。
        assertEquals(6, at(6, isLastPage = false).visible)
        assertEquals(CandidateDensity.RightEnd.PAGER, at(6, isLastPage = false).rightEnd)
        assertEquals(CandidateDensity.RightEnd.EXPAND, at(7, isLastPage = false).rightEnd)
    }

    /**
     * ⛔ **主題關掉 `page_indicator` 之後,右端不得變成空白。**
     *
     * 上一版的 [CandidateDensity.rightEnd] 在 `visible >= pageCandidateCount`
     * 時**無條件**回 `PAGER`,完全不看翻頁鍵畫不畫得出來;而 `PageArrows` 在
     * `state.show == false` 時直接 return,展開鍵又只掛在 `EXPAND` 那一支。
     * 於是「本頁排得下、後面還有頁」＋ `page_indicator.show: false`
     * ＝ **右端空白、使用者鎖死在第 1 頁**,而畫面完全正常。
     *
     * 隨附主題不會踩到(`ThemeDensityTest` 逐份守著),第三方主題踩到
     * 沒有任何東西會叫 —— 所以判準改成「這一顆**真的會被畫出來**」。
     */
    @Test
    fun `翻頁鍵畫不出來時右端換成展開鍵`() {
        fun at(pagerShow: Boolean, expandAvailable: Boolean) = CandidateDensity.barLayout(
            screenWidthDp = 411.43f, barPaddingH = BAR_PADDING_H, reservedEnd = RESERVED_END,
            buttonDp = 40f, leadingDp = 0f,
            widths = List(3) { 56f }, spacing = SPACING,
            pageCandidateCount = 3,          // 本頁 3 個,411 dp 上排得下 6 個
            pageNo = 0, isLastPage = false,  // 後面還有頁
            pagerKind = PageIndicatorKind.ARROWS, pagerShow = pagerShow,
            expandAvailable = expandAvailable, panelOpen = false,
        )

        val normal = at(pagerShow = true, expandAvailable = true)
        assertEquals("翻頁畫得出來時右端就是翻頁", CandidateDensity.RightEnd.PAGER, normal.rightEnd)

        val trapped = at(pagerShow = false, expandAvailable = true)
        assertEquals(
            "翻頁鍵畫不出來而後面還有頁 —— 右端給了一顆畫不出來的翻頁鍵,使用者鎖死在第 1 頁",
            CandidateDensity.RightEnd.EXPAND,
            trapped.rightEnd,
        )
        assertEquals("展開鍵一顆 = 40 dp,量測要扣掉它", 40f, trapped.reservedDp, 0.001f)
        // ⛔ 這一格正是「死路搬進面板裡」的那一格:右端是 `∨`,而面板的翻頁列
        //    從前吃同一份 `page_indicator.show`(這裡是 false)—— 面板打得開、
        //    底下一顆翻頁鍵都沒有、第 2 頁永遠進不去,而 deadEnd() 說沒事。
        //    現在面板的翻頁列與它脫鉤([Pager.panelState]),這條路才真的通。
        assertTrue(
            "面板的翻頁列不得被候選列的 page_indicator 關掉",
            CandidateDensity.panelPagerDrawable(0, isLastPage = false, shownCount = 3),
        )
        assertFalse(
            CandidateDensity.deadEnd(
                trapped, pageCandidateCount = 3, morePages = true,
                panelPagerDrawable = CandidateDensity.panelPagerDrawable(0, false, 3),
            ),
        )
    }

    /**
     * ⛔ **兩條出口都畫不出來是設計錯誤,而且必須看得見。**
     *
     * §10 第 41 條上一版只掃 `scroll: expandable` 與 `expand_button.show`,
     * **沒有看 `page_indicator`** —— 於是「翻頁關掉、展開也關掉」那一半
     * 在執行期是一片空白的右端,而檢核清單一句話都沒說。
     */
    @Test
    fun `兩條出口都關掉就是死路`() {
        fun at(pageCount: Int, pagerShow: Boolean, expandAvailable: Boolean, isLastPage: Boolean) =
            CandidateDensity.barLayout(
                screenWidthDp = 411.43f, barPaddingH = BAR_PADDING_H, reservedEnd = RESERVED_END,
                buttonDp = 40f, leadingDp = 0f,
                widths = List(pageCount) { 56f }, spacing = SPACING,
                pageCandidateCount = pageCount, pageNo = 0, isLastPage = isLastPage,
                pagerKind = PageIndicatorKind.ARROWS, pagerShow = pagerShow,
                expandAvailable = expandAvailable, panelOpen = false,
            )

        // 本頁看得完、後面還有頁,兩條出口都關掉。
        val a = at(3, pagerShow = false, expandAvailable = false, isLastPage = false)
        assertTrue(
            CandidateDensity.deadEnd(
                a, 3, morePages = true,
                panelPagerDrawable = CandidateDensity.panelPagerDrawable(0, false, 3),
            ),
        )

        // 本頁**看不完**,兩條出口都關掉 —— 同一個死路的另一種形狀。
        val b = at(9, pagerShow = false, expandAvailable = false, isLastPage = true)
        assertEquals(CandidateDensity.RightEnd.NONE, b.rightEnd)
        assertTrue(
            CandidateDensity.deadEnd(
                b, 9, morePages = false,
                panelPagerDrawable = CandidateDensity.panelPagerDrawable(0, true, 9),
            ),
        )

        // 只要留一條就不是死路。
        assertFalse(
            CandidateDensity.deadEnd(
                at(9, pagerShow = false, expandAvailable = true, isLastPage = true), 9, false,
                panelPagerDrawable = CandidateDensity.panelPagerDrawable(0, true, 9),
            ),
        )
        assertFalse(
            CandidateDensity.deadEnd(
                at(3, pagerShow = true, expandAvailable = false, isLastPage = false), 3, true,
                panelPagerDrawable = CandidateDensity.panelPagerDrawable(0, false, 3),
            ),
        )
        // 沒有候選 = 候選列現在畫的是工具列,右端空著本來就對。
        assertFalse(
            CandidateDensity.deadEnd(
                at(0, pagerShow = false, expandAvailable = false, isLastPage = true), 0, false,
                panelPagerDrawable = CandidateDensity.panelPagerDrawable(0, true, 0),
            ),
        )
    }

    /**
     * **行內組字串也是真的擠掉候選的。**
     *
     * 實測（411 dp、default-light、`luna_pinyin_tw` 打 `ni`）：組字串那一塊
     * 33.7 dp，模型沒扣它時說得下 7 個、畫面只畫得出 6 個。方向是**估寬**，
     * 而 [CandidateDensity.rightEnd] 正是拿這個數決定「本頁看完了沒」——
     * 高估一個就等於給出一顆讓使用者跳過他沒看過的候選的翻頁鍵。
     */
    @Test
    fun `行內組字串要先扣掉`() {
        val widths = List(9) { 48f }   // 一字候選被 min_width 撐到 48，節距 52
        fun v(leading: Float) = CandidateDensity.barLayout(
            screenWidthDp = 411.43f, barPaddingH = BAR_PADDING_H, reservedEnd = RESERVED_END,
            buttonDp = 40f, leadingDp = leading,
            widths = widths, spacing = SPACING, pageCandidateCount = 9,
            pageNo = 0, isLastPage = false,
            pagerKind = PageIndicatorKind.ARROWS, pagerShow = true,
            expandAvailable = true, panelOpen = false,
        ).visible
        assertEquals("沒有組字串時 411 − 8 − 40 = 363 dp,節距 52 → 7 個", 7, v(0f))
        assertEquals("`ni` 的組字串實測 33.7 dp,扣掉之後就是畫面上的 6 個", 6, v(33.7f))

        // 估寬公式本身：`ni` 兩個拉丁字元 ＋ 左右 10 dp 內距。
        val dp = CandidateDensity.inlinePreeditDp("ni", 14f, 10f)
        assertTrue("估出來的組字串寬度 $dp dp 不在實測的 33.7 dp 附近", dp in 25f..45f)
        assertEquals("沒有組字串時不扣", 0f, CandidateDensity.inlinePreeditDp(null, 14f, 10f), 0.001f)
    }

    /* ═══════════ §10 第 44 條（§8.6.4.3）：高亮不得改變量測寬度 ═══════════ */

    private fun item(style: HighlightStyle) = ItemStyle(
        paddingH = 8f, paddingV = 6f, spacing = 4f, cornerRadius = 6f, minWidth = 48f,
        background = 0x00000000, highlightBackground = 0xFF3060C0.toInt(),
        highlightStyle = style,
        borderWidth = 0f, borderColor = 0x00000000,
        highlightBorderWidth = 0f, highlightBorderColor = 0x00000000,
    )

    private val text = TextStyle(size = 20f, color = 0xFF000000.toInt(), highlightColor = 0xFFFFFFFF.toInt())
    private val label = LabelStyle(true, "{label}", 12f, 0xFF808080.toInt(), 0xFFFFFFFF.toInt())
    private val comment = CommentStyle(true, CommentPosition.AFTER, 12f, 0xFF808080.toInt(), 0xFFFFFFFF.toInt())

    @Test
    fun `三種高亮畫法的量測寬度完全相同`() {
        fun w(style: HighlightStyle): Float {
            val i = item(style)
            return CandidateDensity.itemWidthDp(
                textChars = 2, textSize = text.size,
                labelChars = 0, labelSize = label.size,
                commentChars = 0, commentSize = comment.size,
                paddingH = i.paddingH, minWidth = i.minWidth,
            )
        }
        val fill = w(HighlightStyle.FILL)
        assertFalse(
            "高亮改變了該格的寬度 —— 使用者每移動一次選字，整列就在他眼前重排一次",
            CandidateDensity.highlightChangesWidth(fill, w(HighlightStyle.UNDERLINE)),
        )
        assertFalse(
            CandidateDensity.highlightChangesWidth(fill, w(HighlightStyle.OUTLINE)),
        )
    }

    /**
     * **非 `fill` 的畫法不准用 `text.highlight_color`。**
     *
     * 那個顏色在隨附主題裡是 `$on_accent`（白），它是設計來畫在重點色實心塊上的。
     * 底色換回 surface 之後，白字畫在白底上就是**看不見** —— 高亮的那一個候選
     * 整個消失，而畫面「看起來」一切正常。
     */
    @Test
    fun `underline 的高亮字不是 on_accent`() {
        val ink = CandidateInk.of(item(HighlightStyle.UNDERLINE), text, label, comment, true)
        assertNotEquals(
            "underline 用了 on_accent 當前景 —— 白底白字，高亮的候選會消失",
            text.highlightColor,
            ink.text,
        )
        assertEquals(0xFF3060C0.toInt(), ink.text)
        assertEquals("底線用重點色", 0xFF3060C0.toInt(), ink.underline)
        assertEquals("underline 不鋪底色", item(HighlightStyle.UNDERLINE).background, ink.background)
    }

    @Test
    fun `fill 仍然是實心塊`() {
        val ink = CandidateInk.of(item(HighlightStyle.FILL), text, label, comment, true)
        assertEquals(0xFF3060C0.toInt(), ink.background)
        assertEquals(text.highlightColor, ink.text)
        assertNull("fill 不該再多畫一條底線", ink.underline)
    }

    /**
     * `outline` 在 `highlight_border_width` 是 0 的主題上**必須**退回一條看得見的
     * 邊，不能靜靜地變成「沒有高亮」。
     */
    @Test
    fun `outline 在沒有描邊寬度時不會變成沒有高亮`() {
        val ink = CandidateInk.of(item(HighlightStyle.OUTLINE), text, label, comment, true)
        assertTrue("outline 什麼都沒畫出來", ink.borderWidth > 0f)
        assertEquals(0xFF3060C0.toInt(), ink.borderColor)
    }

    @Test
    fun `沒有高亮的那幾格用的是原本的顏色`() {
        val ink = CandidateInk.of(item(HighlightStyle.UNDERLINE), text, label, comment, false)
        assertEquals(text.color, ink.text)
        assertEquals(label.color, ink.label)
        assertEquals(comment.color, ink.comment)
        assertNull(ink.underline)
    }
}
