package org.luminakey.ime.keyboard

import org.luminakey.ime.theme.LayoutLayer
import org.luminakey.ime.theme.PageIndicatorKind
import org.luminakey.ime.theme.ScrollMode
import org.luminakey.ime.theme.SendSpec

/**
 * 候選列上兩件與 librime 無關、但一定要算對的事 —— 抽成純函式，由
 * [CandidateBarModelTest] 直接驗。
 *
 * 兩件事都來自使用者拿真機測九宮格的回報：
 *
 *   * [InlinePreedit] —— 候選列最左邊那一格印著 `MG GAM`。
 *   * [Pager] —— 候選只有一頁，翻不到第二頁。
 *
 * 放在這裡而不是 [KeyboardView] 裡，理由與 [T9Syllables] 一樣：Compose 裡的
 * 邏輯只能靠截圖驗，而截圖驗不到「第 3 頁的下一頁鍵該不該亮」這種事。
 */

/* ═══════════════════════ 候選列左端的組字串 ═══════════════════════ */

/**
 * `candidates.bar.show_preedit_inline` 那一格該印什麼。
 *
 * ── 問題長什麼樣 ────────────────────────────────────────────────────────
 * 規範說那一格印的是「組字串」。全拼打 `nihao` 時它印 `nihao`、注音打
 * ㄋㄧˇ 時它印 ㄋㄧˇ —— 都是使用者**剛剛按過的東西**，看得懂。
 *
 * 九宮格不是。`t9_pinyin` 是雙編碼方案，鍵送出去的是代表字母
 * `A/D/G/J/M/P/T/W`，於是同一格印出來的是 `MG GAM`。使用者鍵面上按的是
 * `mno` 與 `ghi`，畫面上冒出來的卻是 `MG GAM` —— 那不只是沒有意義，
 * 它會讓人以為「我打出來的是這個」。真機回報的原話：「紅色的沒意義
 * 就沒必要出現」。
 *
 * ── 判準：**這個字元是「一整組字母的代表」嗎** ──────────────────────────
 * 刻意**不**寫成「t9_pinyin 就不要印」，也不寫成「大寫的 ADGJMPTW 就砍掉」：
 *
 *   * 綁方案 id 的話，市集裡任何第三方九宮格方案都不受惠；
 *   * 綁那八個字母的話，哪天方案換一組代表字母，這裡會**靜靜地**恢復成
 *     印代碼 —— 而畫面看起來一切正常。
 *
 * 判準改成一句能一直成立的話：**送出這個字元的那顆鍵，鍵面上是一整組字母
 * 而不是它自己。** 那就是九宮格的定義 —— 送 `M` 的那顆鍵印的是 `mno`
 * （或 `MNO`，兩份佈局的大小寫不同，判準不看大小寫）。使用者按的是「那一組」，
 * 畫面上冒出來的卻是組的**代號**，於是那個字元對他沒有意義。
 *
 * 反過來，全鍵盤送 `n` 的鍵印的就是 `n`、大寫層送 `M` 的鍵印的就是 `M`
 * —— 一鍵一字母，preedit 印什麼就是他按了什麼，原封不動。
 *
 * 沒有任何鍵送得出的字元**一律保留**：那是引擎給的東西，不是按鍵代碼。
 * 已確定的拼音 `ni`（九宮格上沒有任何一顆鍵送 `n`）、漢字、注音符號、
 * 聲調符號、標點都走這一條。`你GAM` 於是變成 `你⋯`。
 *
 * ⚠ 大小寫**要**分:`niGAM` 裡的 `M` 是代碼、`mi` 裡的 `m` 是拼音，
 * 而九宮格的鍵送的是大寫 `M`。只砍掉鍵真的送得出的那一個字元，
 * 小寫拼音不會被連坐。
 *
 * ── 為什麼砍掉之後要留一個 `⋯` ──────────────────────────────────────────
 * `ni` 與 `ni⋯` 是兩種不同的狀態：前者代表「我打完了，就是 ni」，
 * 後者代表「ni 已經定了，後面還有沒定的」。少了那個記號，使用者會以為
 * 剩下的按鍵不見了。
 */
object InlinePreedit {

    /** 砍掉的部分留下的記號。與 [T9Syllables.MORE_LABEL] 同字形，都是「還有」。 */
    const val ELLIPSIS = "⋯"

