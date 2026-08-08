package org.rimequad.ime.keyboard

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.PlatformTextStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import org.rimequad.ime.prefs.AppearanceMode
import org.rimequad.ime.prefs.HEIGHT_SCALE_MAX
import org.rimequad.ime.prefs.HEIGHT_SCALE_MIN
import org.rimequad.ime.prefs.HintVisibility
import org.rimequad.ime.prefs.PrefLabels
import org.rimequad.ime.prefs.PrefLevels
import org.rimequad.ime.prefs.SpaceBehavior
import org.rimequad.ime.theme.ActionVerb
import org.rimequad.ime.theme.KeyAction
import org.rimequad.ime.theme.KeyStyle
import org.rimequad.ime.theme.Theme
import org.rimequad.ime.R

/*
 * 鍵盤上的面板。
 *
 * 三條約束，整組面板都照著走（來自設計提案 C）：
 *   · **一頁就一頁**：不捲動、不翻頁、最多八格。一捲動就變回一個小型設定頁，
 *     那這條路就白走了。
 *   · **分兩組各給一個小標**：「鍵盤的樣子」與「打出來的字」。掃視成本從 8 降到 2。
 *   · **每格除了名字還印目前的值**。這是純圖示格與可用面板的分水嶺 ——
 *     不用點進去就知道現在是什麼。
 *
 * 顏色與尺寸全部來自主題 yaml，這裡一個色碼都沒有寫死。
 */

internal val TOGGLE_SIMPLIFICATION =
    KeyAction(ActionVerb.TOGGLE_OPTION, listOf("simplification"), "toggle:simplification")
internal val TOGGLE_ASCII_PUNCT =
    KeyAction(ActionVerb.TOGGLE_OPTION, listOf("ascii_punct"), "toggle:ascii_punct")

/* ────────────────────────────── 外框 ────────────────────────────── */

/**
 * 蓋住鍵區、但**留下底列**的浮層外框。
 *
 * 高度 = 鍵盤高度 − 最後一列。底列（空白、換行、退格、中／英）必須整列露出來，
 * 理由見 [PanelRoute] 的檔頭。佈局尚未載入時沒有底列可露，就用名目幾何鋪滿。
 */
@Composable
internal fun PanelFrame(
    heightDp: Float,
    theme: Theme,
    style: KeyStyle,
    scaler: PanelScaler,
    title: String,
    showBack: Boolean,
    onBack: () -> Unit,
    onClose: () -> Unit,
    /** 「全部設定 ›」之類的次要出口。放在標題列而不是自成一條，理由見下。 */
    trailing: (@Composable () -> Unit)? = null,
    /** 標題旁邊那句小灰字。 */
    subtitle: String? = null,
    content: @Composable () -> Unit,
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .height(heightDp.dp)
            .background(Color(theme.keyboard.background))
            // 浮層必須自己吃掉點擊。少了這一行，點在面板空白處會穿透到底下的
            // 鍵上 —— 使用者以為自己在關面板，實際上打出了一個字。
            .pointerInput(Unit) { detectTapGestures { } }
            .padding(horizontal = 8.dp, vertical = 4.dp),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(bottom = 2.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            if (showBack) {
                PanelText(
                    text = "‹",
                    style = style,
                    scaler = scaler,
                    size = 0.85f,
                    modifier = Modifier
                        .clickable(onClick = onBack)
                        .padding(horizontal = 10.dp, vertical = 4.dp),
                )
            }
            PanelText(
                text = title,
                style = style,
                scaler = scaler,
                size = 0.72f,
                bold = true,
            )
            if (subtitle != null) {
                PanelText(
                    // 這一行不是客套話，是說明書：使用者不必先關掉面板才能繼續打字。
                    // 它擠在標題旁邊而不是自成一條，是因為面板只有一百多 dp 高 ——
                    // 多一條就等於六格各矮 10dp，那比少一行字嚴重得多。
                    text = subtitle,
                    style = style,
                    scaler = scaler,
                    size = 0.46f,
                    dim = true,
                    modifier = Modifier.padding(start = 8.dp).weight(1f),
                )
            } else {
                Spacer(Modifier.weight(1f))
            }
            trailing?.invoke()
            // 進得去也要出得來：面板一定有一顆看得見的關閉鍵。
            PanelText(
                text = "✕",
                style = style,
                scaler = scaler,
                size = 0.72f,
                modifier = Modifier
                    .clickable(onClick = onClose)
                    .padding(horizontal = 10.dp, vertical = 4.dp),
            )
        }
        Box(Modifier.weight(1f)) { content() }
    }
}

