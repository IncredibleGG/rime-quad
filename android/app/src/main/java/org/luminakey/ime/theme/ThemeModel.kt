package org.luminakey.ime.theme

/**
 * 主題的型別安全模型（docs/theme-format.md §8）。
 *
 * 所有顏色都是 ARGB Int；所有長度單位是 dp；所有文字尺寸單位是 sp（見 §4.3 / §4.4）。
 * 這些物件是不可變的，解析完成後可安全跨執行緒共用。
 */

enum class Appearance { LIGHT, DARK }

enum class Orientation { HORIZONTAL, VERTICAL }

enum class CommentPosition { AFTER, BELOW, HIDDEN }

enum class PageIndicatorKind { ARROWS, DOTS, TEXT, NONE }

enum class ScrollMode { NONE, HORIZONTAL, EXPANDABLE }

enum class Placement { BELOW, ABOVE, AUTO }

enum class Backdrop { NONE, BLUR, VIBRANCY }

enum class HintPosition { TOP_RIGHT, TOP_CENTER, TOP_LEFT, BOTTOM_RIGHT, NONE }

enum class MotionCurve { STANDARD, LINEAR, DECELERATE, ACCELERATE }

enum class HapticStrength { NONE, LIGHT, MEDIUM, HEAVY }

data class FontStack(
    val family: List<String>,
    val weight: Int,
    val italic: Boolean,
    val scriptFallback: Map<String, List<String>>
) {
    /**
     * §8.4.2 的最終字體解析順序：script 分支 ++ 基礎清單 ++ 平台預設。
     * 重複項只保留首次出現。
     */
    fun resolveFor(script: String?, platformDefaults: List<String>): List<String> {
        val out = ArrayList<String>()
        if (script != null) scriptFallback[script]?.let { out.addAll(it) }
        out.addAll(family)
        out.addAll(platformDefaults)
        val seen = LinkedHashSet<String>()
        for (f in out) seen.add(f)
        return seen.toList()
    }
}

data class FontAsset(
    val family: String,
    val file: String,
    val weight: Int,
    val italic: Boolean
)

data class Typography(
    val respectSystemFontScale: Boolean,
    val fontScaleMin: Float,
    val fontScaleMax: Float,
    val fonts: Map<String, FontStack>,
    val assets: List<FontAsset>
) {
    fun font(name: String): FontStack = fonts[name] ?: ThemeDefaults.FONT_STACK

    /** §4.4 的有效字體縮放。 */
    fun effectiveScale(systemFontScale: Float): Float =
        if (!respectSystemFontScale) 1.0f
        else clampFloat(systemFontScale, fontScaleMin, fontScaleMax)
}

data class Metrics(
    val cornerRadius: Float,
    val borderWidth: Float,
    val padding: Float,
    val spacing: Float,
    val elevation: Float
)

data class LabelStyle(
    val show: Boolean,
    val format: String,
    val size: Float,
    val color: Int,
    val highlightColor: Int
) {
    /** §8.6.1：未知佔位符必須原樣保留。 */
    fun render(label: String, indexOnPage: Int): String =
        format.replace("{label}", label)
            .replace("{index}", (indexOnPage + 1).toString())
            .replace("{index0}", indexOnPage.toString())
}

data class TextStyle(val size: Float, val color: Int, val highlightColor: Int)

data class CommentStyle(
    val show: Boolean,
    val position: CommentPosition,
    val size: Float,
    val color: Int,
    val highlightColor: Int
)

/**
 * 高亮怎麼畫（規範草稿 §8.6.4.3）。它回答的問題只有一個：
 * **「我現在按空白鍵會上哪一個」**。
 *
 * ⛔ **三種都不得改變該格的量測寬度。** 使用者每移動一次選字整列就重排，
 * 是比「不夠顯眼」嚴重得多的缺陷。
 *
 * **沒有 `text_only`**：只靠顏色區分不合格（CJK 小字的字重差異幾乎看不出來，
 * 不構成第二個通道），規範不提供一個做錯的選項。
 */
