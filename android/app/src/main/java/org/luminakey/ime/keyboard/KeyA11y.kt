package org.luminakey.ime.keyboard

import org.luminakey.ime.core.RimeStatus
import org.luminakey.ime.theme.ActionVerb
import org.luminakey.ime.theme.KeyAction
import org.luminakey.ime.theme.LabelSource
import org.luminakey.ime.theme.LayoutKey

/*
 * 每一顆鍵「念出來是什麼」—— 純函式的那一半。
 *
 * ── 這不是翻譯問題,是根本沒有字 ─────────────────────────────────────────
 * 改動前全專案 0 個 contentDescription、0 個 semantics。鍵盤是自繪的:一顆鍵
 * 就是一個畫著「⌫」的 Box,語意層什麼都沒有,TalkBack 只念得出「按鈕」,
 * 或者直接跳過。使用者拿到的是一塊念不出來的方格。
 *
 * ── 為什麼名字要留在純函式裡 ────────────────────────────────────────────
 * 與 [org.luminakey.ime.prefs.PrefLevels] / PrefLabels、[KeyboardTypes] /
 * [localizedGroupTitle] 同一個理由:**「這顆鍵該念什麼」是規則,「那句話怎麼
 * 說」是資源。** 規則要能被 JVM 單元測試直接掃過 core/layouts 底下十二份佈局
 * 的每一顆鍵(見 KeyA11yTest);一旦這裡需要 Context,那個測試就得扛
 * Robolectric,而它正是唯一問得出「有沒有哪顆鍵是啞的」的地方。
 *
 * 所以這一檔回傳的是 [KeyName]（代號 + 參數），翻成字在 [KeyA11yLabels]。
 *
 * ── 名字的優先序 ────────────────────────────────────────────────────────
 * 刻意與 §9.6 的鍵面解析([faceOf])**不同**:
 *
 *   1. `label_from` —— 狀態鍵。名字說它切換什麼(「中英切換」),
 *      現在停在哪一態走 `stateDescription`,不混進名字。
 *   2. `icon` —— 語義圖示查表。念的是**它做什麼**而不是它長什麼樣:
 *      地球鍵念「選擇鍵盤」,不念「地球」。
 *   3. 動作 —— 沒有圖示、而鍵面又念不出意思的鍵(`?123`、`ABC`)。
 *      層切換用**目標層自己宣告的名字**(佈局 yaml 裡就有,而且已經在地化),
 *      所以「?123」念出來是「切換到 符號」,不是「問號一二三」。
 *   4. 鍵面 —— 字母、標點、漢字。這些直接念就對了。
 */

/** 一句朗讀名的**代號**。翻成字在 [KeyA11yLabels]。 */
internal enum class A11yLabel {
    BACKSPACE, ENTER, SHIFT, SHIFT_LOCK, SPACE, GLOBE, KEYBOARD_HIDE, SETTINGS, EMOJI,
    SEARCH, GO, DONE, NEXT, CLIPBOARD, UNDO, MIC,
    ARROW_LEFT, ARROW_RIGHT, ARROW_UP, ARROW_DOWN,

    CLEAR, CURSOR_LEFT, CURSOR_RIGHT, CURSOR_HOME, CURSOR_END,
    CANDIDATE_NEXT_PAGE, CANDIDATE_PREV_PAGE,
    SCHEMA_PICKER, SCHEMA_NEXT, SCHEMA_PREV,

    INPUT_MODE, VARIANT, SHAPE,

    /** 帶一個參數:目標層的名字。 */
    LAYER_SWITCH,
    LAYOUT_PRIMARY, LAYOUT_PREVIOUS, LAYOUT_SWITCH,
}

/** 狀態鍵「現在停在哪一態」的代號。 */
internal enum class A11yState { CJK, LATIN, TRADITIONAL, SIMPLIFIED, FULL_WIDTH, HALF_WIDTH }

/** 朗讀名的來源。 */
internal sealed interface KeyName {

