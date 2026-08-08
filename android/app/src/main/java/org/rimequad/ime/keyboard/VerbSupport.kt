package org.rimequad.ime.keyboard

import org.rimequad.ime.theme.ActionVerb

/**
 * 這一端**還沒有實作**的動作動詞。
 *
 * ── 為什麼「還沒實作」需要一份清單 ──────────────────────────────────────
 * 動詞表是四端共用的,某一端還沒跟上是常態。危險的不是沒跟上,是**沒跟上的
 * 表現方式**:
 *
 *     ActionVerb.EMOJI -> Log.i(TAG, "emoji 面板尚未實作")
 *
 * 這一行在畫面上完全看不出來。鍵在、圖示在、按下去有按壓色也有震動,只是
 * 什麼都沒發生 —— 而自動化測試會全綠,因為畫面確實正確。這個專案已經抓到
 * 四顆這種鍵(重輸、中英、按壓色、工具列表情),它們的共同點都是這個。
 *
 * 所以「還沒實作」不能只是分派 `when` 裡的一個分支,必須是一個**查得到的事實**,
 * 三個消費端各自照它做該做的事:
 *
 *   · **工具列** —— 直接不渲染。§8.6.6.1 的規範性預設工具列含 `emoji`,而 12 份
 *     主題有 8 份沒宣告 `items`、其餘 4 份繼承自 default-*,結果是每一份主題都
 *     長出那顆鍵。工具列是 LazyRow,拿掉一項不影響其他項的幾何,可以在執行期做。
 *
 *   · **佈局按鍵** —— **不可以**在執行期偷偷拿掉。鍵有寬度,少一顆整列會重排,
 *     使用者會看到一個位置飄移的鍵盤,比一顆沒反應的鍵更糟。改由
 *     `UnimplementedVerbTest` 在建置期擋下,讓佈局作者決定那個位置該放什麼。
 *
 *   · **分派** —— [org.rimequad.ime.RimeInputMethodService] 在進 `when` 之前先查
 *     這裡並直接返回,所以 `when` 裡不會再留下「安靜的 noop 分支」。
 *
 * ── 動詞實作好了要做什麼 ────────────────────────────────────────────────
 * 把那一行從這裡刪掉,然後補上分派分支。工具列會自己把它放回來,一份 YAML
 * 都不必改 —— 這正是**不從規範與主題裡刪掉 emoji**、而是在渲染端宣告不支援的
 * 理由:桌面端哪天做了表情面板,它們不必反過來把規範改回去。
 */
object VerbSupport {

    /**
     * 順序無意義,但每一項都必須寫清楚**為什麼**還沒有 —— 沒有理由的項目下一個
     * 讀到的人不知道能不能刪。
     */
    val UNIMPLEMENTED: Set<ActionVerb> = setOf(
        // 表情面板還沒做。規範也還沒有 `voice_input`,intl-ios 的麥克風位曾經拿
        // 這個動詞頂替(見該檔註解),那顆鍵因此從來沒有作用過。
        ActionVerb.EMOJI,

        // rime_shell 只給了換頁(`rs_change_page`),沒有「把高亮移到上／下一個
        // 候選」。這是 ABI 缺口不是本端偷懶,已向協調端回報;補上之前任何
        // `candidate:next` / `candidate:prev` 都只會是一顆沒反應的鍵。
        ActionVerb.CANDIDATE_NEXT,
        ActionVerb.CANDIDATE_PREV,
    )

    fun isImplemented(verb: ActionVerb): Boolean = verb !in UNIMPLEMENTED
}
