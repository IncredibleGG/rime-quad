package org.rimequad.ime.theme

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

data class ItemStyle(
    val paddingH: Float,
    val paddingV: Float,
    val spacing: Float,
    val cornerRadius: Float,
    val minWidth: Float,
    val background: Int,
    val highlightBackground: Int,
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
    val window: CandidateWindow
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

data class HeightSpec(val ratio: Float, val min: Float, val max: Float) {
    /**
     * §8.8 的鍵盤高度演算法。[heightScale] 來自佈局的 `metrics.height_scale`，
     * 在夾制之後才乘上去，然後再夾一次到 [min, 1.5 * max]。
     */
    fun resolve(availableDp: Float, heightScale: Float = 1.0f): Float {
        val base = clampFloat(availableDp * ratio, min, max)
        return clampFloat(base * heightScale, min, max * 1.5f)
    }
}

data class KeyboardHeight(val portrait: HeightSpec, val landscape: HeightSpec)

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
    val height: KeyboardHeight,
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
