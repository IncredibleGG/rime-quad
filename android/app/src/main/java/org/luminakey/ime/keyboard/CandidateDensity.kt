package org.luminakey.ime.keyboard

import org.luminakey.ime.theme.CommentStyle
import org.luminakey.ime.theme.HighlightStyle
import org.luminakey.ime.theme.ItemStyle
import org.luminakey.ime.theme.LabelStyle
import org.luminakey.ime.theme.LayoutLayer
import org.luminakey.ime.theme.SendSpec
import org.luminakey.ime.theme.TextStyle

/**
 * 候選列的**密度與資訊架構** —— 純邏輯那一半。
 *
 * ── 起因：使用者拿三星比對，我們一列只排得下 3 個 ────────────────────────
 * 實測（emulator-5558，1080×2400 @420dpi = 411.43 dp 寬，主題 `intl-ios-light`，
 * 佈局 `cn-t9-pinyin`）：候選列一格 **108.95 dp**，一列畫得出 **3 個**。
 * 同一台機器上語燕是 6 個、一格 59.5 dp。差的不是螢幕，是我們自己往一格裡
 * 塞的東西：
 *
 * | 成分 | 一格佔多少 | 單獨拿掉的結果 |
 * |---|---:|---|
 * | 候選旁的註解 `comment`（`ni hao`） | **38.48 dp = 35.3%** | **3 → 4 完整 ＋ 第 5 個露一半** |
 * | 序號 `1 2 3` | 9.90 dp = 9.1% | 仍是 3 |
 * | `item.padding_h` 10→6 | 7.62 dp = 7.0% | 仍是 3 |
 * | `item.spacing` 4→2 | 每個間隔 2 dp | 完全無效 |
 * | **高亮的實心色塊** | **0 dp** | **完全無效**（墨跡座標逐 px 相同） |
 *
 * 兩個結論寫在這裡，因為它們違反直覺：
 *
 *  1. **那個大藍塊一個像素的寬度都不佔。** 它不是密度的成因，它是「每格
 *     10 dp 內距」看起來合理的那個**理由**。
 *  2. **序號＋內距＋間距三項全做只省 19.5 dp/格，結果仍然是 3 個。**
 *     這一輪裡只有註解那一項單獨改得動結果。
 *
 * ── 為什麼註解是多餘的：它與消歧欄同源 ──────────────────────────────────
 * 消歧欄（[T9Syllables]）的內容是 [T9Syllables.readingOf] 從候選的 `comment`
 * 反推的；註解畫的就是 `comment` 原文。差別只有「取第一個音節、去重」與
 * 「印整串」。**同一份讀音畫兩次，而只有註解要付寬度。**
 *
 * 而且今天全 repo 只有 `t9_pinyin`（`spelling_hints: 50`）與 `terra_pinyin`
 * （`spelling_hints: 5`）給得出非空的 `comment`，`luna_pinyin` / `bopomofo` /
 * `stroke` 一律是空的。`t9_pinyin` 的 `for_schema` 只綁三份九宮格佈局 ——
 * 也就是註解**只在九宮格上非空，而九宮格正是唯一有消歧欄的地方**。
 *
 * 所以規則寫成一句話：**能被當成讀音的 `comment`，是消歧欄的內容，
 * 不是註解的內容**（[commentVisible]）。`terra_pinyin` 的 `nǐ` 帶聲調、
 * `simplifier` 的「〔简〕」都解析不出讀音，照顯示 —— 那些本來就不是讀音。
 *
 * ── 為什麼這些判斷非得是純函式不可 ──────────────────────────────────────
 * 「一列排得下幾個」「這一格該不該畫註解」在 Compose 裡只驗得到截圖，而截圖
 * 驗不到「360 dp 的機器上會不會掉到 4 個」。這個專案已經吃過七次「單元測試
 * 綠、使用者打開看不到」的虧，所以判斷全部搬到這裡，由
 * [org.luminakey.ime.keyboard.CandidateDensityTest] 逐條驗。
 */
object CandidateDensity {

