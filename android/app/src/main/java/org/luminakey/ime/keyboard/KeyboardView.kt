package org.luminakey.ime.keyboard

import android.content.res.Configuration
import android.view.HapticFeedbackConstants
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.positionInRoot
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.onLongClick
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import kotlinx.coroutines.delay
import org.luminakey.ime.core.RimeStatus
import org.luminakey.ime.prefs.LocalKeyBehavior
import org.luminakey.ime.R
import org.luminakey.ime.theme.SyllablePlacement
import org.luminakey.ime.theme.HintPosition
import org.luminakey.ime.theme.KeyGeometry
import org.luminakey.ime.theme.KeyboardLayout
import org.luminakey.ime.theme.KeyStyle
import org.luminakey.ime.theme.LabelSource
import org.luminakey.ime.theme.LayoutKey
import org.luminakey.ime.theme.LayoutLayer
import org.luminakey.ime.theme.PageIndicatorStyle
import org.luminakey.ime.theme.Popup
import org.luminakey.ime.theme.PopupLayout
import org.luminakey.ime.theme.SubKey
import org.luminakey.ime.theme.Theme
import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.min

/**
 * 軟鍵盤主體：上方候選列 + 下方由 **yaml 佈局**驅動的鍵盤。
 *
 * ⚠ 這個檔案裡沒有任何一個寫死的鍵。所有鍵面文字、送出內容、寬度權重與
 * 層切換都來自 `core/layouts` 下的 yaml；所有顏色與尺寸都來自
 * `core/themes` 下的 yaml。這就是專案主張「跨平台的是配置不是像素」的兌現處。
 *
 * 規範對照（docs/theme-format.md）：
 *   §8.8   鍵盤高度、padding、間距、key_styles
 *   §9.2   佈局層級的 metrics 覆寫
 *   §9.3   佈版演算法（列高依 weight、列內依 width / units）
 *   §9.4   label / hint / send 三者分離
 *   §9.6   觸發解析（點擊 → tap → send → noop）
 */
@Composable
fun RimeKeyboard(
    state: KeyboardUiState,
    onEvent: (KeyboardEvent) -> Unit,
    modifier: Modifier = Modifier,
) {
    val theme = state.theme
    if (theme == null) {
        Box(
            modifier.fillMaxWidth().height(120.dp),
            contentAlignment = Alignment.Center,
        ) { Text(state.busyMessage ?: stringResource(R.string.keyboard_loading_theme)) }
        return
    }

    val config = LocalConfiguration.current
    val scaler = Scaler(
        effective = theme.typography.effectiveScale(config.fontScale),
        system = LocalDensity.current.fontScale,
    )

    /* ── 拼音消歧欄的狀態（見 [T9Syllables]）──────────────────────────────
     *
     * 刻意**整組留在 UI 這一層**，不進 [KeyboardUiState]、不經過 IME service：
     * 「使用者釘住了哪個讀音」不會改變 librime 的任何狀態，它純粹是候選列的
     * 一個檢視條件。放進服務層只會多一條要維護的同步線。
     *
     * ⚠ 組字內容一變，釘住的讀音就過期了 —— 讀音集合是**這一串按鍵**的性質。
     * 不清掉的話，使用者按了下一鍵、候選全換了，畫面卻還照著上一串的讀音篩。
     */
    var pinnedSyllable by remember { mutableStateOf<String?>(null) }
    var syllableOffset by remember { mutableStateOf(0) }
    LaunchedEffect(state.preedit) {
        pinnedSyllable = null
        syllableOffset = 0
    }
    // 問的是**第幾個**音節：已確定幾個，就問下一個。
    val syllableIndex = state.confirmedSyllables.size
    val readings = remember(state.candidates, syllableIndex) {
        T9Syllables.readingsAt(state.candidates, syllableIndex)
    }
    /* ── 位置由**風格**決定（§8.6.6.3）──────────────────────────────────
     *
     * iOS 是候選列上方一橫排、三星與語燕是左側直欄。同一個功能、同一個方案、
     * 同一份詞庫，位置卻不同 —— 那就是風格。使用者的外觀設定是 iPhone 慣例，
     * 所以他會看到上方那一排，而不是左欄。
     *
     * ⚠ **退化規則(一)：`keyboard_slot` 但佈局沒宣告格位 → 退化成
     * `above_candidates`，不得什麼都不畫。** 少一欄的樣子是「畫面照常顯示
     * 標點」，沒有任何東西會叫 —— 那正是這個專案吃過七次虧的形狀。
     * 4 欄舊版九宮格沒有左標點欄，走的就是這一條。
     */
    val syllableStyle = theme.candidates.syllables
    val declaredSlots = T9Syllables.slotKeys(state.layout, state.layerId)
    // 只有一個讀音時**不出現**：點下去什麼都不會發生的東西不該畫出來。
    // 方案沒有 spelling_hints 時 readings 一定是空的，於是整條自然不出現
    // ——那就是退化規則(二)在本端的實際效果。
    //
    // ⚠ **退化規則(三):引擎改寫不了的方案,一樣不出現。** 同一條理由的另一種
    // 形狀。裝置上若還是舊的單編碼 `t9_pinyin`（`scripts/collect_data.sh` 沒跑
    // 過），comment 照樣有 `ni hao`,讀音因此照樣列得出來 —— 但點下去引擎接不
    // 住,前端只能什麼都不做。那就是一排念得出名字、按下去沒反應的鍵。
    // [KeyboardUiState.syllableRewriteReady] 由 IME service 的啟動探針回答。
    val hasReadings = readings.size >= 2 && state.syllableRewriteReady
    val effectivePlacement = when (syllableStyle.placement) {
        SyllablePlacement.NONE -> SyllablePlacement.NONE
        SyllablePlacement.ABOVE_CANDIDATES -> SyllablePlacement.ABOVE_CANDIDATES
        SyllablePlacement.KEYBOARD_SLOT ->
            if (declaredSlots.size >= T9Syllables.MIN_SLOTS) SyllablePlacement.KEYBOARD_SLOT
            else SyllablePlacement.ABOVE_CANDIDATES
    }
    val slotIds =
        if (effectivePlacement == SyllablePlacement.KEYBOARD_SLOT) declaredSlots else emptyList()
    val showAboveRow =
        effectivePlacement == SyllablePlacement.ABOVE_CANDIDATES && hasReadings
    val shownReadings =
        if (syllableStyle.maxItems > 0) readings.take(syllableStyle.maxItems) else readings
    val disambiguating = slotIds.size >= T9Syllables.MIN_SLOTS && hasReadings
    val pin = if (disambiguating) T9Syllables.resolvePin(readings, pinnedSyllable) else null
    val slotCells: Map<String, T9Syllables.Cell> =
        if (!disambiguating) {
            emptyMap()
        } else {
            slotIds.zip(T9Syllables.cells(readings, slotIds.size, syllableOffset)).toMap()
        }

    Column(
        modifier = modifier
            .fillMaxWidth()
            .background(Color(theme.keyboard.background))
            .padding(bottom = bottomInsetDp(theme.keyboard.honorBottomInset)),
    ) {
        if (showAboveRow) {
            SyllableRow(
                readings = shownReadings,
                theme = theme,
                height = syllableStyle.height,
                onPick = { onEvent(KeyboardEvent.SelectSyllable(it)) },
            )
        }
        CandidateBar(
            state = state,
            theme = theme,
            scaler = scaler,
            onEvent = onEvent,
            pinnedSyllable = pin,
        )
        // 面板一律是**浮層**，不是取代品：底下那一列鍵仍然露出來、仍然按得動。
        // 抄的是三星的處理（docs/reference/samsung/photo_5）。理由不是好看 ——
        // 一個把整個鍵盤蓋掉的面板，只要它自己的關閉鍵出了任何差錯，
        // 使用者就沒有第二條路可走。出口永遠看得見，這條規矩對每一個面板都適用。
        Box {
            KeyGrid(
                state = state,
                theme = theme,
                scaler = scaler,
                onEvent = onEvent,
                slotCells = slotCells,
                pinnedSyllable = pin,
                onSlot = { cell ->
                    when (cell) {
                        // 再點一次同一個讀音 = 取消篩選。不另外做一顆「全部」鍵：
                        // 開關就在使用者剛剛按過的那一格上，找得回來。
                        // 真的把輸入串改寫掉，不是把候選藏起來。改寫成功之後
                        // 引擎會給出下一個音節的候選，這一欄跟著換成第二個音節
                        // 的讀音 —— 「選了一個之後讓我選下一個」就是這麼來的。
                        is T9Syllables.Cell.Reading ->
                            onEvent(KeyboardEvent.SelectSyllable(cell.syllable))
                        T9Syllables.Cell.More ->
                            syllableOffset =
                                T9Syllables.nextOffset(readings, slotIds.size, syllableOffset)
                        // 沒被接管的格位走原鍵自己的 onEvent，到不了這裡；
                        // 留著讓 when 維持窮盡。
                        T9Syllables.Cell.Original -> Unit
                    }
                },
            )
            if (state.panel != PanelRoute.NONE) {
                KeyboardPanelHost(state = state, theme = theme, scaler = scaler, onEvent = onEvent)
            }
        }
    }
}

