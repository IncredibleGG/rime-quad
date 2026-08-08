package org.rimequad.ime.theme

/**
 * 鍵盤佈局的型別安全模型（docs/theme-format.md §9）。
 * **僅行動端消費。** macOS / Windows 實作不應載入本套類別。
 */

enum class LayoutKind { ALPHABETIC, NUMERIC, SYMBOL, PHONETIC, GRID, OTHER }

enum class Direction { LTR, RTL }

/** 鍵面文字改由執行期狀態（rs_status）決定。 */
/**
 * §9.4 的 `label_from`：鍵面文字從哪裡來。
 *
 * [INPUT_MODE_PAIR] 是新增的（格式擴充提案見回報）。它與 [INPUT_MODE] 的差別
 * 不是外觀偏好，是**歧義**：只寫一個「中」的鍵，使用者分不出那是
 * 「現在是中文」還是「按了會變中文」—— 真機回報過。
 * PAIR 同時畫出兩態、強調當前那一態，語義上就沒有第二種讀法。
 */
enum class LabelSource { NONE, INPUT_MODE, INPUT_MODE_PAIR, SHAPE, VARIANT, SCHEMA_NAME, SCHEMA_ID }

enum class SwipeDirection { UP, DOWN, LEFT, RIGHT }

enum class PopupLayout { ROW, GRID }

enum class ActionVerb {
    NOOP,
    LAYER, LAYER_ONCE, LAYER_LOCK,
    SWITCH_LAYOUT,
    TOGGLE_OPTION, SET_OPTION,
    SCHEMA_NEXT, SCHEMA_PREV, SCHEMA_PICKER, SCHEMA_SELECT,
    CANDIDATE_SELECT, CANDIDATE_DELETE,
    CANDIDATE_NEXT_PAGE, CANDIDATE_PREV_PAGE, CANDIDATE_NEXT, CANDIDATE_PREV,
    CURSOR_LEFT, CURSOR_RIGHT, CURSOR_HOME, CURSOR_END,
    CLEAR, HIDE_KEYBOARD, SETTINGS, EMOJI,

    /**
     * `input_mode:toggle` —— 「切中英」這件事的**完整**語義。
     *
     * 舊的 `toggle:ascii_mode` 只切引擎的開關、不動佈局。那在 QWERTY 上剛好
     * 沒問題（本來就是 26 鍵），在九宮格上卻等於**按了沒用**：使用者進了英文
     * 模式，眼前仍然是 abc/def/ghi 八顆鍵，26 個字母一個都打不出來。
     *
     * 所以這個動詞把兩件事綁成一件：切模式，**並且**切到本佈局宣告的
     * 字母層（[KeyboardLayout.alphaLayer]）；切回中文則回到 `default_layer`。
     * 沒宣告字母層的佈局（qwerty、intl-*）只切模式，佈局原地不動 ——
     * 兩邊的鍵長得一樣、按起來一致。
     */
    INPUT_MODE_TOGGLE
}

data class KeyAction(val verb: ActionVerb, val args: List<String>, val raw: String) {
    val arg: String? get() = args.firstOrNull()
}

/**
 * 一個鍵送出什麼（§9.4）。兩種形態互斥。
 *
 * [Keysym] 走 `rs_process_key`，讓 librime 有機會消費；
 * [Text] 繞過引擎直接上屏，只用於 librime 無法表達的字元。
 */
sealed class SendSpec {

    data class Keysym(
        /** 檔案中的原始名稱，保留下來以便回落到 `RimeGetKeycodeByName()`。 */
        val name: String,
        /** 靜態表解析出的值；[Keysym.VOID_SYMBOL] 表示需要 native 回落。 */
        val code: Int,
        val modifiers: Int
    ) : SendSpec()

    data class Text(val text: String) : SendSpec()
}

/** 長按彈出盤 / 滑動的子鍵：只認得 label / hint / send / tap / style。 */
data class SubKey(
    val label: String,
    val hint: String,
    val style: String?,
    val send: SendSpec?,
    val tap: KeyAction?
)

data class Popup(val layout: PopupLayout, val columns: Int, val keys: List<SubKey>)

data class LayoutKey(
    val id: String?,
    val label: String,
    val hint: String,
    val icon: String?,
    val labelFrom: LabelSource,
    val width: Float,
    val style: String,
    val spacer: Boolean,
    val active: Boolean,
    val repeat: Boolean,
    val send: SendSpec?,
    val tap: KeyAction?,
    val doubleTap: KeyAction?,
    val longPress: KeyAction?,
    val popup: Popup?,
    val swipe: Map<SwipeDirection, SubKey>
) {
    /** §9.6 的觸發解析：點擊 → tap → send → noop。 */
    val hasTapBehavior: Boolean get() = tap != null || send != null
}

data class LayoutRow(val weight: Float, val keys: List<LayoutKey>) {
    val widthSum: Float get() = keys.fold(0f) { acc, k -> acc + k.width }
}

data class LayoutLayer(
    val id: String,
    val label: LocalizedString,
    val units: Float,
    val rows: List<LayoutRow>
)

data class LayoutMetrics(
    /** null 代表沿用主題的值。 */
    val rowSpacing: Float?,
    val keySpacing: Float?,
    val heightScale: Float
)

data class KeyboardLayout(
    val id: String,
    val revision: Int,
    val name: LocalizedString,
    val description: LocalizedString,
    val author: String,
    val license: String,
    val kind: LayoutKind,
    val targets: List<String>,
    /** §9.1.1 的**資格**：這份佈局可以給哪些方案用（`"*"` = 全部）。 */
    val forSchema: List<String>,
    /**
     * §9.1.1 的**自動命中**：切到這些方案時應該自動換上這份佈局。
     *
     * 與 [forSchema] 分開，是因為兩者回答的是不同問題。合成一個欄位時，想給
     * 同一個方案提供第二份佈局的作者只能把 `for_schema` 寫成 `"*"` 來避開
     * 自動命中 —— 而那是個謊：照字面它會把九宮格佈局提供給 `luna_pinyin`，
     * 渲染正常但一個字都打不出來（`ADGJMPTW` 是 `t9_pinyin` 的 speller 契約）。
     * 這裡不含 `"*"`：自動命中必須是明確點名的。
     */
    val autoForSchema: List<String>,
    val direction: Direction,
    val defaultLayer: String,
    /**
     * 本佈局的**拉丁字母層**（新增欄位，提案見回報）。null = 沒有。
     *
     * 只有 `input_mode:toggle` 讀它。宣告了，切到英文就跳到這一層、
     * 切回中文就回 [defaultLayer]；沒宣告，那顆鍵只切模式。
     *
     * 必須是本佈局宣告過的層 id —— 跨佈局跳轉是 `switch_layout` 的事，
     * 而且那條路徑會走進「進得去出不來」（見 LayoutEscape 的檔頭）。
     */
    val alphaLayer: String?,
    val primary: Boolean,
    val metrics: LayoutMetrics,
    val layers: List<LayoutLayer>,
    val ancestry: List<String>
) {
    fun layer(id: String): LayoutLayer? = layers.firstOrNull { it.id == id }

    /** §9.1.1：本佈局是否**適用於**某個 librime schema（資格，非自動命中）。 */
    fun matchesSchema(schemaId: String): Boolean =
        forSchema.contains("*") || forSchema.contains(schemaId)

    /** §9.1.1 第 1 步：本佈局是否為某方案的**自動命中**對象。 */
    fun autoMatchesSchema(schemaId: String): Boolean = autoForSchema.contains(schemaId)
}
