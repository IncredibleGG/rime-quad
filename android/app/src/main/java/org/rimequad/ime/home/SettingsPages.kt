@file:OptIn(androidx.compose.ui.ExperimentalComposeUiApi::class)

package org.rimequad.ime.home

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
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
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
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.launch
import org.rimequad.ime.core.RimeCore
import org.rimequad.ime.core.RimeRuntime
import org.rimequad.ime.keyboard.ConfigRepository
import org.rimequad.ime.net.NetworkRequiredCard
import org.rimequad.ime.net.NetworkScreen
import org.rimequad.ime.net.NetworkSwitchCard
import org.rimequad.ime.net.rememberNetworkEnabled
import org.rimequad.ime.prefs.AppearanceMode
import org.rimequad.ime.prefs.HintVisibility
import org.rimequad.ime.prefs.KeyRemapSection
import org.rimequad.ime.prefs.PrefLevels
import org.rimequad.ime.prefs.PrefsStore
import org.rimequad.ime.prefs.SpaceBehavior
import org.rimequad.ime.prefs.UserPrefs
import org.rimequad.ime.prefs.resolveBaseTheme
import org.rimequad.ime.store.StoreController
import org.rimequad.ime.store.StoreScreen
import org.rimequad.ime.update.UpdateController
import org.rimequad.ime.update.UpdateSection

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

    val appearance = buildString {
        append(
            when (prefs.appearanceMode) {
                AppearanceMode.LIGHT -> "一直淺"
                AppearanceMode.DARK -> "一直深"
                else -> "跟著手機"
            }
        )
        append(" · ")
        append(familyNameOf(base.name.get(ConfigRepository.LOCALE), base.id))
    }
    val feel = buildString {
        val s = PrefLevels.indexOfSound(prefs, base.feedback.sound, base.feedback.soundVolume)
        val h = PrefLevels.indexOfHaptic(prefs, base.feedback.haptic, base.feedback.hapticStrength)
        append(if (s == 0) "靜音" else "按鍵音：${PrefLevels.SOUND_LABELS[s]}")
        append(" · ")
        // 「震動中」會被讀成「正在震動」，所以這裡一律加冒號。
        append(if (h == 0) "不震動" else "震動：${PrefLevels.HAPTIC_LABELS[h]}")
    }
    // 兩項都沒設過時不要寫兩次「跟著鍵盤」—— 那看起來像重複，也讓人以為
    // 有兩個地方可以設同一件事。
    val text = if (prefs.simplification == null && prefs.asciiPunct == null) {
        "字體與標點都跟著鍵盤"
    } else {
        buildString {
            append(
                when (prefs.simplification) {
                    true -> "簡體"
                    false -> "繁體"
                    null -> "字體跟著鍵盤"
                }
            )
            append(" · ")
            append(
                when (prefs.asciiPunct) {
                    true -> "半形標點"
                    false -> "全形標點"
                    null -> "標點跟著鍵盤"
                }
            )
        }
    }
    return HomeSummary(
        keyboard = describeKeyboard(current),
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
    for (suffix in listOf("淺色", "深色", "Light", "Dark")) {
        if (s.endsWith(suffix)) s = s.dropLast(suffix.length)
    }
    return s.trimEnd(' ', '・', '·', '-').ifBlank { familyIdOf(fallbackId) }
}

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
            .padding(horizontal = 20.dp),
    ) {
        Spacer(Modifier.height(24.dp))
        PageHeader(title = title, onBack = onBack)
        Spacer(Modifier.height(8.dp))
        content()
        Spacer(Modifier.height(40.dp))
    }
}

/* ────────────────────────────── 鍵盤 ────────────────────────────── */