enum class HighlightStyle {
    /**
     * `highlight_background` 鋪滿整格。iOS 系主題的視覺語彙。
     *
     * **這是預設**（`docs/theme-format.md` §8.6.4 的表與 [ThemeParser] 的
     * `highlightStyle = HighlightStyle.FILL` 兩邊都是它）。
     * ⚠ 從前 [UNDERLINE] 的 KDoc 寫著「預設」—— 那一句是錯的,而且是最貴的
     * 那種註解:讀的人不會去查,他會照著寫測試。
     */
    FILL,

    /** 格底一條 2 dp、寬度等於**候選文字墨跡**的重點色橫條。 */
    UNDERLINE,

    /** `highlight_border_width` / `highlight_border_color` 描邊。 */
    OUTLINE,
}

data class ItemStyle(
    val paddingH: Float,
    val paddingV: Float,
    val spacing: Float,
    val cornerRadius: Float,
    val minWidth: Float,
    val background: Int,
    val highlightBackground: Int,
    val highlightStyle: HighlightStyle,
    val borderWidth: Float,
    val borderColor: Int,
    val highlightBorderWidth: Float,
    val highlightBorderColor: Int
)

data class SeparatorStyle(val show: Boolean, val color: Int, val width: Float)

data class PageIndicatorStyle(
    val show: Boolean,
    val kind: PageIndicatorKind,
    val color: Int,
    val disabledColor: Int,
    val size: Float
)

/** 候選呈現的共用部分。`bar` 與 `window` 各自持有一份已套用覆寫的副本。 */
data class CandidateStyle(
    val orientation: Orientation,
    val label: LabelStyle,
    val text: TextStyle,
    val comment: CommentStyle,
    val item: ItemStyle,
    val separator: SeparatorStyle,
    val pageIndicator: PageIndicatorStyle
)

data class ExpandButton(val show: Boolean, val color: Int, val size: Float)

/**
 * 工具列項目（§8.6.6.1）。
 *
 * 刻意重用 [LabelSource] 與 [KeyAction]：工具列項目就是「沒有 `send` 的鍵」，
 * 不該為它發明第二套詞彙。鍵面文字的解析順序、圖示退化、active 觸發條件
 * 都與按鍵完全相同（§9.6、§8.8.1），渲染器可以共用同一段程式碼。
 */
data class ToolbarItem(
    val icon: String?,
    val label: String,
    val labelFrom: LabelSource,
    val tap: KeyAction
)

/**
 * 無候選時候選列顯示的工具列。
 *
 * 這不是裝飾:它是方案切換的唯一入口,所以 [items] 必定含有
 * `schema:picker` 與 `settings`(解析器會補回被主題刪掉的必備項)。
 */
data class Toolbar(val show: Boolean, val items: List<ToolbarItem>) {
    fun hasAction(verb: ActionVerb): Boolean = items.any { it.tap.verb == verb }
}

/** 行動端候選列。桌面端不消費。 */
data class CandidateBar(
    val style: CandidateStyle,
    val height: Float,
    val background: Int,
    val borderTopWidth: Float,
    val borderTopColor: Int,
    /**
     * 候選列左右兩端的內距（規範草稿 §8.6.4.2）。
     *
     * ⚠ 這個值以前**寫死在渲染端**（`contentPadding = PaddingValues(horizontal = 4.dp)`），
     * 於是規範算 `inner_w` 的公式與畫面實際用的數不是同一組。
     */
    val paddingH: Float,
    /**
     * 右端保留給控制鍵的寬度（規範草稿 §8.6.4.2 / §8.6.6.4）。
     *
     * 預設 40 = **一顆**。實測 411.43 dp 的機器上，80 dp（翻頁＋展開兩顆）
     * 與 40 dp（一顆）的差**恰好是一個候選**（5 vs 6）。
     */
    val reservedEnd: Float,
    val maxVisible: Int,
    val scroll: ScrollMode,
    val expandButton: ExpandButton,
    val showPreeditInline: Boolean,
    val emptyShowsToolbar: Boolean,
    val toolbar: Toolbar
)