    /**
     * §10 第 40 條基準情境的字級。
     *
     * 密度下界**不用主題自己的 `text.size` 去算**：那會變成「字調大一點就
     * 自動合格」。基準情境固定 20 sp、兩字 CJK、無序號無註解，量的是主題往
     * 一格裡加的**固定開銷**（內距、間距、最小寬、右端保留區）。
     */
    const val BASELINE_TEXT_SIZE = 20f

    /** 基準情境的候選字數：兩字詞（「你好」）。 */
    const val BASELINE_TEXT_CHARS = 2

    /**
     * 序號／註解與候選字之間的間隙。
     *
     * ⚠ 規範 §8.6.1 有 `label_gap` / `comment_gap` 兩個欄位，**本端沒有解析**，
     * 渲染端寫死 `3.dp`（`KeyboardView.kt`）。量測公式與渲染端必須用同一個數，
     * 否則「算得下」與「畫得下」會分家 —— 展開面板就踩過一次（估寬把 comment
     * 算進去而面板根本不畫 comment，於是一列少一欄）。
     * 這個常數就是那唯一的一份；規範補齊欄位之後改成讀主題。
     */
    const val GAP_DP = 3f

    /**
     * 拉丁字元相對於字級的寬度。
     *
     * CJK 字面寬 = 1 em 是硬的（實測「你好」20 sp 墨跡 104 px @2.625 = 39.6 dp
     * ≈ 2 × 20）。拉丁不是：實測註解 `ni hao` 12 sp 六個字 35.5 dp = 0.49 em、
     * 序號 `1` 12 sp 6.9 dp = 0.575 em。取 **0.55** ——
     * **刻意取偏寬的那一邊**：估寬了只是少排一個候選，估窄了會切字。
     */
    const val LATIN_EM = 0.55f

    /**
     * 候選列右端每一顆控制鍵的寬度。
     *
     * ⚠ 與 `KeyboardView.CANDIDATE_BAR_BUTTON_DP` 是**同一個數**，
     * `scripts/lib/candbar_geom.py` 讀的是那一份。這裡不另立一份常數，
     * 見 [rightEnd] 的參數 `buttonDp`。
     */

    /* ═══════════════════ 一格有多寬、一列排得下幾個 ═══════════════════ */

    /**
     * 一格候選的量測寬度（dp），規範 §8.6.4.1 第 3 步。
     *
     * ⚠ **這個函式回的數必須與渲染端真的畫出來的一樣。** 兩者一旦分家，
     * 症狀是「畫面莫名其妙少一格」而沒有任何東西會叫。
     *
     * @param textChars 候選文字的字數。CJK 一字算 1 em；拉丁候選會被高估，
     *   那是安全的方向（少排一個，不會切字）。
     */
    fun itemWidthDp(
        textChars: Int,
        textSize: Float,
        labelChars: Int,
        labelSize: Float,
        commentChars: Int,
        commentSize: Float,
        paddingH: Float,
        minWidth: Float,
    ): Float {
        val text = textChars.coerceAtLeast(0) * textSize
        val label = if (labelChars > 0) labelChars * labelSize * LATIN_EM + GAP_DP else 0f
        val comment = if (commentChars > 0) commentChars * commentSize * LATIN_EM + GAP_DP else 0f
        val inner = text + label + comment + 2f * paddingH
        return maxOf(inner, minWidth.coerceAtLeast(0f))
    }

    /**
     * 候選列真正能給候選用的寬度（規範 §8.6.4.2）。
     *
     * `reservedEnd` 是右端保留給控制鍵的寬度。實測 411.43 dp 的機器上，
     * **80 dp（翻頁＋展開兩顆）與 40 dp（一顆）的差恰好是一個候選**（5 vs 6）。
     */
    fun usableDp(screenWidthDp: Float, barPaddingH: Float, reservedEndDp: Float): Float =
        (screenWidthDp - 2f * barPaddingH - reservedEndDp).coerceAtLeast(0f)