@Composable
fun KeyboardPage(onBack: () -> Unit, onOpenStore: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val groups = remember { availableKeyboardGroups(context) }
    val all = remember(groups) { groups.flatMap { it.second } }
    var picked by remember { mutableStateOf(currentKeyboardOf(context, all)) }

    Page(title = "鍵盤", onBack = onBack) {
        Text(
            text = "點一下就換。想換回來隨時再點。",
            fontSize = 13.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(16.dp))
        if (all.isEmpty()) {
            Text("尚無可用的鍵盤。字詞整理完成之後就會出現。", fontSize = 14.sp)
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
                Spacer(Modifier.height(18.dp))
            }
        }
        Spacer(Modifier.height(22.dp))
        PlainCard {
            // 想換鍵盤的人本來就在這一屏，所以市集放這裡而不是「進階」。
            NavRow(
                title = "下載更多鍵盤",
                value = "粵拼 · 五筆 · 倉頡…",
                onClick = onOpenStore,
            )
        }

        // 自訂鍵位。它是「我要用哪種鍵盤」這個問題的延伸，所以跟鍵盤卡片
        // 放在同一頁。整塊在 prefs/KeyRemapSection.kt 裡，這裡只多一行 ——
        // 那條線自己在檔案裡寫了「這塊之後會被搬走」，這裡就是那個去處。
        Spacer(Modifier.height(18.dp))
        KeyRemapSection()
        Spacer(Modifier.height(14.dp))
        Text(
            text = "打字的時候點候選字那一列右邊的 ⚙，也能直接在鍵盤上換。",
            fontSize = 12.5.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
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

    Page(title = "外觀", onBack = onBack) {
        SectionLabel("配色")
        // 「預設」那一組的 pinId 是空字串 = 不指定主題（見 themeFamilies）。
        // 所以這裡不另外放一顆「未設定」晶片 —— 兩顆都叫「預設」只會讓人以為
        // 有兩個地方可以決定同一件事。
        val currentFamily = prefs.themeId?.let { familyIdOf(it) } ?: DEFAULT_THEME_FAMILY
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            for (chunk in families.chunked(2)) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
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
            label = "深淺",
            options = listOf(
                null to "跟著手機",
                AppearanceMode.LIGHT to "一直淺",
                AppearanceMode.DARK to "一直深",
            ),
            selected = prefs.appearanceMode.takeIf { it != AppearanceMode.FOLLOW_SYSTEM },
            onSelect = { v -> edit { p -> p.copy(appearanceMode = v) } },
        )

        Spacer(Modifier.height(10.dp))
        SectionLabel("鍵盤高度")
        PlainCard {
            Column(Modifier.padding(16.dp)) {
                Text("在鍵盤上直接拖", fontSize = 16.sp)
                Text(
                    // 這一項刻意**不給滑桿**：滑桿要在數值與預覽之間來回猜，
                    // 拖曳不用 —— 拖到哪裡就是哪裡，拖完就是最終樣子。
                    text = "看著鍵盤調，比拉滑桿準。按下去會叫出鍵盤，上緣會長出一根把手。",
                    fontSize = 12.5.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(top = 2.dp),
                )
                Spacer(Modifier.height(10.dp))
                OutlinedButton(
                    onClick = {
                        KeyboardPanelRequest.request(context, KeyboardPanelRequest.HEIGHT)
                        askedHeight = true
                        focus.requestFocus()
                        keyboardController?.show()
                    },
                    modifier = Modifier.fillMaxWidth(),
                ) { Text("開始調") }
                if (askedHeight) {
                    Text(
                        text = "鍵盤升起來之後，拖它的上緣就會變高變矮；調好按「好了」。",
                        fontSize = 12.5.sp,
                        color = MaterialTheme.colorScheme.primary,
                        modifier = Modifier.padding(top = 8.dp),
                    )
                }
            }
        }

        SettingGroup(
            label = "候選字大小",
            options = PrefLevels.CANDIDATE_SIZE_LABELS.mapIndexed { i, s -> i to s },
            selected = PrefLevels.indexOfCandidateSize(prefs),
            onSelect = { i -> edit { p -> PrefLevels.withCandidateSize(p, i) } },
        )

        val baseCount = remember(prefs.themeId, prefs.appearanceMode) {
            resolveBaseTheme(repo, prefs, systemDark).candidates.bar.maxVisible
        }
        SettingGroup(
            label = "一次顯示幾個",
            options = PrefLevels.CANDIDATE_COUNT_LABELS.mapIndexed { i, s -> i to s },
            selected = PrefLevels.indexOfCandidateCount(prefs, baseCount),
            onSelect = { i -> edit { p -> PrefLevels.withCandidateCount(p, i) } },
        )

        Spacer(Modifier.height(10.dp))
        PlainCard {
            SwitchRow(
                title = "鍵上顯示小提示",
                subtitle = "拼音佈局鍵面角落的數字",
                checked = prefs.hints != HintVisibility.HIDDEN,
                onCheckedChange = { on ->
                    edit { p ->
                        p.copy(hints = if (on) HintVisibility.SHOWN else HintVisibility.HIDDEN)
                    }
                },
            )
        }

        Spacer(Modifier.height(18.dp))
        SectionLabel("調完在這裡看看")
        TryField(placeholder = "在這裡打字看看", focusRequester = focus)
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
    val scope = rememberCoroutineScope()
    val store = remember { PrefsStore.get(context) }
    val prefs by store.flow.collectAsState(initial = store.current)
    val repo = remember { ConfigRepository(context) }
    val systemDark = (LocalConfiguration.current.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
        Configuration.UI_MODE_NIGHT_YES
    val base = remember(prefs.themeId, prefs.appearanceMode, systemDark) {
        resolveBaseTheme(repo, prefs, systemDark)
    }

    fun edit(block: (UserPrefs) -> UserPrefs) {
        scope.launch { store.update(block) }
    }

    Page(title = "手感", onBack = onBack) {
        Text(
            // 三項合成一頁是因為它們是同一個問題的三個面向：「這個鍵盤按起來
            // 的感覺」。分成三個設定項就是把一件事拆成三次操作。
            text = "這三項只有真的按下去才判斷得出來，所以下面留了一格給你按。",
            fontSize = 13.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        SettingGroup(
            label = "按鍵音",
            options = PrefLevels.SOUND_LABELS.mapIndexed { i, s -> i to s },
            selected = PrefLevels.indexOfSound(prefs, base.feedback.sound, base.feedback.soundVolume),
            onSelect = { i -> edit { p -> PrefLevels.withSound(p, i) } },
        )
        SettingGroup(
            label = "震動",
            options = PrefLevels.HAPTIC_LABELS.mapIndexed { i, s -> i to s },
            selected = PrefLevels.indexOfHaptic(
                prefs,
                base.feedback.haptic,
                base.feedback.hapticStrength,
            ),
            onSelect = { i -> edit { p -> PrefLevels.withHaptic(p, i) } },
        )
        SettingGroup(
            label = "長按多久算長按",
            options = PrefLevels.LONG_PRESS_LABELS.mapIndexed { i, s -> i to s },
            selected = PrefLevels.indexOfLongPress(prefs),
            onSelect = { i -> edit { p -> PrefLevels.withLongPress(p, i) } },
        )
        Spacer(Modifier.height(14.dp))
        SectionLabel("按按看")
        TryField(placeholder = "按下面的鍵試試看")
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

    Page(title = "文字", onBack = onBack) {
        SettingGroup(
            label = "字體",
            // 「跟著鍵盤」= 不干預，讓方案自己的預設生效。它與 false 不是同一件事，
            // 所以不能做成兩態開關 —— 那樣就回不去不干預了。
            options = listOf(null to "跟著鍵盤", false to "繁體", true to "簡體"),
            selected = prefs.simplification,
            onSelect = { v -> edit { p -> p.copy(simplification = v) } },
        )
        SettingGroup(
            label = "標點",
            options = listOf(null to "跟著鍵盤", false to "全形", true to "半形"),
            selected = prefs.asciiPunct,
            onSelect = { v -> edit { p -> p.copy(asciiPunct = v) } },
        )
        SettingGroup(
            label = "空白鍵",
            options = listOf(
                null to "送出候選字",
                SpaceBehavior.ALWAYS_SPACE to "一律打空格",
            ),
            selected = prefs.spaceBehavior.takeIf { it == SpaceBehavior.ALWAYS_SPACE },
            onSelect = { v -> edit { p -> p.copy(spaceBehavior = v) } },
        )
        Spacer(Modifier.height(14.dp))
        SectionLabel("試試看")
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
        modifier = Modifier.fillMaxSize().padding(horizontal = 20.dp),
    ) {
        Spacer(Modifier.height(24.dp))
        PageHeader(title = "連網", onBack = onBack)
        Spacer(Modifier.height(8.dp))
        NetworkSwitchCard()
        Spacer(Modifier.height(14.dp))
        NetworkScreen(modifier = Modifier.fillMaxSize())
    }
}

/* ────────────────────────────── 市集 ────────────────────────────── */

@Composable
fun StorePage(controller: StoreController, onBack: () -> Unit) {
    val online = rememberNetworkEnabled()
    Column(
        modifier = Modifier.fillMaxSize().padding(horizontal = 20.dp),
    ) {
        Spacer(Modifier.height(24.dp))
        PageHeader(title = "下載更多鍵盤", onBack = onBack)
        if (!online) {
            NetworkRequiredCard(what = "下載鍵盤")
            Spacer(Modifier.height(12.dp))
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

    val importer = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri -> if (uri != null) controller.importLocal(uri) }

    Page(title = "進階與問題回報", onBack = onBack) {

        PlainCard {
            NavRow(
                title = "從檔案匯入",
                subtitle = "自己準備的鍵盤方案包（.zip）",
                onClick = { importer.launch(arrayOf("application/zip", "*/*")) },
            )
            RowDivider()
            NavRow(
                title = "重新整理字詞",
                subtitle = if (controller.busy) "整理中…" else "打字怪怪的時候用，約十幾秒",
                onClick = { if (!controller.busy) controller.redeploy() },
            )
            RowDivider()
            // 「曾經走完引導」不是設定，是一件已經發生過的事實 —— 回復預設時
            // 保留它。不保留的話，使用者按下「全部回復預設」之後，下一次冷啟動
            // 會被丟回引導頁，而他明明早就設好了，只是想把設定歸零。
            val settingsPristine = prefs.copy(onboardingDone = null) == UserPrefs()
            NavRow(
                title = "全部回復預設",
                subtitle = if (settingsPristine) "目前全部都是預設值"
                else "把所有設定刪掉，回到隨附的值",
                onClick = {
                    if (!settingsPristine) {
                        scope.launch { store.update { UserPrefs(onboardingDone = it.onboardingDone) } }
                    }
                },
            )
        }

        Spacer(Modifier.height(22.dp))
        SectionLabel("更新")
        if (!online) {
            NetworkRequiredCard(what = "檢查更新")
            Spacer(Modifier.height(10.dp))
        }
        UpdateSection(
            controller = updates,
            autoCheck = prefs.autoCheckUpdate,
            onAutoCheckChange = { v -> scope.launch { store.update { p -> p.copy(autoCheckUpdate = v) } } },
        )

        Spacer(Modifier.height(26.dp))
        SectionLabel("回報問題時，把這段一起附上")
        Text(
            text = "這些數字是給我們看的，你不用懂。",
            fontSize = 12.5.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 10.dp),
        )
        val report = diagnosticsText(context, controller)
        PlainCard {
            Text(
                text = report,
                fontSize = 11.5.sp,
                fontFamily = FontFamily.Monospace,
                modifier = Modifier.padding(14.dp),
            )
        }
        Spacer(Modifier.height(10.dp))
        Button(
            onClick = {
                val cm = context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager
                cm?.setPrimaryClip(ClipData.newPlainText("rime-diagnostics", report))
                copied = true
            },
            modifier = Modifier.fillMaxWidth(),
        ) { Text(if (copied) "已複製" else "複製這段") }
    }
}

/**
 * 診斷資訊 —— **一項都沒刪**，只是換了讀者。
 *
 * ⚠ `controller.enabledSchemas` 這一行**不可以**包在 `remember` 裡。
 * 它是 controller 的 `mutableStateOf`，由 `refreshLocalState()` 在第一次組合
 * **之後**才填進去。若 app 進來時 phase 已經是 READY，memo 起來的空清單就會
 * 一路卡住，畫面上永遠顯示「已啟用 0」，要按下「重新整理字詞」才會對。
 * 這個 bug 修過一次，搬家時語義必須跟著走。
 */
@Composable
private fun diagnosticsText(context: Context, controller: StoreController): String {
    val updates = remember { UpdateController.get(context) }
    val repo = remember { ConfigRepository(context) }
    val phase = rememberRimePhase()
    val schemas = remember(phase, controller.refreshTick) { RimeCore.schemaList() }
    val enabled = controller.enabledSchemas
    val themeIds = remember { repo.themeIds() }

    return buildString {
        appendLine("版本: ${updates.installedVersionName}（${updates.installedVersionCode}）")
        appendLine("實作: ${if (RimeCore.isStub()) "stub 假實作" else "真 librime"}")
        appendLine("so 載入: ${if (RimeCore.libraryLoaded) "成功" else "失敗 — ${RimeCore.libraryLoadError}"}")
        appendLine(
            "ABI 執行期/編譯期/要求: ${RimeCore.abiVersion()} / " +
                "${RimeCore.compiledAbiVersion()} / ${RimeCore.EXPECTED_ABI_VERSION}" +
                "（相容: ${if (RimeCore.abiCompatible()) "是" else "否"}）"
        )
        appendLine("初始化階段: $phase")
        RimeRuntime.initError?.let { appendLine("錯誤: $it") }
        RimeRuntime.migrationNote?.let { appendLine("內建方案遷移: $it") }
        appendLine(
            "解壓耗時: " +
                if (RimeRuntime.extractMillis >= 0) "${RimeRuntime.extractMillis} ms" else "本次未解壓"
        )
        appendLine(
            "首次部署耗時: " +
                if (RimeRuntime.deployMillis >= 0) "${RimeRuntime.deployMillis} ms" else "進行中／未完成"
        )
        appendLine("部署狀態: ${RimeCore.lastDeployStatus}")
        appendLine()
        appendLine(RimeRuntime.describeDataDirs())
        appendLine()
        appendLine("Schema（${schemas.size}）")
        if (schemas.isEmpty()) appendLine("  尚無可用 schema")
        else schemas.forEach { appendLine("  ${it.id}  —  ${it.name}") }
        appendLine()
        appendLine("schema_list patch（已啟用 ${enabled.size}）")
        if (enabled.isEmpty()) appendLine("  default.custom.yaml 沒有 schema_list patch")
        else enabled.forEach { appendLine("  $it") }
        appendLine()
        appendLine("可選配色: ${themeIds.joinToString(", ")}")
        append("連網紀錄檔: ${org.rimequad.ime.net.NetworkAudit.logFilePath}")
    }
}
