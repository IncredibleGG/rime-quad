package org.rimequad.ime.keyboard

import android.content.res.Configuration
import android.view.HapticFeedbackConstants
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
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
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
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
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import kotlinx.coroutines.delay
import org.rimequad.ime.core.RimeStatus
import org.rimequad.ime.theme.HintPosition
import org.rimequad.ime.theme.KeyGeometry
import org.rimequad.ime.theme.KeyboardLayout
import org.rimequad.ime.theme.KeyStyle
import org.rimequad.ime.theme.LabelSource
import org.rimequad.ime.theme.LayoutKey
import org.rimequad.ime.theme.LayoutLayer
import org.rimequad.ime.theme.Popup
import org.rimequad.ime.theme.PopupLayout
import org.rimequad.ime.theme.SubKey
import org.rimequad.ime.theme.Theme
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
        ) { Text(state.busyMessage ?: "正在載入主題…") }
        return
    }

    val config = LocalConfiguration.current
    val scaler = Scaler(
        effective = theme.typography.effectiveScale(config.fontScale),
        system = LocalDensity.current.fontScale,
    )

    Column(
        modifier = modifier
            .fillMaxWidth()
            .background(Color(theme.keyboard.background))
            .padding(bottom = bottomInsetDp(theme.keyboard.honorBottomInset)),
    ) {
        CandidateBar(state = state, theme = theme, scaler = scaler, onEvent = onEvent)
        if (state.schemaPickerOpen) {
            SchemaPicker(state = state, theme = theme, scaler = scaler, onEvent = onEvent)
        } else {
            KeyGrid(state = state, theme = theme, scaler = scaler, onEvent = onEvent)
        }
    }
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
 * §8.8.0 的高度模型：鍵寬 → 鍵高 → 鍵盤高。
 *
 * 注意 `units` 與 `rowsWeight` 取自**當前 layer**，所以 11 欄的注音與 10 欄的
 * QWERTY 會得到相同的鍵長寬比（鍵較窄時鍵也較矮），而不是前者更瘦長。
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
    )
}

/** 佈局尚未載入時的名目鍵盤高度：以 10 欄 4 列估算。 */
@Composable
private fun nominalKeyboardHeight(theme: Theme): Float {
    val config = LocalConfiguration.current
    return theme.keyboard.geometry.resolve(
        widthDp = config.screenWidthDp.toFloat(),
        availHeightDp = config.screenHeightDp.toFloat(),
        landscape = config.orientation == Configuration.ORIENTATION_LANDSCAPE,
        units = 10f,
        rowsWeight = 4f,
        rowCount = 4,
        padding = theme.keyboard.padding,
        keySpacing = theme.keyboard.keySpacing,
        rowSpacing = theme.keyboard.rowSpacing,
    ).keyboardHeight
}

/* ────────────────────────────── 候選列 ────────────────────────────── */