    /** 直接念鍵面上的字。空字串代表**這顆鍵沒有名字** —— KeyA11yTest 會擋。 */
    data class Face(val text: String) : KeyName

    /** 查表。[arg] 是格式參數,目前只有層切換用得到。 */
    data class Named(val label: A11yLabel, val arg: String? = null) : KeyName
}

internal object KeyA11y {

    /**
     * 一顆鍵的朗讀名。
     *
     * @param layerLabels 本佈局的「層 id → 已在地化的層名」。查不到就退回鍵面,
     *                    不自己編一句 —— 編出來的通用說法比佈局作者寫的差。
     */
    fun nameOf(key: LayoutKey, layerLabels: Map<String, String>): KeyName {
        labelSourceLabel(key.labelFrom)?.let { return KeyName.Named(it) }
        key.icon?.let { icon -> ICONS[icon]?.let { return KeyName.Named(it) } }
        actionName(key.tap, layerLabels)?.let { return it }
        return KeyName.Face(key.label)
    }

    /** 工具列項目就是「沒有 send 的鍵」,走同一套規則。 */
    fun toolbarNameOf(
        icon: String?,
        label: String,
        labelFrom: LabelSource,
        tap: KeyAction,
    ): KeyName {
        labelSourceLabel(labelFrom)?.let { return KeyName.Named(it) }
        icon?.let { name -> ICONS[name]?.let { return KeyName.Named(it) } }
        actionName(tap, emptyMap())?.let { return it }
        return KeyName.Face(label)
    }

    /**
     * 狀態鍵現在停在哪一態,`null` 表示這不是狀態鍵。
     *
     * 走 `stateDescription` 而不是併進名字:TalkBack 會在名字後面自己念出狀態,
     * 而且**狀態變了會重念**。塞進 contentDescription 的話,切換之後使用者聽不到
     * 任何回饋 —— 他按了一下,不知道成功了沒有。
     */
    fun stateOf(labelFrom: LabelSource, status: RimeStatus): A11yState? = when (labelFrom) {
        LabelSource.INPUT_MODE, LabelSource.INPUT_MODE_PAIR ->
            if (status.isAsciiMode) A11yState.LATIN else A11yState.CJK
        LabelSource.VARIANT ->
            if (status.isSimplified) A11yState.SIMPLIFIED else A11yState.TRADITIONAL
        LabelSource.SHAPE ->
            if (status.isFullShape) A11yState.FULL_WIDTH else A11yState.HALF_WIDTH
        else -> null
    }

    /**
     * 這顆鍵在語意上必須提供的動作。
     *
     * ⚠ **這是本檔最重要的一個函式。** TalkBack 的「輕點兩下」送的是無障礙的
     * ACTION_CLICK,不會變成 pointer 事件 —— 而 [KeyView] 的觸發整段都在
     * `pointerInput` 裡。少了對應的語意動作,補出來的是一顆念得出名字、
     * 聚焦得到、按下去什麼都不會發生的鍵,正是這個專案抓過五次的那類缺陷
     * 換一個形式。
     */
    fun actionsOf(key: LayoutKey): Set<A11yAction> {
        val out = mutableSetOf<A11yAction>()
        if (key.tap != null || key.send != null) out += A11yAction.CLICK
        if (key.longPress != null || key.popup != null) out += A11yAction.LONG_CLICK
        return out
    }

    private fun labelSourceLabel(source: LabelSource): A11yLabel? = when (source) {
        LabelSource.INPUT_MODE, LabelSource.INPUT_MODE_PAIR -> A11yLabel.INPUT_MODE
        LabelSource.VARIANT -> A11yLabel.VARIANT
        LabelSource.SHAPE -> A11yLabel.SHAPE
        // 方案名／方案 id 本來就是給人看的字,直接念。
        LabelSource.SCHEMA_NAME, LabelSource.SCHEMA_ID, LabelSource.NONE -> null
    }