/**
 * 「收成頂端一條」的面板（手感、候選字）。
 *
 * 這兩項只有真的按下去、真的看到候選字才判斷得出來，所以面板必須把鍵盤
 * 讓出來。它只佔上緣一小條，其餘的鍵**照樣按得動**。
 */
@Composable
internal fun BoxScope.TopStrip(
    theme: Theme,
    style: KeyStyle,
    scaler: PanelScaler,
    title: String,
    onBack: () -> Unit,
    onClose: () -> Unit,
    content: @Composable () -> Unit,
) {
    Column(
        modifier = Modifier
            .align(Alignment.TopCenter)
            .fillMaxWidth()
            .background(Color(theme.keyboard.background))
            .pointerInput(Unit) { detectTapGestures { } }
            .padding(horizontal = 8.dp, vertical = 6.dp),
    ) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            PanelText(
                text = "‹",
                style = style,
                scaler = scaler,
                size = 0.85f,
                modifier = Modifier.clickable(onClick = onBack).padding(horizontal = 8.dp),
            )
            PanelText(
                text = title,
                style = style,
                scaler = scaler,
                size = 0.72f,
                bold = true,
                modifier = Modifier.weight(1f),
            )
            PanelText(
                text = "✕",
                style = style,
                scaler = scaler,
                size = 0.72f,
                modifier = Modifier.clickable(onClick = onClose).padding(horizontal = 10.dp),
            )
        }
        content()
    }
}

/* ────────────────────────────── 小零件 ────────────────────────────── */

/** 讓面板共用鍵盤的字級縮放，字才不會跟鍵面差一截。 */
internal fun interface PanelScaler {
    fun sp(size: Float): androidx.compose.ui.unit.TextUnit
}

@Composable
internal fun PanelText(
    text: String,
    style: KeyStyle,
    scaler: PanelScaler,
    size: Float,
    bold: Boolean = false,
    dim: Boolean = false,
    modifier: Modifier = Modifier,
) {
    Text(
        text = text,
        fontSize = scaler.sp(style.labelSize * size),
        fontWeight = if (bold) FontWeight.SemiBold else FontWeight.Normal,
        color = Color(if (dim) style.hintColor else style.foreground),
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
        modifier = modifier,
    )
}

/**
 * 面板上的一格：名字 + **目前的值**。
 *
 * 「印出目前的值」是純圖示格與可用面板的分水嶺 —— 不用點進去就知道現在是什麼。
 * 但鍵盤上的空間是硬上限：面板只蓋得住「鍵盤高度減掉底列」，實測在
 * 1080×2400 上每一格只有二十幾 dp。所以這裡**依實際分到的高度換排版**：
 * 放得下就兩行（名字大、值小），放不下就收成一行「名字 · 值」——
 * 兩種都印得出值，而截掉一行不會。
 */