@Composable
private fun CandidateBar(
    state: KeyboardUiState,
    theme: Theme,
    scaler: Scaler,
    onEvent: (KeyboardEvent) -> Unit,
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
                ?: if (state.isStub) "⟦STUB⟧ 未接 librime，候選字為假資料" else null

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

            if (bar.showPreeditInline && state.preedit.isNotEmpty()) {
                Text(
                    text = state.preedit,
                    fontSize = scaler.sp(theme.preedit.size),
                    color = Color(theme.preedit.color),
                    maxLines = 1,
                    modifier = Modifier.padding(horizontal = theme.preedit.paddingH.dp),
                )
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

            LazyRow(
                modifier = Modifier.weight(1f).fillMaxHeight(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(style.item.spacing.dp),
                contentPadding = PaddingValues(horizontal = 4.dp),
            ) {
                itemsIndexed(state.candidates) { index, candidate ->
                    val highlighted = index == state.highlighted
                    Row(
                        modifier = Modifier
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
        }
    }
}

/**
 * 無候選時的工具列（§8.6.6 的 `empty_shows_toolbar`）。
 *
 * 規範自己承認「開啟後顯示什麼、長什麼樣完全未規範」（§11），
 * 這裡放的是本輪最需要的東西：目前方案的名字，點下去就是方案切換入口。
 */
@Composable
private fun Toolbar(
    state: KeyboardUiState,
    theme: Theme,
    scaler: Scaler,
    onEvent: (KeyboardEvent) -> Unit,
    modifier: Modifier = Modifier,
) {
    val style = theme.candidates.bar.style
    Row(
        modifier = modifier.padding(horizontal = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(
            text = "⌨ " + state.status.schemaName.ifEmpty { "選擇方案" },
            fontSize = scaler.sp(style.text.size * 0.8f),
            color = Color(style.text.color),
            maxLines = 1,
            modifier = Modifier
                .clip(RoundedCornerShape(style.item.cornerRadius.dp))
                .clickable { onEvent(KeyboardEvent.OpenSchemaPicker) }
                .padding(horizontal = 10.dp, vertical = 4.dp),
        )
        val layoutName = state.layout?.name?.get("zh-Hant").orEmpty()
        if (layoutName.isNotEmpty()) {
            Text(
                text = layoutName,
                fontSize = scaler.sp(style.label.size),
                color = Color(style.label.color),
                maxLines = 1,
            )
        }
    }
}

/* ────────────────────────────── 方案選單 ────────────────────────────── */

@Composable
private fun SchemaPicker(
    state: KeyboardUiState,
    theme: Theme,
    scaler: Scaler,
    onEvent: (KeyboardEvent) -> Unit,
) {
    // 方案選單取代鍵盤時，高度必須跟被取代的那塊一致，否則面板會跳動。
    // 佈局尚未載入時退回一個 10 欄 4 列的名目幾何。
    val layout = state.layout
    val layer = state.layer
    val height = if (layout != null && layer != null) {
        keyboardGeometry(theme, layout, layer).keyboardHeight
    } else {
        nominalKeyboardHeight(theme)
    }
    val style = theme.keyboard.keyStyle("default")
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .height(height.dp)
            .background(Color(theme.keyboard.background))
            .padding(horizontal = 8.dp, vertical = 6.dp),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(bottom = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = "輸入方案",
                fontSize = scaler.sp(style.labelSize * 0.75f),
                color = Color(style.foreground),
                modifier = Modifier.weight(1f),
            )
            Text(
                text = "✕",
                fontSize = scaler.sp(style.labelSize * 0.75f),
                color = Color(style.foreground),
                modifier = Modifier
                    .clickable { onEvent(KeyboardEvent.CloseSchemaPicker) }
                    .padding(horizontal = 12.dp, vertical = 4.dp),
            )
        }
        if (state.schemas.isEmpty()) {
            Text(
                text = "尚無可用方案（rs_schema_list 回傳空）",
                fontSize = scaler.sp(style.labelSize * 0.7f),
                color = Color(style.hintColor),
            )
            return@Column
        }
        LazyColumn(verticalArrangement = Arrangement.spacedBy(theme.keyboard.rowSpacing.dp)) {
            itemsIndexed(state.schemas) { _, schema ->
                val current = schema.id == state.status.schemaId
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(style.cornerRadius.dp))
                        .background(
                            Color(if (current) style.activeBackground else style.background)
                        )
                        .clickable { onEvent(KeyboardEvent.SelectSchema(schema.id)) }
                        .padding(horizontal = 12.dp, vertical = 10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        text = schema.name.ifEmpty { schema.id },
                        fontSize = scaler.sp(style.labelSize * 0.75f),
                        color = Color(if (current) style.activeForeground else style.foreground),
                        maxLines = 1,
                        modifier = Modifier.weight(1f),
                    )
                    Text(
                        text = schema.id,
                        fontSize = scaler.sp(style.hintSize),
                        color = Color(style.hintColor),
                        maxLines = 1,
                    )
                }
            }
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
) {
    val layout = state.layout
    val layer: LayoutLayer? = state.layer
    if (layout == null || layer == null) {
        Box(Modifier.fillMaxWidth().height(160.dp), contentAlignment = Alignment.Center) {
            Text(
                text = state.configProblem ?: "佈局尚未載入",
                color = Color(theme.candidates.shared.text.color),
            )
        }
        return
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
                        if (key.spacer) {
                            Spacer(Modifier.weight(key.width).fillMaxHeight())
                        } else {
                            KeyView(
                                key = key,
                                theme = theme,
                                scaler = scaler,
                                status = state.status,
                                onEvent = onEvent,
                                onPopup = { left, top, w ->
                                    val p = key.popup
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
                                modifier = Modifier.weight(key.width).fillMaxHeight(),
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
    onEvent: (KeyboardEvent) -> Unit,
    onPopup: (left: Float, top: Float, width: Float) -> Unit,
    modifier: Modifier = Modifier,
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
    val active = key.active ||
        (key.labelFrom == LabelSource.INPUT_MODE && status.isAsciiMode)

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

    fun haptic() {
        if (theme.feedback.haptic) view.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
    }

    /** §9.6 的點擊解析：tap → send → noop。 */
    fun fire() {
        haptic()
        val tap = key.tap
        val send = key.send
        when {
            tap != null -> onEvent(KeyboardEvent.Act(tap))
            send != null -> onEvent(KeyboardEvent.Send(send))
        }
    }

    // 有雙擊／長按／彈出盤的鍵不能在按下當下就出字，否則會與後續手勢打架。
    val fireOnDown = doubleTap == null && longPress == null && popup == null

    // §6.3：repeat 勝過 long_press；解析器已保證兩者不同時存在。
    if (key.repeat && pressed) {
        LaunchedEffect(key) {
            delay(400)
            while (true) {
                key.send?.let { onEvent(KeyboardEvent.Send(it)) }
                delay(60)
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

    Box(
        modifier = box.pointerInput(key, status, fireOnDown) {
            detectTapGestures(
                onPress = {
                    pressed = true
                    if (fireOnDown) fire()
                    tryAwaitRelease()
                    pressed = false
                },
                onTap = if (fireOnDown) null else ({ _ -> fire() }),
                onDoubleTap = if (doubleTap == null) {
                    null
                } else {
                    { _ -> haptic(); onEvent(KeyboardEvent.Act(doubleTap)) }
                },
                onLongPress = when {
                    key.repeat -> null
                    longPress != null -> ({ _ -> haptic(); onEvent(KeyboardEvent.Act(longPress)) })
                    popup != null -> ({ _ -> haptic(); onPopup(anchorLeft, anchorTop, anchorWidth) })
                    else -> null
                },
            )
        },
        contentAlignment = Alignment.Center,
    ) {
        val face = keyFace(key, status)
        val fontSize = when {
            face.length > 2 -> style.labelSize * 0.55f
            key.icon != null && key.labelFrom == LabelSource.NONE -> style.iconSize
            else -> style.labelSize
        }
        Text(
            text = face,
            fontSize = scaler.sp(fontSize),
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
 */
private fun keyFace(key: LayoutKey, status: RimeStatus): String {
    val fromStatus = when (key.labelFrom) {
        LabelSource.NONE -> null
        LabelSource.INPUT_MODE -> if (status.isAsciiMode) "英" else "中"
        LabelSource.SHAPE -> if (status.isFullShape) "全" else "半"
        LabelSource.VARIANT -> if (status.isSimplified) "简" else "繁"
        LabelSource.SCHEMA_NAME -> status.schemaName.ifEmpty { null }
        LabelSource.SCHEMA_ID -> status.schemaId.ifEmpty { null }
    }
    if (fromStatus != null) return fromStatus
    key.icon?.let { name -> ICONS[name]?.let { return it } }
    return key.label
}

/**
 * §9.6 的語義圖示。規範要求「渲染器自繪向量」，本輪先以字形代替 ——
 * 這是可見的偏離，已記在回報中。
 */
private val ICONS: Map<String, String> = mapOf(
    "backspace" to "⌫",
    "enter" to "↵",
    "shift" to "⇧",
    "shift_lock" to "⇪",
    "space" to " ",
    "globe" to "🌐",
    "keyboard_hide" to "⌄",
    "settings" to "⚙",
    "emoji" to "☺",
    "search" to "🔍",
    "go" to "→",
    "done" to "✓",
    "next" to "⇥",
    "clipboard" to "📋",
    "undo" to "↶",
    "mic" to "🎤",
    "arrow_left" to "←",
    "arrow_right" to "→",
    "arrow_up" to "↑",
    "arrow_down" to "↓",
)