data class Shadow(
    val show: Boolean,
    val radius: Float,
    val offsetX: Float,
    val offsetY: Float,
    val color: Int
)

/** 桌面端懸浮候選窗。行動端不消費。 */
data class CandidateWindow(
    val style: CandidateStyle,
    val background: Int,
    val cornerRadius: Float,
    val padding: Float,
    val borderWidth: Float,
    val borderColor: Int,
    val minWidth: Float,
    val maxWidth: Float,
    val placement: Placement,
    val offsetX: Float,
    val offsetY: Float,
    val followCaret: Boolean,
    val backdrop: Backdrop,
    val opacity: Float,
    val shadow: Shadow
)

data class Candidates(
    val shared: CandidateStyle,
    val bar: CandidateBar,
    val window: CandidateWindow,
    val syllables: SyllableBar
)

/**
 * 九宮格拼音消歧欄的**位置**（規範 §8.6.6.3，本端先實作、規範由 macOS 端補）。
 *
 * ── 為什麼位置是「風格」而不是「方案」或「佈局」 ────────────────────────
 * 使用者給的截圖：**iOS 是候選列上方一橫排、三星是左側直欄**，語燕也是左側
 * 直欄。同一個功能、同一個方案、同一份詞庫，位置卻不同 —— 那就是風格的定義。
 */
enum class SyllablePlacement {
    /** 不顯示。方案沒有 `spelling_hints` 時的實際效果也是這個。 */
    NONE,

    /** 候選列上方一橫排（iOS 慣例）。**不需要佈局宣告任何東西**。 */
    ABOVE_CANDIDATES,

    /** 借用佈局某一層宣告的格位，通常是左側直欄（三星 / 語燕慣例）。 */
    KEYBOARD_SLOT,
}

/** 什麼時候出現。`ON_DEMAND` 需要佈局上有 `syllables:toggle` 那顆鍵。 */
enum class SyllableTrigger { WHILE_COMPOSING, ON_DEMAND }

data class SyllableBar(
    val placement: SyllablePlacement,
    val trigger: SyllableTrigger,
    /** 一次最多顯示幾個讀音；0 = 不限。 */
    val maxItems: Int,
    /** `above_candidates` 那一排的高度（dp）。 */
    val height: Float,
)

data class Caret(val show: Boolean, val color: Int, val width: Float, val blink: Boolean)

data class SelectionStyle(val color: Int, val textColor: Int)

data class Preedit(
    val show: Boolean,
    val size: Float,
    val color: Int,
    val background: Int,
    val paddingH: Float,
    val paddingV: Float,
    val cornerRadius: Float,
    val caret: Caret,
    val selection: SelectionStyle
)

