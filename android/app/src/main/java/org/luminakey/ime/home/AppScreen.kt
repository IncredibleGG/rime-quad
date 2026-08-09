package org.luminakey.ime.home

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.systemBars
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import org.luminakey.ime.R
import org.luminakey.ime.net.FirstRunNoticeHost
import org.luminakey.ime.net.NetworkSwitchCard
import org.luminakey.ime.prefs.PrefsStore
import org.luminakey.ime.store.StoreController
import org.luminakey.ime.store.StoreOverlays

/**
 * App 只有兩幕：**第一次打開**（引導）與**之後每一次**（設定）。
 *
 * 深度上限是兩層，而且第二層一定是終點 —— 唯一的例外是「下載更多鍵盤」，
 * 它本質上就是另一個世界的入口。
 */
enum class Route {
    HOME,
    KEYBOARD,
    APPEARANCE,
    FEEL,
    TEXT,
    ADVANCED,
    NETWORK,
    STORE,
}

/**
 * 整個 App 的殼。
 *
 * @param openStore true = 直接落在市集（軟鍵盤或別的入口帶進來的意圖）。
 * @param focusEpoch 由 Activity 的 `onWindowFocusChanged` 推進；
 *                   見 [rememberImeSystemState] 為什麼非它不可。
 */
@Composable
fun RimeAppScreen(
    openStore: Boolean = false,
    focusEpoch: Int = 0,
    startInOnboarding: Boolean,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    val store = remember { StoreController(context.applicationContext) }
    val system = rememberImeSystemState(focusEpoch)
    val phase = rememberRimePhase()
    val stage = stageOf(system, phase)

    var onboarding by remember { mutableStateOf(startInOnboarding && !openStore) }
    var route by remember { mutableStateOf(if (openStore) Route.STORE else Route.HOME) }

    // 市集與診斷都需要本機清單；進到 App 就讀一次，phase 變了再讀一次。
    LaunchedEffect(phase, store.refreshTick) { store.refreshLocalState() }

    // targetSdk 35 起 Android 強制 edge-to-edge，內容會鑽到狀態列與手勢列底下。
    // 這裡一次讓開，底下每一頁就都不必自己記得處理。
    Box(modifier.fillMaxSize().windowInsetsPadding(WindowInsets.systemBars)) {
        if (onboarding) {
            OnboardingScreen(
                stage = stage,
                system = system,
                onFinished = { onboarding = false },
            )
        } else {
            // 每一個第二層都有看得見的返回鍵，系統返回鍵是第二條路，不是唯一那條。
            BackHandler(enabled = route != Route.HOME) { route = Route.HOME }
            when (route) {
                Route.HOME -> HomeScreen(
                    stage = stage,
                    system = system,
                    onNavigate = { route = it },
                )

                Route.KEYBOARD -> KeyboardPage(
                    onBack = { route = Route.HOME },
                    onOpenStore = { route = Route.STORE },
                )

                Route.APPEARANCE -> AppearancePage(onBack = { route = Route.HOME })
                Route.FEEL -> FeelPage(onBack = { route = Route.HOME })
                Route.TEXT -> TextPage(onBack = { route = Route.HOME })
                Route.ADVANCED -> AdvancedPage(
                    controller = store,
                    onBack = { route = Route.HOME },
                )

                Route.NETWORK -> NetworkPage(onBack = { route = Route.HOME })
                Route.STORE -> StorePage(controller = store, onBack = { route = Route.HOME })
            }
        }
        // 部署進度／市集結果畫在所有頁面之外：按的是同一個 rs_deploy()，
        // 提示不該有兩套。
        StoreOverlays(store)
        // 連網那條線的一次性說明。它蓋在最上面是刻意的 —— 「預設不連網」
        // 是這個專案的招牌，第一次打開就該講清楚。
        FirstRunNoticeHost()
    }
}

/* ────────────────────────────── 第二幕：首頁 ────────────────────────────── */

/**
 * 之後每一次打開看到的東西。
 *
 * 四列，一張卡，不捲動。17 個設定項一項沒掉，只是收成四個問題：
 * **用哪種鍵盤 · 長什麼樣 · 按起來什麼感覺 · 打出來是什麼字**。
 * 分類的依據不是技術歸屬，是使用者當下腦中的那句話。
 *
 * 每一列只有兩樣東西：名字，和現在的值。沒有說明文字 —— 說明文字是給
 * 不確定的人看的，而「注音 · 大千排列」這行灰字本身就是最好的說明。
 */