    /**
     * 一列**完整**畫得出幾個。
     *
     * 只數完整畫得出來的：畫一半的那一個對使用者而言是「還有東西」的提示，
     * 不是一個可以放心點的候選。[rightEnd] 靠這個數決定右端那一顆是什麼，
     * 所以它寧可少算，不可多算 —— 多算一個就等於**讓使用者跳過他沒看見的候選**。
     */
    fun visibleCount(usableDp: Float, widths: List<Float>, spacing: Float): Int {
        var used = 0f
        var n = 0
        for (w in widths) {
            val next = if (n == 0) w else used + spacing + w
            if (next > usableDp) break
            used = next
            n++
        }
        return n
    }

    /**
     * §10 第 40 條的基準情境：兩字 CJK 候選、無序號無註解、`text.size: 20`。
     *
     * 這是**主題的**密度，不是某一次輸入的密度：同一份主題在同一個螢幕寬上
     * 只有一個答案，所以建置期測試量得到、擋得住。
     */
    fun baselineVisible(
        screenWidthDp: Float,
        barPaddingH: Float,
        reservedEndDp: Float,
        paddingH: Float,
        spacing: Float,
        minWidth: Float,
        textSize: Float = BASELINE_TEXT_SIZE,
    ): Int {
        val w = itemWidthDp(
            textChars = BASELINE_TEXT_CHARS,
            textSize = textSize,
            labelChars = 0,
            labelSize = 0f,
            commentChars = 0,
            commentSize = 0f,
            paddingH = paddingH,
            minWidth = minWidth,
        )
        val usable = usableDp(screenWidthDp, barPaddingH, reservedEndDp)
        // 一格的節距是固定的，直接用除法會比 visibleCount 好讀，但兩者必須同義 ——
        // 測試裡有一條就是在驗這件事。
        return visibleCount(usable, List(64) { w }, spacing)
    }

    /* ═════════════════════ 註解：與消歧欄互斥 ═════════════════════ */

    /**
     * 這一輪要不要畫候選旁的註解（規範 §8.6.3.1，**僅行動端候選列**）。
     *
     * @param readings 本輪候選的 `comment` 逐則套 [T9Syllables.readingOf] 之後
     *   去重的結果（[T9Syllables.readingsOf]）。非空 = 這些 comment 是讀音，
     *   而讀音有消歧欄在畫。
     *
     * ⚠ **門檻刻意是 1，而消歧欄的門檻是 2。** 相同的話，讀音收斂到只剩一個時
     * 消歧欄收起來、註解同時跑出來，每一格寬度一起改變 —— 使用者正在看著候選
     * 挑字，整列在他眼前重排。這與「候選不得上下跳」是同一條紀律。
     *
     * ⚠ 桌面端的 `candidates.window` **不套用**本條：桌面端不畫消歧欄，
     * 關掉註解等於憑空少一份資訊、沒有任何東西補上。
     */
    fun commentVisible(commentShow: Boolean, readings: List<String>): Boolean =
        commentShow && readings.isEmpty()

    /* ═════════════════════ 序號：按得到才畫 ═════════════════════ */

    /**
     * 這一層送得出 `1`–`9` 裡的任何一個嗎。
     *
     * 序號 `1 2 3` 的用途只有一個：讓使用者按數字鍵選第 N 個。行動端多數佈局
     * 沒有數字鍵，那時候序號是一段**沒有對應動作的文字**，而它每格吃掉約 10 dp。
     *
     * ⚠ 只認 `send.keysym`。`hint`（鍵面角落小字）**不送出任何東西** ——
     * 拿它當判準就會做出「畫面上有數字、按下去沒有數字」的序號。
     */
    fun layerSendsSelectionDigit(layer: LayoutLayer?): Boolean {
        if (layer == null) return false
        for (row in layer.rows) {
            for (key in row.keys) {
                if (key.spacer) continue
                val send = key.send
                if (send !is SendSpec.Keysym) continue
                if (send.name.length == 1 && send.name[0] in '1'..'9') return true
            }
        }
        return false
    }

