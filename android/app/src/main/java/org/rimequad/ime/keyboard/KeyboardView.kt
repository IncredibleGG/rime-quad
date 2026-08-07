package org.rimequad.ime.keyboard

import android.content.res.Configuration
import android.view.HapticFeedbackConstants
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
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
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import kotlinx.coroutines.delay
import org.rimequad.ime.core.RimeStatus
import org.rimequad.ime.prefs.LocalKeyBehavior
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
        // 鍵盤類型選單是**浮層**，不是取代品：底下那一列鍵仍然露出來、仍然按得動。
        // 抄的是三星的處理（docs/reference/samsung/photo_5）。理由不是好看 ——
        // 一個把整個鍵盤蓋掉的面板，只要它自己的關閉鍵出了任何差錯，
        // 使用者就沒有第二條路可走。出口永遠看得見，這條規矩對選單同樣適用。
        Box {
            KeyGrid(state = state, theme = theme, scaler = scaler, onEvent = onEvent)
            if (state.schemaPickerOpen) {
                SchemaPicker(state = state, theme = theme, scaler = scaler, onEvent = onEvent)
            }
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
    if (!toolbar.show || toolbar.items.isEmpty()) {
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
        itemsIndexed(toolbar.items) { _, item ->
            val face = faceOf(item.labelFrom, item.icon, item.label, state.status)
            val active = isActiveFace(false, item.labelFrom, state.status)
            Box(
                modifier = Modifier
                    .fillMaxHeight()
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
                    text = face,
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
private fun SchemaPicker(
    state: KeyboardUiState,
    theme: Theme,
    scaler: Scaler,
    onEvent: (KeyboardEvent) -> Unit,
) {
    // 浮層高度 = 鍵盤高度 − 最後一列 —— 底列（空白、Enter、退格、中英）
    // 必須整列露出來。佈局尚未載入時沒有底列可露，就用名目幾何鋪滿。
    val layout = state.layout
    val layer = state.layer
    val height = if (layout != null && layer != null) {
        val g = keyboardGeometry(theme, layout, layer)
        val rowSpacing = layout.metrics.rowSpacing ?: theme.keyboard.rowSpacing
        val pad = theme.keyboard.padding
        val weights = layer.rows.fold(0f) { acc, r -> acc + r.weight }
        val usable = g.keyboardHeight - pad.top - pad.bottom -
            rowSpacing * (layer.rows.size - 1)
        val lastRow = if (weights > 0f) {
            usable * (layer.rows.lastOrNull()?.weight ?: 0f) / weights
        } else {
            0f
        }
        (g.keyboardHeight - lastRow - rowSpacing - pad.bottom)
            .coerceAtLeast(g.keyboardHeight * 0.5f)
    } else {
        nominalKeyboardHeight(theme)
    }
    val style = theme.keyboard.keyStyle("default")
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .height(height.dp)
            .background(Color(theme.keyboard.background))
            // 浮層必須自己吃掉點擊。少了這一行，點在選單空白處會穿透到底下
            // 的鍵上 —— 使用者以為自己在關選單，實際上打出了一個字。
            .pointerInput(Unit) { detectTapGestures { } }
            .padding(horizontal = 8.dp, vertical = 6.dp),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(bottom = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = "⌨  鍵盤類型",
                fontSize = scaler.sp(style.labelSize * 0.75f),
                color = Color(style.foreground),
                modifier = Modifier.weight(1f),
            )
            // 進得去也要出得來：選單一定有一顆看得見的關閉鍵。
            Text(
                text = "✕",
                fontSize = scaler.sp(style.labelSize * 0.75f),
                color = Color(style.foreground),
                modifier = Modifier
                    .clickable { onEvent(KeyboardEvent.CloseSchemaPicker) }
                    .padding(horizontal = 12.dp, vertical = 4.dp),
            )
        }
        if (state.keyboardTypes.isEmpty()) {
            Text(
                text = "尚無可用方案（rs_schema_list 回傳空）",
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
                        text = group.title,
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
        Column(modifier = Modifier.weight(1f)) {
            // 主標題是佈局名 = 使用者眼裡的鍵盤長相；副標題是方案名。
            // 兩者合起來就是那個組合標題：「九宮格拼音 ／ 朙月拼音」。
            // 分兩行而不是串成一行，是因為分組之後同一組裡方案名大量重複，
            // 串成一行會讓每一項的前半截長得一模一樣，反而看不出差別。
            Text(
                text = type.title,
                fontSize = scaler.sp(style.labelSize * 0.62f),
                color = Color(if (current) style.activeForeground else style.foreground),
                maxLines = 1,
                // 這裡與 §9.6 的鍵面不同，可以省略號化：選單有第二行的方案名
                // 撐著語意，而鍵面上「注音·臺」對使用者是沒有意義的。
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                text = type.subtitle,
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

    // 按鍵回饋與重複時間來自使用者偏好(org.rimequad.ime.prefs.KeyBehavior):
    // 震動開關與強度、按鍵音與音量、重複的起始延遲與間隔。
    // 沒有人提供這個 CompositionLocal 時,KeyBehavior.DEFAULT 的行為與本檔
    // 引入偏好之前**完全一致**(震動 KEYBOARD_TAP、無按鍵音、400/60 ms)。
    val behavior = LocalKeyBehavior.current

    fun haptic() = behavior.onKeyPress(view)

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
        LaunchedEffect(key, behavior) {
            delay(behavior.repeatDelayMs.toLong())
            while (true) {
                key.send?.let { onEvent(KeyboardEvent.Send(it)) }
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

    BoxWithConstraints(
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
        val face = faceOf(key.labelFrom, key.icon, key.label, status)
        // 圖示與文字用的是兩個不同的基準字級（§8.8.1 的 icon_size / label_size）。
        val base = if (key.icon != null && key.labelFrom == LabelSource.NONE) {
            style.iconSize
        } else {
            style.labelSize
        }
        Text(
            text = face,
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
    val fromStatus = when (labelFrom) {
        LabelSource.NONE -> null
        LabelSource.INPUT_MODE -> if (status.isAsciiMode) "英" else "中"
        LabelSource.SHAPE -> if (status.isFullShape) "全" else "半"
        LabelSource.VARIANT -> if (status.isSimplified) "简" else "繁"
        LabelSource.SCHEMA_NAME -> status.schemaName.ifEmpty { null }
        LabelSource.SCHEMA_ID -> status.schemaId.ifEmpty { null }
    }
    if (fromStatus != null) return fromStatus
    icon?.let { name -> ICONS[name]?.let { return it } }
    return label
}

/** §8.8.1 的 active：佈局宣告的鎖定，或執行期狀態（英數模式的中／英鍵）。 */
internal fun isActiveFace(declared: Boolean, labelFrom: LabelSource, status: RimeStatus): Boolean =
    declared || (labelFrom == LabelSource.INPUT_MODE && status.isAsciiMode)

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