/* ────────────────────────────── 面板 ────────────────────────────── */

/**
 * 依 [KeyboardUiState.panel] 畫出對應的面板。
 *
 * 三種形態：
 *   · **蓋住鍵區、留下底列**（八格、鍵盤類型、外觀、空白鍵）
 *   · **收成頂端一條**（手感、候選字）—— 那兩項非得按得到鍵、看得到候選字不可
 *   · **原地變暗 + 把手**（高度）
 */
@Composable
private fun BoxScope.KeyboardPanelHost(
    state: KeyboardUiState,
    theme: Theme,
    scaler: Scaler,
    onEvent: (KeyboardEvent) -> Unit,
) {
    val style = theme.keyboard.keyStyle("default")
    val panelScaler = PanelScaler { size -> scaler.sp(size) }
    val layout = state.layout
    val layer = state.layer
    val fullHeight = if (layout != null && layer != null) {
        keyboardGeometry(theme, layout, layer).keyboardHeight
    } else {
        nominalKeyboardHeight(theme)
    }
    val panelHeight = if (layout != null && layer != null) {
        panelHeightLeavingBottomRow(theme, layout, layer, fullHeight)
    } else {
        fullHeight
    }
    val close = { onEvent(KeyboardEvent.OpenPanel(PanelRoute.NONE)) }
    val back = { onEvent(KeyboardEvent.PanelBack) }

    when (state.panel) {
        PanelRoute.NONE -> Unit

        PanelRoute.QUICK -> PanelFrame(
            heightDp = panelHeight,
            theme = theme,
            style = style,
            scaler = panelScaler,
            title = stringResource(R.string.panel_title_quick),
            showBack = false,
            onBack = back,
            onClose = close,
            trailing = {
                AllSettingsLink(style, panelScaler) { onEvent(KeyboardEvent.OpenAppSettings) }
            },
            subtitle = stringResource(R.string.panel_subtitle_quick),
        ) {
            QuickPanelContent(state, style, panelScaler, theme, onEvent)
        }

        PanelRoute.TYPES -> PanelFrame(
            heightDp = panelHeight,
            theme = theme,
            style = style,
            scaler = panelScaler,
            title = stringResource(R.string.panel_keyboard_type),
            showBack = true,
            onBack = back,
            onClose = close,
            trailing = {
                AllSettingsLink(style, panelScaler) { onEvent(KeyboardEvent.OpenAppSettings) }
            },
            subtitle = stringResource(R.string.panel_subtitle_types),
        ) {
            SchemaPickerContent(state = state, theme = theme, scaler = scaler, onEvent = onEvent)
        }

        PanelRoute.APPEARANCE -> PanelFrame(
            heightDp = panelHeight,
            theme = theme,
            style = style,
            scaler = panelScaler,
            title = stringResource(R.string.panel_appearance),
            showBack = true,
            onBack = back,
            onClose = close,
        ) {
            AppearancePanelContent(state, style, panelScaler, onEvent)
        }

        PanelRoute.TEXT -> PanelFrame(
            heightDp = panelHeight,
            theme = theme,
            style = style,
            scaler = panelScaler,
            title = stringResource(R.string.panel_group_output),
            showBack = true,
            onBack = back,
            onClose = close,
        ) {
            TextPanelContent(state, style, panelScaler, onEvent)
        }

        PanelRoute.FEEL -> TopStrip(
            theme = theme,
            style = style,
            scaler = panelScaler,
            title = stringResource(R.string.panel_feel),
            onBack = back,
            onClose = close,
        ) {
            FeelStripContent(state, style, panelScaler, theme, onEvent)
        }

        PanelRoute.CANDIDATES -> TopStrip(
            theme = theme,
            style = style,
            scaler = panelScaler,
            title = stringResource(R.string.panel_candidates),
            onBack = back,
            onClose = close,
        ) {
            CandidatesStripContent(state, style, panelScaler, theme, onEvent)
        }

        PanelRoute.HEIGHT -> HeightEditor(
            state = state,
            theme = theme,
            style = style,
            scaler = panelScaler,
            keyboardHeightDp = fullHeight,
            onEvent = onEvent,
        )
    }
}

/**
 * 浮層高度 = 鍵盤高度 − 最後一列。
 *
 * 底列（空白、Enter、退格、中英）必須整列露出來 —— 這是所有鍵盤內面板
 * 「出口結構性存在」的兌現處，不是每個面板各自記得加返回鍵。
 */
internal fun panelHeightLeavingBottomRow(
    theme: Theme,
    layout: KeyboardLayout,
    layer: LayoutLayer,
    keyboardHeight: Float,
): Float {
    val rowSpacing = layout.metrics.rowSpacing ?: theme.keyboard.rowSpacing
    val pad = theme.keyboard.padding
    val weights = layer.rows.fold(0f) { acc, r -> acc + r.weight }
    val usable = keyboardHeight - pad.top - pad.bottom - rowSpacing * (layer.rows.size - 1)
    val lastRow = if (weights > 0f) usable * (layer.rows.lastOrNull()?.weight ?: 0f) / weights else 0f
    return (keyboardHeight - lastRow - rowSpacing - pad.bottom)
        .coerceAtLeast(keyboardHeight * 0.5f)
}

/* ────────────────────────────── 尺寸 ────────────────────────────── */

/**
 * §8.8 的 `honor_bottom_inset`。
 *
 * 系統會在 IME 視窗底部自行畫「收起鍵盤」箭頭、輸入法切換鈕與手勢列。
 * 不讓出這段高度的話，那些東西會直接壓在我們最後一列鍵上面 ——
 * 使用者看到的就是「鍵盤底下多出一條只放了一個小圖示的空白列」，
 * 而且最後一列的鍵有一部分按不到。Gboard 讓出的是約 60dp。
 *
 * 這裡刻意直接向 View 問 inset，而不是用 Compose 的 `navigationBarsPadding()`：
 * IME 視窗的 inset 是否派送給 Compose，取決於 InputMethodService 有沒有呼叫過
 * `setDecorFitsSystemWindows(false)`，那不在本檔案的掌握範圍內；
 * `ViewCompat.getRootWindowInsets()` 兩種情況下都拿得到值。
 */
@Composable
private fun bottomInsetDp(enabled: Boolean): Dp {
    if (!enabled) return 0.dp
    val view = LocalView.current
    val px = ViewCompat.getRootWindowInsets(view)
        ?.getInsets(WindowInsetsCompat.Type.navigationBars())
        ?.bottom ?: 0
    return with(LocalDensity.current) { px.toDp() }
}

/**
 * §4.4 的有效字體縮放。
 *
 * Compose 的 `.sp` 本身會再乘一次系統 fontScale，所以這裡先把它除掉，
 * 讓最終像素數嚴格等於規範的 `size * effective_scale * dp→px 係數`。
 */
private class Scaler(private val effective: Float, private val system: Float) {
    fun sp(size: Float): TextUnit = (size * effective / (if (system > 0f) system else 1f)).sp
}

/**
 * §8.8.0 的高度模型：參考格 → 高度預算 → 列高。
 *
 * 鍵盤總高只由**裝置寬度**與主題的參考格決定，與當前 layer 的欄數、列數都無關；
 * 當前 layer 只負責把預算依 `Σ row.weight` 分掉。所以打開數字列（多一列）
 * 鍵盤不會長高，只是每一列變矮 —— 這是三星實機量到的行為（四列九宮格與
 * 五列全鍵盤總高只差 1%）。
 */
@Composable
private fun keyboardGeometry(
    theme: Theme,
    layout: KeyboardLayout,
    layer: LayoutLayer,
): KeyGeometry.Resolved {
    val config = LocalConfiguration.current
    return theme.keyboard.geometry.resolve(
        widthDp = config.screenWidthDp.toFloat(),
        availHeightDp = config.screenHeightDp.toFloat(),
        landscape = config.orientation == Configuration.ORIENTATION_LANDSCAPE,
        units = layer.units,
        rowsWeight = layer.rows.fold(0f) { acc, r -> acc + r.weight },
        rowCount = layer.rows.size,
        padding = theme.keyboard.padding,
        keySpacing = layout.metrics.keySpacing ?: theme.keyboard.keySpacing,
        rowSpacing = layout.metrics.rowSpacing ?: theme.keyboard.rowSpacing,
        heightScale = layout.metrics.heightScale,
        keyCount = layer.rows.maxOfOrNull { it.keys.size } ?: 0,
        // 預算只看主題，佈局的 §9.2 覆寫不參與 —— 見 KeyGeometry.resolve。
        refKeySpacing = theme.keyboard.keySpacing,
        refRowSpacing = theme.keyboard.rowSpacing,
    )
}

