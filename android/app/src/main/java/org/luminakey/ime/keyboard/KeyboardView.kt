package org.luminakey.ime.keyboard

import android.content.res.Configuration
import android.util.Log
import android.view.HapticFeedbackConstants
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
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
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.LocalTextStyle
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
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
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
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import kotlinx.coroutines.delay
import org.luminakey.ime.core.RimeStatus
import org.luminakey.ime.core.FeedbackPlan
import org.luminakey.ime.prefs.LocalKeyBehavior
import org.luminakey.ime.R
import org.luminakey.ime.theme.SyllablePlacement
import org.luminakey.ime.theme.Diagnostic
import org.luminakey.ime.theme.DiagnosticCode
import org.luminakey.ime.theme.DiagnosticText
import org.luminakey.ime.theme.HighlightStyle
import org.luminakey.ime.theme.HintPosition
import org.luminakey.ime.theme.KeyGeometry
import org.luminakey.ime.theme.KeyboardLayout
import org.luminakey.ime.theme.KeyStyle
import org.luminakey.ime.theme.LabelSource
import org.luminakey.ime.theme.LayoutKey
import org.luminakey.ime.theme.ExpandButton
import org.luminakey.ime.theme.LayoutLayer
import org.luminakey.ime.theme.PageIndicatorStyle
import org.luminakey.ime.theme.Popup
import org.luminakey.ime.theme.PopupLayout
import org.luminakey.ime.theme.SendSpec
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
    /* ── 候選列展開（§8.6.6 的 `scroll: expandable`，見 [Expander]）────────
     *
     * 與釘住讀音同一條理由，整組留在 UI 這一層：展不展開不改變 librime 的
     * 任何狀態，它只是候選列的一個檢視條件。
     */
    var candidatesExpanded by remember { mutableStateOf(false) }
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
    /* ⚠ **候選列與展開面板必須拿到同一份索引清單。**
     * 這裡放的是**引擎的頁內索引**，不是畫面位置 —— 選字走
     * `rs_select_candidate(index_on_page)`。兩邊各算一次的話，消歧欄一篩，
     * 展開面板上的第 2 個就會與候選列上的第 2 個是不同的字，
     * 而畫面上完全看不出來。見 [Expander] 的檔頭。
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
    /* ── 消歧欄那一側是**活的**嗎（§8.6.3.1）───────────────────────────────
     *
     * 候選列壓掉註解的正當性只有一條:同一份讀音消歧欄已經畫過了。
     * 消歧欄畫不出來的時候壓註解 = 把讀音憑空刪掉,而沒有任何東西補上。
     *
     * ⚠ 這裡刻意**不含** `readings.size >= 2`（即不用 [hasReadings]）:
     *   讀音從 2 收斂到 1 是使用者挑字的每一步都會發生的事,算進來就等於
     *   「消歧欄收起來的同一瞬間整列候選重排」。門檻的不對稱是刻意的,
     *   見 [CandidateDensity.commentVisible]。
     *
     * ⚠ 也刻意不看 `declaredSlots`:`keyboard_slot` 在格位不夠時會**退化成**
     *   `above_candidates`（退化規則一）,那時候消歧欄照樣畫得出來。
     *   看 [effectivePlacement] 而不是主題原文,兩者差的正是這一格。
     */
    val syllableSideAlive =
        effectivePlacement != SyllablePlacement.NONE && state.syllableRewriteReady
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
        val shownCandidates = remember(state.candidates, pin, state.highlighted) {
            T9Syllables.visibleIndices(state.candidates, pin, state.highlighted)
        }
        val expand = Expander.state(
            mode = theme.candidates.bar.scroll,
            showButton = theme.candidates.bar.expandButton.show,
            candidateCount = shownCandidates.size,
            wanted = candidatesExpanded,
        )
        CandidateBar(
            state = state,
            theme = theme,
            scaler = scaler,
            onEvent = onEvent,
            shown = shownCandidates,
            expand = expand,
            syllableSideAlive = syllableSideAlive,
            onToggleExpand = { candidatesExpanded = !candidatesExpanded },
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
                selectableCandidates = remember(shownCandidates) { shownCandidates.toSet() },
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
            if (expand.expanded) {
                CandidateExpandedPanel(
                    state = state,
                    theme = theme,
                    scaler = scaler,
                    shown = shownCandidates,
                    onPick = {
                        candidatesExpanded = false
                        onEvent(KeyboardEvent.Candidate(it))
                    },
                    onPage = { backward ->
                        onEvent(KeyboardEvent.Page(backward = backward))
                    },
                )
            }
            if (state.panel != PanelRoute.NONE) {
                KeyboardPanelHost(state = state, theme = theme, scaler = scaler, onEvent = onEvent)
            }
        }
    }
}

/**
 * 候選列右端的展開／收合鍵（§8.6.6 的 `expand_button`）。
 *
 * 與翻頁鍵一樣給滿 40dp 的觸控目標:一個 18sp 的字元只有十幾 dp 寬,點不到。
 */
@Composable
private fun ExpandButton(
    expanded: Boolean,
    button: ExpandButton,
    scaler: Scaler,
    onClick: () -> Unit,
) {
    val desc = stringResource(
        if (expanded) R.string.a11y_candidates_collapse else R.string.a11y_candidates_expand
    )
    Box(
        modifier = Modifier
            .fillMaxHeight()
            .width(CANDIDATE_BAR_BUTTON_DP.dp)
            .semantics(mergeDescendants = true) {
                contentDescription = desc
                role = Role.Button
            }
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = if (expanded) "\u2303" else "\u2304",
            fontSize = scaler.sp(button.size),
            color = Color(button.color),
        )
    }
}

/**
 * 展開之後的多列候選面板。
 *
 * ⚠ **畫的是當前這一頁,不是全部攤平。** `rs_select_candidate` 吃的是頁內
 * 索引,攤平之後畫面上的第 6 個對引擎而言是下一頁的第 1 個 —— 使用者點下去
 * 會上屏別的字,而畫面完全正常。要看後面的,走底下那兩顆翻頁鍵
 * (`rs_change_page`)。理由與實測見 [Expander] 的檔頭。
 *
 * 與其他面板同一條規矩:**浮層,底列的鍵仍然露出來**。面板自己的收合鍵
 * 出任何差錯時,使用者都還有第二條路。
 */
