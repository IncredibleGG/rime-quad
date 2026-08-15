@file:OptIn(androidx.compose.ui.ExperimentalComposeUiApi::class)

package org.luminakey.ime.home

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.res.Configuration
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import kotlinx.coroutines.launch
import org.luminakey.ime.R
import org.luminakey.ime.core.DeployEstimate
import org.luminakey.ime.core.RimeCore
import org.luminakey.ime.core.RimeRuntime
import org.luminakey.ime.keyboard.ConfigRepository
import org.luminakey.ime.net.NetworkRequiredCard
import org.luminakey.ime.net.NetworkScreen
import org.luminakey.ime.net.NetworkSwitchCard
import org.luminakey.ime.net.rememberNetworkEnabled
import org.luminakey.ime.core.KeyRole
import org.luminakey.ime.prefs.AppearanceMode
import org.luminakey.ime.prefs.FeedbackPreview
import org.luminakey.ime.prefs.KeyHaptics
import org.luminakey.ime.prefs.KeySounds
import org.luminakey.ime.prefs.HintVisibility
import org.luminakey.ime.prefs.KeyRemapSection
import org.luminakey.ime.prefs.PrefLabels
import org.luminakey.ime.prefs.PrefLevels
import org.luminakey.ime.prefs.PrefsStore
import org.luminakey.ime.prefs.SpaceBehavior
import org.luminakey.ime.prefs.UserPrefs
import org.luminakey.ime.prefs.resolveBaseTheme
import org.luminakey.ime.store.BackupSection
import org.luminakey.ime.store.StoreController
import org.luminakey.ime.store.StoreScreen
import org.luminakey.ime.update.UpdateController
import org.luminakey.ime.update.UpdateSection

/**
 * 第二層。**每一頁都是終點** —— 這裡沒有任何一個 `›`，全部是分段控制、
 * 開關、卡片，看得到全部選項、當場改完。唯一的例外是「下載更多鍵盤」，
 * 它本質上就是另一個世界的入口。
 */

/* ────────────────────────── 首頁那四行灰字 ────────────────────────── */

class HomeSummary(
    val keyboard: String,
    val appearance: String,
    val feel: String,
    val text: String,
)

@Composable
fun rememberHomeSummary(prefs: UserPrefs): HomeSummary {
    val context = LocalContext.current
    val repo = remember { ConfigRepository(context) }
    val systemDark = (LocalConfiguration.current.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
        Configuration.UI_MODE_NIGHT_YES
    val base = remember(prefs.themeId, prefs.appearanceMode, systemDark) {
        resolveBaseTheme(repo, prefs, systemDark)
    }
    val current = remember(prefs) { currentKeyboardOf(context, availableKeyboards(context)) }

    val sep = stringResource(R.string.summary_separator)
    val soundLabels = PrefLabels.sound
    val hapticLabels = PrefLabels.haptic

    val appearance = buildString {
        append(
            stringResource(
                when (prefs.appearanceMode) {
                    AppearanceMode.LIGHT -> R.string.summary_always_light
                    AppearanceMode.DARK -> R.string.summary_always_dark
                    else -> R.string.summary_follow_phone
                }
            )
        )
        append(sep)
        append(familyNameOf(base.name.get(ConfigRepository.LOCALE), base.id))
    }
    val feel = buildString {
        val s = PrefLevels.indexOfSound(prefs, base.feedback.sound, base.feedback.soundVolume)
        val h = PrefLevels.indexOfHaptic(prefs, base.feedback.haptic, base.feedback.hapticStrength)
        append(
            if (s == 0) stringResource(R.string.summary_silent)
            else stringResource(R.string.summary_sound, soundLabels[s])
        )
        append(sep)
        // 「震動中」會被讀成「正在震動」，所以這裡一律加冒號。
        append(
            if (h == 0) stringResource(R.string.summary_no_vibration)
            else stringResource(R.string.summary_vibration, hapticLabels[h])
        )
    }
    // 兩項都沒設過時不要寫兩次「跟著鍵盤」—— 那看起來像重複，也讓人以為
    // 有兩個地方可以設同一件事。
    val text = if (prefs.simplification == null && prefs.asciiPunct == null) {
        stringResource(R.string.summary_text_all_default)
    } else {
        buildString {
            append(
                stringResource(
                    when (prefs.simplification) {
                        true -> R.string.summary_simplified
                        false -> R.string.summary_traditional
                        null -> R.string.summary_chars_follow
                    }
                )
            )
            append(sep)
            append(
                stringResource(
                    when (prefs.asciiPunct) {
                        true -> R.string.summary_punct_half
                        false -> R.string.summary_punct_full
                        null -> R.string.summary_punct_follow
                    }
                )
            )
        }
    }
    return HomeSummary(
        // ⚠ 方案名要先經過 [SchemaVariantLabel]。少了它,首頁上「打字方式
        // 注音·臺灣正體」與它正下方那一列「打出來的字 簡體」會自己打自己 ——
        // 兩句話講的是不同的東西（方案的預設字集 vs 使用者的覆寫）,而使用者
        // 沒有義務知道這件事。判準與「什麼時候不該動」見那支的檔頭。
        keyboard = describeKeyboard(current, prefs.simplification),
        appearance = appearance,
        feel = feel,
        text = text,
    )
}

/**
 * 「預設淺色」→「預設」、「Android 慣例・淺色」→「Android 慣例」。
 *
 * 深淺已經是**另一個**控制項了，把它留在配色的名字裡會讓使用者以為
 * 這裡有兩個地方可以決定同一件事。
 */
internal fun familyNameOf(name: String, fallbackId: String): String {
    var s = name.ifBlank { fallbackId }
    // 三種語言各有自己的寫法，而且英文的深淺常常寫成括號：
    //   「預設淺色」「默认浅色」「Default Light」「Android style (light)」
    // 全部要剝掉，因為深淺已經是**另一個**控制項了。
    for (suffix in DEPTH_SUFFIXES) {
        if (s.endsWith(suffix, ignoreCase = true)) {
            s = s.dropLast(suffix.length)
            break
        }
    }
    return s.trimEnd(' ', '・', '·', '-').ifBlank { familyIdOf(fallbackId) }
}

private val DEPTH_SUFFIXES = listOf(
    "(light)", "(dark)", "（淺色）", "（深色）", "（浅色）",
    "淺色", "深色", "浅色", "Light", "Dark",
)

internal const val DEFAULT_THEME_FAMILY = "default"

internal fun familyIdOf(themeId: String): String =
    themeId.removeSuffix("-light").removeSuffix("-dark")

/* ────────────────────────────── 頁面外框 ────────────────────────────── */

@Composable
private fun Page(
    title: String,
    onBack: () -> Unit,
    content: @Composable () -> Unit,
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = Space.s6),
    ) {
        Spacer(Modifier.height(Space.s7))
        PageHeader(title = title, onBack = onBack)
        Spacer(Modifier.height(Space.s5))
        content()
        Spacer(Modifier.height(Space.s8))
    }
}