    /** §8.6.1.1。視覺上不畫；無障礙朗讀**永遠**含序號，那是兩件事。 */
    fun labelVisible(labelShow: Boolean, layerSendsSelectionDigit: Boolean): Boolean =
        labelShow && layerSendsSelectionDigit

    /* ═════════════════════ 右端那一顆控制鍵 ═════════════════════ */

    /**
     * 候選列右端要畫什麼（規範 §8.6.6.4）。
     *
     * ── ⛔ 本頁還有畫不出來的候選時，不得提供「下一頁」──────────────────
     * 翻頁鍵的語義是「本頁我看完了」。本頁沒看完就給翻頁，等於讓使用者
     * **跳過他從未看見的候選**，而畫面完全正常 —— 這正是本專案抓過七次的形狀。
     *
     * 而這件事**在改這一版之前就已經成立**：`Pager.state` 的 `candidateCount`
     * 參數，KDoc 寫的是「這一頁**畫得出來**的候選數」，呼叫端傳的卻是
     * `state.candidates.size`（整頁 9 個），`nextEnabled = !isLastPage` 與
     * 「畫得出來幾個」完全脫鉤。也就是說：**現在按 `›` 就是在跳過 6 個
     * 沒看過的候選。**
     *
     * ── 為什麼是「最多一顆」──────────────────────────────────────────
     * 翻頁鍵與展開鍵解決的是**同一個問題**（「還有更多」）。兩顆一起出現，
     * 是同一份資訊的第二份 —— 與註解／消歧欄同一個病 —— 而它們一起吃掉
     * 候選列 19.4% 的寬度。
     *
     * @param visible [visibleCount] 的結果。
     * @param pageCandidateCount 本頁畫得出來**與畫不出來**的候選總數。
     * @param expandAvailable 主題的 `scroll == expandable && expand_button.show`。
     * @param panelOpen 展開面板現在開著嗎。
     */
    fun rightEnd(
        visible: Int,
        pageCandidateCount: Int,
        expandAvailable: Boolean,
        panelOpen: Boolean = false,
    ): RightEnd = when {
        pageCandidateCount <= 0 -> RightEnd.NONE
        // 面板開著的時候那一顆**必須**是收合鍵：面板自己沒有關閉鍵,
        // 換成翻頁鍵就等於把一片蓋住鍵盤的浮層留在畫面上而沒有出口。
        // （翻頁之後本頁候選變少、visible >= 本頁數,就會走到這一格。）
        panelOpen && expandAvailable -> RightEnd.EXPAND
        visible < pageCandidateCount ->
            // 主題自己關掉展開鍵時退回翻頁：把使用者**鎖死在第 1 頁**比讓他
            // 跳過幾個候選更糟。這一格不該出現在隨附主題上，
            // `ThemeDensityTest` 逐份擋著（§8.6.6.4 第 4 條：捲動不得是唯一路徑）。
            if (expandAvailable) RightEnd.EXPAND else RightEnd.PAGER
        else -> RightEnd.PAGER
    }

    /**
     * 右端那一格畫的是什麼。**三選一，不可能同時兩顆** —— 這正是把它寫成
     * enum 而不是兩個 boolean 的理由：兩個 boolean 允許「都是 true」，
     * 而那就是現況的缺陷。
     */
    enum class RightEnd {
        /** 什麼都不畫（沒有候選＝候選列現在是工具列）。 */
        NONE,

        /** 展開 `∨`。翻頁在**展開面板內部**。 */
        EXPAND,

        /** 翻頁 `‹ ›`（由 [Pager.state] 決定各自畫不畫）。 */
        PAGER,
    }

    /**
     * 量測時右端要扣掉多少。
     *
     * 翻頁那一支在第 2 頁以後會畫**兩顆**（上一頁＋下一頁），所以扣的寬度
     * 與頁次有關。刻意在「決定右端是什麼」**之前**就算得出來 ——
     * 兩者互相依賴的話，會出現「因為算得下所以畫兩顆、畫了兩顆就算不下」的迴圈。
     *
     * @param reservedEnd 主題的 `bar.reserved_end`（一顆）。
     */
    fun reservedForMeasure(
        reservedEnd: Float,
        buttonDp: Float,
        pageNo: Int,
        pageIndicatorShown: Boolean,
    ): Float =
        if (pageIndicatorShown && pageNo > 0) reservedEnd + buttonDp else reservedEnd