@Composable
fun HomeScreen(
    stage: SetupStage,
    system: ImeSystemState,
    onNavigate: (Route) -> Unit,
) {
    val context = LocalContext.current
    val prefsStore = remember { PrefsStore.get(context) }
    val prefs by prefsStore.flow.collectAsState(initial = prefsStore.current)
    val summary = rememberHomeSummary(prefs)

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = Space.s6),
    ) {
        Spacer(Modifier.height(Space.s8))
        Text(
            text = stringResource(R.string.home_title),
            fontSize = TypeScale.t1,
            fontWeight = FontWeight.SemiBold,
            // 一頁只有一個 t1，而它就是這一頁的標題。
            modifier = Modifier.semantics { heading() },
        )
        Spacer(Modifier.height(Space.s7))

        // ⚠ 誠實條。持久化的那個布林只決定「開在哪一頁」，**不是**「輸入法
        // 可以用」的證據 —— 使用者完全可能引導完成之後又把鍵盤換回別的。
        // 那時 App 該開在設定（他已經是老手了），但這裡必須誠實地說出來，
        // 並給一顆一鍵切回去的按鈕。
        if (stage != SetupStage.READY) {
            NotReadyStrip(stage = stage, system = system)
            Spacer(Modifier.height(Space.s5))
        }

        // 離線是這個專案的招牌，所以它在第一層、在最上面，不埋在任何一層底下。
        // 開關與「連過哪些地方」是同一組（同一件事的兩面），所以它們之間用
        // s3，而與底下那張設定卡之間用 s7 —— 同一組的距離小於跨組的距離，
        // 這是分組唯一可靠的手段（§3.1 規則 1）。
        NetworkSwitchCard()
        Spacer(Modifier.height(Space.s3))
        PlainCard {
            NavRow(
                title = stringResource(R.string.home_network_log),
                value = null,
                subtitle = stringResource(R.string.home_network_log_sub),
                onClick = { onNavigate(Route.NETWORK) },
            )
        }

        Spacer(Modifier.height(Space.s7))

        // 一張卡，四列。17 個設定項一項沒掉，只是收成四個問題：
        // **用哪種打字方式 · 長什麼樣 · 按起來什麼感覺 · 打出來是什麼字**。
        PlainCard {
            NavRow(stringResource(R.string.home_typing_method), summary.keyboard) {
                onNavigate(Route.KEYBOARD)
            }
            RowDivider()
            NavRow(stringResource(R.string.home_appearance), summary.appearance) {
                onNavigate(Route.APPEARANCE)
            }
            RowDivider()
            NavRow(stringResource(R.string.home_feel), summary.feel) { onNavigate(Route.FEEL) }
            RowDivider()
            NavRow(stringResource(R.string.home_text), summary.text) { onNavigate(Route.TEXT) }
        }

        Spacer(Modifier.height(Space.s7))
        SectionLabel(stringResource(R.string.home_try_it))
        TryField()
        Text(
            text = stringResource(R.string.home_try_it_hint),
            fontSize = TypeScale.t5,
            lineHeight = TypeScale.t5Line,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(top = Space.s3, start = Space.s1),
        )

        Spacer(Modifier.height(Space.s7))
        Hairline()
        QuietRow(stringResource(R.string.home_advanced)) { onNavigate(Route.ADVANCED) }
        Spacer(Modifier.height(Space.s8))
    }
}

@Composable
private fun NotReadyStrip(stage: SetupStage, system: ImeSystemState) {
    val context = LocalContext.current
    PlainCard {
        Column(Modifier.fillMaxWidth().padding(Space.s5)) {
            Text(
                text = when (stage) {
                    SetupStage.NOT_ENABLED -> stringResource(R.string.not_ready_not_enabled)
                    SetupStage.ENABLED_NOT_DEFAULT -> stringResource(R.string.not_ready_not_default)
                    SetupStage.PREPARING -> stringResource(R.string.not_ready_preparing)
                    SetupStage.READY -> ""
                },
                fontSize = TypeScale.t3,
                fontWeight = FontWeight.SemiBold,
            )
            Text(
                text = when (stage) {
                    SetupStage.NOT_ENABLED -> stringResource(R.string.not_ready_not_enabled_body)
                    SetupStage.ENABLED_NOT_DEFAULT ->
                        system.currentImeLabel
                            ?.let { stringResource(R.string.not_ready_not_default_body_current, it) }
                            ?: stringResource(R.string.not_ready_not_default_body)

                    SetupStage.PREPARING -> stringResource(R.string.not_ready_preparing_body)
                    SetupStage.READY -> ""
                },
                fontSize = TypeScale.t5,
                lineHeight = TypeScale.t5Line,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                // s1：標題與它正下方的副標。s2 以上會讓那兩行看起來不像同一組。
                modifier = Modifier.padding(top = Space.s1),
            )
            // PREPARING 沒有按鈕（沒有人能加速它），所以那一格不要留一段空白 ——
            // 空白在別的狀態下是「按鈕在這裡」的預告，留著會讓人以為少畫了東西。
            if (stage == SetupStage.NOT_ENABLED || stage == SetupStage.ENABLED_NOT_DEFAULT) {
                Spacer(Modifier.height(Space.s4))
                // 本屏唯一的實心按鈕（A1）：不必讀字就知道該點哪裡。
                PrimaryWide(
                    text = stringResource(
                        if (stage == SetupStage.NOT_ENABLED) R.string.not_ready_open_settings
                        else R.string.not_ready_switch
                    ),
                ) {
                    if (stage == SetupStage.NOT_ENABLED) openImeSettings(context)
                    else showImePicker(context)
                }
            }
        }
    }
}

@Composable
private fun PrimaryWide(text: String, onClick: () -> Unit) {
    androidx.compose.material3.Button(
        onClick = onClick,
        modifier = Modifier.fillMaxWidth(),
    ) { Text(text) }
}