/**
 * 佈局尚未載入時的鍵盤高度。
 *
 * 新模型下這就是**預算本身** —— 不必再假裝一份「10 欄 4 列」的佈局去估，
 * 而且估出來的值與待會真的載進來的那份佈局完全相同，鍵盤不會在載入完成的
 * 那一刻跳一下高度。
 */
@Composable
private fun nominalKeyboardHeight(theme: Theme): Float {
    val config = LocalConfiguration.current
    return theme.keyboard.geometry.budget(
        widthDp = config.screenWidthDp.toFloat(),
        availHeightDp = config.screenHeightDp.toFloat(),
        landscape = config.orientation == Configuration.ORIENTATION_LANDSCAPE,
        padding = theme.keyboard.padding,
        keySpacing = theme.keyboard.keySpacing,
        rowSpacing = theme.keyboard.rowSpacing,
    )
}

/* ────────────────────────────── 候選列 ────────────────────────────── */

@Composable
private fun CandidateBar(
    state: KeyboardUiState,
    theme: Theme,
    scaler: Scaler,
    onEvent: (KeyboardEvent) -> Unit,
    /** 使用者在消歧欄釘住的讀音；null = 不篩。見 [T9Syllables]。 */
    pinnedSyllable: String? = null,
) {
    val bar = theme.candidates.bar
    val style = bar.style
    Column(modifier = Modifier.fillMaxWidth().background(Color(bar.background))) {
        if (bar.borderTopWidth > 0f) {
            Box(
                Modifier
                    .fillMaxWidth()
                    .height(bar.borderTopWidth.dp)
                    .background(Color(bar.borderTopColor))
            )
        }
        Row(
            modifier = Modifier.fillMaxWidth().height(bar.height.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            val notice = state.fatalMessage
                ?: state.busyMessage
                ?: state.configProblem
                ?: if (state.isStub) stringResource(R.string.keyboard_stub_notice) else null

            if (notice != null) {
                Text(
                    text = notice,
                    fontSize = scaler.sp(theme.preedit.size * 0.8f),
                    maxLines = 2,
                    color = Color(style.label.color),
                    modifier = Modifier.weight(1f).padding(horizontal = 8.dp),
                )
                return@Row
            }

            // §8.6.6 的 show_preedit_inline。印的**不是**引擎的 preedit 原文:
            // 九宮格那一格會印出 `MG GAM`（雙編碼方案的代表字母），對使用者
            // 沒有任何意義，而且會讓人以為自己打出來的是那一串。判準與退化
            // 規則見 [InlinePreedit]。
            val inlinePreedit = remember(state.preedit, state.layout, state.layerId) {
                if (!bar.showPreeditInline) null
                else InlinePreedit.forDisplay(
                    state.preedit,
                    InlinePreedit.groupCodeChars(state.layer),
                )
            }
            if (inlinePreedit != null) {
                Text(
                    text = inlinePreedit,
                    fontSize = scaler.sp(theme.preedit.size),
                    color = Color(theme.preedit.color),
                    maxLines = 1,
                    modifier = Modifier.padding(horizontal = theme.preedit.paddingH.dp),
                )
            }

            // 「候選字」面板打開時，候選列**自己就是預覽**：不另畫一條假的示範列，
            // 而是把真的候選列填上樣本字，讓它跟著「一次幾個」「字多大」即時變化。
            // 走的是同一段程式碼，所以看到的就是最後的樣子。
            if (state.candidates.isEmpty() && state.panel == PanelRoute.CANDIDATES) {
                val cap = bar.maxVisible
                val sample = if (cap > 0) SAMPLE_CANDIDATES.take(cap) else SAMPLE_CANDIDATES
                LazyRow(
                    modifier = Modifier.weight(1f).fillMaxHeight(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(style.item.spacing.dp),
                    contentPadding = PaddingValues(horizontal = 4.dp),
                ) {
                    itemsIndexed(sample) { index, word ->
                        Row(
                            modifier = Modifier
                                .clip(RoundedCornerShape(style.item.cornerRadius.dp))
                                .background(
                                    Color(
                                        if (index == 0) style.item.highlightBackground
                                        else style.item.background
                                    )
                                )
                                .padding(
                                    horizontal = style.item.paddingH.dp,
                                    vertical = style.item.paddingV.dp,
                                ),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            Text(
                                text = word,
                                fontSize = scaler.sp(style.text.size),
                                color = Color(
                                    if (index == 0) style.text.highlightColor else style.text.color
                                ),
                            )
                        }
                    }
                }
                return@Row
            }

            if (state.candidates.isEmpty()) {
                if (bar.emptyShowsToolbar) {
                    Toolbar(
                        state = state,
                        theme = theme,
                        scaler = scaler,
                        onEvent = onEvent,
                        modifier = Modifier.weight(1f).fillMaxHeight(),
                    )
                } else {
                    Spacer(Modifier.weight(1f))
                }
                return@Row
            }

            // 消歧欄釘住讀音時只留該讀音的候選。清單裡放的是**引擎的頁內索引**,
            // 不是畫面位置 —— 選字走 rs_select_candidate(index_on_page),兩者一旦
            // 脫鉤,使用者點第二個卻選到第五個,而畫面完全正常。見 [T9Syllables]。
            val shown = remember(state.candidates, pinnedSyllable, state.highlighted) {
                T9Syllables.visibleIndices(state.candidates, pinnedSyllable, state.highlighted)
            }
            LazyRow(
                modifier = Modifier.weight(1f).fillMaxHeight(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(style.item.spacing.dp),
                contentPadding = PaddingValues(horizontal = 4.dp),
            ) {
                itemsIndexed(shown) { _, index ->
                    val candidate = state.candidates[index]
                    val highlighted = index == state.highlighted
                    // 候選字本身念得出來,但少了序號使用者無從說「我要第三個」;
                    // 而「現在停在哪一個」走 stateDescription,選字移動時會重念。
                    // 序號用**引擎索引**而不是畫面位置:篩選之後畫面上的第二個
                    // 仍然是引擎的第五個,念錯的話使用者說出來的指令會落在別處。
                    val candDesc = stringResource(
                        R.string.a11y_candidate, index + 1, candidate.text
                    )
                    val candState = stringResource(R.string.a11y_candidate_current)
                    Row(
                        modifier = Modifier
                            .semantics(mergeDescendants = true) {
                                contentDescription = candDesc
                                role = Role.Button
                                if (highlighted) stateDescription = candState
                            }
                            .clip(RoundedCornerShape(style.item.cornerRadius.dp))
                            .background(
                                Color(
                                    if (highlighted) style.item.highlightBackground
                                    else style.item.background
                                )
                            )
                            .clickable { onEvent(KeyboardEvent.Candidate(index)) }
                            .padding(
                                horizontal = style.item.paddingH.dp,
                                vertical = style.item.paddingV.dp,
                            ),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        if (style.label.show && candidate.label.isNotEmpty()) {
                            Text(
                                text = style.label.render(candidate.label, index),
                                fontSize = scaler.sp(style.label.size),
                                color = Color(
                                    if (highlighted) style.label.highlightColor
                                    else style.label.color
                                ),
                                modifier = Modifier.padding(end = 3.dp),
                            )
                        }
                        Text(
                            text = candidate.text,
                            fontSize = scaler.sp(style.text.size),
                            fontWeight = if (highlighted) FontWeight.SemiBold else FontWeight.Normal,
                            color = Color(
                                if (highlighted) style.text.highlightColor else style.text.color
                            ),
                        )
                        if (style.comment.show && candidate.comment.isNotEmpty()) {
                            Text(
                                text = candidate.comment,
                                fontSize = scaler.sp(style.comment.size),
                                color = Color(
                                    if (highlighted) style.comment.highlightColor
                                    else style.comment.color
                                ),
                                modifier = Modifier.padding(start = 3.dp),
                            )
                        }
                    }
                }
            }

            // §8.6.5 的 page_indicator。規範的預設本來就是 show:true / arrows,
            // 本端一直沒有畫 —— 於是 rs_change_page 與 KeyboardEvent.Page
            // 兩邊都做好了,中間沒有任何東西會送出它:使用者看到的就是
            //「候選只有 5 個,下一頁就沒了」。判準抽在 [Pager]。
            PageArrows(
                state = Pager.state(
                    kind = style.pageIndicator.kind,
                    show = style.pageIndicator.show,
                    pageNo = state.pageNo,
                    isLastPage = state.isLastPage,
                    candidateCount = state.candidates.size,
                ),
                style = style.pageIndicator,
                scaler = scaler,
                onEvent = onEvent,
            )
        }
    }
}

/**
 * 候選列右端的上一頁／下一頁。
 *
 * 兩顆都**永遠留在原地**（不可用時變灰，用 §8.6.5 的 `disabled_color`）:
 * 把不可用的那一顆藏起來，整條候選列會在翻頁的瞬間橫向位移，使用者剛瞄準的
 * 候選就跑掉了。
 */
@Composable
private fun PageArrows(
    state: Pager.State,
    style: PageIndicatorStyle,
    scaler: Scaler,
    onEvent: (KeyboardEvent) -> Unit,
) {
    if (!state.show) return
    val prevDesc = stringResource(R.string.a11y_candidate_prev_page)
    val nextDesc = stringResource(R.string.a11y_candidate_next_page)
    Row(verticalAlignment = Alignment.CenterVertically) {
        PageArrow("‹", prevDesc, state.prevEnabled, style, scaler) {
            onEvent(KeyboardEvent.Page(backward = true))
        }
        PageArrow("›", nextDesc, state.nextEnabled, style, scaler) {
            onEvent(KeyboardEvent.Page(backward = false))
        }
    }
}

@Composable
private fun PageArrow(
    glyph: String,
    description: String,
    enabled: Boolean,
    style: PageIndicatorStyle,
    scaler: Scaler,
    onClick: () -> Unit,
) {
    Box(
        modifier = Modifier
            .fillMaxHeight()
            // 觸控目標要夠大 —— 一個 14sp 的字元只有十幾 dp 寬,那是點不到的。
            .width(40.dp)
            .semantics(mergeDescendants = true) {
                contentDescription = description
                role = Role.Button
            }
            .clickable(enabled = enabled, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = glyph,
            fontSize = scaler.sp(style.size),
            color = Color(if (enabled) style.color else style.disabledColor),
        )
    }
}

/**
 * 無候選時的工具列（§8.6.6 的 `empty_shows_toolbar`、§8.6.6.1 的 `toolbar`）。
 *
 * ── 為什麼這一段不能寫死 ────────────────────────────────────────────────
 * 行動端沒有選單列，所以這條列就是**主要導覽**：切鍵盤、切中英、開設定、
 * 收鍵盤全在這裡。改動前它是寫死的「方案名 + 佈局名」，解析器完整解析出來的
 * `toolbar.items`（連 §8.6.6.1 的必備項補回都做了）一項也沒被畫出來 ——
 * 主題想加一顆表情鍵、想把中英切換移到工具列，都無處可施。
 *
 * ── 就是「沒有 send 的鍵」 ──────────────────────────────────────────────
 * 規範刻意讓工具列項目重用 [LabelSource] 與 [KeyAction]，所以鍵面文字解析
 * （§9.6 的 `label_from` → `icon` → `label`）、圖示退化、active 的觸發條件
 * 都與按鍵共用同一段程式碼（[faceOf]、[isActiveFace]），不另立一套詞彙。
 *
 * `show: false` 時整條列留白 —— 但使用者仍然摸得到 `schema:picker`：
 * 佈局的空白鍵長按、以及設定畫面都是既有的第二條路。
 */
@Composable
private fun Toolbar(
    state: KeyboardUiState,
    theme: Theme,
    scaler: Scaler,
    onEvent: (KeyboardEvent) -> Unit,
    modifier: Modifier = Modifier,
) {
    val bar = theme.candidates.bar
    val toolbar = bar.toolbar
    // 本端還沒實作的動詞不畫出來（理由與另外兩個消費端見 [VerbSupport]）。
    //
    // 過濾放在「畫的時候」而不是「解析的時候」是刻意的:解析出來的 Theme 是
    // 規範的忠實表示,四端要拿它互相對照;「Android 這一版剛好還沒做」是本端的
    // 事實,不該混進去。同理也不從 core/themes/*.yaml 裡把 emoji 刪掉 ——
    // 桌面端做出表情面板時不必反過來把主題改回來。
    val items = toolbar.items.filter { VerbSupport.isImplemented(it.tap.verb) }
    if (!toolbar.show || items.isEmpty()) {
        Spacer(modifier)
        return
    }
    val style = bar.style
    LazyRow(
        modifier = modifier,
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(2.dp),
        contentPadding = PaddingValues(horizontal = 4.dp, vertical = 3.dp),
    ) {
        itemsIndexed(items) { _, item ->
            val face = faceOf(item.labelFrom, item.icon, item.label, state.status)
            val active = isActiveFace(false, item.labelFrom, state.status)
            // 工具列項目就是「沒有 send 的鍵」,朗讀名走同一套規則(見 KeyA11y)。
            // 這裡用 clickable,語意上的點擊動作它自己會帶,所以只要補名字與狀態。
            val itemDesc =
                toolbarItemDescription(item.icon, item.label, item.labelFrom, item.tap)
            val itemState = a11yStateText(item.labelFrom, state.status)
            Box(
                modifier = Modifier
                    .fillMaxHeight()
                    .semantics(mergeDescendants = true) {
                        contentDescription = itemDesc
                        role = Role.Button
                        itemState?.let { stateDescription = it }
                    }
                    .clip(RoundedCornerShape(style.item.cornerRadius.dp))
                    .background(
                        Color(
                            if (active) style.item.highlightBackground else style.item.background
                        )
                    )
                    .clickable { onEvent(KeyboardEvent.Act(item.tap)) }
                    .padding(horizontal = 10.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    text = if (item.labelFrom == LabelSource.INPUT_MODE_PAIR) {
                        inputModeFace(
                            asciiMode = state.status.isAsciiMode,
                            activeColor = style.text.color,
                            idleColor = style.label.color,
                        )
                    } else {
                        AnnotatedString(face)
                    },
                    fontSize = scaler.sp(style.text.size),
                    color = Color(
                        if (active) style.text.highlightColor else style.text.color
                    ),
                    maxLines = 1,
                )
            }
        }
    }
}

/* ──────────────────────────── 鍵盤類型選單 ──────────────────────────── */

/**
 * 「鍵盤類型」選單 —— `schema:picker` 打開的東西。
 *
 * ── 為什麼不是「方案選單」 ──────────────────────────────────────────────
 * 改動前這裡列的是方案，佈局則由 `for_schema` 自動綁定，使用者**碰不到**。
 * 於是 repo 裡九份佈局有八份是使用者永遠看不見的死程式碼：想用九宮格打
 * 朙月拼音、想要一條常駐數字列，都沒有任何入口。
 *
 * 現在列的是（方案 × 佈局）攤平後的清單，照三星鍵盤類型選單的模型：
 * 「拼音全键盘 / 拼音九键」在我們的模型裡是同一個方案配不同佈局，
 * 「双拼 / 笔画 / 五笔」是不同方案，而使用者一項都不必分辨 —— 他只回答
 * 「我要用哪種鍵盤」。清單怎麼算出來的見 [KeyboardTypes]。
 *
 * 選了一項會**同時**換方案與佈局，並把「使用者明確挑過」記進
 * [LayoutHost.pinLayout]，之後的自動切換不再覆蓋它（§9.1.1 的 SHOULD）。
 */
@Composable
private fun SchemaPickerContent(
    state: KeyboardUiState,
    theme: Theme,
    scaler: Scaler,
    onEvent: (KeyboardEvent) -> Unit,
) {
    val style = theme.keyboard.keyStyle("default")
    Column(modifier = Modifier.fillMaxSize()) {
        if (state.keyboardTypes.isEmpty()) {
            Text(
                text = stringResource(R.string.keyboard_no_schema),
                fontSize = scaler.sp(style.labelSize * 0.7f),
                color = Color(style.hintColor),
            )
            return@Column
        }
        // 兩欄，與三星一致。單欄在這個高度只放得下一項半 —— 十幾種鍵盤
        // 塞進一條一項高的縫裡，等於逼使用者盲捲。
        LazyColumn(verticalArrangement = Arrangement.spacedBy(theme.keyboard.rowSpacing.dp)) {
            for (group in state.keyboardTypes) {
                item(key = "group:" + group.title) {
                    Text(
                        // ⚠ 一定要經過 localizedGroupTitle。分組鍵在 [KeyboardTypes]
                        // 裡是寫死的中文代號（那是為了讓純函式測得動），直接畫出來
                        // 就是把「中文（臺灣正體）」印在英文使用者的鍵盤上 ——
                        // 而 App 裡同一份清單顯示的是 "Chinese (Taiwan)"。
                        text = localizedGroupTitle(LocalContext.current, group.title),
                        fontSize = scaler.sp(style.hintSize * 1.15f),
                        color = Color(style.hintColor),
                        maxLines = 1,
                        modifier = Modifier.padding(start = 4.dp, top = 4.dp, bottom = 2.dp),
                    )
                }
                // 先按方案切開再兩兩配對：不切的話一列的左右兩張卡會分屬
                // 兩個方案，而分組標題只到語言層級，使用者得逐張讀第二行
                // 才知道自己在看什麼。
                val rows = group.types
                    .groupBy { it.schemaId }
                    .values
                    .flatMap { it.chunked(2) }
                items(rows, key = { it.first().key }) { pair ->
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(
                            theme.keyboard.keySpacing.dp
                        ),
                    ) {
                        for (type in pair) {
                            KeyboardTypeCard(
                                type = type,
                                current = type.schemaId == state.status.schemaId &&
                                    type.layoutId == state.layout?.id,
                                style = style,
                                scaler = scaler,
                                onEvent = onEvent,
                                modifier = Modifier.weight(1f),
                            )
                        }
                        // 奇數項時右半格留白，不要讓最後一項橫跨整列 ——
                        // 那看起來像一個不同的東西。
                        if (pair.size == 1) Spacer(Modifier.weight(1f))
                    }
                }
            }
        }
    }
}

@Composable
private fun KeyboardTypeCard(
    type: KeyboardType,
    current: Boolean,
    style: KeyStyle,
    scaler: Scaler,
    onEvent: (KeyboardEvent) -> Unit,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier = modifier
            .clip(RoundedCornerShape(style.cornerRadius.dp))
            .background(Color(if (current) style.activeBackground else style.background))
            .clickable {
                onEvent(KeyboardEvent.SelectKeyboardType(type.schemaId, type.layoutId))
            }
            .padding(horizontal = 10.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        // 「自動選擇鍵盤」那張卡的標題同樣是代號不是文案,見 KeyboardTypeLabels。
        val shown = type.localized(LocalContext.current)
        Column(modifier = Modifier.weight(1f)) {
            // 主標題是佈局名 = 使用者眼裡的鍵盤長相；副標題是方案名。
            // 兩者合起來就是那個組合標題：「九宮格拼音 ／ 朙月拼音」。
            // 分兩行而不是串成一行，是因為分組之後同一組裡方案名大量重複，
            // 串成一行會讓每一項的前半截長得一模一樣，反而看不出差別。
            Text(
                text = shown.title,
                fontSize = scaler.sp(style.labelSize * 0.62f),
                color = Color(if (current) style.activeForeground else style.foreground),
                maxLines = 1,
                // 這裡與 §9.6 的鍵面不同，可以省略號化：選單有第二行的方案名
                // 撐著語意，而鍵面上「注音·臺」對使用者是沒有意義的。
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                text = shown.subtitle,
                fontSize = scaler.sp(style.hintSize),
                // 選中的那張卡是 active 配色（多半是強調色底），主題的 hintColor
                // 是配著一般底色調出來的，壓在強調色上會糊掉。選中時改用
                // activeForeground 淡化 —— 對比度由主題自己的那組色保證。
                color = if (current) {
                    Color(style.activeForeground).copy(alpha = 0.75f)
                } else {
                    Color(style.hintColor)
                },
                maxLines = 1,
            )
        }
        if (current) {
            Text(
                text = "✓",
                fontSize = scaler.sp(style.labelSize * 0.62f),
                color = Color(style.activeForeground),
                maxLines = 1,
                modifier = Modifier.padding(start = 4.dp),
            )
        }
    }
}

/* ────────────────────────────── 鍵盤本體 ────────────────────────────── */

private class PopupRequest(
    val popup: Popup,
    /** 相對於 KeyGrid 左上角的錨點（dp）。 */
    val anchorLeft: Float,
    val anchorTop: Float,
    val anchorWidth: Float,
)

@Composable
private fun KeyGrid(
    state: KeyboardUiState,
    theme: Theme,
    scaler: Scaler,
    onEvent: (KeyboardEvent) -> Unit,
    /** 組字中被消歧欄接管的格位（key.id → 要畫什麼）。空 map = 照佈局畫。 */
    slotCells: Map<String, T9Syllables.Cell> = emptyMap(),
    pinnedSyllable: String? = null,
    onSlot: (T9Syllables.Cell) -> Unit = {},
) {
    val layout = state.layout
    val layer: LayoutLayer? = state.layer
    if (layout == null || layer == null) {
        Box(Modifier.fillMaxWidth().height(160.dp), contentAlignment = Alignment.Center) {
            Text(
                text = state.configProblem
                    ?: stringResource(R.string.keyboard_layout_not_loaded),
                color = Color(theme.candidates.shared.text.color),
            )
        }
        return
    }

    // 層切換鍵的朗讀名要用**目標層自己宣告的名字**（見 KeyA11y），
    // 那是佈局作者寫的、已經在地化過的字。
    val layerLabels = remember(layout) {
        layout.layers.associate { it.id to it.label.get(ConfigRepository.LOCALE) }
    }

    val height = keyboardGeometry(theme, layout, layer).keyboardHeight
    // §9.2：佈局的 metrics 覆寫主題的間距（null = 沿用主題）。
    val rowSpacing = layout.metrics.rowSpacing ?: theme.keyboard.rowSpacing
    val keySpacing = layout.metrics.keySpacing ?: theme.keyboard.keySpacing
    val pad = theme.keyboard.padding

    val density = LocalDensity.current
    var gridOrigin by remember { mutableStateOf(Offset.Zero) }
    var gridWidthDp by remember { mutableStateOf(0f) }
    var popupRequest by remember { mutableStateOf<PopupRequest?>(null) }

    Box(
        modifier = Modifier
            .fillMaxWidth()
            .height(height.dp)
            .onGloballyPositioned {
                gridOrigin = it.positionInRoot()
                gridWidthDp = with(density) { it.size.width.toDp().value }
            },
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(
                    start = pad.left.dp,
                    end = pad.right.dp,
                    top = pad.top.dp,
                    bottom = pad.bottom.dp,
                ),
            verticalArrangement = Arrangement.spacedBy(rowSpacing.dp),
        ) {
            for (row in layer.rows) {
                Row(
                    modifier = Modifier.fillMaxWidth().weight(row.weight),
                    horizontalArrangement = Arrangement.spacedBy(keySpacing.dp),
                ) {
                    for (key in row.keys) {
                        // 拼音消歧欄：只換鍵面與行為，幾何一格都不動 ——
                        // 組字途中自己重排的鍵盤比看不到讀音更糟。
                        //
                        // ⚠ **這一格的四個決定不在這裡做。** 鍵面、點下去做
                        // 什麼、長按盤開不開、朗讀名念什麼 —— 四件必須同進
                        // 同出,而它們原本是四個各自判斷 `cell == null` 的
                        // 運算式,可以分岔。分岔的樣子在螢幕上看不出來:一顆
                        // 畫得對、按下去什麼都不做、長按盤也開不出來的標點鍵
                        // （task #78 的孿生兄弟）。
                        //
                        // 四件事現在由 [T9Syllables.renderSlot] 一次算完 ——
                        // 那是純函式,單元測試摸得到;這一層(@Composable)在
                        // 本模組裡沒有任何東西摸得到。KeyGrid 只准照著做,
                        // 由 T9SyllablesTest 的「KeyGrid 不自己決定那一格的
                        // 行為」守著。
                        val slot =
                            T9Syllables.renderSlot(key, slotCells[key.id], pinnedSyllable)
                        val shownKey = slot.key
                        val tapCell = slot.tapCell
                        val speaks = slot.speaks
                        if (shownKey.spacer) {
                            Spacer(Modifier.weight(shownKey.width).fillMaxHeight())
                        } else {
                            KeyView(
                                key = shownKey,
                                theme = theme,
                                scaler = scaler,
                                status = state.status,
                                layerLabels = layerLabels,
                                // 消歧欄不是 §9.5 的動作動詞（它沒有 YAML 表示法），
                                // 所以行為由這一層包起來，而不是發明一個動詞。
                                onEvent =
                                    if (tapCell == null) onEvent else ({ onSlot(tapCell) }),
                                descriptionOverride =
                                    if (speaks == null) null else syllableDescription(speaks),
                                stateOverride = if (slot.pinned) {
                                    stringResource(R.string.a11y_syllable_pinned)
                                } else {
                                    null
                                },
                                onPopup = { left, top, w ->
                                    // 長按盤一律走 slot.popup:被接管的格位沒有盤
                                    // （slotKey 清掉了）,沒被接管的格位**盤要還
                                    // 開得出來** —— 把「，」的長按盤在組字中關掉,
                                    // 就是一顆看得到卻摸不到的鍵。
                                    val p = slot.popup
                                    if (p != null) {
                                        val originDp = with(density) {
                                            Offset(
                                                gridOrigin.x.toDp().value,
                                                gridOrigin.y.toDp().value,
                                            )
                                        }
                                        popupRequest = PopupRequest(
                                            popup = p,
                                            anchorLeft = left - originDp.x,
                                            anchorTop = top - originDp.y,
                                            anchorWidth = w,
                                        )
                                    }
                                },
                                modifier = Modifier.weight(shownKey.width).fillMaxHeight(),
                            )
                        }
                    }
                    // §9.3：Σ width < units 時，剩餘空間留在該列末端。
                    val slack = layer.units - row.widthSum
                    if (slack > 0.01f) Spacer(Modifier.weight(slack).fillMaxHeight())
                }
            }
        }

        val req = popupRequest
        if (req != null) {
            // 點空白處關閉。放在面板之前，面板自己會吃掉自己的點擊。
            Box(Modifier.fillMaxSize().clickable { popupRequest = null })
            PopupPanel(
                request = req,
                theme = theme,
                scaler = scaler,
                gridWidthDp = gridWidthDp,
                onPick = { sub ->
                    popupRequest = null
                    dispatchSubKey(sub, onEvent)
                },
            )
        }
    }
}

/**
 * 消歧欄那幾格念出來是什麼。
 *
 * 與 [KeyA11y] 分開，是因為那一支是**純函式**（它要能被單元測試掃過十二份
 * 佈局的每一顆鍵，不能扛 Robolectric），而這幾顆鍵根本不在佈局裡 ——
 * 它們是執行期依候選讀音長出來的。規則與資源仍然分兩層：這裡只有資源。
 */
@Composable
private fun syllableDescription(cell: T9Syllables.Cell): String? = when (cell) {
    is T9Syllables.Cell.Reading -> stringResource(R.string.a11y_syllable, cell.syllable)
    T9Syllables.Cell.More -> stringResource(R.string.a11y_syllable_more)
    // 沒被接管的格位念的是它原本的名字（「，」「。」「？」），走 KeyA11y
    // 那一條，不會進來這裡；留著讓 when 維持窮盡。
    T9Syllables.Cell.Original -> null
}

private fun dispatchSubKey(sub: SubKey, onEvent: (KeyboardEvent) -> Unit) {
    val tap = sub.tap
    val send = sub.send
    when {
        tap != null -> onEvent(KeyboardEvent.Act(tap))
        send != null -> onEvent(KeyboardEvent.Send(send))
    }
}

@Composable
private fun PopupPanel(
    request: PopupRequest,
    theme: Theme,
    scaler: Scaler,
    gridWidthDp: Float,
    onPick: (SubKey) -> Unit,
) {
    val p = theme.keyboard.popup
    if (!p.show) return
    val keys = request.popup.keys
    val columns = when (request.popup.layout) {
        PopupLayout.GRID -> min(request.popup.columns, p.maxColumns)
        PopupLayout.ROW -> min(keys.size, p.maxColumns)
    }.coerceAtLeast(1)
    val rows = ceil(keys.size / columns.toFloat()).toInt()

    val cell = p.itemSize + p.itemPadding * 2f
    val panelWidth = cell * columns
    val panelHeight = cell * rows
    val left = (request.anchorLeft + request.anchorWidth / 2f - panelWidth / 2f)
        .coerceIn(0f, max(0f, gridWidthDp - panelWidth))
    val top = max(0f, request.anchorTop - panelHeight - 4f)

    Column(
        modifier = Modifier
            .offset { IntOffset(left.dp.roundToPx(), top.dp.roundToPx()) }
            .width(panelWidth.dp)
            .shadow(p.elevation.dp, RoundedCornerShape(p.cornerRadius.dp))
            .clip(RoundedCornerShape(p.cornerRadius.dp))
            .background(Color(p.background)),
    ) {
        for (r in 0 until rows) {
            Row(Modifier.fillMaxWidth()) {
                for (c in 0 until columns) {
                    val idx = r * columns + c
                    if (idx >= keys.size) {
                        Spacer(Modifier.weight(1f).height(cell.dp))
                    } else {
                        val sub = keys[idx]
                        Box(
                            modifier = Modifier
                                .weight(1f)
                                .height(cell.dp)
                                .clickable { onPick(sub) },
                            contentAlignment = Alignment.Center,
                        ) {
                            Text(
                                text = sub.label,
                                fontSize = scaler.sp(p.itemSize),
                                color = Color(p.foreground),
                                maxLines = 1,
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun KeyView(
    key: LayoutKey,
    theme: Theme,
    scaler: Scaler,
    status: RimeStatus,
    layerLabels: Map<String, String>,
    onEvent: (KeyboardEvent) -> Unit,
    onPopup: (left: Float, top: Float, width: Float) -> Unit,
    modifier: Modifier = Modifier,
    /**
     * 蓋掉 [KeyA11y] 算出來的朗讀名。
     *
     * 只給執行期合成的鍵用（目前只有拼音消歧欄）：那些鍵不在任何佈局檔裡，
     * [KeyA11y.nameOf] 只能退回鍵面 —— 「⋯」念成「⋯」，「ni」念成「ni」，
     * 使用者聽不出那是一顆做什麼的鍵。
     */
    descriptionOverride: String? = null,
    /** 同上，蓋掉 stateDescription（例如「已選取」）。 */
    stateOverride: String? = null,
) {
    val view = LocalView.current
    val density = LocalDensity.current
    val style: KeyStyle = theme.keyboard.keyStyle(key.style)
    var pressed by remember { mutableStateOf(false) }
    var anchorLeft by remember { mutableStateOf(0f) }
    var anchorTop by remember { mutableStateOf(0f) }
    var anchorWidth by remember { mutableStateOf(0f) }

    // §8.8.1 的 active 指鎖定狀態：佈局的 `active: true`，或執行期狀態
    // （中英切換鍵在英數模式時）。後者規範沒明說，見回報中的規範缺口。
    val active = isActiveFace(key.active, key.labelFrom, status)

    val background = when {
        pressed -> style.pressedBackground
        active -> style.activeBackground
        else -> style.background
    }
    val foreground = when {
        pressed -> style.pressedForeground
        active -> style.activeForeground
        else -> style.foreground
    }

    val doubleTap = key.doubleTap
    val longPress = key.longPress
    val popup = key.popup

    // 按鍵回饋與重複時間來自使用者偏好(org.luminakey.ime.prefs.KeyBehavior):
    // 震動開關與強度、按鍵音與音量、重複的起始延遲與間隔。
    // 沒有人提供這個 CompositionLocal 時,KeyBehavior.DEFAULT 的行為與本檔
    // 引入偏好之前**完全一致**(震動 KEYBOARD_TAP、無按鍵音、400/60 ms)。
    val behavior = LocalKeyBehavior.current

    // ⚠ `status` 以前是 `pointerInput` 的 key 之一，那是這顆鍵「按下去就變灰、
    // 變不回來」的另一半原因：使用者按下**這一輪組字的第一顆鍵**時，
    // `isComposing` 由 false 翻成 true，key 一變 Compose 就把手勢協程重置掉 ——
    // 手指還按著，回饋就沒了（修好 finally 之前則是永遠卡在按下色）。
    //
    // 而 `status` 根本沒有被手勢邏輯用到：它只影響鍵面文字與 active 色，
    // 那兩者走的是重組，不是手勢。真正需要「隨時是最新的」是這幾個
    // callback，用 rememberUpdatedState 餵進去即可，不必重啟協程。
    //
    // 現在只剩 `key` 是 key：佈局換掉時本來就該重來一次。
    val currentOnEvent by rememberUpdatedState(onEvent)
    val currentOnPopup by rememberUpdatedState(onPopup)
    val currentBehavior by rememberUpdatedState(behavior)

    fun haptic() = currentBehavior.onKeyPress(view)

    /** §9.6 的點擊解析：tap → send → noop。 */
    fun fire() {
        haptic()
        val tap = key.tap
        val send = key.send
        when {
            tap != null -> currentOnEvent(KeyboardEvent.Act(tap))
            send != null -> currentOnEvent(KeyboardEvent.Send(send))
        }
    }

    // 有雙擊／長按／彈出盤的鍵不能在按下當下就出字，否則會與後續手勢打架。
    val fireOnDown = doubleTap == null && longPress == null && popup == null

    // §6.3：repeat 勝過 long_press；解析器已保證兩者不同時存在。
    if (key.repeat && pressed) {
        LaunchedEffect(key, behavior) {
            delay(behavior.repeatDelayMs.toLong())
            while (true) {
                key.send?.let { currentOnEvent(KeyboardEvent.Send(it)) }
                delay(behavior.repeatIntervalMs.toLong())
            }
        }
    }

    var box = modifier.onGloballyPositioned {
        val pos = it.positionInRoot()
        with(density) {
            anchorLeft = pos.x.toDp().value
            anchorTop = pos.y.toDp().value
            anchorWidth = it.size.width.toDp().value
        }
    }
    if (style.elevation > 0f) {
        box = box.shadow(style.elevation.dp, RoundedCornerShape(style.cornerRadius.dp))
    }
    box = box.clip(RoundedCornerShape(style.cornerRadius.dp)).background(Color(background))
    if (style.borderWidth > 0f) {
        box = box.border(
            style.borderWidth.dp,
            Color(style.borderColor),
            RoundedCornerShape(style.cornerRadius.dp),
        )
    }

    // ⚠ 語意層必須自己帶動作,不能只帶名字。
    //
    // 這顆鍵的觸發全部在下面的 `pointerInput` 裡,而 TalkBack 的「輕點兩下」
    // 送的是無障礙的 ACTION_CLICK,**不會**變成 pointer 事件。只補
    // contentDescription 的話,做出來的是一顆念得出名字、聚焦得到、按下去
    // 什麼都不會發生的鍵 —— 正是這個專案抓過五次的那一類缺陷,換一個形式。
    //
    // 所以 onClick 直接呼叫同一個 fire(),長按同理。用 clearAndSetSemantics
    // 而不是 semantics:鍵面上的「⌫」若留在語意樹裡,TalkBack 會把那個字元
    // 連同名字一起念出來。
    val description = descriptionOverride ?: keyDescription(key, layerLabels)
    val spokenState = stateOverride ?: a11yStateText(key.labelFrom, status)
    val typeLabel = stringResource(R.string.a11y_action_type)
    val moreLabel = stringResource(R.string.a11y_action_more)
    val hasLongPress = longPress != null || popup != null

    BoxWithConstraints(
        modifier = box
            .clearAndSetSemantics {
                contentDescription = description
                role = Role.Button
                spokenState?.let { stateDescription = it }
                if (key.tap != null || key.send != null) {
                    onClick(label = typeLabel) { fire(); true }
                }
                if (hasLongPress) {
                    onLongClick(label = moreLabel) {
                        haptic()
                        when {
                            longPress != null -> currentOnEvent(KeyboardEvent.Act(longPress))
                            else -> currentOnPopup(anchorLeft, anchorTop, anchorWidth)
                        }
                        true
                    }
                }
            }
            .pointerInput(key) {
            detectTapGestures(
                onPress = {
                    trackPressed({ pressed = it }) {
                        if (fireOnDown) fire()
                        tryAwaitRelease()
                    }
                },
                onTap = if (fireOnDown) null else ({ _ -> fire() }),
                onDoubleTap = if (doubleTap == null) {
                    null
                } else {
                    { _ -> haptic(); currentOnEvent(KeyboardEvent.Act(doubleTap)) }
                },
                onLongPress = when {
                    key.repeat -> null
                    longPress != null ->
                        ({ _ -> haptic(); currentOnEvent(KeyboardEvent.Act(longPress)) })
                    popup != null ->
                        ({ _ -> haptic(); currentOnPopup(anchorLeft, anchorTop, anchorWidth) })
                    else -> null
                },
            )
        },
        contentAlignment = Alignment.Center,
    ) {
        val face = faceOf(key.labelFrom, key.icon, key.label, status)
        // 圖示與文字用的是兩個不同的基準字級（§8.8.1 的 icon_size / label_size）。
        val base = if (key.icon != null && key.labelFrom == LabelSource.NONE) {
            style.iconSize
        } else {
            style.labelSize
        }
        Text(
            text = if (key.labelFrom == LabelSource.INPUT_MODE_PAIR) {
                inputModeFace(status.isAsciiMode, foreground, style.hintColor)
            } else {
                AnnotatedString(face)
            },
            fontSize = fittedLabelSize(face, base, constraints.maxWidth, scaler),
            color = Color(foreground),
            maxLines = 1,
            textAlign = TextAlign.Center,
        )
        if (key.hint.isNotEmpty() && style.hintPosition != HintPosition.NONE) {
            Text(
                text = key.hint,
                fontSize = scaler.sp(style.hintSize),
                color = Color(style.hintColor),
                maxLines = 1,
                modifier = Modifier
                    .align(
                        when (style.hintPosition) {
                            HintPosition.TOP_RIGHT -> Alignment.TopEnd
                            HintPosition.TOP_CENTER -> Alignment.TopCenter
                            HintPosition.TOP_LEFT -> Alignment.TopStart
                            HintPosition.BOTTOM_RIGHT -> Alignment.BottomEnd
                            HintPosition.NONE -> Alignment.TopEnd
                        }
                    )
                    .padding(horizontal = 3.dp, vertical = 1.dp),
            )
        }
    }
}

/**
 * 鍵面文字。**這是 §9.4 的核心：鍵面文字與送出內容是兩件事。**
 *
 * 優先序：`label_from`（執行期狀態）→ `icon` → `label`。
 * 規範沒有明文規定 `icon` 與 `label_from` 同時存在時誰勝出（空白鍵正是這種鍵），
 * 這裡讓執行期狀態勝出，見回報中的規範缺口。
 *
 * 參數刻意攤成三個而不是收 [LayoutKey]：§8.6.6.1 的工具列項目就是「沒有
 * `send` 的鍵」，它的鍵面解析必須與按鍵**一模一樣**，共用同一支函式才保證
 * 得了。兩邊各寫一次就會各自漂移。
 */
internal fun faceOf(
    labelFrom: LabelSource,
    icon: String?,
    label: String,
    status: RimeStatus,
): String {
    // ⚠ 底下這幾個漢字**刻意不進 strings.xml**。它們不是介面文案,是§9.4 的
    // 鍵面:一顆中文輸入法的「中／En」鍵在英文系統上仍然印「中」,「繁／简」
    // 鍵印的就是那兩個字本身 —— 那是它切換的東西,不是它的說明。四端共用
    // 同一份鍵面才對得起來,翻成 Trad/Simp 反而是把規範改掉。
    val fromStatus = when (labelFrom) {
        LabelSource.NONE -> null
        LabelSource.INPUT_MODE -> if (status.isAsciiMode) INPUT_MODE_LATIN else INPUT_MODE_CJK
        LabelSource.INPUT_MODE_PAIR -> INPUT_MODE_PAIR_TEXT
        LabelSource.SHAPE -> if (status.isFullShape) "全" else "半"
        LabelSource.VARIANT -> if (status.isSimplified) "简" else "繁"
        LabelSource.SCHEMA_NAME -> status.schemaName.ifEmpty { null }
        LabelSource.SCHEMA_ID -> status.schemaId.ifEmpty { null }
    }
    if (fromStatus != null) return fromStatus
    icon?.let { name -> ICONS[name]?.let { return it } }
    return label
}

/**
 * 按下狀態的生命週期：**進去一定出得來**。
 *
 * ── 為什麼這五行值得一支獨立的函式 ──────────────────────────────────────
 * 這裡曾經是直白的三行：
 *
 * ```
 * pressed = true
 * tryAwaitRelease()
 * pressed = false      // ← 有可能一次都不會執行
 * ```
 *
 * `tryAwaitRelease()` 是**懸掛點**，而它所在的手勢協程隨時會被 Compose 取消：
 * `Modifier.pointerInput` 的 key 變了、節點被卸下、或者
 * `LocalViewConfiguration` / `LocalDensity` 換了值 —— 最後這條最狠，
 * 它會對整棵樹的**每一個** `pointerInput` 呼叫 `onViewConfigurationChange()`，
 * 而那支的實作就是 `resetPointerInputHandler()`。取消是以例外的形式從懸掛點
 * 拋出來的，所以寫在它後面的那一行直接被跳過。
 *
 * 跳過的後果不是「少一次動畫」，是**這顆鍵永遠停在按下色**：這個狀態的唯一
 * 擁有者就是這條已經死掉的協程，沒有第二個地方會把它改回來。使用者回報的
 * 「點一下他就變成灰色了，變不回來白色了」就是這麼來的（見
 * KeyPressStateTest 的檔頭，那裡記了完整的因果鏈）。
 *
 * 拆成函式而不是就地補一個 `finally`，是為了讓這件事**測得到**：
 * 渲染需要整個 Compose 執行期，這條規則不需要。
 */
internal suspend fun trackPressed(
    setPressed: (Boolean) -> Unit,
    awaitRelease: suspend () -> Unit,
) {
    setPressed(true)
    try {
        awaitRelease()
    } finally {
        // 取消路徑也要走到這裡，所以不能寫在 try 之後。
        setPressed(false)
    }
}

/**
 * §8.8.1 的 active：佈局宣告的鎖定，或執行期狀態（英數模式的中／英鍵）。
 *
 * [LabelSource.INPUT_MODE_PAIR] **刻意不**列入：那顆鍵已經在鍵面上同時畫出
 * 兩態並強調了當前那一態，再把整顆鍵染成 accent 色只是重複同一個訊息，
 * 而且會讓「英文模式」看起來像「這顆鍵被鎖住了」。
 */
internal fun isActiveFace(declared: Boolean, labelFrom: LabelSource, status: RimeStatus): Boolean =
    declared || (labelFrom == LabelSource.INPUT_MODE && status.isAsciiMode)

/* ─────────────────── 中／En：同時畫出兩態 ─────────────────── */

/** 中文那一段。 */
internal const val INPUT_MODE_CJK = "中"

/** 英數那一段。用 `En` 而不是 `英`：一邊漢字一邊拉丁，兩態一眼就分得開。 */
internal const val INPUT_MODE_LATIN = "En"

internal const val INPUT_MODE_SEPARATOR = "/"

/** 量測與無樣式回落用的純文字形態。 */
internal const val INPUT_MODE_PAIR_TEXT =
    INPUT_MODE_CJK + INPUT_MODE_SEPARATOR + INPUT_MODE_LATIN

/**
 * 把「中/En」畫成兩段：當前那一態用鍵面前景色 + 粗體，另一態退到 hint 色。
 *
 * ── 為什麼非得兩段不可 ──────────────────────────────────────────────────
 * 使用者回報：「這裡的『中』要修改成『中/en』，這樣別人才知道你現在的語言
 * 是啥。」只寫一個字的切換鍵有兩種讀法 —— 「現在是中文」與「按了會變中文」
 * —— 而它們指向相反的操作。同時畫出兩態、強調其中一態，這個歧義**在結構上
 * 就不存在**，不必靠使用者猜。三星實機是同一個作法（中/En，斜線分隔）。
 */
internal fun inputModeFace(
    asciiMode: Boolean,
    activeColor: Int,
    idleColor: Int,
): AnnotatedString {
    val on = SpanStyle(color = Color(activeColor), fontWeight = FontWeight.Bold)
    val off = SpanStyle(color = Color(idleColor))
    return buildAnnotatedString {
        withStyle(if (asciiMode) off else on) { append(INPUT_MODE_CJK) }
        withStyle(off) { append(INPUT_MODE_SEPARATOR) }
        withStyle(if (asciiMode) on else off) { append(INPUT_MODE_LATIN) }
    }
}

/**
 * §9.6「鍵面文字放不下時」的下限：`label_size × 0.5`。
 */
internal const val MIN_LABEL_SHRINK = 0.5f

/**
 * §9.6：解析出來的鍵面文字放不下時，**等比縮小字級**求完整顯示。
 *
 * ── 改動前是怎麼壞的 ────────────────────────────────────────────────────
 * 舊碼寫的是 `face.length > 2 → labelSize × 0.55`：依**字數**縮放，
 * 而不是依鍵寬。三個後果都看得見：
 *
 *   · 同一列裡「分詞」（2 字）用滿級數、「abc」（3 字）只剩 55%，視覺跳動；
 *   · 中／英切換鍵的鍵面只有一個字，卻因為 `schema_name` 這類長字串共用
 *     同一條規則而顯得毫無道理；
 *   · 真正放不下的長字串（「注音·臺灣正體」7 字）跟「abc」縮得一樣多，
 *     該縮的沒縮夠，不該縮的縮過頭。
 *
 * ── 現在的作法 ──────────────────────────────────────────────────────────
 * 用 [rememberTextMeasurer] 在基準字級下量一次實際文字寬度，與這顆鍵**實際
 * 分配到的寬度**（[BoxWithConstraints] 的 `constraints.maxWidth`）相比，
 * 放不下才按比例縮，並夾在規範的 `× 0.5` 下限。放得下的一律用滿級數 ——
 * 所以「分詞」「abc」「中」現在全部同級數，因為它們**全都放得下**。
 *
 * 量測結果以 `remember` 快取，key 是（文字、字級、可用寬度）：
 * 同一顆鍵在狀態不變時不會重複量。
 */
@Composable
private fun fittedLabelSize(
    text: String,
    baseSize: Float,
    availableWidthPx: Int,
    scaler: Scaler,
): TextUnit {
    val base = scaler.sp(baseSize)
    if (text.isBlank() || availableWidthPx <= 0) return base
    val measurer = rememberTextMeasurer()
    // 左右各留一點餘裕，否則字會貼著鍵的圓角邊緣。
    val usable = availableWidthPx - with(LocalDensity.current) { LABEL_INSET_DP.dp.toPx() } * 2f
    if (usable <= 0f) return base
    val measured = remember(text, base, measurer) {
        measurer.measure(
            text = AnnotatedString(text),
            style = TextStyle(fontSize = base),
            maxLines = 1,
            softWrap = false,
        ).size.width.toFloat()
    }
    val ratio = shrinkRatio(measured, usable)
    return if (ratio >= 1f) base else scaler.sp(baseSize * ratio)
}

private const val LABEL_INSET_DP = 2f

/**
 * 量到的寬度 → 該用幾成字級。純算術，與 Compose 無關，方便直接測。
 *
 * 放得下回 1（**不放大**：規範只說縮小），放不下按比例縮並夾在
 * [MIN_LABEL_SHRINK]；到了下限仍放不下就讓它截斷（§9.6 允許最後才截斷）。
 */
internal fun shrinkRatio(measuredPx: Float, availablePx: Float): Float {
    if (measuredPx <= 0f || availablePx <= 0f) return 1f
    if (measuredPx <= availablePx) return 1f
    return (availablePx / measuredPx).coerceAtLeast(MIN_LABEL_SHRINK)
}

/**
 * §9.6 的語義圖示。規範要求「渲染器自繪向量」（SHOULD），本輪仍以規範的
 * **替代字形表**代替 —— 那張表是規範性的，四端必須用同一份，不得自行挑選。
 *
 * ⚠ 仍未實作的是退化路徑的第 3、4 步（字形缺字時退回 `label`、`label` 也空
 * 就畫空白）：Compose 沒有現成的「這個字形在當前字體裡有沒有」查詢。
 * 已記在回報中。
 */
private val ICONS: Map<String, String> = mapOf(
    "backspace" to "⌫",
    "enter" to "↵",
    "shift" to "⇧",
    "shift_lock" to "⇪",
    // 規範刻意讓空白鍵退化為空白：`␣` 在多數字體裡是方框，比空白更糟。
    "space" to " ",
    "globe" to "🌐",
    "keyboard_hide" to "⌄",
    "settings" to "⚙",
    "emoji" to "☺",
    "search" to "⌕",
    "go" to "↵",
    "done" to "↵",
    "next" to "↵",
    "clipboard" to "❐",
    "undo" to "↶",
    "mic" to "🎤",
    "arrow_left" to "←",
    "arrow_right" to "→",
    "arrow_up" to "↑",
    "arrow_down" to "↓",
)

/**
 * 候選列**上方**那一排讀音（iOS 慣例）。
 *
 * 與左側直欄是同一個功能的兩種位置，走的也是同一個事件
 * （[KeyboardEvent.SelectSyllable] → 改寫引擎的輸入串），所以「選了一個之後
 * 讓我選下一個」在兩種風格底下的行為完全一樣，差的只有畫在哪裡。
 *
 * ⚠ 這一排是**加在鍵盤之上**的，不吃候選列的高度：吃掉候選列會讓候選在組字
 * 途中忽然變矮又變回來，而使用者正在那一列上點字。
 */
@androidx.compose.runtime.Composable
private fun SyllableRow(
    readings: List<String>,
    theme: org.luminakey.ime.theme.Theme,
    height: Float,
    onPick: (String) -> Unit,
) {
    val style = theme.candidates.bar.style
    androidx.compose.foundation.layout.Row(
        modifier = androidx.compose.ui.Modifier
            .fillMaxWidth()
            .height(height.dp)
            .background(androidx.compose.ui.graphics.Color(theme.candidates.bar.background)),
        verticalAlignment = androidx.compose.ui.Alignment.CenterVertically,
    ) {
        readings.forEach { syllable ->
            androidx.compose.foundation.layout.Box(
                modifier = androidx.compose.ui.Modifier
                    .padding(horizontal = 12.dp)
                    // 每一格都要是真的按鈕：少了 semantics，TalkBack 使用者
                    // 會念得出來卻按不到，那又是一個「看得到摸不到」。
                    .clickable { onPick(syllable) },
                contentAlignment = androidx.compose.ui.Alignment.Center,
            ) {
                androidx.compose.foundation.text.BasicText(
                    text = syllable,
                    style = androidx.compose.ui.text.TextStyle(
                        color = androidx.compose.ui.graphics.Color(style.label.color),
                        fontSize = androidx.compose.ui.unit.TextUnit(
                            style.label.size,
                            androidx.compose.ui.unit.TextUnitType.Sp,
                        ),
                    ),
                )
            }
        }
    }
}