@Composable
internal fun Tile(
    name: String,
    value: String,
    style: KeyStyle,
    scaler: PanelScaler,
    active: Boolean = false,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    BoxWithConstraints(
        modifier = modifier
            .clip(RoundedCornerShape(style.cornerRadius.dp))
            .background(Color(if (active) style.activeBackground else style.background))
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        val fg = Color(if (active) style.activeForeground else style.foreground)
        val dim = if (active) fg.copy(alpha = 0.8f) else Color(style.hintColor)

        val nameSize = scaler.sp(style.labelSize * 0.6f)
        val valueSize = scaler.sp(style.hintSize * 1.05f)
        // 兩行放不放得下是**算出來的**,不是猜的。這裡兩件事必須綁在一起:
        // 給每一行明確的 lineHeight,然後拿同一組數字去比高度。少了前者,
        // 行框高度由字體的 ascent/descent 加上 font padding 決定,那是一個
        // 這裡看不到的值 —— 而原本的門檻(34dp)正是照那個看不到的值猜的,
        // 猜低了:實測格子有 41dp、判定走兩行,第二行卻被切掉一半。
        //
        // 被切掉的偏偏是**目前的值**,也就是這一格存在的理由(見上面的檔頭)。
        // 名字還在、格子還在、看起來只是有點擠 —— 又是一個「畫面正常但功能
        // 沒了」的樣子,所以寧可退成一行。
        val needed = with(LocalDensity.current) {
            (nameSize * TILE_LINE_HEIGHT).toDp() +
                (valueSize * TILE_LINE_HEIGHT).toDp() +
                TILE_VERTICAL_PADDING * 2
        }
        if (maxHeight >= needed) {
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(horizontal = 9.dp, vertical = TILE_VERTICAL_PADDING),
                verticalArrangement = Arrangement.Center,
            ) {
                Text(
                    text = name,
                    fontSize = nameSize,
                    lineHeight = nameSize * TILE_LINE_HEIGHT,
                    style = FLAT_LINE_BOX,
                    fontWeight = FontWeight.Medium,
                    color = fg,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    text = value,
                    fontSize = valueSize,
                    lineHeight = valueSize * TILE_LINE_HEIGHT,
                    style = FLAT_LINE_BOX,
                    color = dim,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        } else {
            Row(
                modifier = Modifier.fillMaxWidth().padding(horizontal = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = name,
                    fontSize = scaler.sp(style.labelSize * 0.58f),
                    fontWeight = FontWeight.Medium,
                    color = fg,
                    maxLines = 1,
                )
                Text(
                    text = "  $value",
                    fontSize = scaler.sp(style.hintSize * 1.05f),
                    color = dim,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }
}

/**
 * 一行的行框是字級的幾倍。
 *
 * 這個數字**同時**餵給 `lineHeight` 與「放不放得下」的計算,所以它不是估計值,
 * 是約定:宣告成 1.25 倍,行框就真的是 1.25 倍。1.25 對拉丁字母與漢字都放得下
 * (漢字是方的,拉丁有 ascender/descender),再小會開始擦到 g、y 的下伸部。
 */
private const val TILE_LINE_HEIGHT = 1.25f

/** 格子上下各留這麼多。同樣同時用於排版與計算。 */
private val TILE_VERTICAL_PADDING = 4.dp

/**
 * 關掉 font padding。
 *
 * 開著的話,行框會額外加上字體自己的 ascent/descent 留白 —— 那個值隨字體
 * 而異、在程式裡看不到,也算不出來,於是 `lineHeight` 的宣告就不作數了。
 * 關掉之後 lineHeight 說多少就是多少,上面那個計算才成立。
 */
private val FLAT_LINE_BOX =
    TextStyle(platformStyle = PlatformTextStyle(includeFontPadding = false))

/** 面板裡的分段控制。四個明確的檔位，不給滑桿 —— 理由見 PrefLevels。 */
@Composable
internal fun PanelSegmented(
    labels: List<String>,
    selected: Int,
    style: KeyStyle,
    scaler: PanelScaler,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier = modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        labels.forEachIndexed { i, label ->
            val on = i == selected
            Box(
                modifier = Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(style.cornerRadius.dp))
                    .background(Color(if (on) style.activeBackground else style.background))
                    .clickable { onSelect(i) }
                    .padding(vertical = 7.dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(
                    text = label,
                    fontSize = scaler.sp(style.labelSize * 0.55f),
                    fontWeight = if (on) FontWeight.SemiBold else FontWeight.Normal,
                    color = Color(if (on) style.activeForeground else style.foreground),
                    maxLines = 1,
                    textAlign = TextAlign.Center,
                )
            }
        }
    }
}

@Composable
internal fun PanelGroupLabel(text: String, style: KeyStyle, scaler: PanelScaler) {
    Text(
        text = text,
        fontSize = scaler.sp(style.hintSize * 1.1f),
        color = Color(style.hintColor),
        maxLines = 1,
        modifier = Modifier.padding(start = 4.dp, top = 1.dp, bottom = 1.dp),
    )
}

/* ────────────────────────────── 八格 ────────────────────────────── */

@Composable
internal fun QuickPanelContent(
    state: KeyboardUiState,
    style: KeyStyle,
    scaler: PanelScaler,
    baseTheme: Theme,
    onEvent: (KeyboardEvent) -> Unit,
) {
    val p = state.prefs
    val heightLabel = when {
        p.keyboardHeightScale == null || p.keyboardHeightScale == 1f ->
            stringResource(R.string.panel_height_standard)
        p.keyboardHeightScale > 1f -> stringResource(R.string.panel_height_taller)
        else -> stringResource(R.string.panel_height_shorter)
    }
    val sound = PrefLevels.indexOfSound(p, baseTheme.feedback.sound, baseTheme.feedback.soundVolume)
    val haptic =
        PrefLevels.indexOfHaptic(p, baseTheme.feedback.haptic, baseTheme.feedback.hapticStrength)
    val countIdx = PrefLevels.indexOfCandidateCount(p, baseTheme.candidates.bar.maxVisible)
    val sizeIdx = PrefLevels.indexOfCandidateSize(p)

    // ⚠ 必須 fillMaxSize：底下每一列都用 weight 分高度，而 weight 只有在
    // Column 自己有**確定的高度**時才算得出來。少了這一行，格子會全部塌成
    // 零高度，畫面上只剩兩個小標 —— 實測踩過。
    Column(
        modifier = Modifier.fillMaxSize(),
        verticalArrangement = Arrangement.spacedBy(3.dp),
    ) {
        // ── 為什麼是六格而不是八格 ──────────────────────────────────────
        // 面板只蓋得住「鍵盤高度減掉底列」那一塊，實測在 1080×2400 的
        // Pixel 6 上只有約 170dp。八格（四列）分下來每列不到 20dp，
        // 名字與值會被壓成兩條線 —— 那比少兩格嚴重得多。
        //
        // 所以收成六格兩列，被擠掉的三項各自搬進最貼近它的就地編輯器：
        //   · 鍵上小字 → 「外觀」（它就是一個純視覺的二選一）
        //   · 標點、空白鍵 → 「文字」（跟繁簡是同一個問題的三個面向）
        // 一項都沒有掉，只是不再各佔一格。
        PanelGroupLabel(stringResource(R.string.panel_group_look), style, scaler)
        Row(Modifier.weight(1f), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Tile(stringResource(R.string.panel_height), heightLabel, style, scaler, modifier = Modifier.weight(1f).fillMaxHeight()) {
                onEvent(KeyboardEvent.OpenPanel(PanelRoute.HEIGHT))
            }
            Tile(
                name = stringResource(R.string.panel_appearance),
                value = shortThemeName(state),
                style = style,
                scaler = scaler,
                modifier = Modifier.weight(1f).fillMaxHeight(),
            ) { onEvent(KeyboardEvent.OpenPanel(PanelRoute.APPEARANCE)) }
            Tile(
                name = stringResource(R.string.panel_feel),
                value = stringResource(
                    R.string.panel_summary_pair,
                    PrefLabels.sound[sound],
                    PrefLabels.haptic[haptic],
                ),
                style = style,
                scaler = scaler,
                modifier = Modifier.weight(1f).fillMaxHeight(),
            ) { onEvent(KeyboardEvent.OpenPanel(PanelRoute.FEEL)) }
        }

        PanelGroupLabel(stringResource(R.string.panel_group_output), style, scaler)
        Row(Modifier.weight(1f), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Tile(
                name = stringResource(R.string.panel_keyboard_type),
                value = state.layout?.name?.get(ConfigRepository.LOCALE)?.ifEmpty { null }
                    ?: state.status.schemaName
                        .ifEmpty { stringResource(R.string.panel_keyboard_type_auto) },
                style = style,
                scaler = scaler,
                modifier = Modifier.weight(1f).fillMaxHeight(),
            ) { onEvent(KeyboardEvent.OpenPanel(PanelRoute.TYPES)) }
            Tile(
                name = stringResource(R.string.panel_candidates),
                value = stringResource(
                    R.string.panel_summary_pair,
                    PrefLabels.candidateCount[countIdx],
                    PrefLabels.candidateSize[sizeIdx],
                ),
                style = style,
                scaler = scaler,
                modifier = Modifier.weight(1f).fillMaxHeight(),
            ) { onEvent(KeyboardEvent.OpenPanel(PanelRoute.CANDIDATES)) }
            Tile(
                name = stringResource(R.string.panel_text),
                value = stringResource(
                    R.string.panel_summary_pair,
                    stringResource(
                        if (state.status.isSimplified) R.string.panel_variant_simplified_short
                        else R.string.panel_variant_traditional_short
                    ),
                    stringResource(
                        if (state.status.isAsciiPunct) R.string.panel_punct_half_short
                        else R.string.panel_punct_full_short
                    ),
                ),
                style = style,
                scaler = scaler,
                modifier = Modifier.weight(1f).fillMaxHeight(),
            ) { onEvent(KeyboardEvent.OpenPanel(PanelRoute.TEXT)) }
        }
    }
}

@Composable
private fun shortThemeName(state: KeyboardUiState): String {
    val t = state.theme ?: return stringResource(R.string.appearance_default_theme)
    return t.name.get(ConfigRepository.LOCALE).ifEmpty { t.id }
}

/* ────────────────────────────── 外觀 ────────────────────────────── */

@Composable
internal fun AppearancePanelContent(
    state: KeyboardUiState,
    style: KeyStyle,
    scaler: PanelScaler,
    onEvent: (KeyboardEvent) -> Unit,
) {
    val p = state.prefs
    val currentFamily = p.themeId?.removeSuffix("-light")?.removeSuffix("-dark")
    // 小標放在**左邊**而不是各自佔一條：面板只有一百多 dp 高，三條小標就是
    // 五十幾 dp，等於三排控制項各矮十幾 dp。左標籤把那些高度還回來。
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        PanelSegmentedLabelled(
            stringResource(R.string.appearance_light_dark),
            listOf(
                stringResource(R.string.appearance_follow_phone),
                stringResource(R.string.appearance_always_light),
                stringResource(R.string.appearance_always_dark),
            ),
            when (p.appearanceMode) {
                AppearanceMode.LIGHT -> 1
                AppearanceMode.DARK -> 2
                else -> 0
            },
            style, scaler,
        ) { i ->
            onEvent(
                KeyboardEvent.EditPrefs { pref ->
                    pref.copy(
                        appearanceMode = when (i) {
                            1 -> AppearanceMode.LIGHT
                            2 -> AppearanceMode.DARK
                            else -> null
                        }
                    )
                }
            )
        }
        Row(verticalAlignment = Alignment.CenterVertically) {
            PanelText(
                text = stringResource(R.string.appearance_colours),
                style = style,
                scaler = scaler,
                size = 0.55f,
                dim = true,
                modifier = Modifier.width(64.dp),
            )
            // 這一列是整個面板唯一允許橫向捲動的地方：配色數量會隨主題市集
            // 成長，而它是**同一種東西的並列**，不是一個分層清單 —— 橫捲不會迷路。
            LazyRow(
                modifier = Modifier.weight(1f),
                horizontalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                items(state.themeChoices) { choice ->
                    val on = (choice.familyId.ifEmpty { null }) == currentFamily
                    Box(
                        modifier = Modifier
                            .clip(RoundedCornerShape(style.cornerRadius.dp))
                            .background(Color(if (on) style.activeBackground else style.background))
                            .clickable {
                                onEvent(
                                    KeyboardEvent.EditPrefs { pref ->
                                        pref.copy(themeId = choice.pinId.ifEmpty { null })
                                    }
                                )
                            }
                            .padding(horizontal = 10.dp, vertical = 7.dp),
                    ) {
                        Text(
                            text = choice.name,
                            fontSize = scaler.sp(style.labelSize * 0.55f),
                            color = Color(if (on) style.activeForeground else style.foreground),
                            maxLines = 1,
                        )
                    }
                }
            }
        }
        PanelSegmentedLabelled(
            stringResource(R.string.panel_hints),
            listOf(
                stringResource(R.string.panel_hints_show),
                stringResource(R.string.panel_hints_hide),
            ),
            if (p.hints == HintVisibility.HIDDEN) 1 else 0, style, scaler,
        ) { i ->
            onEvent(
                KeyboardEvent.EditPrefs { pref ->
                    pref.copy(hints = if (i == 1) HintVisibility.HIDDEN else HintVisibility.SHOWN)
                }
            )
        }
    }
}

/* ────────────────────────────── 手感 ────────────────────────────── */

@Composable
internal fun FeelStripContent(
    state: KeyboardUiState,
    style: KeyStyle,
    scaler: PanelScaler,
    baseTheme: Theme,
    onEvent: (KeyboardEvent) -> Unit,
) {
    val p = state.prefs
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        PanelSegmentedLabelled(
            stringResource(R.string.feel_sound), PrefLabels.sound,
            PrefLevels.indexOfSound(p, baseTheme.feedback.sound, baseTheme.feedback.soundVolume),
            style, scaler,
        ) { i -> onEvent(KeyboardEvent.EditPrefs { pref -> PrefLevels.withSound(pref, i) }) }
        PanelSegmentedLabelled(
            stringResource(R.string.feel_vibration), PrefLabels.haptic,
            PrefLevels.indexOfHaptic(p, baseTheme.feedback.haptic, baseTheme.feedback.hapticStrength),
            style, scaler,
        ) { i -> onEvent(KeyboardEvent.EditPrefs { pref -> PrefLevels.withHaptic(pref, i) }) }
        PanelSegmentedLabelled(
            stringResource(R.string.panel_long_press), PrefLabels.longPress,
            PrefLevels.indexOfLongPress(p), style, scaler,
        ) { i -> onEvent(KeyboardEvent.EditPrefs { pref -> PrefLevels.withLongPress(pref, i) }) }
        PanelText(
            text = stringResource(R.string.panel_feel_try),
            style = style,
            scaler = scaler,
            size = 0.5f,
            dim = true,
            modifier = Modifier.padding(top = 2.dp),
        )
    }
}

@Composable
internal fun PanelSegmentedLabelled(
    label: String,
    labels: List<String>,
    selected: Int,
    style: KeyStyle,
    scaler: PanelScaler,
    onSelect: (Int) -> Unit,
) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        PanelText(
            text = label,
            style = style,
            scaler = scaler,
            size = 0.55f,
            dim = true,
            modifier = Modifier.width(64.dp),
        )
        PanelSegmented(labels, selected, style, scaler, onSelect, Modifier.weight(1f))
    }
}