    /**
     * 當前這一層裡，哪些字元是「一整組字母的代表」。
     *
     * 條件兩個都要成立：這一層有鍵送得出它，而且那顆鍵的**鍵面不只它自己**。
     * 鍵面取 `label` —— 那就是使用者眼睛看到的那一行。
     *
     * ⚠ 消歧欄執行期會把 `pu_comma` 之類的鍵面換成 `ni`（見
     * [T9Syllables.slotKey]），這裡讀的是**佈局檔原本的內容**，不受它影響。
     *
     * 佈局還沒載進來（layer 為 null）時回空集合 —— 什麼都不知道就什麼都不砍，
     * 不能因為沒有資訊就把東西藏掉。
     */
    fun groupCodeChars(layer: LayoutLayer?): Set<Char> {
        if (layer == null) return emptySet()
        val out = HashSet<Char>()
        for (row in layer.rows) {
            for (key in row.keys) {
                if (key.spacer) continue
                val send = key.send
                if (send !is SendSpec.Keysym || send.name.length != 1) continue
                val ch = send.name[0]
                // 鍵面就是它自己（`M` → `M`）= 一鍵一字母，不是代表。
                if (key.label.length > 1) out.add(ch)
            }
        }
        return out
    }

    /**
     * 要印在候選列左端的字；**null = 那一格整個不要出現**。
     *
     * @param groupCodes [groupCodeChars] 的結果。
     */
    fun forDisplay(preedit: String, groupCodes: Set<Char>): String? {
        if (preedit.isEmpty()) return null
        if (groupCodes.isEmpty()) return preedit
        val sb = StringBuilder(preedit.length + ELLIPSIS.length)
        var dropped = false
        var kept = false
        for (ch in preedit) {
            if (ch in groupCodes) {
                dropped = true
                continue
            }
            sb.append(ch)
            if (!ch.isWhitespace() && ch != '\'') kept = true
        }
        if (!dropped) return preedit
        if (!kept) return null
        // 砍掉代碼之後留在頭尾的分隔符（speller 插進去的空白與 `'`）是懸空的，
        // 一起收掉；中間連續的空白也收成一個。
        val body = sb.toString().trim(' ', '\'').replace(Regex(" {2,}"), " ")
        if (body.isEmpty()) return null
        return body + ELLIPSIS
    }
}

/* ═══════════════════════════ 候選翻頁 ═══════════════════════════ */

/**
 * 候選列的翻頁控制（規範 §8.6.5 的 `page_indicator`）。
 *
 * ── 問題長什麼樣 ────────────────────────────────────────────────────────
 * 真機回報：「候選詞只有 5 個，下一頁就沒了」。查下去是兩件事疊在一起：
 *
 *   1. 方案的 `menu/page_size` 是 **5**（規範 §8.6.6.3.6 寫的是 9，
 *      對不上；`core/data/` 是協調端的檔案，已寫進 coordination.md §5）。
 *   2. **本端根本沒有翻頁的入口。** `rs_change_page` 早就在 ABI 裡、
 *      [KeyboardEvent.Page] 也早就在 IME service 裡有處理，
 *      但畫面上沒有任何東西會送出它 —— 整條路從頭到尾沒有人走過。
 *
 * 規範 §8.6.5 的 `page_indicator` 預設就是 `show: true` / `style: arrows`，
 * 所以這不是新功能，是**本端一直沒有實作規範裡已經有的東西**。
 *
 * ── 為什麼是「換一頁」而不是「往後接一段」 ──────────────────────────────
 * 橫向捲到底自動接上下一頁看起來更順，但 `rs_select_candidate` 吃的是
 * **頁內索引**。把兩頁接在一起之後，畫面上的第 7 個是引擎的第 2 個 ——
 * 使用者點下去會選到別的字，而畫面完全正常。這正是本專案抓過六次的那類缺陷。
 * 一次只畫引擎當前的那一頁，索引就不可能對不上。
 */
object Pager {

