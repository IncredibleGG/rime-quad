package org.luminakey.ime.keyboard

import org.luminakey.ime.theme.ActionVerb

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
 *   · **分派** —— [org.luminakey.ime.RimeInputMethodService] 在進 `when` 之前先查
 *     這裡並直接返回,所以 `when` 裡不會再留下「安靜的 noop 分支」。
 *
 * ── 動詞實作好了要做什麼 ────────────────────────────────────────────────
 * 把那一行從這裡刪掉,然後補上分派分支。工具列會自己把它放回來,一份 YAML
 * 都不必改 —— 這正是**不從規範與主題裡刪掉 emoji**、而是在渲染端宣告不支援的
 * 理由:桌面端哪天做了表情面板,它們不必反過來把規範改回去。
 */
object VerbSupport {

    /**
     * 引擎還沒好的時候，這個動詞**做得到事嗎**。
     *
     * ── 為什麼需要它（工單 #105 的第二半）──────────────────────────────
     * 首次部署那十幾秒，`RimeCore` 沒有 session：`setOption` / `selectSchema` /
     * `schemaList` 全部是空轉。而工具列照樣把「中/En」與「繁」畫出來，按下去
     * **什麼都不會發生** —— 那正是這個專案抓過六次的
     * 「念得出名字、按下去沒反應」的鍵。
     *
     * 修 #105 的過程本身差點又做出一個：把候選列那行提示搬到鍵區之後，
     * 工具列整排回來了（那是好事，走查抱怨過它整排消失），
     * 但其中兩顆在那十幾秒裡是死的。
     *
     * ── 判準：這個動作需不需要問引擎 ──────────────────────────────────
     * 需要 → 引擎不在時**不畫**。看不到比按不動好：按不動的鍵會讓人以為
     *        自己按錯了，或者這個 app 壞了。
     * 不需要 → 照畫。🌐（換輸入法）與 ⚙（設定）尤其重要 ——
     *        它們是使用者在這十幾秒裡唯一的兩條出路，
     *        「出口永遠看得見」在這一格也成立。
     */
    fun needsEngine(verb: ActionVerb): Boolean = when (verb) {
        // 都要問 librime 的狀態或改它的開關。
        ActionVerb.TOGGLE_OPTION,
        ActionVerb.SET_OPTION,
        ActionVerb.SCHEMA_NEXT,
        ActionVerb.SCHEMA_PREV,
        ActionVerb.SCHEMA_PICKER,
        ActionVerb.SCHEMA_SELECT,
        ActionVerb.CANDIDATE_SELECT,
        ActionVerb.CANDIDATE_DELETE,
        ActionVerb.CANDIDATE_NEXT_PAGE,
        ActionVerb.CANDIDATE_PREV_PAGE,
        ActionVerb.CANDIDATE_NEXT,
        ActionVerb.CANDIDATE_PREV,
        ActionVerb.CLEAR,
        ActionVerb.INPUT_MODE_TOGGLE,
        -> true

        // 完全不碰引擎：換層、換佈局、移游標、收鍵盤、開設定。
        ActionVerb.NOOP,
        ActionVerb.LAYER,
        ActionVerb.LAYER_ONCE,
        ActionVerb.LAYER_LOCK,
        ActionVerb.SWITCH_LAYOUT,
        ActionVerb.CURSOR_LEFT,
        ActionVerb.CURSOR_RIGHT,
        ActionVerb.CURSOR_HOME,
        ActionVerb.CURSOR_END,
        ActionVerb.HIDE_KEYBOARD,
        ActionVerb.SETTINGS,
        ActionVerb.EMOJI,
        -> false
    }


    /**
     * 順序無意義,但每一項都必須寫清楚**為什麼**還沒有 —— 沒有理由的項目下一個
     * 讀到的人不知道能不能刪。
     */
    val UNIMPLEMENTED: Set<ActionVerb> = setOf(
        // 表情面板還沒做。規範也還沒有 `voice_input`,intl-ios 的麥克風位曾經拿
        // 這個動詞頂替(見該檔註解),那顆鍵因此從來沒有作用過。
        ActionVerb.EMOJI,

        // ⚠ 這一項的理由**換過一次**,原本寫的是「rime_shell 沒有移動高亮的
        // 函式,是 ABI 缺口」。那已經不成立了:`rs_highlight_candidate` 現在
        // 在 core/include/rime_shell.h(§9.5 也把規範改成以它為準)。
        //
        // 現在的真正理由是:**Android 這一端還沒接 JNI 綁定。** 門面層有函式,
        // 但 android/app/src/main/cpp/jni_bridge.cc 與 core/RimeCore.kt 都沒有
        // 對應的 nativeHighlightCandidate,而那兩個檔案不屬於本支線,
        // 已照 coordination.md §2 寫進 §5 回報,不自行跨界修改。
        //
        // 接上之後把這兩項刪掉並補分派;⚠ 分派要用 `rs_highlight_candidate`,
        // 不是 `rs_select_candidate(i+1)` —— 後者會**選定**候選(依方案可能
        // 直接上屏),規範 §9.5 明文點名這個寫法是錯的。
        ActionVerb.CANDIDATE_NEXT,
        ActionVerb.CANDIDATE_PREV,
    )

    fun isImplemented(verb: ActionVerb): Boolean = verb !in UNIMPLEMENTED
}