/* ────────────────────────────── 候選字 ────────────────────────────── */

@Composable
internal fun CandidatesStripContent(
    state: KeyboardUiState,
    style: KeyStyle,
    scaler: PanelScaler,
    baseTheme: Theme,
    onEvent: (KeyboardEvent) -> Unit,
) {
    val p = state.prefs
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        PanelSegmentedLabelled(
            stringResource(R.string.panel_candidate_count), PrefLabels.candidateCount,
            PrefLevels.indexOfCandidateCount(p, baseTheme.candidates.bar.maxVisible),
            style, scaler,
        ) { i -> onEvent(KeyboardEvent.EditPrefs { pref -> PrefLevels.withCandidateCount(pref, i) }) }
        PanelSegmentedLabelled(
            stringResource(R.string.panel_candidate_size), PrefLabels.candidateSize,
            PrefLevels.indexOfCandidateSize(p),
            style, scaler,
        ) { i -> onEvent(KeyboardEvent.EditPrefs { pref -> PrefLevels.withCandidateSize(pref, i) }) }
        PanelText(
            // 上面那條候選列**就是**預覽：它此刻填的是真的字，跟著這兩排一起變。
            text = stringResource(R.string.panel_candidates_preview),
            style = style,
            scaler = scaler,
            size = 0.5f,
            dim = true,
        )
    }
}