/**
 * §8.8.0 的高度模型：**參考格 → 高度預算 → 列高**。
 *
 * 這是本模型的第三版，前兩版都被實機量測否決：
 *
 * * **v1 初稿**「螢幕高 × ratio → 鍵盤高 → 鍵高」：鍵高綁螢幕高、鍵寬綁螢幕寬，
 *   長寬比失控（20:9 螢幕上鍵被拉成 1:1.94）。
 * * **v2**「鍵寬 × key_aspect → 鍵高 → 鍵盤高＝鍵高 × 列數」：長寬比穩了，
 *   但**鍵盤總高隨列數線性成長**——使用者一打開數字列，鍵盤就長高 20%，
 *   而三星實機量到的是四列九宮格與五列全鍵盤**總高只差 1%**
 *   （鍵高 54.4 vs 43.2 dp，也就是總高固定、除以列數）。
 *
 * v3 把因果拆成兩段：
 *
 * 1. **預算**只看裝置寬度與一組固定的「參考格」（`reference_grid`，預設 10 欄
 *    4 列）。`key_aspect` 仍是主控參數，但它描述的是**參考格上那顆鍵**的胖瘦，
 *    與當前佈局的欄數、列數都無關。於是同一台機器上每一份佈局的總高都相同。
 * 2. **當前 layer 把預算分掉**：`列高 = 可用高 / Σweight`。列數多 → 每列矮，
 *    總高不變。這正是三星／Gboard 的行為。
 *
 * **取捨（必須明講）**：總高固定與「鍵長寬比隨欄數自適應」在數學上不可兼得
 * ——固定總高⇒固定列高，欄數一變長寬比就跟著變。本模型選擇總高固定，
 * 因為量測顯示那才是使用者感知的量（鍵盤忽然長高比鍵稍胖稍瘦明顯得多）。
 * 欄數變化仍由**寬度方向**吸收（欄多則鍵窄），這與所有出貨鍵盤一致。
 * 而 v2 擔心的「11 欄注音比 10 欄 QWERTY 瘦長」在本模型下不會發生：注音多
 * 一列，列高同時變矮，實測 w/h 反而比 QWERTY 更胖（0.91 vs 0.78）。
 */
