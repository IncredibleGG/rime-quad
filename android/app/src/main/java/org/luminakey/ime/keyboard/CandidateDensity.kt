package org.luminakey.ime.keyboard

import org.luminakey.ime.theme.CommentStyle
import org.luminakey.ime.theme.HighlightStyle
import org.luminakey.ime.theme.ItemStyle
import org.luminakey.ime.theme.LabelStyle
import org.luminakey.ime.theme.LayoutLayer
import org.luminakey.ime.theme.PageIndicatorKind
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
     *
     * @param leadingDp 候選之前那一段的寬度 —— 今天只有行內組字串
     *   （`show_preedit_inline`）。**它與右端保留區一樣是真的擠掉候選的**，
     *   而這個公式一度沒有扣它：實測 411 dp 的機器上打 `ni`，模型說 7 個、
     *   畫面只畫得出 6 個；打 `niang` 時差到 61.9 dp。方向是**估寬**（把
     *   畫不出來的那一個算成畫得出來），而 [rightEnd] 正是拿這個數決定
     *   「本頁看完了沒」—— 高估一個就等於給出一顆讓使用者跳過他沒看過的
     *   候選的翻頁鍵。見 [inlinePreeditDp]。
     */
    fun usableDp(
        screenWidthDp: Float,
        barPaddingH: Float,
        reservedEndDp: Float,
        leadingDp: Float = 0f,
    ): Float =
        (screenWidthDp - 2f * barPaddingH - reservedEndDp - leadingDp.coerceAtLeast(0f))
            .coerceAtLeast(0f)

    /**
     * 行內組字串（`bar.show_preedit_inline`）畫出來有多寬。
     *
     * 渲染端是 `Text(…, modifier = Modifier.padding(horizontal = preedit.padding_h))`，
     * 所以寬度 = 字面 ＋ 左右內距各一份。字面用與 [itemWidthDp] 同一套估法：
     * CJK 一字 1 em、拉丁 [LATIN_EM]。組字串幾乎一定是拉丁（拼音／代碼），
     * 而 [LATIN_EM] 刻意取偏寬的那一邊 —— 這裡估寬是**安全**的方向
     * （少排一個候選，而不是讓一個畫不出來的候選被當成畫得出來）。
     */
    fun inlinePreeditDp(text: String?, textSize: Float, paddingH: Float): Float {
        if (text.isNullOrEmpty()) return 0f
        var w = 0f
        for (ch in text) {
            w += if (ch.code < 0x2000) textSize * LATIN_EM else textSize
        }
        return w + 2f * paddingH
    }

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

    /**
     * 基準情境的**第二格**:消歧欄那一側**不活**,於是註解回來(§8.6.3.1)。
     *
     * ── 為什麼非得有第二個基準不可 ────────────────────────────────────────
     * [baselineVisible] 量的是「消歧欄活著」那一格 —— 而這一版把候選從 3 個
     * 拉到 6 個的**招牌成果,有 35% 來自把註解收起來**。註解收得起來的前提是
     * 消歧欄那一側活著([commentVisible] 的 `syllableSideAlive`),而它在兩種
     * 使用者真的會遇到的組態下是 false:
     *
     *   · 主題寫 `candidates.syllables.placement: none`
     *   · 啟動探針判定這個方案改寫不動輸入串(`syllableRewriteReady == false`,
     *     裝置上是舊的單編碼 `t9_pinyin` 就會這樣)
     *
     * 那兩格底下註解照畫,**而密度證據全是探針通過的情況下取的** ——
     * 招牌成果在那個組態下自動歸零,沒有任何守門盯著它。
     *
     * 模型值(411.43 dp、`default-light`、兩字 CJK ＋ 讀音註解 `ni hao`):
     * 一格 56.00 → **98.60 dp**(+42.60),一列 **6 → 3 個**。
     *
     * @param commentChars 註解字數。T9 兩字候選的讀音是 `ni hao` = 6。
     */
    fun baselineVisibleWithComment(
        screenWidthDp: Float,
        barPaddingH: Float,
        reservedEndDp: Float,
        paddingH: Float,
        spacing: Float,
        minWidth: Float,
        commentSize: Float,
        commentChars: Int = BASELINE_COMMENT_CHARS,
        textSize: Float = BASELINE_TEXT_SIZE,
    ): Int {
        val w = itemWidthDp(
            textChars = BASELINE_TEXT_CHARS,
            textSize = textSize,
            labelChars = 0,
            labelSize = 0f,
            commentChars = commentChars,
            commentSize = commentSize,
            paddingH = paddingH,
            minWidth = minWidth,
        )
        return visibleCount(usableDp(screenWidthDp, barPaddingH, reservedEndDp), List(64) { w }, spacing)
    }

    /** 基準情境的註解字數:T9 兩字候選的讀音 `ni hao`。 */
    const val BASELINE_COMMENT_CHARS = 6

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
     *
     * ── ⛔ 「消歧欄在畫」才准壓,不是「讀音解析得出來」才准壓 ────────────────
     * 上一版的判準只有 `readings`,於是**消歧欄那一側根本不會畫出來**的三種
     * 狀態下,讀音一份都看不到 —— 註解被壓掉,而補上它的那個東西不存在。
     * 那是把資訊憑空刪掉,不是把重複的第二份收起來:
     *
     * | 狀態 | 消歧欄 | 註解(上一版) | 使用者看得到讀音嗎 |
     * |---|---|---|---|
     * | 主題寫 `syllables.placement: none` | 不畫 | **被壓掉** | **看不到** |
     * | `syllableRewriteReady == false`(啟動探針還沒回答) | 不畫 | **被壓掉** | **看不到** |
     * | 只有一個相異讀音 | 不畫(門檻 2) | 被壓掉 | 看不到 —— **刻意保留**,見下 |
     *
     * 前兩格這一版修掉了:[syllableSideAlive] 為 false 時註解照畫。
     *
     * @param syllableSideAlive 消歧欄那一側現在是**活的**嗎 ——
     *   `placement != NONE && syllableRewriteReady`。⚠ 這裡刻意**不含**
     *   「讀音數 ≥ 2」那一項:讀音從 2 收斂到 1 是使用者正在挑字的**每一步**
     *   都會發生的事,把它算進來就是「消歧欄收起來的同一瞬間,每一格候選
     *   同時變寬、整列在他眼前重排」。門檻維持不對稱(註解 1、消歧欄 2),
     *   理由與本函式開頭那一段相同。
     */
    fun commentVisible(
        commentShow: Boolean,
        readings: List<String>,
        syllableSideAlive: Boolean,
    ): Boolean = commentShow && !(syllableSideAlive && readings.isNotEmpty())

    /* ═════════════════════ 序號：按得到才畫 ═════════════════════ */

    /**
     * 這一層有沒有**整排**直接按得到的 `1`–`9`。
     *
     * ── 為什麼從「有沒有任何一顆數字鍵」改成「九顆全在」───────────────────
     * 上一版的判準是「這一層有沒有 `send.keysym` 落在 1..9」，實測（emulator-5558）
     * 四份會亮的佈局裡**三份是錯的**：
     *
     * | 佈局 | 上一版 | 實際 |
     * |---|---|---|
     * | `bopomofo-dachen/bopomofo` | 畫 | ㄅ=1、ㄉ=2、ˇ=3… 那是**大千鍵位標示**，不是數字鍵 |
     * | `t9-pinyin/t9` | 畫 1..6 | 只有 `k1` 送得出 `1`，其餘八顆什麼都不送 |
     * | `cn-t9-pinyin-numrow/t9` | 畫 | 數字鍵是真的，但引擎那一側收不到（見 [SelectionDigits]）|
     *
     * 前兩格靠「九顆全在」就擋掉：注音那一排湊得出 1..9 是因為它真的送那些
     * keysym（大千鍵位），這一條擋不住它 —— 擋它的是 [SelectionDigits]。
     * `t9-pinyin/t9` 只湊得出一顆，這一條當場擋下。
     *
     * ── ⚠ 只認 `send`。`popup` / `long_press` / `swipe` 都不算 ───────────
     *  · `hint`（鍵面角落小字）**不送出任何東西**，拿它當判準就是「畫面上有
     *    數字、按下去沒有數字」。
     *  · `swipe` 在本端**根本沒有實作** —— `key.swipe` 只出現在解析、序列化與
     *    九宮格建構三處，`KeyboardView` 從頭到尾沒有讀它（`docs/theme-format.md`
     *    也寫明 swipe 是 OPTIONAL）。照它判就會做出「要使用者上滑、上滑卻沒
     *    反應」的序號。
     *  · `popup`（長按彈出盤）是**活的**，`qwerty` 長按 `e` 選得到第 3 個 ——
     *    但序號 `3` 承諾的是「按 3」，不是「長按某顆鍵再從盤裡挑 3」。
     *    多一道手勢的路徑不足以正當化那 9.9 dp/格，所以這裡刻意不收。
     *    這是 fail-closed 的方向：證不出來就不畫。
     */
    fun layerHasSelectionDigitRow(layer: LayoutLayer?): Boolean {
        if (layer == null) return false
        val found = HashSet<Char>(9)
        for (row in layer.rows) {
            for (key in row.keys) {
                if (key.spacer) continue
                val send = key.send
                if (send !is SendSpec.Keysym) continue
                if (send.modifiers != 0) continue
                if (send.name.length == 1 && send.name[0] in '1'..'9') found.add(send.name[0])
            }
        }
        return found.size == 9
    }

    /**
     * 這一格（佈局層 × 方案）**真的**按得到數字選字嗎（§8.6.1.1，fail-closed）。
     *
     * 兩個條件都要成立，任何一個問不出來就是「不畫」：
     *
     *  1. [layerHasSelectionDigitRow]：這一層直接按得到 1..9。
     *  2. [SelectionDigits.works]：**在真機上按下去真的選到了第 N 個**。
     *
     * ── 為什麼第 2 條非要實測不可 ────────────────────────────────────────
     * 「數字鍵送得出 3」與「按 3 會選第 3 個」是兩件事，而中間隔著 librime 的
     * 兩層攔截，兩層都不在佈局檔裡：
     *
     *  · **`speller/alphabet`**：`bopomofo` 的 alphabet 是 `'1qaz2wsx…6347'`，
     *    十個數字全是**字母**。3 在「聲調還沒打」時是合法的下一個字母 →
     *    被 speller 收走；聲調打完之後拼不下去才輪到 selector。同一顆鍵在
     *    兩個看不出差別的狀態下做兩件事。
     *  · **`recognizer/patterns`**：`core/data/shared/default.yaml` 的
     *    `uppercase: "[A-Z][-_+.'0-9A-Za-z]*$"` 字元集**含 `0-9`**，而九宮格
     *    的鍵刻意送大寫 `A/D/G/J/M/P/T/W`（`t9_pinyin.schema.yaml` 檔頭寫著
     *    「就是為了讓數字鍵完全不進 speller」）。於是整串組字永遠落在
     *    `uppercase` 這個 pattern 裡，數字被 recognizer 收走、附加到輸入串，
     *    走不到 selector。實測：`MGGAM` 之後按 3 → `rs_process_key` 回
     *    consumed、preedit 變成 `MGGAM3`、上屏框變成 `3⋯`，**使用者已經打好
     *    的組字被毀掉**。當初為了避開 speller 吃數字而選大寫，剛好踩進
     *    recognizer 吃數字。
     *
     * 而且第二條的答案**會隨使用者打到哪裡而翻面**（`MGGAM` 不行、消歧成
     * `niGAM` 之後可以）。「每一刻都正確」的序號＝在使用者眼前出現與消失、
     * 每次都讓整列重排的序號 —— 那與本檔 [commentVisible] 守的是同一條紀律。
     * 所以本端取的是**保守的常數答案**：只要那個 (佈局, 方案) 存在任何一個
     * 「按下去不選字」的常見狀態，整格就是 `no`。
     *
     * ⚠ 那份實測表**不是手寫常數**，它由 `scripts/verify_selection_digit.sh`
     *   在真機上按數字鍵量出來、也由同一支腳本校驗。見 [SelectionDigits]。
     */
    fun selectionDigitUsable(layer: LayoutLayer?, layoutId: String?, schemaId: String?): Boolean =
        layerHasSelectionDigitRow(layer) && SelectionDigits.works(layoutId, schemaId)

    /** §8.6.1.1。視覺上不畫；無障礙朗讀**永遠**含序號，那是兩件事。 */
    fun labelVisible(labelShow: Boolean, selectionDigitUsable: Boolean): Boolean =
        labelShow && selectionDigitUsable

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
     * ── ⛔ 判準是「這一顆**真的會被畫出來**」,不是「照理說應該是這一顆」───
     * 上一版在 `visible >= pageCandidateCount` 時**無條件**回 [RightEnd.PAGER],
     * 完全不看翻頁鍵畫不畫得出來。而 `PageArrows` 在 `state.show == false` 時
     * 直接 return —— 主題只要寫 `page_indicator.show: false`（或 `style: none`），
     * 「本頁排得下、但後面還有頁」這一格就變成**右端空白、使用者鎖死在第 1 頁**：
     * 沒有翻頁鍵、也沒有展開鍵（上一版的展開鍵只掛在 `EXPAND` 那一支），
     * 而畫面完全正常、沒有任何東西會叫。隨附主題不會踩到，第三方主題踩到
     * 就是死路。所以這裡改成吃 [pagerDrawable]：**畫不出來的那一顆不算數。**
     *
     * ── 「後面還有候選」時必須至少留一條出口 ────────────────────────────────
     * 兩條出口（翻頁、展開）都畫不出來而後面還有候選 = **設計錯誤**，
     * 不是一種要在執行期優雅處理的頁況。這裡誠實回 [RightEnd.NONE]
     * （渲染端因此什麼都不畫，量測也不留寬度），而把它擋下來的是建置期的
     * [deadEnd] ＋ `ThemeDensityTest`：隨附主題逐份、逐頁況掃過一遍。
     *
     * @param visible [visibleCount] 的結果。
     * @param pageCandidateCount 本頁畫得出來**與畫不出來**的候選總數。
     * @param expandAvailable 主題的 `scroll == expandable && expand_button.show`。
     * @param panelOpen 展開面板現在開著嗎。
     * @param pagerDrawable 翻頁那一組在這一格頁況下**真的會畫出至少一顆箭頭**嗎
     *   （`Pager.state().show` 且 prev/next 至少一顆是活的 —— 與 `PageArrows`
     *   逐條同義）。
     * @param morePages 這一頁之後還有候選（`!isLastPage`）。它與
     *   `visible < pageCandidateCount` 是「還有東西沒看到」的兩種形狀，
     *   兩種都需要出口。
     */
    fun rightEnd(
        visible: Int,
        pageCandidateCount: Int,
        expandAvailable: Boolean,
        panelOpen: Boolean,
        pagerDrawable: Boolean,
        morePages: Boolean,
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
            // ⚠ 退路自己也要畫得出來 —— 翻頁鍵不畫時退到它身上等於退到空白。
            if (expandAvailable) RightEnd.EXPAND
            else if (pagerDrawable) RightEnd.PAGER
            else RightEnd.NONE
        // 本頁看得完 → 翻頁才是誠實的,而且它得真的畫得出來。
        pagerDrawable -> RightEnd.PAGER
        // 本頁看得完、後面還有頁,而翻頁那一組畫不出來（主題關掉了
        // `page_indicator`）—— 展開鍵是剩下的唯一出口。
        //
        // ⛔ 這一段的 KDoc 從前寫「面板自己帶著翻頁列,所以那條路走得到
        //   第 2 頁」——**那是假的**。面板的翻頁列吃的是同一份
        //   `style.pageIndicator`,而把 `rightEnd` 推進這一支的**唯一原因**
        //   正是那份設定被關掉了。於是:右端有 `∨`、按下去面板打開、
        //   底部翻頁鍵一顆都不畫、第 2 頁永遠進不去 —— 而 [deadEnd] 只問
        //   「右端是不是 NONE」,這一格是 EXPAND,永遠不紅。
        //
        //   現在面板的翻頁列走 [Pager.panelState](恆 show、`kind` 恆 ARROWS),
        //   與候選列的 `page_indicator` 脫鉤 —— 這句話才第一次成立。
        //   [deadEnd] 也不再只看右端:它問「這條路到不到得了下一頁」。
        morePages && expandAvailable -> RightEnd.EXPAND
        // 後面沒有候選、本頁也看得完：右端本來就不必畫東西。
        // （上一版在這裡回 PAGER 而 `Pager.State.show` 是 false，畫出來的
        // 結果相同 —— 但「回一個畫不出來的答案」正是這一版要消滅的形狀。）
        else -> RightEnd.NONE
    }

    /**
     * ⛔ **死路偵測:還有候選沒看到,而使用者從這一格走不出去。**
     *
     * 這不是一種頁況,是主題把出口關掉造成的**設計錯誤**
     * （`page_indicator.show: false` ＋ `expand_button.show: false`,
     * 或 `scroll: none`）。§10 第 41 條上一版只掃 `scroll` 與
     * `expand_button.show`,**沒有看 `page_indicator`**;而上一版的這個函式
     * 只問「右端是不是 [RightEnd.NONE]」—— 於是同一條死路第三次躲過去:
     * 它搬進了面板裡(右端是 `EXPAND`,面板打得開,面板裡的翻頁列一顆不畫)。
     *
     * 所以判準改成問**那條路通不通得到下一頁**,而不是「右端有沒有東西」:
     *
     * | 右端 | 使用者要怎麼看到下一個候選 | 是死路嗎 |
     * |---|---|---|
     * | [RightEnd.NONE]   | 沒有東西可按                       | **是** |
     * | [RightEnd.PAGER]  | 按 `›` 翻頁                        | 否 |
     * | [RightEnd.EXPAND] | 按 `∨` 開面板 → 面板裡看完這一頁;   | 面板的翻頁列 |
     * |                   | 下一頁靠**面板自己的翻頁列**        | 畫不出來就**是** |
     *
     * @param morePages `!isLastPage`。
     * @param panelPagerDrawable 展開面板底部那一列翻頁,在這一格頁況下
     *   真的會畫出至少一顆箭頭嗎（[panelPagerDrawable]）。它是 `EXPAND`
     *   這條路**唯一**的下一頁入口 —— 面板畫的是這一頁。
     */
    fun deadEnd(
        layout: BarLayout,
        pageCandidateCount: Int,
        morePages: Boolean,
        panelPagerDrawable: Boolean,
    ): Boolean {
        if (pageCandidateCount <= 0) return false
        val unseenOnThisPage = layout.visible < pageCandidateCount
        if (!unseenOnThisPage && !morePages) return false
        return when (layout.rightEnd) {
            RightEnd.NONE -> true
            // 翻頁鍵按得到下一頁。(它有可能跳過本頁畫不出來的那幾個 ——
            //  那是 §8.6.6.4 第 2 條的問題,由 `每一份主題都留著展開面板`
            //  那一關擋,不是死路。)
            RightEnd.PAGER -> false
            // 面板看得到**本頁**全部;還有下一頁的話,得靠面板自己的翻頁列。
            RightEnd.EXPAND -> morePages && !panelPagerDrawable
        }
    }

    /**
     * 展開面板底部那一列翻頁,在這一格頁況下真的畫得出至少一顆箭頭嗎。
     *
     * 與 `KeyboardView` 裡那個呼叫端逐條同義（[Pager.panelState] ＋
     * `PageArrows` 在 `!show` 時整組 return、按不動的那一顆不畫）——
     * **同一份判準只有一份實作**,不然這裡說通、畫面上不通。
     *
     * @param shownCount 面板真的畫出來的那幾個（已過 `T9Syllables` 篩選）。
     */
    fun panelPagerDrawable(pageNo: Int, isLastPage: Boolean, shownCount: Int): Boolean =
        Pager.panelState(pageNo, isLastPage, shownCount).let { st ->
            st.show && (st.prevEnabled || st.nextEnabled)
        }

    /**
     * 右端那一格畫的是什麼。**三選一，不可能同時兩顆** —— 這正是把它寫成
     * enum 而不是兩個 boolean 的理由：兩個 boolean 允許「都是 true」，
     * 而那就是現況的缺陷。
     */
    enum class RightEnd {
        /**
         * 什麼都不畫。兩種情形：沒有候選（候選列現在是工具列）；
         * 或者「該畫的那一顆畫不出來」—— 後者若同時還有沒看到的候選，
         * 就是 [deadEnd] 要擋下的設計錯誤。
         */
        NONE,

        /** 展開 `∨`。翻頁在**展開面板內部**。 */
        EXPAND,

        /** 翻頁 `‹ ›`（由 [Pager.state] 決定各自畫不畫）。 */
        PAGER,
    }

    /**
     * 右端 `n` 顆控制鍵佔多少寬度。
     *
     * 第一顆用主題的 `bar.reserved_end`（主題可以自己加一點呼吸空間），
     * 之後每一顆就是一顆按鍵的寬度。**一顆都不畫時是 0** ——
     * 「保留」保留的是真的會畫出來的東西。
     */
    fun reservedDp(reservedEnd: Float, buttonDp: Float, buttons: Int): Float = when {
        buttons <= 0 -> 0f
        else -> reservedEnd + (buttons - 1) * buttonDp
    }

    /**
     * 候選列的完整版面決定：一列畫幾個、右端畫什麼、右端佔多寬。
     *
     * ── ⛔ 為什麼非得是一個函式不可 ────────────────────────────────────────
     * 上一版把它拆成兩半：`reservedForMeasure()`（量測時扣多少）與 [rightEnd]
     * ＋ [Pager.state]（實際畫幾顆）。**兩半在 11 種頁況裡有 5 種對不上**，
     * 全部是「量測扣得比畫出來的多 40 dp」：
     *
     * | 頁況 | 量測扣 | 實際畫 |
     * |---|---:|---:|
     * | 第 1 頁又是最後一頁（翻頁整組不畫） | 40 | **0** |
     * | 第 2 頁起、本頁看不完（右端是展開鍵一顆） | 80 | **40** |
     * | 第 2 頁起、又是最後一頁（只有「上一頁」一顆） | 80 | **40** |
     * | 面板開著、第 2 頁起（右端是收合鍵一顆） | 80 | **40** |
     *
     * 那一版的 KDoc 說拆開是為了避開「因為算得下所以畫兩顆、畫了兩顆就算不下」
     * 的迴圈。迴圈是真的，但**拆開不是解**：拆開只是讓兩邊各自猜，而猜錯
     * 沒有任何東西會叫。這裡改成把迴圈**解掉**：
     *
     *  1. 翻頁那一組畫幾顆，只跟「第幾頁、是不是最後一頁、主題開不開指示器、
     *     本頁有沒有候選」有關，**與 `visible` 無關** —— 算得出來。
     *  2. 展開鍵永遠是一顆。
     *  3. 於是只剩兩個候選解。各自算一次 `visible`，取**自洽**的那一個：
     *     · 假設走翻頁：扣翻頁那幾顆之後 `visible >= 本頁候選數` 才自洽
     *       （本頁真的看完了，翻頁才誠實）。
     *     · 假設走展開：扣一顆之後 `visible < 本頁候選數` 才自洽。
     *     翻頁先試 —— 它有可能是 0 顆，讓候選拿到最多的寬度。
     *
     * 回傳的 [BarLayout.reservedDp] **就是**渲染端會畫出來的那幾顆的寬度，
     * 而 [BarLayout.pager] 就是渲染端要用的那一份 [Pager.State]。渲染端不再
     * 自己算第二遍，也就沒有第二份會漂掉的公式。
     *
     * @param widths 本頁**要畫的**每一格的量測寬（[itemWidthDp]），順序同畫面。
     * @param leadingDp 候選之前那一段（行內組字串）的寬度，見 [usableDp]。
     * @param pageCandidateCount 本頁畫得出來**與畫不出來**的候選總數。
     */
    fun barLayout(
        screenWidthDp: Float,
        barPaddingH: Float,
        reservedEnd: Float,
        buttonDp: Float,
        leadingDp: Float,
        widths: List<Float>,
        spacing: Float,
        pageCandidateCount: Int,
        pageNo: Int,
        isLastPage: Boolean,
        pagerKind: PageIndicatorKind,
        pagerShow: Boolean,
        expandAvailable: Boolean,
        panelOpen: Boolean,
    ): BarLayout {
        fun fits(reserved: Float): Int = visibleCount(
            usableDp(screenWidthDp, barPaddingH, reserved, leadingDp), widths, spacing
        )

        // 翻頁那一組：畫幾顆只跟頁況有關，與 visible 無關。
        // `candidateCount` 傳的是**本頁要畫的候選總數**而不是 visible：
        // 那個參數的門檻是「> 0」，而 [RightEnd.PAGER] 這一格永遠滿足
        // `visible >= pageCandidateCount > 0`，兩者同義 —— 但只有前者算得出來，
        // 用後者就把剛剛解掉的迴圈又接回去了。「按 › 不得跳過沒看過的候選」
        // 這條規矩由 [rightEnd] 保證，不由這個參數保證。
        val pagerState = Pager.state(
            kind = pagerKind,
            show = pagerShow,
            pageNo = pageNo,
            isLastPage = isLastPage,
            candidateCount = pageCandidateCount,
        )
        val pagerButtons = if (!pagerState.show) 0 else {
            (if (pagerState.prevEnabled) 1 else 0) + (if (pagerState.nextEnabled) 1 else 0)
        }
        // ⛔ 「翻頁鍵會不會被畫出來」與「right end 該是翻頁嗎」必須是同一個數。
        //    `PageArrows` 在 `!show` 時整組 return、`!prevEnabled` / `!nextEnabled`
        //    的那一顆各自不畫 —— 所以真的畫得出來 ⟺ 顆數 > 0。
        val pagerDrawable = pagerButtons > 0
        val pagerDp = reservedDp(reservedEnd, buttonDp, pagerButtons)
        val expandDp = reservedDp(reservedEnd, buttonDp, 1)

        // 判準仍然走 [rightEnd] 那一支 —— ⛔ 那條規矩只有一份實作。
        // 餵進去的 `visible` 是「假設走翻頁」算出來的：翻頁那幾顆扣掉之後
        // 本頁仍然看得完，翻頁才是誠實的。
        val vPager = fits(pagerDp)
        return when (
            rightEnd(
                visible = vPager,
                pageCandidateCount = pageCandidateCount,
                expandAvailable = expandAvailable,
                panelOpen = panelOpen,
                pagerDrawable = pagerDrawable,
                morePages = !isLastPage,
            )
        ) {
            RightEnd.NONE -> BarLayout(fits(0f), RightEnd.NONE, 0f, null)
            RightEnd.PAGER -> BarLayout(vPager, RightEnd.PAGER, pagerDp, pagerState)
            RightEnd.EXPAND -> BarLayout(fits(expandDp), RightEnd.EXPAND, expandDp, null)
        }
    }

    /**
     * [barLayout] 的結果。**量測扣掉的就是畫出來的那幾顆**，這是它存在的理由。
     *
     * @param pager 右端是翻頁時要用的那一份狀態；其餘情況為 null
     *   （`RightEnd` 已經說了畫什麼，再給一份 Pager.State 只會多一個
     *   「兩邊不一致」的機會）。
     */
    data class BarLayout(
        val visible: Int,
        val rightEnd: RightEnd,
        val reservedDp: Float,
        val pager: Pager.State?,
    )

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