    /**
     * 兩顆箭頭各自的狀態。`show=false` 時整組不畫。
     *
     * ── 「不可用時要不要畫出來」：**不要**（這一版改掉了上一版的答案）──────
     * 上一版留著它並染灰，理由是「藏起來的話整條候選列會橫向位移」。
     * 那個理由**是錯的**，而代價是真的：
     *
     *   · 候選列是一個 `weight(1f)` 的 `LazyRow`，項目**靠左排**。兩顆箭頭在
     *     它右邊。少一顆箭頭時 LazyRow 只是右邊多 40 dp 可用寬度 ——
     *     已經畫出來的候選一顆都不會移動。會位移的是右端那顆箭頭本身。
     *   · 而那 40 dp 是**真的擠掉候選**的。實測（emulator-5558，1080×2400
     *     @420dpi，九宮格打「你好」）：引擎給 5 個候選，畫得下的只有 3 個，
     *     第 4 個被切一半。第一頁時那顆「上一頁」永遠是死的，它佔的 40 dp
     *     等於用一顆按不動的鍵換掉一個候選字。
     *
     * 所以現在：**按不動的那一顆不畫**。`prevEnabled` / `nextEnabled` 因此
     * 同時是「這顆按得動嗎」與「這顆要不要出現」——兩者本來就該一致，
     * 一顆畫得出來卻按不動的鍵正是這個專案抓過五次的那一類缺陷。
     */
    data class State(
        val show: Boolean,
        val prevEnabled: Boolean,
        val nextEnabled: Boolean,
    )

    /**
     * @param candidateCount 這一頁畫得出來的候選數。0 = 沒有候選，
     *   那時候候選列畫的是工具列，翻頁鍵不該擠進去。
     */
    fun state(
        kind: PageIndicatorKind,
        show: Boolean,
        pageNo: Int,
        isLastPage: Boolean,
        candidateCount: Int,
    ): State {
        val visible = show &&
            kind != PageIndicatorKind.NONE &&
            candidateCount > 0 &&
            // 第一頁又是最後一頁 = 總共就這幾個。這時候兩顆箭頭都是死的，
            // 畫出來只是兩顆按不動的灰鍵,不如不要。
            !(pageNo <= 0 && isLastPage)
        return State(
            show = visible,
            prevEnabled = pageNo > 0,
            nextEnabled = !isLastPage,
        )
    }

    /**
     * ⚠ `dots` 與 `text`（`n/m`）都需要**總頁數**，而 `rs_snapshot` 的
     * `menu` 只給得出 `page_no` 與 `is_last_page` —— 總頁數在 ABI 裡不存在。
     * 硬做出來的 `3/?` 或一排長度會跳動的點，比箭頭更難懂。
     *
     * 所以這兩種樣式在本端**退化成箭頭**，並且這件事寫在這裡而不是靜靜發生。
     * 需要它們的時候要先在 `core/` 補「總頁數」（已寫進 coordination.md §5，
     * 與桌面端要的「展開候選網格」是同一件事）。
     */
    fun degradesToArrows(kind: PageIndicatorKind): Boolean =
        kind == PageIndicatorKind.DOTS || kind == PageIndicatorKind.TEXT
}

/* ═══════════════════════ 候選列展開（§8.6.6 的 scroll）═══════════════════════ */

/**
 * 候選列的「展開成多列」（規範 §8.6.6 的 `scroll: expandable` 與 `expand_button`）。
 *
 * ── 問題長什麼樣 ────────────────────────────────────────────────────────
 * 使用者的原話是「候選詞只有三個」。三層量下來，三個是**畫面**的數字，
 * 不是引擎的：
 *
 *   引擎（`rime_console`，繁體，打 `nihao`）  5 個：你好 妳好 逆號 擬好 你
 *   畫面（emulator-5558，1080×2400 @420dpi）
 *     直式・預設字級   完整畫得出 3 個，第 4 個「擬好」只畫得出「擬」
 *     直式・font_scale 1.30   **3 個**
 *     九宮格・直式・預設字級  **3 個**（每一項都掛著 `ni hao` 讀音，項寬幾乎翻倍）
 *     橫式・預設字級   5 個全上，右邊還空著一大半
 *
 * 也就是說：把 `menu/page_size` 從 5 調大，直式**一個字都不會多出來** ——
 * 那一列本來就已經畫不完 5 個。調大 page_size 而不動這一列，等於沒改。
 *
 * ── 為什麼是「展開」而不是「橫向捲動」 ──────────────────────────────────
 * 候選列**早就已經**是一個 `LazyRow`，也就是說它一直都捲得動 —— 而使用者
 * 仍然回報「只有三個」。捲動解決不了這件事，因為畫面上沒有任何東西說得出
 * 「右邊還有」：44dp 高的一條帶子上，看不見的東西等於不存在。
 *
 * 規範對這件事早就有答案：`candidates.bar.scroll` 的**預設值就是
 * `expandable`**，`expand_button.show` 的預設值就是 `true`，而隨附的每一份
 * 主題都照著寫了。缺的不是決定，是**本端一直沒有把它畫出來** ——
 * [ThemeModel] 解析了 `scroll` 與 `expand_button` 兩個欄位，然後沒有任何一
 * 行程式碼讀它們。這與 [Pager] 是同一種形狀：規範裡有、主題裡有、畫面上沒有。
 *
 * ── ⚠ 展開的東西是「這一頁」，不是「全部攤平」 ──────────────────────────
 * **這是這一格唯一會傷到使用者的地方，先寫在最前面。**
 *
 * `rs_select_candidate` 吃的是**頁內索引**（`index_on_page`）。實測（模擬器
 * emulator-5558，`luna_pinyin_tw`，打 `ni`）：
 *
 *     第 1 頁  [0]=你 [1]=擬
 *     第 2 頁  [0]=妳 [1]=妮
 *     在第 2 頁 select(0) → 上屏「妳」
 *
 * 上屏的是第 2 頁的第 0 個，不是第 1 頁的第 0 個 —— 索引確實是頁內的。
 * 所以把第 2 頁接在第 1 頁後面攤成一長條之後，畫面上的第 6 個對引擎而言
 * 是第 1 個：**使用者點下去會上屏別的字，而畫面完全正常**。
 *
 * 因此展開面板畫的是**當前這一頁**的候選，翻頁仍然走 [Pager] 的箭頭
 * （`rs_change_page`）。[rows] 收進去的是引擎索引、吐出來的還是引擎索引，
 * 它只決定「哪幾個排在同一列」，**從不重新編號**。
 */