@Composable
private fun BoxScope.CandidateExpandedPanel(
    state: KeyboardUiState,
    theme: Theme,
    scaler: Scaler,
    shown: List<Int>,
    onPick: (Int) -> Unit,
    onPage: (Boolean) -> Unit,
) {
    val bar = theme.candidates.bar
    val style = bar.style
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
    BoxWithConstraints(
        modifier = Modifier
            .align(Alignment.TopStart)
            .fillMaxWidth()
            .height(panelHeight.dp)
            .background(Color(bar.background)),
    ) {
        // 一列放幾個由**量到的寬度**決定,不寫死。項寬以最長的那一項估
        // (CJK 一個字約一個字身),估寬了只是少排一欄,估窄了才會切字。
        //
        // ⚠ 估的必須是**畫得出來的那些字**。第一版把 comment 也算進去,而
        //   面板根本不畫 comment ——「你好」被當成「你好 ni hao」去估,一列
        //   於是只排得下 2 欄,9 個候選排成 5 列、擠掉最後一個與翻頁鍵。
        //   估寬公式與渲染內容一旦分家,症狀就是「畫面莫名其妙少一格」。
        val labelShown = CandidateDensity.labelVisible(
            style.label.show,
            CandidateDensity.selectionDigitUsable(layer, layout?.id, state.status.schemaId),
        )
        val longest = shown.maxOfOrNull { i -> state.candidates[i].text.length } ?: 1
        val longestLabel = if (labelShown) {
            shown.maxOfOrNull { i -> style.label.render(state.candidates[i].label, i).length } ?: 0
        } else {
            0
        }
        // 估寬走與候選列**同一個**函式。兩處各寫一份公式正是上一版踩過的坑
        //（面板把 comment 算進估寬，而它根本不畫 comment，於是一列少一欄）。
        val itemDp = CandidateDensity.itemWidthDp(
            textChars = longest,
            textSize = scaler.scaled(style.text.size),
            labelChars = longestLabel,
            labelSize = scaler.scaled(style.label.size),
            // ⚠ 面板**不畫** comment，所以估寬也不准把它算進去。
            commentChars = 0,
            commentSize = 0f,
            paddingH = style.item.paddingH,
            minWidth = style.item.minWidth,
        ) + style.item.spacing
        val perRow = Expander.perRow(maxWidth.value, itemDp)
        val rows = Expander.rows(shown, perRow)
        Column(modifier = Modifier.fillMaxSize()) {
          // 列數乘上列高有可能超過面板 —— 那時候要捲，**不是**讓最後幾列
          // 連同翻頁鍵一起被切掉。翻頁鍵留在捲動區外面:它是「看後面那一頁」
          // 的唯一入口,不可以捲不到就消失。
          Column(
            modifier = Modifier.weight(1f).fillMaxWidth().verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(2.dp),
          ) {
            for (row in rows) {
                Row(
                    modifier = Modifier.fillMaxWidth().height(bar.height.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(style.item.spacing.dp),
                ) {
                    for (index in row) {
                        val candidate = state.candidates[index]
                        val highlighted = index == state.highlighted
                        val ink = CandidateInk.of(
                            style.item, style.text, style.label, style.comment, highlighted
                        )
                        // 念出來的序號用**引擎索引**,與候選列一致 ——
                        // 兩處念不一樣的話,使用者說「第三個」會落在別處。
                        val candDesc = stringResource(
                            R.string.a11y_candidate, index + 1, candidate.text
                        )
                        Row(
                            modifier = Modifier
                                .weight(1f)
                                // §8.6.4.2／ui-design §3.6 的觸控下界，與候選列
                                // 同一條（那一側寫在 LazyRow 的每一項上）。
                                //
                                // ⚠ **這一條在隨附主題底下今天不會生效，寫出來是
                                //   為了不讓它變成下一次的洞。** 面板這一側的格寬
                                //   是 `weight(1f)` 平分出來的，而 `perRow` 由
                                //   `Expander.perRow(maxWidth, itemDp)` 決定、
                                //   `itemDp ≥ item.min_width + spacing`，所以
                                //   平分之後每一格必然 ≥ `min_width`（實測 411 dp
                                //   寬、perRow=5 → 79 dp/格）。
                                //   會低於下界的是**第三方主題**：`candidates.bar.item`
                                //   的 `min_width` 由主題給，把它設成 0 的話估寬就
                                //   只剩文字寬＋內距，`perRow` 於是頂到 5 而每一格
                                //   縮到文字寬。那時候這一行才是唯一擋著的東西。
                                .widthIn(min = style.item.minWidth.dp)
                                .fillMaxHeight()
                                .semantics(mergeDescendants = true) {
                                    contentDescription = candDesc
                                    role = Role.Button
                                }
                                .clip(RoundedCornerShape(style.item.cornerRadius.dp))
                                .background(Color(ink.background))
                                .then(
                                    if (ink.borderWidth > 0f) {
                                        Modifier.border(
                                            ink.borderWidth.dp,
                                            Color(ink.borderColor),
                                            RoundedCornerShape(style.item.cornerRadius.dp),
                                        )
                                    } else {
                                        Modifier
                                    }
                                )
                                .clickable { onPick(index) }
                                .padding(horizontal = style.item.paddingH.dp),
                            verticalAlignment = Alignment.CenterVertically,
                        ) {
                            if (labelShown && candidate.label.isNotEmpty()) {
                                Text(
                                    text = style.label.render(candidate.label, index),
                                    fontSize = scaler.sp(style.label.size),
                                    color = Color(ink.label),
                                    modifier = Modifier.padding(end = CandidateDensity.GAP_DP.dp),
                                )
                            }
                            Text(
                                text = candidate.text,
                                fontSize = scaler.sp(style.text.size),
                                maxLines = 1,
                                color = Color(ink.text),
                                modifier = if (ink.underline != null) {
                                    Modifier.drawBehind {
                                        val h = 2.dp.toPx()
                                        drawRect(
                                            color = Color(ink.underline),
                                            topLeft = Offset(0f, size.height - h),
                                            size = Size(size.width, h),
                                        )
                                    }
                                } else {
                                    Modifier
                                },
                            )
                        }
                    }
                    // 最後一列不滿時補空位,免得剩下的那幾個被拉成整列寬 ——
                    // 那看起來像另一種東西。
                    repeat(perRow - row.size) { Spacer(Modifier.weight(1f)) }
                }
            }
          }
            // 翻頁留在面板裡:展開的是**這一頁**,要看後面的得換頁。
            Row(
                modifier = Modifier.fillMaxWidth().height(bar.height.dp),
                horizontalArrangement = Arrangement.End,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                PageArrows(
                    // ⚠ `candidateCount` 傳的是**面板真的畫出來的那幾個**
                    //   (`shown`,已經過 T9Syllables.visibleIndices 篩選),不是
                    //   未篩的整頁 `state.candidates`。那個參數的 KDoc 從第一天
                    //   就寫著「這一頁畫得出來的候選數」,而這個呼叫端一直傳
                    //   整頁的數量 —— 上一輪只修好了候選列那一個呼叫端,
                    //   這一個原封不動。
                    // ⛔ **不吃 `style.pageIndicator.show` / `.kind`。** 那個開關關的是
                    //   候選列右端那一組;面板裡這一列是面板自己的唯一導覽,被同一個
                    //   開關關掉的話,`rightEnd` 好不容易推過來的這條路就通不到第 2 頁,
                    //   而 `deadEnd()` 只看右端、永遠不會紅。見 [Pager.panelState]。
                    state = Pager.panelState(
                        pageNo = state.pageNo,
                        isLastPage = state.isLastPage,
                        shownCount = shown.size,
                    ),
                    style = style.pageIndicator,
                    scaler = scaler,
                    onEvent = { ev -> if (ev is KeyboardEvent.Page) onPage(ev.backward) },
                )
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

    /**
     * 同一個字級在**版面上**佔多少 dp。
     *
     * 密度計算（[CandidateDensity]）用的必須是這個數，不是主題檔裡的原始值：
     * 使用者把系統字級調到 1.3 之後，一格真的變寬了，而「一列排得下幾個」
     * 跟著要變少。拿原始值去算的話，字級一調大，畫面上的候選就開始被切掉，
     * 而右端仍然自信地畫著翻頁鍵。
     */
    fun scaled(size: Float): Float = size * effective
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
    /**
     * 要畫哪幾個候選 —— 放的是**引擎的頁內索引**，由呼叫端算好。
     * 不在這裡算，是因為展開面板要畫的是同一份；兩邊各算一次就會分岔。
     */
    shown: List<Int>,
    /** 展開鍵的狀態，見 [Expander]。 */
    expand: Expander.State,
    /**
     * 消歧欄那一側現在畫得出來嗎（`placement != none && syllableRewriteReady`）。
     * 這是「壓掉註解」的**前提**：畫不出來就沒有第二份讀音，
     * 壓掉等於刪掉。見 [CandidateDensity.commentVisible]。
     */
    syllableSideAlive: Boolean,
    onToggleExpand: () -> Unit,
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
                    contentPadding = PaddingValues(horizontal = bar.paddingH.dp),
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

            /* ── §8.6.3.1／§8.6.1.1：這一輪要不要畫註解與序號 ─────────────
             *
             * 兩者都是**每一格都要付**的寬度，而它們今天各自解決的問題都已經
             * 有別的東西在解決：
             *   · 註解 = 消歧欄的第二份（同一個 `candidate.comment` 欄位）
             *   · 序號 = 沒有數字鍵的佈局上，一段按不到的文字
             * 判準抽在 [CandidateDensity]，這裡只負責接上去。
             */
            val readings = remember(state.candidates) {
                T9Syllables.readingsOf(state.candidates)
            }
            // ⚠ 壓掉註解的前提是**消歧欄那一側畫得出來**。上一版只看 readings,
            //   於是 `placement: none` 與「啟動探針還沒回答」兩種狀態下讀音
            //   一份都看不到 —— 那是把資訊憑空刪掉。見 [CandidateDensity.commentVisible]。
            val commentShown = CandidateDensity.commentVisible(
                style.comment.show, readings, syllableSideAlive
            )
            // ⚠ 序號的判準吃**三個**東西：這一層、這一份佈局、現在的方案。
            //   少了後兩個就會做出「畫面上有 1 2 3、按 3 卻把使用者打好的組字
            //   毀掉」的序號（`cn-t9-pinyin-numrow` ＋ `t9_pinyin` 實測）。
            //   fail-closed：查不到就不畫。見 [CandidateDensity.selectionDigitUsable]。
            val labelShown = remember(state.layer, state.layout?.id, state.status.schemaId, style.label.show) {
                CandidateDensity.labelVisible(
                    style.label.show,
                    CandidateDensity.selectionDigitUsable(
                        state.layer, state.layout?.id, state.status.schemaId
                    ),
                )
            }

            /* ── §8.6.4.2／§8.6.6.4：一列排得下幾個，右端那一顆是什麼 ──────
             *
             * ⚠ 順序不能倒過來：**先算得下幾個，才知道右端該畫什麼**。
             * 反過來（先畫翻頁鍵、再看排得下幾個）就是現況的缺陷 ——
             * `nextEnabled = !isLastPage` 與「畫得出來幾個」完全脫鉤，
             * 於是一個「看得到 3 個、其實有 9 個」的候選列照樣給你翻頁鍵，
             * 而按下去跳過的正是那 6 個沒看過的候選。
             */
            val screenWidthDp = LocalConfiguration.current.screenWidthDp.toFloat()
            // ⚠ 行內組字串**也是**真的擠掉候選的。它與右端保留區一樣要先扣掉 ——
            //   沒扣的那一版在 411 dp 的機器上打 `ni` 說得下 7 個、畫面只畫得出
            //   6 個,而 `rightEnd` 正是拿這個數決定「本頁看完了沒」。
            val leadingDp = CandidateDensity.inlinePreeditDp(
                inlinePreedit, scaler.scaled(theme.preedit.size), theme.preedit.paddingH
            )
            val barLayout = CandidateDensity.barLayout(
                screenWidthDp = screenWidthDp,
                barPaddingH = bar.paddingH,
                reservedEnd = bar.reservedEnd,
                buttonDp = CANDIDATE_BAR_BUTTON_DP.toFloat(),
                leadingDp = leadingDp,
                widths = shown.map { i ->
                    val c = state.candidates[i]
                    CandidateDensity.itemWidthDp(
                        textChars = c.text.length,
                        textSize = scaler.scaled(style.text.size),
                        labelChars =
                            if (labelShown) style.label.render(c.label, i).length else 0,
                        labelSize = scaler.scaled(style.label.size),
                        commentChars = if (commentShown) c.comment.length else 0,
                        commentSize = scaler.scaled(style.comment.size),
                        paddingH = style.item.paddingH,
                        minWidth = style.item.minWidth,
                    )
                },
                spacing = style.item.spacing,
                pageCandidateCount = shown.size,
                pageNo = state.pageNo,
                isLastPage = state.isLastPage,
                pagerKind = style.pageIndicator.kind,
                pagerShow = style.pageIndicator.show,
                expandAvailable = expand.show,
                panelOpen = expand.expanded,
            )

            /* ── §10 第 41 條:死路 —— 只發診斷,不改畫面 ────────────────
             *
             * ⛔ [CandidateDensity.deadEnd] 從第三輪起就有整套單元測試,而
             *   **產品碼一個呼叫端都沒有**。也就是說:隨附主題由測試守著,
             *   而使用者自帶主題真的把兩條出口都關掉時,裝置上沒有任何東西
             *   會叫 —— 他看到的是一列候選、按不到後面那些,沒有任何線索。
             *
             * ⚠ 這裡**刻意只發一則 §6.5 的 code + args 診斷**,不強制畫出
             *   展開鍵。死路本身已經在第四輪結構性地封掉了
             *   ([Pager.panelState] 型別上就沒有 show/kind),這一行只是讓它
             *   被看見;加 UI 退路是會造出下一件新缺陷的那種改動。
             */
            val barDeadEnd = CandidateDensity.deadEnd(
                layout = barLayout,
                pageCandidateCount = shown.size,
                morePages = !state.isLastPage,
                panelPagerDrawable = CandidateDensity.panelPagerDrawable(
                    pageNo = state.pageNo,
                    isLastPage = state.isLastPage,
                    shownCount = shown.size,
                ),
            )
            // 每一格頁況只叫一次。少了 key,重組一次就多印一行,而洗版的日誌
            // 與沒有日誌一樣沒有人看。
            LaunchedEffect(barDeadEnd, theme.id, barLayout.rightEnd, state.pageNo) {
                if (barDeadEnd) {
                    Log.w(
                        BAR_TAG,
                        DiagnosticText.render(
                            Diagnostic(
                                DiagnosticCode.BAR_DEAD_END,
                                listOf(theme.id, barLayout.rightEnd.name.lowercase()),
                                "candidates.bar",
                            )
                        ),
                    )
                }
            }

            // `shown` 由呼叫端算好，裡面是**引擎的頁內索引**，不是畫面位置 ——
            // 選字走 rs_select_candidate(index_on_page),兩者一旦脫鉤,使用者
            // 點第二個卻選到第五個,而畫面完全正常。見 [T9Syllables] 與 [Expander]。
            LazyRow(
                modifier = Modifier.weight(1f).fillMaxHeight(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(style.item.spacing.dp),
                contentPadding = PaddingValues(horizontal = bar.paddingH.dp),
            ) {
                itemsIndexed(shown) { _, index ->
                    val candidate = state.candidates[index]
                    val highlighted = index == state.highlighted
                    val ink = CandidateInk.of(
                        style.item, style.text, style.label, style.comment, highlighted
                    )
                    // 候選字本身念得出來,但少了序號使用者無從說「我要第三個」;
                    // 而「現在停在哪一個」走 stateDescription,選字移動時會重念。
                    // 序號用**引擎索引**而不是畫面位置:篩選之後畫面上的第二個
                    // 仍然是引擎的第五個,念錯的話使用者說出來的指令會落在別處。
                    //
                    // ⚠ 這一段**不受 labelShown 影響**。視覺上不畫序號與朗讀時
                    //   不念序號是兩件事:使用者說「我要第三個」靠的是後者。
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
                            // §8.6.4.2:觸控目標的下界。以前這個欄位解析了卻沒有
                            // 任何 widthIn 套用它 —— 一字候選的可點寬度只有
                            // `text.size + 2 × padding_h`,在 20 sp / 8 dp 之下是
                            // 36 dp,低於 ui-design §3.6 的 48 dp。
                            .widthIn(min = style.item.minWidth.dp)
                            .clip(RoundedCornerShape(style.item.cornerRadius.dp))
                            .background(Color(ink.background))
                            .then(
                                // `item.border_width` / `highlight_border_width` 在
                                // ThemeParser 解析了,而候選格的 modifier 鏈上一直
                                // 沒有 border —— 兩個欄位在本端畫不出來。接上去
                                // 之後 `highlight_style: outline` 才真的有東西。
                                // border 畫在內側,**不改變量測寬度**（§8.6.4.3）。
                                if (ink.borderWidth > 0f) {
                                    Modifier.border(
                                        ink.borderWidth.dp,
                                        Color(ink.borderColor),
                                        RoundedCornerShape(style.item.cornerRadius.dp),
                                    )
                                } else {
                                    Modifier
                                }
                            )
                            .clickable { onEvent(KeyboardEvent.Candidate(index)) }
                            .padding(
                                horizontal = style.item.paddingH.dp,
                                vertical = style.item.paddingV.dp,
                            ),
                        verticalAlignment = Alignment.CenterVertically,
                        // min_width 撐出來的餘裕要平均分在兩邊 —— 靠左的話
                        // 一字候選看起來像是黏在左邊那一個上。
                        horizontalArrangement = Arrangement.Center,
                    ) {
                        if (labelShown && candidate.label.isNotEmpty()) {
                            Text(
                                text = style.label.render(candidate.label, index),
                                fontSize = scaler.sp(style.label.size),
                                color = Color(ink.label),
                                modifier = Modifier.padding(end = CandidateDensity.GAP_DP.dp),
                            )
                        }
                        Text(
                            text = candidate.text,
                            fontSize = scaler.sp(style.text.size),
                            fontWeight = if (highlighted) FontWeight.SemiBold else FontWeight.Normal,
                            color = Color(ink.text),
                            // §8.6.4.3 的 `underline`:格底一條 2 dp、寬度等於
                            // **候選文字墨跡**。走 drawBehind 而不是多一個
                            // Box/Spacer,因為它一個 dp 的版面都不佔 ——
                            // 高亮不得改變該格的量測寬度。
                            modifier = if (ink.underline != null) {
                                Modifier.drawBehind {
                                    val h = 2.dp.toPx()
                                    drawRect(
                                        color = Color(ink.underline),
                                        topLeft = Offset(0f, size.height - h),
                                        size = Size(size.width, h),
                                    )
                                }
                            } else {
                                Modifier
                            },
                        )
                        if (commentShown && candidate.comment.isNotEmpty()) {
                            Text(
                                text = candidate.comment,
                                fontSize = scaler.sp(style.comment.size),
                                color = Color(ink.comment),
                                modifier = Modifier.padding(start = CandidateDensity.GAP_DP.dp),
                            )
                        }
                    }
                }
            }

            /* ── §8.6.6.4:右端**最多一顆** ─────────────────────────────────
             *
             * 翻頁鍵與展開鍵解決的是同一個問題(「還有更多」)。兩顆一起出現
             * 是同一份資訊的第二份 —— 與註解／消歧欄同一個病 —— 而它們一起
             * 吃掉候選列 19.4% 的寬度(實測 411.43 dp:80 dp 對 40 dp 恰好
             * 差一個候選)。寫成 `when` 而不是兩個 `if`,「都畫」就不再是一個
             * 表達得出來的狀態。
             */
            when (barLayout.rightEnd) {
                // 什麼都不畫。兩種情形:本頁沒有候選(候選列現在是工具列);
                // 或者「該畫的那一顆畫不出來」—— 後者只要還有沒看到的候選就是
                // 死路,而擋它的是建置期的 [CandidateDensity.deadEnd] ＋
                // `ThemeDensityTest`,不是這裡的執行期分支。
                CandidateDensity.RightEnd.NONE -> Unit

                // 本頁還有畫不出來的候選 → 出口是展開面板,翻頁在面板裡面。
                CandidateDensity.RightEnd.EXPAND -> ExpandButton(
                    expanded = expand.expanded,
                    button = bar.expandButton,
                    scaler = scaler,
                    onClick = onToggleExpand,
                )

                // 本頁全部畫得出來 = 本頁看完了,這時候翻頁才是誠實的。
                // ⚠ 這一份 Pager.State 是 [CandidateDensity.barLayout] **算量測寬度
                //   時用的那一份**,不是這裡另外算的第二份。上一版的量測與繪製
                //   各算一次,11 種頁況裡 5 種對不上(量測扣 80、實際畫 40)。
                // ⚠ 走到這一支就代表 `PageArrows` **真的會畫出至少一顆**
                //   ([CandidateDensity.barLayout] 的 `pagerDrawable`)。上一版
                //   在這裡收到過 `show=false` 的狀態,於是右端整片空白。
                CandidateDensity.RightEnd.PAGER -> PageArrows(
                    state = barLayout.pager ?: Pager.State(false, false, false),
                    style = style.pageIndicator,
                    scaler = scaler,
                    onEvent = onEvent,
                )
            }
        }
    }
}

/**
 * 候選列右端那幾顆方鍵（上一頁／下一頁／展開）的觸控目標寬度。
 *
 * ⚠ **這是唯一的一份,而且 scripts/verify_candbar.sh 讀它。** 那支腳本從前
 * 把翻頁鍵寫成「螢幕最右端那一點」;展開鍵搬進候選列之後,那一點變成了
 * 展開鍵 —— 於是它整輪點的都是展開鍵,而**照樣全綠**(artifact 的檔名叫
 * 2-page2.png,畫面上卻是第 1 頁的展開面板)。現在它從這個常數與主題
 * 算出座標。改這個數字,腳本會自動跟上;改成不是 `= 數字` 的形式,
 * 腳本會**當場報錯**,不會安靜地算錯。
 */
internal const val CANDIDATE_BAR_BUTTON_DP = 40

/** 候選列的執行期診斷（§6.5）用的 logcat tag。 */
private const val BAR_TAG = "CandidateBar"

/**
 * 候選列右端的上一頁／下一頁。
 *
 * **按不動的那一顆不畫**，理由與實測值見 [Pager.State]：那 40 dp 是真的把
 * 候選字擠出畫面的，而候選列靠左排，少一顆箭頭不會讓任何候選移動。
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
        if (state.prevEnabled) {
            PageArrow("‹", prevDesc, style, scaler) {
                onEvent(KeyboardEvent.Page(backward = true))
            }
        }
        if (state.nextEnabled) {
            PageArrow("›", nextDesc, style, scaler) {
                onEvent(KeyboardEvent.Page(backward = false))
            }
        }
    }
}

@Composable
private fun PageArrow(
    glyph: String,
    description: String,
    style: PageIndicatorStyle,
    scaler: Scaler,
    onClick: () -> Unit,
) {
    Box(
        modifier = Modifier
            .fillMaxHeight()
            // 觸控目標要夠大 —— 一個 14sp 的字元只有十幾 dp 寬,那是點不到的。
            .width(CANDIDATE_BAR_BUTTON_DP.dp)
            .semantics(mergeDescendants = true) {
                contentDescription = description
                role = Role.Button
            }
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = glyph,
            fontSize = scaler.sp(style.size),
            color = Color(style.color),
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
                            color = style.text.color,
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
    /**
     * 候選列上**真的畫得出序號**的那幾格（頁內索引）。工單 #99：
     * 專用數字列的 `3` 要選的是「畫面上標著 3 的那一個」——
     * T9 消歧欄篩掉的那幾格畫面上沒有序號，按下去選中一個看不見的候選
     * 就是新的缺陷，所以那幾格按下去**什麼都不做**。
     */
    selectableCandidates: Set<Int> = emptySet(),
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

    // §9.3 的鍵縫**折進 weight**，讓命中格含縫（見 [KeyCells]）。
    // 這幾個數字只跟主題與佈局有關，與這一幀的狀態無關。
    val outerLeft = KeyCells.outerPad(pad.left, keySpacing)
    val outerRight = KeyCells.outerPad(pad.right, keySpacing)
    val outerTop = KeyCells.outerPad(pad.top, rowSpacing)
    val outerBottom = KeyCells.outerPad(pad.bottom, rowSpacing)
    val rowCount = layer.rows.size
    // 列高：先算「改動前畫出來的高度」，再折進列距。
    val usableH = height - pad.top - pad.bottom - rowSpacing * (rowCount - 1).coerceAtLeast(0)
    val rowSizes = KeyCells.visibleSizes(
        weightsIn = layer.rows.map { it.weight },
        total = layer.rows.sumOf { it.weight.toDouble() }.toFloat(),
        availableDp = usableH + rowSpacing * (rowCount - 1).coerceAtLeast(0),
        spacingDp = rowSpacing,
    )
    val rowWeights = KeyCells.weights(rowSizes, rowSpacing, outerTop, outerBottom)

    // 「這一層有沒有專用數字列」是**整層**的性質,一層算一次(工單 #99)。
    val digitRowActive = remember(layer) { SelectionDigitKeys.rowActive(layer) }

    BoxWithConstraints(
        modifier = Modifier
            .fillMaxWidth()
            .height(height.dp)
            .onGloballyPositioned {
                gridOrigin = it.positionInRoot()
                gridWidthDp = with(density) { it.size.width.toDp().value }
            },
    ) {
        // 鍵寬要用**實際寬度**算，不能等 onGloballyPositioned —— 那要慢一幀，
        // 而第一幀畫錯的鍵盤是使用者看得見的。
        val innerW = maxWidth.value - pad.left - pad.right
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(
                    start = (pad.left - outerLeft).dp,
                    end = (pad.right - outerRight).dp,
                    top = (pad.top - outerTop).dp,
                    bottom = (pad.bottom - outerBottom).dp,
                ),
            verticalArrangement = Arrangement.spacedBy(0.dp),
        ) {
            for ((rowIndex, row) in layer.rows.withIndex()) {
                // §9.3：Σ width < units 時剩餘空間留在該列末端 —— 它在版面上
                // 就是多一個子項，所以也多吃一道鍵縫（與改動前的行為一致）。
                val slack = layer.units - row.widthSum
                val hasSlack = slack > 0.01f
                val childWidths =
                    row.keys.map { it.width } + (if (hasSlack) listOf(slack) else emptyList())
                val childSizes = KeyCells.visibleSizes(
                    weightsIn = childWidths,
                    total = layer.units,
                    availableDp = innerW,
                    spacingDp = keySpacing,
                )
                val childWeights =
                    KeyCells.weights(childSizes, keySpacing, outerLeft, outerRight)
                val childCount = childWeights.size
                val padTop = KeyCells.padStart(rowIndex, rowSpacing, outerTop)
                val padBottom =
                    KeyCells.padEnd(rowIndex, rowCount, rowSpacing, outerBottom)
                Row(
                    modifier = Modifier.fillMaxWidth().weight(rowWeights[rowIndex]),
                    horizontalArrangement = Arrangement.spacedBy(0.dp),
                ) {
                    for ((keyIndex, key) in row.keys.withIndex()) {
                        val cell = KeyCells.Inset(
                            start = KeyCells.padStart(keyIndex, keySpacing, outerLeft),
                            end = KeyCells.padEnd(keyIndex, childCount, keySpacing, outerRight),
                            top = padTop,
                            bottom = padBottom,
                        )
                        val cellWeight = childWeights[keyIndex]
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
                        // ⚠ 整排 1–9 都在才算「專用數字列」;逐顆再問 label 與
                        //   keysym 是不是同一個數字;最後再問「當下按下去要做
                        //   什麼」。三層判準都在 [SelectionDigitKeys](純函式)。
                        val digitAct =
                            if (!digitRowActive) null
                            else SelectionDigitKeys.digitOf(slot.key)?.let { d ->
                                SelectionDigitKeys.act(
                                    digit = d,
                                    // ⛔ **問引擎有沒有輸入串,不要問顯示用的 preedit。**
                                    //
                                    // 這裡實際上問的是「點下去會不會毀掉使用者已經打好的東西」
                                    // —— 而那件事只有引擎答得出來。`state.preedit` 是
                                    // 畫面那一層的東西:九宮格是雙編碼,它拿到的是
                                    // 代表字母串(`MG GAM`),而 #68 之後這串在宿主與候選列
                                    // 上都被濾成空字串 —— 拿「畫不畫得出來」當「有沒有在組字」,
                                    // 兩者不同步的那一刻就是把數字送回引擎、被
                                    // `recognizer/patterns` 收走、組字變成 `3⋯` 的那一刻。
                                    //
                                    // ⊙ 候選非空是**充分**條件(有候選就一定在組字),
                                    //   但不是必要條件:引擎在組字而**本頁一個候選都沒有**時,
                                    //   [SelectionDigitKeys.act] 的答案是 `Ignore`(什麼都不做)——
                                    //   而舊判準在那一格會答 `SendDigit`,也就是把數字送回引擎。
                                    //   兩個答案差的正好是「毀不毀掉組字」。
                                    composing = state.status.isComposing ||
                                        state.candidates.isNotEmpty(),
                                    selectableIndices = selectableCandidates,
                                )
                            }
                        val shownKey = slot.key
                        val tapCell = slot.tapCell
                        val speaks = slot.speaks
                        if (shownKey.spacer) {
                            Spacer(Modifier.weight(cellWeight).fillMaxHeight())
                        } else {
                            KeyView(
                                key = shownKey,
                                theme = theme,
                                scaler = scaler,
                                status = state.status,
                                layerLabels = layerLabels,
                                cell = cell,
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
                                digitAct = digitAct,
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
                                modifier = Modifier.weight(cellWeight).fillMaxHeight(),
                            )
                        }
                    }
                    // §9.3：Σ width < units 時，剩餘空間留在該列末端。
                    // 它的 weight 已經在上面與鍵一起算過（也吃了一道鍵縫）。
                    if (hasSlack) Spacer(Modifier.weight(childWeights.last()).fillMaxHeight())
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

/**
 * 專用數字列那一顆鍵按下去（工單 #99）。
 *
 * 回 `Unit` = 這一顆由這裡處理掉了（含「刻意什麼都不做」）；
 * 回 `null` = 不是數字鍵，呼叫端照 §9.6 的 tap → send → noop 走。
 *
 * ⚠ 兩個呼叫端（單擊、自動重複）**必須**都走這一支。兩處各寫一份的話，
 *   長按數字鍵會走回那條把 keysym 丟給引擎、被 recognizer 收走、
 *   毀掉組字的路，而單擊是對的 —— 而畫面上看不出差別。
 */
private fun fireDigit(
    key: LayoutKey,
    act: SelectionDigitKeys.Act?,
    onEvent: (KeyboardEvent) -> Unit,
): Unit? = when (act) {
    null -> null
    is SelectionDigitKeys.Act.Select -> onEvent(KeyboardEvent.Candidate(act.indexOnPage))
    // 沒有在組字 —— 照常打一個數字。
    SelectionDigitKeys.Act.SendDigit -> key.send?.let { onEvent(KeyboardEvent.Send(it)) } ?: Unit
    // ⛔ 索引超過本頁候選數（或那一格被消歧欄篩掉）—— **什麼都不做**。
    //    從前這一格是「送給引擎」，而那正是毀掉組字的那條路。
    SelectionDigitKeys.Act.Ignore -> Unit
}

/**
 * §9.6 的點擊解析（tap → send → noop，而數字鍵在組字中由
 * [SelectionDigitKeys] 接管），做成一個**手勢協程可以抓著不放**的東西。
 *
 * ⛔ **`digitAct` 是一個「問」，不是一份抄本 —— 這一行就是工單 #99 的第二半。**
 *
 * `Modifier.pointerInput(key)` 的協程在這顆鍵**第一次被碰到**時才啟動，
 * 而它的 key 只有 `key`：組字狀態變了不會讓它重啟。所以它捕捉到的
 * 區域變數**從此凍住**。把當下的 [SelectionDigitKeys.Act] 抄一份進去，
 * 做出來的就是「這顆鍵第一次被按時是什麼狀態，它一輩子就是那個狀態」。
 *
 * 2026-08-14 emulator-5558 / lumina_test2 實測（`cn-t9-pinyin-numrow` ＋
 * `t9_pinyin`），同一顆 `n3`：
 *
 * ```
 * 07:38:01.585  launch key=n3 captured=SendDigit          ← 閒置按下去，協程啟動
 * 07:38:09.907  compose key=n3 act=Select(2) cands=7      ← 組字了，畫面改成「選第 3 個」
 * 07:38:20.956  fire   key=n3 captured=SendDigit
 *                                fresh=Select(2)          ← 送出去的仍是第一行捕捉到的那一份
 * ```
 *
 * 結果是數字進了引擎、被 `recognizer/patterns` 收走，宿主輸入框變成
 * `33⋯` —— **使用者剛打好的組字沒了**。判準（[SelectionDigitKeys.act]）
 * 從頭到尾都是對的：`composing=true` 時它只回 `Select` 或 `Ignore`，
 * 兩者都不會把數字送進引擎。過期的是**送到判準面前的那份答案**。
 *
 * 同一份檔案在 `KeyView` 裡已經為 `onEvent` / `onPopup` / `behavior`
 * 寫過這件事（`rememberUpdatedState`）；`digitAct` 是後來加的，漏了。
 *
 * ⚠ 這個函式存在的唯一理由是**讓那條路測得到**：`KeyView` 是
 * `@Composable`，本模組的單元測試（純 JVM、沒有 compose-ui-test）
 * 一行都摸不到它。見 `KeyFireTest`。
 */
internal fun keyFire(
    key: LayoutKey,
    digitAct: () -> SelectionDigitKeys.Act?,
    onEvent: (KeyboardEvent) -> Unit,
): () -> Unit = {
    fireDigit(key, digitAct(), onEvent) ?: run {
        val tap = key.tap
        val send = key.send
        when {
            tap != null -> onEvent(KeyboardEvent.Act(tap))
            send != null -> onEvent(KeyboardEvent.Send(send))
        }
    }
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
    /**
     * 這一格四邊各要往內縮多少（dp）—— **鍵縫**。
     *
     * 命中格是整格（含縫），畫出來的那一塊往內縮回原位。理由與量到的數字
     * 見 [KeyCells]。給 `Inset.ZERO` 時行為與折縫之前完全一樣。
     */
    cell: KeyCells.Inset,
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
    /**
     * 這一顆是「專用數字列」上的選字數字鍵的話，按下去要做什麼（工單 #99）。
     * 不是數字鍵就是 null，一切照舊。
     *
     * 由呼叫端算：判準要看**整層**（整排 1–9 都在）、也要看**當下**
     * （有沒有在組字、那一格畫不畫得出來），而 KeyView 兩者都看不到。
     * 決定本身是純函式 [SelectionDigitKeys.act]，單元測試摸得到。
     *
     * ⛔ 非 null 時**不得**走 [KeyboardEvent.Send]：組字中把 keysym 丟給引擎
     * 會被 `recognizer/patterns` 收走並**毀掉使用者已經打好的組字**。
     */
    digitAct: SelectionDigitKeys.Act? = null,
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
    // ⛔ `digitAct` 也在這一份名單上,而它是後來才加的參數 —— 漏掉它的代價
    //   不是「慢一幀」,是**同一顆數字鍵按過一次之後就一直答第一次那個答案**。
    //   為什麼漏掉它會凍住、凍在什麼時候,寫在 [keyFire] 的檔頭。
    val currentDigitAct by rememberUpdatedState(digitAct)

    // 這一顆鍵在「按鍵音」上算哪一種。系統的按鍵音效本來就分四個角色
    // (STANDARD / SPACEBAR / DELETE / RETURN),而在這一版之前我們一律送
    // STANDARD —— 連免費的區分都沒有用上。自帶音色也照這四個角色出素材。
    //
    // 以 key 為 remember 的 key:角色只跟佈局有關,按鍵當下不必重算。
    val keyRole = remember(key) {
        FeedbackPlan.roleOf((key.send as? SendSpec.Keysym)?.name, key.id)
    }

    fun haptic() = currentBehavior.onKeyPress(view, keyRole)

    // §9.6 的點擊解析。⚠ **只建立一次**（`key` 變了才重來）—— 底下
    // `pointerInput(key)` 的手勢協程抓走的就是它，而那個協程在這顆鍵
    // **第一次被碰到**時啟動、之後不再重啟。所以它手上必須是一個「問」，
    // 不是一份抄本；理由與實測見 [keyFire]。
    val resolveTap = remember(key) {
        keyFire(key, { currentDigitAct }, { currentOnEvent(it) })
    }

    fun fire() {
        haptic()
        resolveTap()
    }

    // 有雙擊／長按／彈出盤的鍵不能在按下當下就出字，否則會與後續手勢打架。
    val fireOnDown = doubleTap == null && longPress == null && popup == null

    // §6.3：repeat 勝過 long_press；解析器已保證兩者不同時存在。
    if (key.repeat && pressed) {
        LaunchedEffect(key, behavior) {
            delay(behavior.repeatDelayMs.toLong())
            while (true) {
                // 自動重複走**同一條**解析 —— 兩處各寫一份的話,長按數字鍵
                // 會走回那條毀掉組字的路,而單擊是對的。
                fireDigit(key, currentDigitAct, currentOnEvent)
                    ?: key.send?.let { currentOnEvent(KeyboardEvent.Send(it)) }
                delay(behavior.repeatIntervalMs.toLong())
            }
        }
    }

    /**
     * 「畫出來的那一塊」從這裡開始 —— 它接在 padding **後面**。
     *
     * ⚠ 順序是這個修法的全部內容：`Modifier` 的鏈上，padding 之前的修飾子
     *   看到的是**整格**，之後的看到的是縮進去的那一塊。所以
     *   語意 + `pointerInput`（命中）必須排在 padding 前面，
     *   `clip` / `background` / `border` / `BoxWithConstraints` 的量測
     *   （外觀）排在後面。寫反的話不是編譯錯誤，是「看起來完全一樣、
     *   鍵縫照樣點不到」—— 也就是什麼都沒改。
     */
    var painted: Modifier = Modifier.onGloballyPositioned {
        val pos = it.positionInRoot()
        with(density) {
            anchorLeft = pos.x.toDp().value
            anchorTop = pos.y.toDp().value
            anchorWidth = it.size.width.toDp().value
        }
    }
    if (style.elevation > 0f) {
        painted = painted.shadow(style.elevation.dp, RoundedCornerShape(style.cornerRadius.dp))
    }
    painted = painted.clip(RoundedCornerShape(style.cornerRadius.dp)).background(Color(background))
    if (style.borderWidth > 0f) {
        painted = painted.border(
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
        modifier = modifier
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
        }
            // ── 命中格到此為止 ────────────────────────────────────────────
            .padding(
                start = cell.start.dp,
                end = cell.end.dp,
                top = cell.top.dp,
                bottom = cell.bottom.dp,
            )
            .then(painted),
        contentAlignment = Alignment.Center,
    ) {
        val face = faceOf(key.labelFrom, key.icon, key.label, status)
        // 圖示與文字用的是兩個不同的基準字級（§8.8.1 的 icon_size / label_size）。
        val base = if (key.icon != null && key.labelFrom == LabelSource.NONE) {
            style.iconSize
        } else {
            style.labelSize
        }
        val shownFace = if (key.labelFrom == LabelSource.INPUT_MODE_PAIR) {
            inputModeFace(status.isAsciiMode, foreground)
        } else {
            AnnotatedString(face)
        }
        // ⚠ 量的必須是**帶樣式的那一份**。中／En 的當前那一態是 Bold，
        //   粗體比較寬；量純文字的話會量出「放得下」而畫出來放不下 ——
        //   實測（font_scale 1.30、qwerty 底列）畫出來的是「中/」，En 整段不見。
        //   量測用的那一份刻意**不帶顏色**：字寬與顏色無關，帶著它等於
        //   每按一次鍵（按下色一換）就重新量一次。
        val measuredFace = if (key.labelFrom == LabelSource.INPUT_MODE_PAIR) {
            inputModeMeasureFace(status.isAsciiMode)
        } else {
            AnnotatedString(face)
        }
        Text(
            text = shownFace,
            fontSize = fittedLabelSize(measuredFace, base, constraints.maxWidth, scaler),
            color = Color(foreground),
            maxLines = 1,
            // ⚠ **這一行才是「中/En 變成中/」的真兇。**
            //   `Text` 的 `softWrap` 預設是 true，於是放不下時 Compose 先做
            //   **斷行**（CJK 每個字之間、`/` 後面都是合法的斷點），再讓
            //   `maxLines = 1` 把第二行整段丟掉。畫面上看到的就是「中/」——
            //   不是被切一半，是第二行不見了。
            //   量測那一支用的是 `softWrap = false`（量單行寬度），兩邊
            //   對不上：量到「一行放得下」，畫出來卻斷成兩行。
            softWrap = false,
            // 到了 §9.6 的縮放下限仍然放不下時才截斷 —— 而截斷必須看得出來。
            // Compose 的預設是 `Clip`（無聲切掉），那會讓一顆鍵讀起來像
            // 另一顆鍵。
            overflow = TextOverflow.Ellipsis,
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
/** 未選中那半縮小到這個比例。相對值（em），所以跟著 `label_size` 與系統字級走。 */
internal const val INPUT_MODE_IDLE_SCALE = 0.82f

/**
 * 量測用的那一份：**同樣的字重與大小，不帶顏色**。
 *
 * 顏色不影響字寬，但它會隨按下狀態改變 —— 把它帶進 `remember` 的 key 裡，
 * 等於每按一次鍵就重新量一次文字。
 */
internal fun inputModeMeasureFace(asciiMode: Boolean): AnnotatedString =
    inputModeFace(asciiMode, color = 0)

/**
 * ⚠ **兩半用同一個顏色。** 上一版把未選中那半染成 `hint_color`，
 * 而那個欄位是給鍵角落那幾個小數字用的（它們坐在白色的 `$key` 上）。
 * 壓到 `modifier` 樣式的灰底（`$key_mod`）上之後，實測對比只有 **2.84:1**
 * （淺色主題）—— WCAG 小字要 4.5:1。而且**十二份主題全部不合格**
 * （最好的一份也只有 4.48:1），所以那不是某一份主題調錯了，
 * 是「拿 hint_color 當正文顏色」這個用法本身就不成立。
 *
 * 那能不能只把顏色調暗一點就好？在這種中間調的鍵底上**不能**：
 * 要滿足 4.5:1，可用的亮度區間窄到與 `foreground` 幾乎分不出來
 * （default-light 上是 L 0.057 → 0.091 這一段）。也就是說
 * 「明顯比較淡」與「看得清楚」在這個底色上是互斥的。
 *
 * 所以狀態改用**不靠顏色的兩個線索**：當前那一態是**粗體且滿級數**，
 * 另一態是**常規字重且 [INPUT_MODE_IDLE_SCALE] 倍大小**。兩者都與對比無關，
 * 而且字級差在小螢幕上比顏色差更看得出來。
 */
internal fun inputModeFace(
    asciiMode: Boolean,
    color: Int,
): AnnotatedString {
    val tint = Color(color)
    val on = SpanStyle(color = tint, fontWeight = FontWeight.Bold)
    val off = SpanStyle(
        color = tint,
        fontWeight = FontWeight.Normal,
        fontSize = INPUT_MODE_IDLE_SCALE.em,
    )
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
    text: AnnotatedString,
    baseSize: Float,
    availableWidthPx: Int,
    scaler: Scaler,
): TextUnit {
    val base = scaler.sp(baseSize)
    if (text.text.isBlank() || availableWidthPx <= 0) return base
    val measurer = rememberTextMeasurer()
    // 左右各留一點餘裕，否則字會貼著鍵的圓角邊緣。
    val usable = availableWidthPx - with(LocalDensity.current) { LABEL_INSET_DP.dp.toPx() } * 2f
    if (usable <= 0f) return base
    // ⚠ **量的必須是「畫的時候會用的那一份 style」。**
    //   `Text` 用的是 `LocalTextStyle.current` 合併之後的樣式 —— 那裡面有
    //   字族與 `letterSpacing`（Material3 的 bodyLarge 是 0.5sp）。
    //   拿一份光禿禿的 `TextStyle(fontSize = base)` 去量，量到的一定比畫出來的窄，
    //   於是「量起來放得下、畫出來放不下」。這正是中／En 那顆鍵的形狀。
    val painted = LocalTextStyle.current.merge(TextStyle(fontSize = base))
    val measured = remember(text, painted, measurer) {
        measurer.measure(
            // ⚠ 收 AnnotatedString 而不是 String：**字重也是寬度的一部分**。
            //   簽章上就把樣式丟掉的話，這個缺陷會靜靜地回來 ——
            //   而且畫面看起來只是「那顆鍵的字短了一截」。
            text = text,
            style = painted,
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