data class KeyGeometry(
    val aspect: Float,
    val keyHeightMin: Float,
    val keyHeightMax: Float,
    /** §8.8.0 的參考格欄數。預算的分母，**不是**當前 layer 的欄數。 */
    val referenceUnits: Float,
    /** §8.8.0 的參考格列數。預算裡放得下幾列。 */
    val referenceRows: Float,
    /** 預算除完之後，實際列高的可用性下界（§8.8.0 第 4 步）。 */
    val rowHeightMin: Float,
    /** 同上的上界，防止極少列的層把鍵撐成長條。 */
    val rowHeightMax: Float,
    val maxScreenRatioPortrait: Float,
    val maxScreenRatioLandscape: Float
) {
    /**
     * 一次算完的結果，供渲染器直接使用。
     *
     * [keyWidth] 是**平均鍵寬**（該列可用寬 / 鍵數），不是「一單位寬」——
     * 競品對照用的長寬比就是用它算的，見 §8.8.0.1。
     * [budgetHeight] 是未經可用性護欄夾制前的預算；[keyboardHeight] 與它不同
     * 就代表護欄生效了（總高不再固定）。
     */
    data class Resolved(
        val keyWidth: Float,
        val keyHeight: Float,
        val keyboardHeight: Float,
        val budgetHeight: Float
    ) {
        /** 護欄有沒有把總高推離預算。 */
        val budgetHonored: Boolean get() = kotlin.math.abs(keyboardHeight - budgetHeight) < 0.01f
    }

    /**
     * 與當前佈局無關的鍵盤總高預算（§8.8.0 第 1–3 步）。
     *
     * 佈局尚未載入時也算得出來，所以渲染器不必再用「10 欄 4 列」去估。
     */
    fun budget(
        widthDp: Float,
        availHeightDp: Float,
        landscape: Boolean,
        padding: EdgeInsets,
        keySpacing: Float,
        rowSpacing: Float,
        heightScale: Float = 1.0f
    ): Float {
        val refUnits = if (referenceUnits <= 0f) 10f else referenceUnits
        val refRows = if (referenceRows <= 0f) 4f else referenceRows

        // 1. 參考鍵寬：只看裝置寬度與參考欄數。
        val innerW = widthDp - padding.left - padding.right - keySpacing * (refUnits - 1f)
        val refKeyW = (if (innerW > 0f) innerW else 0f) / refUnits

        // 2. 參考鍵高：key_aspect 仍是主控，key_height 的上下界把它拉回參考鍵盤那條線。
        val refKeyH = clampFloat(refKeyW * aspect, keyHeightMin, keyHeightMax)

        // 3. 預算＝參考格排滿的高度。height_scale（§9.2）在這裡整體縮放。
        val refGaps = (refRows - 1f).coerceAtLeast(0f)
        val raw = (refKeyH * refRows + rowSpacing * refGaps + padding.top + padding.bottom) *
            heightScale

        // 4. 安全網：鍵盤永遠不得超過螢幕的這個比例（摺疊機、平板橫放、超小螢幕）。
        val ratio = if (landscape) maxScreenRatioLandscape else maxScreenRatioPortrait
        val cap = availHeightDp * ratio
        return if (availHeightDp > 0f && raw > cap) cap else raw
    }

    /**
     * @param widthDp     鍵盤可用寬度（通常是螢幕寬）
     * @param availHeightDp 當前方向下宿主視窗可用高度，只用於安全網
     * @param units       當前 layer 的欄數（§9.3）。**只影響鍵寬，不影響高度。**
     * @param rowsWeight  當前 layer 的 Σ row.weight（不是列數 —— 列可以有不同權重）
     * @param rowCount    當前 layer 的列數，只用來算列間距總和
     * @param keyCount    當前 layer 最寬那一列排出的元素數，用來算平均鍵寬（§9.3）。
     *                    0 = 不知道，退回用 units 當鍵數。
     * @param heightScale 佈局的 `metrics.height_scale`（§9.2）
     */
    fun resolve(
        widthDp: Float,
        availHeightDp: Float,
        landscape: Boolean,
        units: Float,
        rowsWeight: Float,
        rowCount: Int,
        padding: EdgeInsets,
        keySpacing: Float,
        rowSpacing: Float,
        heightScale: Float = 1.0f,
        keyCount: Int = 0,
        refKeySpacing: Float = keySpacing,
        refRowSpacing: Float = rowSpacing
    ): Resolved {
        val safeUnits = if (units <= 0f) 1f else units
        val safeRowsW = if (rowsWeight <= 0f) 1f else rowsWeight
        val gaps = if (rowCount > 1) rowCount - 1 else 0

        // 參考格一律用**主題的**間距，不是佈局覆寫過的（§9.2）。
        // 否則佈局只要調一下 key_spacing，鍵盤總高就跟著變 ——
        // 「同一份主題下任兩份佈局總高相同」這條保證會在最不起眼的地方破功
        // （實測：bopomofo-dachen 的 key_spacing: 4 讓它比別人高 3%）。
        // 佈局的覆寫仍然完整作用在**預算怎麼分**與列內佈版上。
        val budget = budget(
            widthDp, availHeightDp, landscape, padding, refKeySpacing, refRowSpacing, heightScale
        )

        // 4. 當前 layer 把預算分掉。**除以 Σweight，不是列數** —— 數字列的
        //    weight 0.83 必須真的讓那一列矮下來，否則整份佈局會被撐高。
        val chrome = rowSpacing * gaps + padding.top + padding.bottom
        val usable = (budget - chrome).coerceAtLeast(1f)
        var keyH = clampFloat(usable / safeRowsW, rowHeightMin, rowHeightMax)

        // 5. 護欄沒作用時 h 就等於 budget（總高固定）。作用了才會偏離，
        //    那是刻意的：與其給一顆 20dp 高按不到的鍵，不如讓鍵盤高一點。
        var h = keyH * safeRowsW + chrome

        // 5b. 但安全網永遠是最外層的保證。列高下界把 h 推過螢幕比例上限時
        //     （極矮的視窗 + 列數多的佈局），下界讓位 —— 規範說的是
        //     「鍵盤**永遠**不得超過螢幕的這個比例」，那句話不能有例外，
        //     否則摺疊機外螢幕上鍵盤會蓋掉整個畫面。
        val ratio = if (landscape) maxScreenRatioLandscape else maxScreenRatioPortrait
        val cap = availHeightDp * ratio
        if (availHeightDp > 0f && h > cap) {
            keyH = ((cap - chrome) / safeRowsW).coerceAtLeast(1f)
            h = keyH * safeRowsW + chrome
        }

        // 6. 平均鍵寬：§9.3 的列內佈版把 spacing 記在**元素數**上（keys.count − 1），
        //    不是 units − 1。這裡與 §9.3 對齊，兩條公式不再各算各的。
        val n = if (keyCount > 0) keyCount.toFloat() else safeUnits
        val rowInnerW = (widthDp - padding.left - padding.right - keySpacing * (n - 1f))
            .coerceAtLeast(0f)
        val keyW = rowInnerW / n

        return Resolved(
            keyWidth = keyW,
            keyHeight = keyH,
            keyboardHeight = h,
            budgetHeight = budget
        )
    }
}