object Expander {

    /** 展開鍵的狀態。`show=false` 時整顆不畫。 */
    data class State(
        val show: Boolean,
        /** 面板真的要畫出來嗎。`show=false` 時必為 false。 */
        val expanded: Boolean,
    )

    /**
     * @param mode 主題的 `candidates.bar.scroll`。
     * @param showButton 主題的 `candidates.bar.expand_button.show`。
     * @param candidateCount 這一頁畫得出來的候選數。0 = 候選列現在畫的是工具列，
     *   展開鍵不該擠進去（與 [Pager.state] 同一條理由）。
     * @param wanted 使用者現在有沒有按開。
     */
    fun state(
        mode: ScrollMode,
        showButton: Boolean,
        candidateCount: Int,
        wanted: Boolean,
    ): State {
        val show = showButton && mode == ScrollMode.EXPANDABLE && candidateCount > 0
        // 候選變空（選完字、清掉組字）時面板一定要跟著收 —— 否則畫面上會留下
        // 一片蓋住鍵盤、內容卻是空的浮層,而使用者沒有東西可以點。
        return State(show = show, expanded = show && wanted)
    }

    /**
     * 把一頁的候選切成幾列。
     *
     * @param indices **引擎的頁內索引**，順序就是引擎給的順序。
     * @param perRow 一列放幾個；小於 1 一律當 1（畫面量到 0 欄時不可以整批消失）。
     * @return 每一列一個清單，裡面**還是引擎索引**。
     *
     * ⚠ 這個函式做的事只有分列。它不排序、不去重、不重新編號、不丟東西 ——
     *   `rows(x, n).flatten() == x` 永遠成立。那一條等式就是「點到隔壁的字」
     *   這個缺陷的守門：任何一種把位置當成索引的寫法都會讓它紅。
     */
    fun rows(indices: List<Int>, perRow: Int): List<List<Int>> {
        if (indices.isEmpty()) return emptyList()
        val n = if (perRow < 1) 1 else perRow
        return indices.chunked(n)
    }

    /**
     * 一列放得下幾個 —— 由**量到的寬度**決定，不寫死。
     *
     * @param availableDp 面板可用寬度（dp）。
     * @param itemDp 一項的寬度（dp），呼叫端量出來的。
     * @param maxPerRow 上限；再寬也不要把一列排成一長串，那跟沒展開一樣難掃。
     *
     * 至少回 1：算出 0 的話整頁會變成 0 列，也就是「展開之後一片空白」。
     */
    fun perRow(availableDp: Float, itemDp: Float, maxPerRow: Int = 5): Int {
        if (itemDp <= 0f || availableDp <= 0f) return 1
        val fit = (availableDp / itemDp).toInt()
        return fit.coerceIn(1, if (maxPerRow < 1) 1 else maxPerRow)
    }
}
