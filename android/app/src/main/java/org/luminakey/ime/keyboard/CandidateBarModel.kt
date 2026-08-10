package org.luminakey.ime.keyboard

import org.luminakey.ime.theme.LayoutLayer
import org.luminakey.ime.theme.PageIndicatorKind
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
 * ── 判準與切分都在 [PreeditParts] ──────────────────────────────────────
 * 「哪些字元是一整組字母的代號」這條判準，以及照它切出來的
 * 「讀得懂的一段 / 還是代碼的一段」，**與宿主 app 輸入框那一端共用**
 * （見 [HostPreedit]）。各切各的話，同一串按鍵在鍵盤上是 `ni⋯`、
 * 在使用者的 app 裡卻是 `ni GAM` —— 上一輪就是只修了這一格。
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
     * 與宿主輸入框（[HostPreedit.forHost]）唯一的差別就在這個 null：
     * 候選列上沒有東西可印時整格收掉，候選往左靠；而宿主的組字區**不能空**
     * ——空字串會讓組字區當場消失。兩邊其餘部分走的是同一個 [PreeditParts.of]。
     *
     * @param groupCodes [groupCodeChars] 的結果。
     */
    fun forDisplay(preedit: String, groupCodes: Set<Char>): String? {
        if (preedit.isEmpty()) return null
        val split = PreeditParts.of(preedit, groupCodes)
        if (!split.hasPending) return split.raw
        if (split.settled.isEmpty()) return null
        return split.settled + ELLIPSIS
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
     * 「不可用時要不要畫出來」：**要**。規範 §8.6.5 給了 `disabled_color`，
     * 就是為了讓它留在原地變灰 —— 第一頁時把上一頁鍵藏起來，整條候選列會
     * 橫向位移一次，使用者剛瞄準的候選就跑掉了。
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