data class EdgeInsets(val left: Float, val top: Float, val right: Float, val bottom: Float)

data class KeyStyle(
    val background: Int,
    val pressedBackground: Int,
    val foreground: Int,
    val pressedForeground: Int,
    val activeBackground: Int,
    val activeForeground: Int,
    val cornerRadius: Float,
    val borderWidth: Float,
    val borderColor: Int,
    val elevation: Float,
    val font: String,
    val labelSize: Float,
    val hintSize: Float,
    val hintColor: Int,
    val hintPosition: HintPosition,
    val iconSize: Float
)

data class PopupStyle(
    val show: Boolean,
    val background: Int,
    val foreground: Int,
    val highlightBackground: Int,
    val highlightForeground: Int,
    val cornerRadius: Float,
    val itemSize: Float,
    val itemPadding: Float,
    val elevation: Float,
    val maxColumns: Int
)

data class PressPreviewStyle(
    val show: Boolean,
    val background: Int,
    val foreground: Int,
    val size: Float,
    val cornerRadius: Float,
    val elevation: Float
)

/** 僅行動端消費；桌面端實作必須整段忽略。 */
data class Keyboard(
    val background: Int,
    val geometry: KeyGeometry,
    val padding: EdgeInsets,
    val rowSpacing: Float,
    val keySpacing: Float,
    val honorBottomInset: Boolean,
    val keyStyles: Map<String, KeyStyle>,
    val popup: PopupStyle,
    val pressPreview: PressPreviewStyle
) {
    /** §6.3：佈局引用不存在的 style 名 → 退回 `default`。 */
    fun keyStyle(name: String): KeyStyle =
        keyStyles[name] ?: keyStyles["default"] ?: ThemeDefaults.KEY_STYLE
}

data class Motion(
    val enabled: Boolean,
    val respectReduceMotion: Boolean,
    val curve: MotionCurve,
    val keyPressMs: Int,
    val keyReleaseMs: Int,
    val candidateChangeMs: Int,
    val popupMs: Int,
    val windowShowMs: Int
) {
    /** §8.9：系統開啟「減少動態效果」時所有 duration 視為 0。 */
    fun durationOf(base: Int, reduceMotionOn: Boolean): Int =
        if (!enabled || (respectReduceMotion && reduceMotionOn)) 0 else base
}

/** 僅行動端。分類上其實是「行為」而非「外觀」，見規範 §8.10 的說明。 */
data class Feedback(
    val haptic: Boolean,
    val hapticStrength: HapticStrength,
    val sound: Boolean,
    val soundVolume: Float
)

data class Theme(
    val id: String,
    val revision: Int,
    val name: LocalizedString,
    val description: LocalizedString,
    val author: String,
    val license: String,
    val appearance: Appearance,
    val counterpart: String?,
    /** 由根祖先到自身的 id 鏈，供除錯與主題編輯器使用。 */
    val ancestry: List<String>,
    val palette: Map<String, Int>,
    val typography: Typography,
    val metrics: Metrics,
    val candidates: Candidates,
    val preedit: Preedit,
    val keyboard: Keyboard,
    val motion: Motion,
    val feedback: Feedback
)