    private fun actionName(tap: KeyAction?, layerLabels: Map<String, String>): KeyName? {
        val action = tap ?: return null
        return when (action.verb) {
            ActionVerb.LAYER, ActionVerb.LAYER_ONCE, ActionVerb.LAYER_LOCK -> {
                val target = layerLabels[action.arg].orEmpty()
                if (target.isEmpty()) null
                else KeyName.Named(A11yLabel.LAYER_SWITCH, target)
            }
            ActionVerb.SWITCH_LAYOUT -> KeyName.Named(
                when (action.arg) {
                    "@primary" -> A11yLabel.LAYOUT_PRIMARY
                    "@previous" -> A11yLabel.LAYOUT_PREVIOUS
                    else -> A11yLabel.LAYOUT_SWITCH
                }
            )
            ActionVerb.SCHEMA_PICKER -> KeyName.Named(A11yLabel.SCHEMA_PICKER)
            ActionVerb.SCHEMA_NEXT -> KeyName.Named(A11yLabel.SCHEMA_NEXT)
            ActionVerb.SCHEMA_PREV -> KeyName.Named(A11yLabel.SCHEMA_PREV)
            ActionVerb.SETTINGS -> KeyName.Named(A11yLabel.SETTINGS)
            ActionVerb.HIDE_KEYBOARD -> KeyName.Named(A11yLabel.KEYBOARD_HIDE)
            ActionVerb.CLEAR -> KeyName.Named(A11yLabel.CLEAR)
            ActionVerb.CURSOR_LEFT -> KeyName.Named(A11yLabel.CURSOR_LEFT)
            ActionVerb.CURSOR_RIGHT -> KeyName.Named(A11yLabel.CURSOR_RIGHT)
            ActionVerb.CURSOR_HOME -> KeyName.Named(A11yLabel.CURSOR_HOME)
            ActionVerb.CURSOR_END -> KeyName.Named(A11yLabel.CURSOR_END)
            ActionVerb.CANDIDATE_NEXT_PAGE -> KeyName.Named(A11yLabel.CANDIDATE_NEXT_PAGE)
            ActionVerb.CANDIDATE_PREV_PAGE -> KeyName.Named(A11yLabel.CANDIDATE_PREV_PAGE)
            ActionVerb.INPUT_MODE_TOGGLE -> KeyName.Named(A11yLabel.INPUT_MODE)
            ActionVerb.EMOJI -> KeyName.Named(A11yLabel.EMOJI)
            else -> null
        }
    }

    /** §9.6 的語義圖示名 → 朗讀名。這張表要與 [KNOWN_ICONS] 一樣長,由測試守著。 */
    private val ICONS: Map<String, A11yLabel> = mapOf(
        "backspace" to A11yLabel.BACKSPACE,
        "enter" to A11yLabel.ENTER,
        "shift" to A11yLabel.SHIFT,
        "shift_lock" to A11yLabel.SHIFT_LOCK,
        "space" to A11yLabel.SPACE,
        "globe" to A11yLabel.GLOBE,
        "keyboard_hide" to A11yLabel.KEYBOARD_HIDE,
        "settings" to A11yLabel.SETTINGS,
        "emoji" to A11yLabel.EMOJI,
        "search" to A11yLabel.SEARCH,
        "go" to A11yLabel.GO,
        "done" to A11yLabel.DONE,
        "next" to A11yLabel.NEXT,
        "clipboard" to A11yLabel.CLIPBOARD,
        "undo" to A11yLabel.UNDO,
        "mic" to A11yLabel.MIC,
        "arrow_left" to A11yLabel.ARROW_LEFT,
        "arrow_right" to A11yLabel.ARROW_RIGHT,
        "arrow_up" to A11yLabel.ARROW_UP,
        "arrow_down" to A11yLabel.ARROW_DOWN,
    )

    /** 測試用:圖示表的鍵集合,拿來與解析器的 KNOWN_ICONS 對照。 */
    internal val ICON_NAMES: Set<String> get() = ICONS.keys
}

/** 語意上的動作。 */
internal enum class A11yAction { CLICK, LONG_CLICK }