/* ────────────────────────────── 文字 ────────────────────────────── */

/**
 * 「打出來的字」的三個面向：字體、標點、空白鍵。
 *
 * 三項放同一頁是刻意的 —— 它們都是「我按下去會得到什麼字」的一部分。
 * 前兩項是**打字當下才會想改**的（這則訊息要繁體還是簡體、這句話後面要
 * 全形還是半形逗號），所以非在鍵盤上不可。
 */
@Composable
internal fun TextPanelContent(
    state: KeyboardUiState,
    style: KeyStyle,
    scaler: PanelScaler,
    onEvent: (KeyboardEvent) -> Unit,
) {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        PanelSegmentedLabelled(
            stringResource(R.string.text_characters),
            listOf(
                stringResource(R.string.text_traditional),
                stringResource(R.string.text_simplified),
            ),
            if (state.status.isSimplified) 1 else 0, style, scaler,
        ) { i ->
            // 走 toggle:simplification 而不是直接寫偏好：隨附的 luna_pinyin 系列
            // 沒有 simplification 開關，用的是 zh_hant/zh_hans 互斥選項組，
            // 那段補償邏輯只有 IME service 那一支有（見 applyVariant）。
            if ((i == 1) != state.status.isSimplified) {
                onEvent(KeyboardEvent.Act(TOGGLE_SIMPLIFICATION))
            }
        }
        PanelSegmentedLabelled(
            stringResource(R.string.text_punctuation),
            listOf(
                stringResource(R.string.text_punct_full),
                stringResource(R.string.text_punct_half),
            ),
            if (state.status.isAsciiPunct) 1 else 0, style, scaler,
        ) { i ->
            if ((i == 1) != state.status.isAsciiPunct) {
                onEvent(KeyboardEvent.Act(TOGGLE_ASCII_PUNCT))
            }
        }
        PanelSegmentedLabelled(
            stringResource(R.string.text_space_key),
            listOf(
                stringResource(R.string.panel_space_commits),
                stringResource(R.string.panel_space_always),
            ),
            if (state.prefs.spaceBehavior == SpaceBehavior.ALWAYS_SPACE) 1 else 0, style, scaler,
        ) { i ->
            onEvent(
                KeyboardEvent.EditPrefs { pref ->
                    pref.copy(spaceBehavior = if (i == 1) SpaceBehavior.ALWAYS_SPACE else null)
                }
            )
        }
    }
}