    /* ═════════════════════ 高亮不得改變寬度 ═════════════════════ */

    /**
     * ⛔ **高亮不得改變該格的量測寬度。**
     *
     * `fill`（鋪滿整格的實心塊）、`underline`（格底一條 2 dp）、`outline`
     * （描邊）三種都不改：實測把 `highlight_background` 換成 `transparent`
     * 之後，每一段墨跡座標與原版**逐 px 相同**。
     *
     * 這個函式存在的唯一理由是讓那條規矩**測得到**：任何一種「高亮時多加
     * 內距／多畫一個圖示」的寫法都會讓它紅。不然使用者每移動一次選字，
     * 整列就在他眼前重排一次。
     */
    fun highlightChangesWidth(
        widthNormal: Float,
        widthHighlighted: Float,
    ): Boolean = widthNormal != widthHighlighted
}

/**
 * 一格候選各部位的顏色與描邊 —— 由 `item.highlight_style` 決定。
 *
 * ── 為什麼不能只換底色就算了 ────────────────────────────────────────────
 * `text.highlight_color` 在隨附主題裡是 `$on_accent`（白），它是設計來畫在
 * **重點色實心塊上**的。改成 `underline` / `outline` 之後底色是 surface，
 * 白字畫在白底上就是**看不見** —— 高亮的那一個候選會整個消失，而畫面
 * 「看起來」一切正常。這正是本專案抓過七次的形狀，所以顏色的選擇跟著
 * 畫法一起決定，不是各自為政。
 *
 * 非 `fill` 的兩種畫法一律用 `item.highlight_background`（重點色）當**前景**：
 * 它與 surface 的對比是主題自己保證過的（不然實心塊上的白字也不會可讀）。
 */
object CandidateInk {

    data class Ink(
        val background: Int,
        val text: Int,
        val label: Int,
        val comment: Int,
        /** 非 null = 在候選文字底下畫一條這個顏色的橫線。 */
        val underline: Int?,
        val borderWidth: Float,
        val borderColor: Int,
    )

    fun of(
        item: ItemStyle,
        text: TextStyle,
        label: LabelStyle,
        comment: CommentStyle,
        highlighted: Boolean,
    ): Ink {
        if (!highlighted) {
            return Ink(
                background = item.background,
                text = text.color,
                label = label.color,
                comment = comment.color,
                underline = null,
                borderWidth = item.borderWidth,
                borderColor = item.borderColor,
            )
        }
        return when (item.highlightStyle) {
            HighlightStyle.FILL -> Ink(
                background = item.highlightBackground,
                text = text.highlightColor,
                label = label.highlightColor,
                comment = comment.highlightColor,
                underline = null,
                borderWidth = item.borderWidth,
                borderColor = item.borderColor,
            )
            HighlightStyle.UNDERLINE -> Ink(
                background = item.background,
                text = item.highlightBackground,
                label = label.color,
                comment = comment.color,
                underline = item.highlightBackground,
                borderWidth = item.borderWidth,
                borderColor = item.borderColor,
            )
            HighlightStyle.OUTLINE -> Ink(
                background = item.background,
                text = item.highlightBackground,
                label = label.color,
                comment = comment.color,
                underline = null,
                // 描邊寬度是 0 的話 `outline` 什麼都畫不出來 —— 那是「看得到卻
                // 摸不到」的反面：摸得到卻看不到。退回一條 1 dp,不要靜靜地
                // 變成「沒有高亮」。
                borderWidth = if (item.highlightBorderWidth > 0f) item.highlightBorderWidth else 1f,
                borderColor = if (item.highlightBorderWidth > 0f) item.highlightBorderColor
                else item.highlightBackground,
            )
        }
    }
}
