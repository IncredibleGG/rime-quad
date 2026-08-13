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
import org.luminakey.ime.theme.SendSpec
import org.luminakey.ime.theme.TextStyle

/**
 * 候選列的密度與資訊架構（規範草稿 §8.6.1.1 / §8.6.3.1 / §8.6.4.2 /
 * §8.6.4.3 / §8.6.6.4，以及 §10 的第 40、42、43、44 條）。
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
            CandidateDensity.commentVisible(commentShow = true, readings = readings),
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
            CandidateDensity.commentVisible(commentShow = true, readings = readings),
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
            CandidateDensity.commentVisible(commentShow = true, readings = readings),
        )
    }

    @Test
    fun `simplifier 的字形提示不是讀音，註解照畫`() {
        val cands = listOf(RimeCandidate("简", "〔简〕"))
        assertEquals(emptyList<String>(), T9Syllables.readingsOf(cands))
        assertTrue(
            CandidateDensity.commentVisible(true, T9Syllables.readingsOf(cands)),
        )
    }

    @Test
    fun `主題自己關掉 comment 時仍然不畫`() {
        assertFalse(CandidateDensity.commentVisible(commentShow = false, readings = emptyList()))
    }

    /* ═══════════ §10 第 43 條：序號按得到才畫 ═══════════ */

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

    @Test
    fun `沒有數字鍵的層不畫序號`() {
        val t9 = layer("t9", "A", "D", "G", "J", "M", "P", "T", "W", "0")
        assertFalse(
            "這一層送得出的只有代表字母與 0 —— 序號 1 2 3 在上面按不到",
            CandidateDensity.labelVisible(true, CandidateDensity.layerSendsSelectionDigit(t9)),
        )
    }

    @Test
    fun `有數字列的層畫序號`() {
        val numrow = layer("t9", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0")
        assertTrue(
            CandidateDensity.labelVisible(true, CandidateDensity.layerSendsSelectionDigit(numrow)),
        )
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
                    keys = listOf(
                        LayoutKey(
                            id = "e_q", label = "q", hint = "1", icon = null,
                            labelFrom = LabelSource.NONE, width = 1f, style = "default",
                            spacer = false, active = false, repeat = false,
                            send = SendSpec.Keysym("q", 0, 0),
                            tap = null, doubleTap = null, longPress = null,
                            popup = null, swipe = emptyMap(),
                        )
                    ),
                )
            ),
        )
        assertFalse(
            "把 hint 當成「按得到數字」—— 那是看得到卻打不出來的那一類",
            CandidateDensity.layerSendsSelectionDigit(hintOnly),
        )
    }

    @Test
    fun `佈局還沒載進來時不畫序號`() {
        assertFalse(CandidateDensity.layerSendsSelectionDigit(null))
    }

    /* ═══════════ §10 第 44 條：右端最多一顆 ═══════════ */

    @Test
    fun `本頁還有畫不出來的候選時右端是展開鍵，不是翻頁`() {
        val end = CandidateDensity.rightEnd(
            visible = 6, pageCandidateCount = 9, expandAvailable = true
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
            CandidateDensity.rightEnd(visible = 6, pageCandidateCount = 4, expandAvailable = true),
        )
        assertEquals(
            CandidateDensity.RightEnd.PAGER,
            CandidateDensity.rightEnd(visible = 9, pageCandidateCount = 9, expandAvailable = true),
        )
    }

    @Test
    fun `沒有候選時右端一顆都不畫`() {
        assertEquals(
            CandidateDensity.RightEnd.NONE,
            CandidateDensity.rightEnd(visible = 0, pageCandidateCount = 0, expandAvailable = true),
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
                visible = 6, pageCandidateCount = 3, expandAvailable = true, panelOpen = true
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

    /** 第 2 頁以後翻頁會畫兩顆，量測時就要先扣掉 —— 否則最後一個會被切掉。 */
    @Test
    fun `第二頁的保留區要多算一顆`() {
        assertEquals(
            40f,
            CandidateDensity.reservedForMeasure(40f, 40f, pageNo = 0, pageIndicatorShown = true),
            0.001f,
        )
        assertEquals(
            80f,
            CandidateDensity.reservedForMeasure(40f, 40f, pageNo = 1, pageIndicatorShown = true),
            0.001f,
        )
        assertEquals(
            "主題關掉翻頁指示器時不該白留一顆的寬度",
            40f,
            CandidateDensity.reservedForMeasure(40f, 40f, pageNo = 3, pageIndicatorShown = false),
            0.001f,
        )
    }

    /* ═══════════ §8.6.4.3：高亮不得改變量測寬度 ═══════════ */

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