/* ────────────────────────────── 高度 ────────────────────────────── */

/**
 * 就地拖曳調高度。
 *
 * **高度不是一個數字，是一個手勢。** 把手就在鍵盤**上緣** —— 也就是要拖的
 * 那條邊本身；鍵盤即時跟著長高，底下的鍵變半透明但不消失，因為使用者要看的
 * 就是「變高之後鍵長什麼樣」。
 *
 * 「回原本 / 好了」釘在把手正下方那一條，跟著把手一起走：手指剛拖完就在那裡，
 * 不必再往下伸去找。（語燕把那兩顆放在鍵盤正中央，還蓋住三顆字母鍵。）
 *
 * 畫面上刻意不出現任何百分比或倍數 —— 使用者要的是「再高一點」，不是 1.15。
 */
@Composable
internal fun BoxScope.HeightEditor(
    state: KeyboardUiState,
    theme: Theme,
    style: KeyStyle,
    scaler: PanelScaler,
    keyboardHeightDp: Float,
    onEvent: (KeyboardEvent) -> Unit,
) {
    val density = LocalDensity.current
    val startScale = state.heightDraft ?: state.prefs.keyboardHeightScale ?: 1f
    var accumulated by remember { mutableFloatStateOf(0f) }

    // 變暗：深色底加黑遮罩是無效的（本來就黑），所以一律用「鍵盤底色」半透明
    // 蓋上去 —— 兩個模式下都是把鍵往背景推一階，輪廓還在。
    // ⚠ 一定要 matchParentSize()，不可以用 fillMaxSize()。
    //
    // 這個 Box 的父層是包住 KeyGrid 的那個 Box，而 IME 視窗的高度是
    // wrap_content —— 傳進來的 max height 是**整個螢幕**。`fillMaxSize()` 會照
    // 那個 max 撐開，於是父 Box 跟著變成螢幕那麼高，整個輸入法視窗被撐滿，
    // 鍵盤縮在最上面、底下一大片空白。實測踩過。
    // `matchParentSize()` 是在其他子項量完之後才取父層尺寸，自己不參與撐大。
    Box(
        Modifier
            .matchParentSize()
            .background(Color(theme.keyboard.background).copy(alpha = 0.62f))
            .pointerInput(Unit) { detectTapGestures { } }
    )

    Column(
        modifier = Modifier.align(Alignment.TopCenter).fillMaxWidth(),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        // 把手本身就是那條邊。
        Box(
            modifier = Modifier
                .padding(top = 2.dp)
                .width(64.dp)
                .height(20.dp)
                .clip(RoundedCornerShape(10.dp))
                .background(Color(style.activeBackground))
                .pointerInput(keyboardHeightDp, startScale) {
                    detectDragGestures(
                        onDragStart = { accumulated = 0f },
                        onDragEnd = { accumulated = 0f },
                    ) { change, drag ->
                        change.consume()
                        accumulated += with(density) { drag.y.toDp().value }
                        val h = if (keyboardHeightDp > 1f) keyboardHeightDp else 240f
                        // 往上拖 = 變高。用「拖了幾成鍵盤高」換算成倍率，
                        // 拖到哪裡就是哪裡，不必在滑桿和預覽之間來回猜。
                        val next = (startScale - accumulated / h)
                            .coerceIn(HEIGHT_SCALE_MIN, HEIGHT_SCALE_MAX)
                        onEvent(KeyboardEvent.DragHeight(next))
                    }
                },
            contentAlignment = Alignment.Center,
        ) {
            Text(
                text = "↕",
                fontSize = scaler.sp(style.labelSize * 0.6f),
                color = Color(style.activeForeground),
            )
        }
        Spacer(Modifier.height(6.dp))
        PanelText(stringResource(R.string.panel_height_drag), style, scaler, 0.55f)
        Spacer(Modifier.height(8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            PillButton(stringResource(R.string.panel_height_reset), style, scaler, filled = false) {
                onEvent(KeyboardEvent.ResetHeight)
            }
            PillButton(stringResource(R.string.panel_height_done), style, scaler, filled = true) {
                onEvent(KeyboardEvent.CommitHeight)
            }
        }
    }
}

@Composable
private fun PillButton(
    text: String,
    style: KeyStyle,
    scaler: PanelScaler,
    filled: Boolean,
    onClick: () -> Unit,
) {
    Box(
        modifier = Modifier
            .clip(RoundedCornerShape(18.dp))
            .background(Color(if (filled) style.activeBackground else style.background))
            .clickable(onClick = onClick)
            .padding(horizontal = 20.dp, vertical = 9.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = text,
            fontSize = scaler.sp(style.labelSize * 0.6f),
            fontWeight = FontWeight.SemiBold,
            color = Color(if (filled) style.activeForeground else style.foreground),
            maxLines = 1,
        )
    }
}

/* ────────────────────────────── 逃生梯 ────────────────────────────── */

/**
 * 「全部設定 ›」。
 *
 * 刻意是整個面板上最不顯眼的一項 —— 這條路的預設就是**不去 App**。
 * 它存在只是為了那幾件鍵盤做不到的事：下載方案、匯入檔案、檢查更新、
 * 重新整理詞庫、看診斷。
 */
@Composable
internal fun AllSettingsLink(
    style: KeyStyle,
    scaler: PanelScaler,
    onOpenApp: () -> Unit,
) {
    PanelText(
        text = stringResource(R.string.panel_all_settings),
        style = style,
        scaler = scaler,
        size = 0.5f,
        dim = true,
        modifier = Modifier
            .clickable(onClick = onOpenApp)
            .padding(horizontal = 8.dp, vertical = 4.dp),
    )
}

/**
 * 「候選字」面板的預覽用字。它們走的是**真的候選列**，不是另畫一條假的。
 *
 * 不進 strings.xml:這是一組中文候選字的樣本,用來讓使用者看清楚字級與筆畫,
 * 換成英文單字就示範不出「候選字看不看得清楚」這件事。介面語言是英文的
 * 使用者,打的仍然是中文。
 */
internal val SAMPLE_CANDIDATES = listOf("你好", "妳好", "擬好", "泥壕", "你毫", "尼號", "妮豪", "泥號", "擬耗")