/* ────────────────────────────── 鍵盤 ────────────────────────────── */

@Composable
fun KeyboardPage(
    controller: StoreController,
    onBack: () -> Unit,
    onOpenStore: () -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    // 簡繁是**使用者的覆寫**,它一改,分組小標上的方案名就要跟著改
    //   —— 所以它必須進 remember 的鍵,否則小標會停在切換之前那一版。
    val simplified = rememberSimplification()
    val groups = remember(RimeRuntime.phase, simplified) {
        availableKeyboardGroups(context, simplified)
    }
    val all = remember(groups) { groups.flatMap { it.second } }
    var picked by remember { mutableStateOf(currentKeyboardOf(context, all)) }

    Page(title = stringResource(R.string.page_typing_method), onBack = onBack) {
        Text(
            text = stringResource(R.string.keyboard_page_hint),
            fontSize = TypeScale.t5,
            lineHeight = TypeScale.t5Line,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(Space.s5))
        if (all.isEmpty()) {
            // ⚠ 用**傳進來的**那一個 controller,不要在這裡 new 一個。
            //   StoreController 不是單例:每一個實例有自己的工作執行緒與自己的
            //   job 狀態,而畫進度的 StoreOverlays 盯的是 AppScreen 持有的那一個。
            //   在這裡 new 一個的話,按下去會**真的**開始整理,但畫面上不會有
            //   任何進度、任何結果、任何提示 —— 使用者按了之後看到的是「沒反應」,
            //   而且要等十幾秒才會發現字詞其實整理過了。
            TypingEmptyState(busy = controller.busy, onRefresh = { controller.redeploy() })
        } else {
            for ((schemaName, types) in groups) {
                SectionLabel(schemaName)
                KeyboardGrid(
                    types = types,
                    selectedKey = picked?.key,
                    onPick = { t ->
                        picked = t
                        scope.launch { applyKeyboardChoice(context, t) }
                    },
                    showSubtitle = false,
                )
                Spacer(Modifier.height(Space.s7))
            }
        }
        Spacer(Modifier.height(Space.s7))
        PlainCard {
            // 想換打字方式的人本來就在這一屏，所以市集放這裡而不是「進階」。
            NavRow(
                title = stringResource(R.string.keyboard_get_more),
                value = stringResource(R.string.keyboard_get_more_value),
                onClick = onOpenStore,
            )
        }

        // 自訂鍵位。它是「我要用哪種打字方式」這個問題的延伸，所以跟卡片
        // 放在同一頁。整塊在 prefs/KeyRemapSection.kt 裡，這裡只多一行 ——
        // 那條線自己在檔案裡寫了「這塊之後會被搬走」，這裡就是那個去處。
        Spacer(Modifier.height(Space.s7))
        KeyRemapSection()
        Spacer(Modifier.height(Space.s4))
        Text(
            text = stringResource(R.string.keyboard_page_footnote),
            fontSize = TypeScale.t5,
            lineHeight = TypeScale.t5Line,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

/**
 * 一個打字方式都沒有時的空狀態（§4.7 / §7.3）。
 *
 * 三件事缺一不可：**為什麼是空的、這是不是正常、下一步按哪裡**。
 * 這一頁的空**不正常**（安裝時就該有四種），所以第二句必須明講 ——
 * 說成「這是正常的」會讓使用者安心地等一個永遠不會來的東西。
 *
 * ⚠ 整理中時這裡不是一顆按不動的按鈕，而是一段狀態文字：
 * `redeploy()` 內部 `if (busy) return`，所以那時的按鈕會是一顆按了
 * 什麼都不會發生的按鈕 —— 正是本專案抓過五次的那一種。
 */
@Composable
private fun TypingEmptyState(busy: Boolean, onRefresh: () -> Unit) {
    PlainCard {
        Column(Modifier.fillMaxWidth().padding(Space.s5)) {
            Text(
                text = stringResource(R.string.typing_empty_title),
                fontSize = TypeScale.t3,
                fontWeight = FontWeight.SemiBold,
            )
            Text(
                text = stringResource(R.string.typing_empty_body),
                fontSize = TypeScale.t5,
                lineHeight = TypeScale.t5Line,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(top = Space.s1),
            )
            Spacer(Modifier.height(Space.s4))
            if (busy) {
                Text(
                    text = stringResource(R.string.advanced_redeploy_running),
                    fontSize = TypeScale.t4,
                    color = MaterialTheme.colorScheme.primary,
                )
            } else {
                Button(
                    onClick = onRefresh,
                    modifier = Modifier.fillMaxWidth().heightIn(min = Dimens.touchTarget),
                ) {
                    Text(stringResource(R.string.typing_empty_action), fontSize = TypeScale.t4)
                }
            }
        }
    }
}

/* ────────────────────────────── 外觀 ────────────────────────────── */

@Composable
fun AppearancePage(onBack: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val store = remember { PrefsStore.get(context) }
    val prefs by store.flow.collectAsState(initial = store.current)
    val repo = remember { ConfigRepository(context) }
    val families = remember { themeFamilies(repo) }
    val focus = remember { FocusRequester() }
    // requestFocus() 只是把游標放進去，**不會**把軟鍵盤叫上來 —— 使用者按了
    // 「開始調」卻什麼都沒發生。要鍵盤真的升起，得再明講一次。
    val keyboardController = LocalSoftwareKeyboardController.current
    var askedHeight by remember { mutableStateOf(false) }
    val systemDark = (LocalConfiguration.current.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
        Configuration.UI_MODE_NIGHT_YES

    fun edit(block: (UserPrefs) -> UserPrefs) {
        scope.launch { store.update(block) }
    }

    Page(title = stringResource(R.string.page_appearance), onBack = onBack) {
        SectionLabel(stringResource(R.string.appearance_colours))
        // 「預設」那一組的 pinId 是空字串 = 不指定主題（見 themeFamilies）。
        // 所以這裡不另外放一顆「未設定」晶片 —— 兩顆都叫「預設」只會讓人以為
        // 有兩個地方可以決定同一件事。
        val currentFamily = prefs.themeId?.let { familyIdOf(it) } ?: DEFAULT_THEME_FAMILY
        Column(verticalArrangement = Arrangement.spacedBy(Space.s3)) {
            for (chunk in families.chunked(2)) {
                Row(horizontalArrangement = Arrangement.spacedBy(Space.s3)) {
                    for (family in chunk) {
                        Chip(
                            label = family.name,
                            selected = currentFamily == family.id,
                            onClick = {
                                // 只釘配色，**不動深淺** —— 深淺是下面那個控制項的事。
                                // 釘的是淺色那一份；跟隨系統時 LayoutHost 會在夜間
                                // 自動換到它宣告的 counterpart。
                                edit { p -> p.copy(themeId = family.pinId.ifEmpty { null }) }
                            },
                            modifier = Modifier.weight(1f),
                        )
                    }
                    if (chunk.size == 1) Spacer(Modifier.weight(1f))
                }
            }
        }

        SettingGroup(
            label = stringResource(R.string.appearance_light_dark),
            options = listOf(
                null to stringResource(R.string.appearance_follow_phone),
                AppearanceMode.LIGHT to stringResource(R.string.appearance_always_light),
                AppearanceMode.DARK to stringResource(R.string.appearance_always_dark),
            ),
            selected = prefs.appearanceMode.takeIf { it != AppearanceMode.FOLLOW_SYSTEM },
            onSelect = { v -> edit { p -> p.copy(appearanceMode = v) } },
        )

        Spacer(Modifier.height(Space.s4))
        SectionLabel(stringResource(R.string.appearance_height))
        PlainCard {
            Column(Modifier.padding(Space.s5)) {
                Text(
                    text = stringResource(R.string.appearance_height_title),
                    fontSize = TypeScale.t3,
                    fontWeight = FontWeight.Medium,
                )
                Text(
                    // 這一項刻意**不給滑桿**：滑桿要在數值與預覽之間來回猜，
                    // 拖曳不用 —— 拖到哪裡就是哪裡，拖完就是最終樣子。
                    text = stringResource(R.string.appearance_height_body),
                    fontSize = TypeScale.t5,
                    lineHeight = TypeScale.t5Line,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(top = Space.s1),
                )
                Spacer(Modifier.height(Space.s4))
                OutlinedButton(
                    onClick = {
                        KeyboardPanelRequest.request(context, KeyboardPanelRequest.HEIGHT)
                        askedHeight = true
                        focus.requestFocus()
                        keyboardController?.show()
                    },
                    modifier = Modifier.fillMaxWidth().heightIn(min = Dimens.touchTarget),
                ) {
                    Text(
                        text = stringResource(R.string.appearance_height_action),
                        fontSize = TypeScale.t4,
                    )
                }
                if (askedHeight) {
                    Text(
                        text = stringResource(R.string.appearance_height_after),
                        fontSize = TypeScale.t5,
                        lineHeight = TypeScale.t5Line,
                        color = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.padding(top = Space.s3),
                    )
                }
            }
        }

        SettingGroup(
            label = stringResource(R.string.appearance_candidate_size),
            options = PrefLabels.candidateSize.mapIndexed { i, s -> i to s },
            selected = PrefLevels.indexOfCandidateSize(prefs),
            onSelect = { i -> edit { p -> PrefLevels.withCandidateSize(p, i) } },
        )

        val baseCount = remember(prefs.themeId, prefs.appearanceMode) {
            resolveBaseTheme(repo, prefs, systemDark).candidates.bar.maxVisible
        }
        SettingGroup(
            label = stringResource(R.string.appearance_candidate_count),
            options = PrefLabels.candidateCount.mapIndexed { i, s -> i to s },
            selected = PrefLevels.indexOfCandidateCount(prefs, baseCount),
            onSelect = { i -> edit { p -> PrefLevels.withCandidateCount(p, i) } },
            // A 層設定：改了要重新整理字詞才生效，所以當場說（§4.1 最後一條）。
            // 不說的話使用者會改完、發現沒變、再改一次，然後認定壞掉。
            footnote = stringResource(R.string.appearance_candidate_count_note),
        )

        Spacer(Modifier.height(Space.s4))
        PlainCard {
            SwitchRow(
                title = stringResource(R.string.appearance_hints),
                subtitle = stringResource(R.string.appearance_hints_sub),
                checked = prefs.hints != HintVisibility.HIDDEN,
                onCheckedChange = { on ->
                    edit { p ->
                        p.copy(hints = if (on) HintVisibility.SHOWN else HintVisibility.HIDDEN)
                    }
                },
            )
        }

        Spacer(Modifier.height(Space.s7))
        SectionLabel(stringResource(R.string.appearance_preview))
        TryField(focusRequester = focus)
    }
}

/** 一組淺／深主題 = 使用者眼裡的一個「配色」。 */
class ThemeFamily(val id: String, val name: String, val pinId: String)

internal fun themeFamilies(repo: ConfigRepository): List<ThemeFamily> {
    val out = LinkedHashMap<String, ThemeFamily>()
    for (id in repo.themeIds().sorted()) {
        val family = familyIdOf(id)
        if (out.containsKey(family)) continue
        val theme = repo.loadTheme(id).value ?: continue
        // 釘淺色那一份：applyThemePrefs 會在需要深色時自己跳到 counterpart。
        val pin = if (id.endsWith("-dark")) id.removeSuffix("-dark") + "-light" else id
        out[family] = ThemeFamily(
            id = family,
            name = familyNameOf(theme.name.get(ConfigRepository.LOCALE), id),
            // 「預設」= 不指定，不是「指定 default-light」。存一份預設值的副本
            // 會讓主題檔日後更新時使用者被釘在舊的那一份上（UserPrefs 第一原則）。
            pinId = when {
                family == DEFAULT_THEME_FAMILY -> ""
                repo.themeIds().contains(pin) -> pin
                else -> id
            },
        )
    }
    return out.values.toList()
}

/* ────────────────────────────── 手感 ────────────────────────────── */

@Composable
fun FeelPage(onBack: () -> Unit) {
    val context = LocalContext.current
    val view = LocalView.current
    val scope = rememberCoroutineScope()
    val store = remember { PrefsStore.get(context) }
    val prefs by store.flow.collectAsState(initial = store.current)
    val repo = remember { ConfigRepository(context) }
    val systemDark = (LocalConfiguration.current.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
        Configuration.UI_MODE_NIGHT_YES
    val base = remember(prefs.themeId, prefs.appearanceMode, systemDark) {
        resolveBaseTheme(repo, prefs, systemDark)
    }

    // 自帶音色要有 SoundPool 才播得出來。輸入法那一份可能還沒起來（使用者
    // 可能根本還沒啟用這個鍵盤），所以這一頁自己拿一份 —— KeySounds 用的是
    // 引用計數，兩邊各自 acquire/release 不會互相踩到。
    DisposableEffect(Unit) {
        KeySounds.acquire(context)
        onDispose { KeySounds.release() }
    }
    // 裝置能力，量一次。false 時下面那一行會把「這支手機分不出輕重」講出來。
    val amplitudeControl = remember { KeyHaptics.hasAmplitudeControl(context) }

    fun edit(block: (UserPrefs) -> UserPrefs) {
        scope.launch { store.update(block) }
    }

    /**
     * 改設定，**並且當場試一次**。
     *
     * ⚠ 試播餵的是 `block(prefs)` 而不是畫面上的 `prefs`：DataStore 的寫入是
     *   非同步的，用舊的那一份試播會每次都慢一格 —— 選「水滴」聽到「敲擊」，
     *   看起來像「音色沒生效」。
     */
    fun editAndTry(role: KeyRole = KeyRole.STANDARD, block: (UserPrefs) -> UserPrefs) {
        val next = block(prefs)
        scope.launch { store.update(block) }
        FeedbackPreview.play(context, view, base, next, role)
    }

    Page(title = stringResource(R.string.page_feel), onBack = onBack) {
        Text(
            // 四項合成一頁是因為它們是同一個問題的四個面向：「這個鍵盤按起來
            // 的感覺」。分成四個設定項就是把一件事拆成四次操作。
            text = stringResource(R.string.feel_intro),
            fontSize = TypeScale.t5,
            lineHeight = TypeScale.t5Line,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        val soundLevel =
            PrefLevels.indexOfSound(prefs, base.feedback.sound, base.feedback.soundVolume)
        SettingGroup(
            label = stringResource(R.string.feel_sound),
            options = PrefLabels.sound.mapIndexed { i, s -> i to s },
            selected = soundLevel,
            onSelect = { i -> editAndTry { p -> PrefLevels.withSound(p, i) } },
        )
        // 音色。與音量分開兩個控制項 —— 合成一個會變成 8 格（ui-design §4.2）。
        //
        // 「按鍵音＝關」的時候這一列**不畫成灰的、也不隱藏**（§1）：點下去會
        // 順便把音量開到「小」並試播一次。一次操作取代兩次，而且沒有死控制項。
        SettingGroup(
            label = stringResource(R.string.feel_timbre),
            options = PrefLabels.timbre.mapIndexed { i, s -> i to s },
            selected = PrefLevels.indexOfTimbre(prefs),
            onSelect = { i ->
                editAndTry { p -> PrefLevels.withTimbre(p, i, soundLevel) }
            },
            // ⚠ 走查說「按鍵音＝關的時候這一列該灰而沒灰」。**刻意不灰**，
            //   理由就在上面那一段：灰掉會變成一個死控制項，使用者得先開音量
            //   再回來選音色（兩次操作），而現在點一下就同時做完兩件事。
            //
            //   但走查指出的困惑是真的：關著的時候，這一排看起來像沒有作用。
            //   所以補的是**一句話**而不是一層灰 —— 說出點下去會發生什麼。
            //   （這是刻意偏離工單字面的一條，見報告。）
            footnote = if (soundLevel == 0) {
                stringResource(R.string.feel_timbre_sound_off)
            } else {
                null
            },
        )
        SettingGroup(
            label = stringResource(R.string.feel_vibration),
            options = PrefLabels.haptic.mapIndexed { i, s -> i to s },
            selected = PrefLevels.indexOfHaptic(
                prefs,
                base.feedback.haptic,
                base.feedback.hapticStrength,
            ),
            onSelect = { i -> editAndTry { p -> PrefLevels.withHaptic(p, i) } },
            // ⚠ 這一行有兩個工作，兩個都不能省：
            //   1. 說明為什麼權限清單上有「震動」—— 那是使用者查得到的東西，
            //      不解釋就會變成一個沒有答案的疑問。
            //   2. 馬達沒有振幅控制時**要講出來**。無聲降級等於在畫面上留三個
            //      感覺一樣的假檔位。
            footnote = stringResource(
                if (amplitudeControl) R.string.feel_haptic_note else R.string.feel_haptic_flat
            ),
        )
        SettingGroup(
            label = stringResource(R.string.feel_long_press),
            options = PrefLabels.longPress.mapIndexed { i, s -> i to s },
            selected = PrefLevels.indexOfLongPress(prefs),
            // 長按門檻沒有「試播」可言 —— 它是一段時間，不是一個回饋。
            onSelect = { i -> edit { p -> PrefLevels.withLongPress(p, i) } },
        )
        Spacer(Modifier.height(Space.s7))
        SectionLabel(stringResource(R.string.feel_try))
        TryField(placeholder = stringResource(R.string.feel_try_placeholder))
    }
}

/* ────────────────────────────── 文字 ────────────────────────────── */

@Composable
fun TextPage(onBack: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val store = remember { PrefsStore.get(context) }
    val prefs by store.flow.collectAsState(initial = store.current)

    fun edit(block: (UserPrefs) -> UserPrefs) {
        scope.launch { store.update(block) }
    }

    val followKeyboard = stringResource(R.string.text_follow_keyboard)

    Page(title = stringResource(R.string.page_text), onBack = onBack) {
        SettingGroup(
            label = stringResource(R.string.text_characters),
            // 「跟著鍵盤」= 不干預，讓方案自己的預設生效。它與 false 不是同一件事，
            // 所以不能做成兩態開關 —— 那樣就回不去不干預了。
            options = listOf(
                null to followKeyboard,
                false to stringResource(R.string.text_traditional),
                true to stringResource(R.string.text_simplified),
            ),
            selected = prefs.simplification,
            onSelect = { v -> edit { p -> p.copy(simplification = v) } },
        )
        // 字集守門。放在「字體」正下方，因為它只有在使用者選過簡或繁之後
        // 才說得通 —— 它的意思就是「我剛才選的那一種，要不要**只**留那一種」。
        //
        // ⚠ 這一組的說明文字是**產品承諾的邊界**，不是裝飾。這一層做不到
        //   絕對純度（見 docs/settings-model.md §4.7），畫面上就不可以讓人
        //   以為它做得到。文案要說三件事：會拿掉什麼、拿不乾淨、以及
        //   「整段被拿光時會退回原樣」。
        SettingGroup(
            label = stringResource(R.string.text_charset),
            // 只有兩態，而且**沒有「跟著鍵盤」** —— 方案自己沒有這個概念，
            // 不干預在這裡等於關掉，多一格只會讓人以為有第三種行為。
            options = listOf(
                true to stringResource(R.string.text_charset_strict),
                false to stringResource(R.string.text_charset_all),
            ),
            // 未設定 = 開。寫 `!= false` 而不是 `== true`，見 UserPrefs.charsetGuard。
            selected = prefs.charsetGuard != false,
            onSelect = { v -> edit { p -> p.copy(charsetGuard = v) } },
            footnote = stringResource(R.string.text_charset_note),
        )
        SettingGroup(
            label = stringResource(R.string.text_punctuation),
            options = listOf(
                null to followKeyboard,
                false to stringResource(R.string.text_punct_full),
                true to stringResource(R.string.text_punct_half),
            ),
            selected = prefs.asciiPunct,
            onSelect = { v -> edit { p -> p.copy(asciiPunct = v) } },
        )
        SettingGroup(
            label = stringResource(R.string.text_space_key),
            options = listOf(
                null to stringResource(R.string.text_space_commits),
                SpaceBehavior.ALWAYS_SPACE to stringResource(R.string.text_space_always),
            ),
            selected = prefs.spaceBehavior.takeIf { it == SpaceBehavior.ALWAYS_SPACE },
            onSelect = { v -> edit { p -> p.copy(spaceBehavior = v) } },
        )
        Spacer(Modifier.height(Space.s7))
        SectionLabel(stringResource(R.string.text_try))
        TryField()
    }
}

/* ────────────────────────────── 連網紀錄 ────────────────────────────── */

@Composable
fun NetworkPage(onBack: () -> Unit) {
    // ⚠ 這一頁**不能**用 [Page]：`NetworkScreen` 內部是 LazyColumn，塞進一個
    // 會捲動的 Column 裡會被量到無限高，Compose 直接丟 IllegalStateException
    // ——畫面不是變醜，是整個 app 當掉，使用者連返回鍵都按不到。實測踩過。
    // 所以標題與開關固定在上面，清單自己佔滿剩下的高度、自己捲。
    Column(
        modifier = Modifier.fillMaxSize().padding(horizontal = Space.s6),
    ) {
        Spacer(Modifier.height(Space.s7))
        PageHeader(title = stringResource(R.string.page_network), onBack = onBack)
        Spacer(Modifier.height(Space.s5))
        NetworkSwitchCard()
        Spacer(Modifier.height(Space.s5))
        NetworkScreen(modifier = Modifier.fillMaxSize())
    }
}

/* ────────────────────────────── 市集 ────────────────────────────── */

@Composable
fun StorePage(controller: StoreController, onBack: () -> Unit) {
    val online = rememberNetworkEnabled()
    Column(
        modifier = Modifier.fillMaxSize().padding(horizontal = Space.s6),
    ) {
        Spacer(Modifier.height(Space.s7))
        PageHeader(title = stringResource(R.string.page_store), onBack = onBack)
        if (!online) {
            NetworkRequiredCard(what = stringResource(R.string.network_what_download_keyboards))
            Spacer(Modifier.height(Space.s4))
        }
        StoreScreen(controller = controller, modifier = Modifier.fillMaxSize())
    }
}

/* ────────────────────────── 進階與問題回報 ────────────────────────── */

@Composable
fun AdvancedPage(controller: StoreController, onBack: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val store = remember { PrefsStore.get(context) }
    val prefs by store.flow.collectAsState(initial = store.current)
    val updates = remember { UpdateController.get(context) }
    val online = rememberNetworkEnabled()
    var copied by remember { mutableStateOf(false) }
    var confirmReset by remember { mutableStateOf(false) }

    val importer = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri -> if (uri != null) controller.importLocal(uri) }

    // 「曾經走完引導」不是設定，是一件**已經發生過的事實** —— 回復預設時保留它。
    // 不保留的話，使用者按下「全部回復預設」之後，下一次冷啟動會被丟回引導頁，
    // 而他明明早就設好了，只是想把設定歸零（§4.9 最後一條）。
    val settingsPristine = prefs.copy(onboardingDone = null) == UserPrefs()

    Page(title = stringResource(R.string.page_advanced), onBack = onBack) {

        Text(
            text = stringResource(R.string.advanced_intro),
            fontSize = TypeScale.t5,
            lineHeight = TypeScale.t5Line,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(Space.s5))

        PlainCard {
            // ⚠ 整理中時**不畫成一列可以點的東西**。
            //   `redeploy()` 內部第一行是 `if (busy) return`，所以整理中的那一列
            //   是一顆按了什麼都不會發生的按鈕 —— 畫面完全正常、自動化全過、
            //   使用者按了沒反應，正是本專案抓過五次的那一種缺陷。
            //   進行中的進度由 StoreOverlays 統一畫，這裡只誠實地說「在跑了」。
            if (controller.busy) {
                StatusRow(
                    title = stringResource(R.string.advanced_redeploy),
                    detail = stringResource(R.string.advanced_redeploy_running),
                )
            } else {
                NavRow(
                    title = stringResource(R.string.advanced_redeploy),
                    subtitle = stringResource(
                        R.string.advanced_redeploy_sub,
                        DeployEstimate.TYPICAL_SECONDS,
                    ),
                    onClick = { controller.redeploy() },
                )
            }
            RowDivider()
            NavRow(
                title = stringResource(R.string.advanced_import),
                subtitle = stringResource(R.string.advanced_import_sub),
                onClick = { importer.launch(arrayOf("application/zip", "*/*")) },
            )
        }

        // 換手機的路徑。allowBackup=false 的代價就是這一段 —— 沒有它，
        // 使用者換手機等於從零開始。放在「更新」之前：它比更新更常被找。
        // 整塊 UI 住在 store/BackupSection.kt，這裡只留入口。
        Spacer(Modifier.height(Space.s7))
        SectionLabel(stringResource(R.string.backup_section))
        BackupSection()

        Spacer(Modifier.height(Space.s7))
        SectionLabel(stringResource(R.string.advanced_updates))
        if (!online) {
            NetworkRequiredCard(what = stringResource(R.string.network_what_check_updates))
            Spacer(Modifier.height(Space.s4))
        }
        UpdateSection(
            controller = updates,
            autoCheck = prefs.autoCheckUpdate,
            onAutoCheckChange = { v -> scope.launch { store.update { p -> p.copy(autoCheckUpdate = v) } } },
        )

        Spacer(Modifier.height(Space.s7))
        SectionLabel(stringResource(R.string.advanced_report))
        val report = diagnosticsText(context, controller)
        // ⚠ 那句「這些數字是給我們看的」**在框裡面**，就在傾印的正上方。
        //
        // 走查 A5 抓到的是這一段的位置，不是它的存在：這片傾印（schema、
        // deploy、patch、ABI、路徑、方案 id）在 1080×2400 上超過一個螢幕高，
        // 而說明擺在框外的上方 —— 使用者捲到傾印本身時，那句話早就滾出畫面了。
        // 於是他在「出事時被引導去的那一頁」上，看到的是一整片沒有人解釋的
        // 開發者輸出。Windows 端（windows/common/ui_strings.cc 的
        // kDiagnosticsNote）之所以不算外漏，就是因為那句話跟著那片東西。
        //
        // 同一句話還缺了後半段的**出口**：Windows 寫的是「你不用懂 ——
        // 回報問題時複製過去就好」。少了後半句，讀者知道「這不是給我看的」
        // 卻不知道該拿它做什麼。三種語言都補齊了。
        PlainCard {
            Column(Modifier.padding(Space.s5)) {
                Text(
                    text = stringResource(R.string.advanced_report_sub),
                    fontSize = TypeScale.t5,
                    lineHeight = TypeScale.t5Line,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(bottom = Space.s4),
                )
                Text(
                    text = report,
                    fontSize = TypeScale.t6,
                    fontFamily = FontFamily.Monospace,
                )
            }
        }
        Spacer(Modifier.height(Space.s4))
        // 本頁唯一的實心按鈕（A1）。破壞性的那一顆在最底下，而且是外框（C1）。
        Button(
            onClick = {
                val cm = context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager
                cm?.setPrimaryClip(ClipData.newPlainText("luminakey-diagnostics", report))
                copied = true
            },
            modifier = Modifier.fillMaxWidth().heightIn(min = Dimens.touchTarget),
        ) {
            Text(
                text = stringResource(if (copied) R.string.advanced_copied else R.string.advanced_copy),
                fontSize = TypeScale.t4,
            )
        }

        // ── 破壞性動作：該頁最後一個區塊，上面隔一條 hairline + 一個 s7（§4.9）──
        Spacer(Modifier.height(Space.s7))
        Hairline()
        Spacer(Modifier.height(Space.s7))
        OutlinedButton(
            onClick = { confirmReset = true },
            // ⚠ 停用態必須同時給「為什麼」（D1）。那句話就在下面的註腳裡，
            //   會隨 settingsPristine 換成「目前全部都是預設值」。
            enabled = !settingsPristine,
            modifier = Modifier.fillMaxWidth().heightIn(min = Dimens.touchTarget),
            colors = ButtonDefaults.outlinedButtonColors(
                // 危險色**只上文字與外框，底是透明的**。實心底是「主要動作」的
                // 視覺，會讓整頁最危險的東西看起來最該按。
                contentColor = MaterialTheme.colorScheme.error,
            ),
        ) {
            Text(stringResource(R.string.advanced_reset), fontSize = TypeScale.t4)
        }
        Text(
            text = stringResource(
                if (settingsPristine) R.string.advanced_reset_pristine
                else R.string.advanced_reset_sub
            ),
            fontSize = TypeScale.t5,
            lineHeight = TypeScale.t5Line,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(top = Space.s3, start = Space.s1),
        )
    }

    if (confirmReset) {
        ResetConfirmDialog(
            onDismiss = { confirmReset = false },
            onConfirm = {
                confirmReset = false
                scope.launch { store.update { UserPrefs(onboardingDone = it.onboardingDone) } }
            },
        )
    }
}

/**
 * 一列**唯讀狀態**：長得像設定列，但沒有 `›`、不可點。
 *
 * 存在的理由是誠實：一個動作正在跑的時候，那一列不該還是一顆按鈕。
 */
@Composable
private fun StatusRow(title: String, detail: String) {
    Column(
        Modifier
            .fillMaxWidth()
            .heightIn(min = Dimens.row)
            .padding(horizontal = Space.s5, vertical = Space.s4),
    ) {
        Text(text = title, fontSize = TypeScale.t3, fontWeight = FontWeight.Medium)
        Text(
            text = detail,
            fontSize = TypeScale.t5,
            lineHeight = TypeScale.t5Line,
            color = MaterialTheme.colorScheme.primary,
            modifier = Modifier.padding(top = Space.s1),
        )
    }
}

/**
 * 「全部回復預設」的二次確認（§4.9 / C3–C5）。
 *
 * 三條規則各自對應一行程式：
 *   · **確認鍵寫出它會做什麼**，不是「確定」—— 使用者在對話框上讀到的最後
 *     一個詞，應該就是他即將發生的事。
 *   · **預設焦點在取消。** 這是唯一一個「按錯了救不回來」的地方，而 Android 上
 *     連按兩下 Enter／用實體鍵盤操作的人是真的存在的。
 *   · **同時點名會消失的與不會消失的**，兩句都要。只講「會被刪掉」會讓使用者
 *     以為自己加的詞也沒了，於是不敢按 —— 然後帶著一組壞掉的設定繼續用。
 */
@Composable
private fun ResetConfirmDialog(onDismiss: () -> Unit, onConfirm: () -> Unit) {
    val cancelFocus = remember { FocusRequester() }
    LaunchedEffect(Unit) { runCatching { cancelFocus.requestFocus() } }
    AlertDialog(
        onDismissRequest = onDismiss,
        shape = RoundedCornerShape(Radius.large),
        title = {
            Text(
                text = stringResource(R.string.reset_confirm_title),
                fontSize = TypeScale.t2,
                fontWeight = FontWeight.SemiBold,
            )
        },
        text = {
            Column {
                Text(
                    text = stringResource(R.string.reset_confirm_lost),
                    fontSize = TypeScale.t5,
                    lineHeight = TypeScale.t5Line,
                )
                Spacer(Modifier.height(Space.s3))
                Text(
                    text = stringResource(R.string.reset_confirm_kept),
                    fontSize = TypeScale.t5,
                    lineHeight = TypeScale.t5Line,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        },
        confirmButton = {
            TextButton(
                onClick = onConfirm,
                colors = ButtonDefaults.textButtonColors(
                    contentColor = MaterialTheme.colorScheme.error,
                ),
            ) {
                Text(stringResource(R.string.reset_confirm_do), fontSize = TypeScale.t4)
            }
        },
        dismissButton = {
            TextButton(
                onClick = onDismiss,
                modifier = Modifier.focusRequester(cancelFocus),
            ) {
                Text(stringResource(R.string.reset_confirm_cancel), fontSize = TypeScale.t4)
            }
        },
    )
}

/**
 * 診斷資訊 —— **一項都沒刪**，只是換了讀者。
 *
 * ⚠ `controller.enabledSchemas` 這一行**不可以**包在 `remember` 裡。
 * 它是 controller 的 `mutableStateOf`，由 `refreshLocalState()` 在第一次組合
 * **之後**才填進去。若 app 進來時 phase 已經是 READY，memo 起來的空清單就會
 * 一路卡住，畫面上永遠顯示「已啟用 0」，要按下「重新整理字詞」才會對。
 * 這個 bug 修過一次，搬家時語義必須跟著走。
 *
 * ⚠ **這一段刻意不在地化，永遠是英文。** 它不是介面文字，是一份要被複製、
 * 貼到 issue 裡的回報載荷；上面那行小灰字已經先說了「這些數字是給我們看的，
 * 你不用懂」。翻譯它會產生兩個具體的壞處：
 *   · 收到的 issue 會有三種語言的欄位名，grep 不到、也對不起來；
 *   · `schema` / `ABI` / `deploy` 這些字沒有不失真的譯法，硬翻只會讓回報者
 *     以為自己看懂了，然後在描述問題時用錯詞。
 * 使用者看得見的每一個字仍然跟著系統語言走 —— 這一塊是那條規則的唯一例外，
 * 而且是刻意的。
 */
@Composable
private fun diagnosticsText(context: Context, controller: StoreController): String {
    val updates = remember { UpdateController.get(context) }
    val repo = remember { ConfigRepository(context) }
    val phase = rememberRimePhase()
    val schemas = remember(phase, controller.refreshTick) { RimeCore.schemaList() }
    val enabled = controller.enabledSchemas
    val themeIds = remember { repo.themeIds() }

    // 使用者的語系也記一筆：在地化之後，「他看到的是哪一份 strings.xml」
    // 本身就是一條會影響重現的線索。
    val locale = ConfigRepository.LOCALE

    return buildString {
        appendLine("version: ${updates.installedVersionName} (${updates.installedVersionCode})")
        appendLine("locale: $locale")
        appendLine("impl: ${if (RimeCore.isStub()) "stub" else "real librime"}")
        appendLine(".so load: ${if (RimeCore.libraryLoaded) "ok" else "failed — ${RimeCore.libraryLoadError}"}")
        appendLine(
            "ABI runtime/compiled/expected: ${RimeCore.abiVersion()} / " +
                "${RimeCore.compiledAbiVersion()} / ${RimeCore.EXPECTED_ABI_VERSION}" +
                " (compatible: ${if (RimeCore.abiCompatible()) "yes" else "no"})"
        )
        appendLine("init phase: $phase")
        RimeRuntime.initError?.let { appendLine("error: $it") }
        RimeRuntime.migrationNote?.let { appendLine("builtin schema migration: $it") }
        appendLine(
            "extract: " +
                if (RimeRuntime.extractMillis >= 0) "${RimeRuntime.extractMillis} ms"
                else "not extracted this run"
        )
        appendLine(
            "first deploy: " +
                if (RimeRuntime.deployMillis >= 0) "${RimeRuntime.deployMillis} ms"
                else "in progress / never finished"
        )
        appendLine("deploy status: ${RimeCore.lastDeployStatus}")
        appendLine()
        appendLine(RimeRuntime.describeDataDirs())
        appendLine()
        appendLine("schemas (${schemas.size})")
        if (schemas.isEmpty()) appendLine("  none available")
        else schemas.forEach { appendLine("  ${it.id}  —  ${it.name}") }
        appendLine()
        appendLine("schema_list patch (enabled ${enabled.size})")
        if (enabled.isEmpty()) appendLine("  no schema_list patch in default.custom.yaml")
        else enabled.forEach { appendLine("  $it") }
        appendLine()
        appendLine("themes: ${themeIds.joinToString(", ")}")
        append("network log file: ${org.luminakey.ime.net.NetworkAudit.logFilePath}")
    }
}
