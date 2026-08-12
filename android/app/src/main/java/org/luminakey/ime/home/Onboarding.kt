package org.luminakey.ime.home

import android.content.Context
import android.content.Intent
import android.provider.Settings
import android.view.inputmethod.InputMethodManager
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.luminakey.ime.R
import org.luminakey.ime.core.DeployEstimate
import org.luminakey.ime.core.RimeRuntime
import org.luminakey.ime.keyboard.KeyboardType
import org.luminakey.ime.prefs.PrefsStore

/**
 * 第一幕：第一次打開。
 *
 * ── 一屏就是全部 ────────────────────────────────────────────────────────
 * 這裡看起來像三、四個畫面，其實是**同一屏的幾個狀態**。沒有分頁、沒有
 * 「下一步」、沒有「上一步」—— 沒有「第幾步」就不可能卡在第幾步。
 *
 * 而且每一個狀態都是**從系統即時算出來的**（見 [stageOf]），不是自己記的
 * 旗標。所以順序怎麼走都不會壞：先從系統設定弄好再開 App、中途離開、
 * 隔天再回來，畫面永遠反映當下的真實狀態。
 *
 * ── 等待藏在人的動作後面 ────────────────────────────────────────────────
 * 首次部署的實測耗時見 [org.luminakey.ime.core.DeployEstimate]。這段藏不掉，
 * 但可以跟人的動作**並行**：字詞整理在 App 一打開就開始跑，而使用者這時要去
 * 系統設定按兩趟 —— 人走這兩趟的時間本來就超過那十幾秒。
 *
 * **這個前提是實測過的**：App 被切到背景（使用者人在系統的輸入法設定頁）
 * 期間，部署照樣跑完，phase 從 DEPLOYING 走到 READY 全程發生在背景。
 * 所以進度條出現在畫面最下面、細細一條，旁邊寫著「不用等它」——
 * 它不是一道關卡，是一件背景在做的事。
 *
 * ── 真的有人比它快 ──────────────────────────────────────────────────────
 * 例如先從系統設定設好再開 App。那時才會看到 [SetupStage.PREPARING] 這一屏，
 * 而它也不放轉圈圈，拿來問**唯一值得問的那個問題**：注音還是拼音、
 * 全鍵還是九宮格。第一張已經預選好，什麼都不做也是正確答案。
 */
@Composable
fun OnboardingScreen(
    stage: SetupStage,
    system: ImeSystemState,
    /**
     * ⚠ 與 [stage] 重複了一部分，但**不能省**：[stage] 在整段準備期間都是
     * `PREPARING`，看不出 EXTRACTING → DEPLOYING 的轉換；而 [PreparingBody] 需要
     * 那個轉換來重讀鍵盤清單。把它當參數傳下去，也正是讓那個 Composable
     * **不會被 Compose 跳過重組**的手段（見 [PreparingBody] 的註解）。
     */
    phase: RimeRuntime.Phase,
    /** 整理字詞失敗的原因。見 [rememberRimeStatus]。 */
    initError: String?,
    /** 哪一種失敗 —— 決定失敗那一屏給不給按鈕，見 [actionOf]。 */
    failure: RimeRuntime.Failure,
    onRefreshWords: () -> Unit,
    onFinished: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    // D 線的結論：持久化的東西只有這一個布林，而且它**只**決定 app 冷啟時
    // 落在哪一頁，絕不代表「輸入法可以用」。寫入時機是「第一次觀察到系統
    // 說我們已經是預設輸入法」，而不是「使用者按了完成」—— 以系統事實為準。
    LaunchedEffect(stage) {
        // FAILED 也算「兩步都做完了」：那一格的前提就是系統那兩步已經成立，
        // 只是引擎出了事。下次冷啟落在設定頁是對的 —— 首頁的那條橫幅會誠實地
        // 說失敗，而且給得出「重新整理字詞」；把他丟回引導頁反而繞遠路。
        if (stage == SetupStage.READY ||
            stage == SetupStage.PREPARING ||
            stage == SetupStage.FAILED
        ) {
            val store = PrefsStore.get(context)
            if (store.current.onboardingDone != true) {
                store.update { it.copy(onboardingDone = true) }
            }
        }
    }

    // 「準備中」那一屏有一排鍵盤卡，長度不固定，所以只有它捲動；
    // 另外三種狀態一律**不捲動** —— 引導頁一旦要捲，「一屏就是全部」就破了。
    val base = modifier.fillMaxSize().padding(horizontal = Space.s6)

    // 第 1 步：這是什麼（§8）。
    //
    // 它與底下三種狀態不同 —— 那三種是**系統事實推導出來的**，這一步不是，
    // 它是一段話。所以它需要一個自己的旗標，而這正是本專案唯一可以接受
    // 「記旗標」的地方：記錯的後果是多讀一次介紹，不是被困在某一步出不來。
    //
    // 用 rememberSaveable 而不是持久化：轉螢幕要留著，但下次冷啟動時
    // 若使用者仍在引導中，再看一次介紹沒有害處 —— 他還沒走到任何地方。
    var introSeen by rememberSaveable { mutableStateOf(false) }
    if (!introSeen && stage != SetupStage.READY) {
        Column(base) {
            Spacer(Modifier.height(Space.s8))
            IntroBody(onStart = { introSeen = true })
        }
        return
    }

    when (stage) {
        SetupStage.NOT_ENABLED, SetupStage.ENABLED_NOT_DEFAULT ->
            Column(base) {
                Spacer(Modifier.height(Space.s8))
                TwoStepBody(stage = stage, system = system)
            }

        SetupStage.PREPARING ->
            Column(base.verticalScroll(rememberScrollState())) {
                Spacer(Modifier.height(Space.s8))
                PreparingBody(phase = phase)
                Spacer(Modifier.height(Space.s8))
            }

        SetupStage.FAILED ->
            Column(base.verticalScroll(rememberScrollState())) {
                Spacer(Modifier.height(Space.s8))
                FailedBody(
                    error = initError,
                    failure = failure,
                    onRefreshWords = onRefreshWords,
                    onFinished = onFinished,
                )
                Spacer(Modifier.height(Space.s8))
            }

        // ⚠ 這一屏也要捲動。它從只有標題與試打框，變成還帶著那一排鍵盤卡
        // （見 [ReadyBody] 的註解）—— 卡片數量隨裝了幾個方案而變，不捲的話
        // 小螢幕上「進階設定」那條出路會被推到畫面外。
        SetupStage.READY ->
            Column(base.verticalScroll(rememberScrollState())) {
                Spacer(Modifier.height(Space.s8))
                ReadyBody(phase = phase, onFinished = onFinished)
                Spacer(Modifier.height(Space.s8))
            }
    }
}

/* ─────────────────────── 第 1 步：這是什麼 ─────────────────────── */

/**
 * 「離線是預設」只有這一次機會講清楚。
 *
 * 這一屏刻意**只有一顆按鈕，而且沒有「跳過」**。第 2、3 步是系統層級的操作，
 * 沒做就是不能打字 —— 給一顆跳過鍵等於讓使用者帶著一個壞掉的東西離開
 * （Trime 的精靈按下 SKIP 之後 IME 進入崩潰迴圈，崩的是 `ColorManager`，
 * 錯誤訊息對使用者毫無意義）。
 *
 * 三點各佔一行、不放圖示：這三句話本身就是這個產品與其他輸入法的**全部差別**，
 * 配一個圖示只會把眼睛帶去看圖示。
 */
@Composable
private fun ColumnScope.IntroBody(onStart: () -> Unit) {
    Text(
        text = stringResource(R.string.intro_title),
        fontSize = TypeScale.t1,
        lineHeight = TypeScale.t1Line,
        fontWeight = FontWeight.SemiBold,
        modifier = Modifier.semantics { heading() },
    )
    Spacer(Modifier.height(Space.s1))
    Text(
        text = stringResource(R.string.intro_lead),
        fontSize = TypeScale.t3,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )

    Spacer(Modifier.height(Space.s7))
    for (line in listOf(
        R.string.intro_point_offline,
        R.string.intro_point_control,
        R.string.intro_point_no_account,
    )) {
        Text(
            text = stringResource(line),
            fontSize = TypeScale.t5,
            lineHeight = TypeScale.t5Line,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = Space.s4),
        )
    }

    // 剩下的空間刻意留白，按鈕壓在底部：第一屏的重點是那三句話，不是按鈕。
    Spacer(Modifier.weight(1f))
    Button(
        onClick = onStart,
        modifier = Modifier.fillMaxWidth().heightIn(min = Dimens.touchTarget),
    ) { Text(stringResource(R.string.intro_start), fontSize = TypeScale.t4) }
    Spacer(Modifier.height(Space.s8))
}

/* ─────────────────────── 狀態 1 與 2：兩步 ─────────────────────── */

@Composable
private fun ColumnScope.TwoStepBody(stage: SetupStage, system: ImeSystemState) {
    val context = LocalContext.current
    val enabled = stage != SetupStage.NOT_ENABLED

    Text(
        text = stringResource(
            if (enabled) R.string.onboarding_last_step_title
            else R.string.onboarding_two_steps_title
        ),
        fontSize = TypeScale.t1,
        fontWeight = FontWeight.SemiBold,
        lineHeight = TypeScale.t1Line,
    )
    Spacer(Modifier.height(Space.s1))
    Text(
        text = if (enabled) {
            val now = system.currentImeLabel
            if (now != null) stringResource(R.string.onboarding_last_step_body_current, now)
            else stringResource(R.string.onboarding_last_step_body)
        } else {
            stringResource(R.string.onboarding_two_steps_body)
        },
        fontSize = TypeScale.t3,
        lineHeight = TypeScale.t3Line,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )

    Spacer(Modifier.height(Space.s7))

    PlainCard {
        StepRow(
            index = 1,
            done = enabled,
            title = stringResource(
                if (enabled) R.string.step_1_title_done else R.string.step_1_title
            ),
            state = if (enabled) null else stringResource(R.string.step_1_state_off),
            // 畫面上任何時候**只有一顆實心按鈕** —— 不必讀字也知道該點哪裡。
            action = if (enabled) null else stringResource(R.string.step_1_action),
            primary = !enabled,
            onClick = { openImeSettings(context) },
        )
        RowDivider()
        StepRow(
            index = 2,
            done = false,
            title = stringResource(R.string.step_2_title),
            state = stringResource(
                if (enabled) R.string.step_2_state_pending else R.string.step_2_state_waiting
            ),
            action = stringResource(R.string.step_2_action),
            primary = enabled,
            // 第 2 步在第 1 步完成之前是停用的 —— 那個順序是 Android 強制的，
            // 提早給一顆按得動的按鈕，只會讓人按了沒反應。
            enabled = enabled,
            onClick = { showImePicker(context) },
        )
    }

    // 剩下的空間刻意留白：第一屏的內容只佔約一半，剩下是空的。
    // 塞滿是為了顯得功能多，不是為了好用。
    Spacer(Modifier.weight(1f))
    DeployLine(
        hint = stringResource(
            if (enabled) R.string.deploy_hint_one_step else R.string.deploy_hint_two_steps
        )
    )
    Spacer(Modifier.height(Space.s8))
}

@Composable
private fun StepRow(
    index: Int,
    done: Boolean,
    title: String,
    state: String?,
    action: String?,
    primary: Boolean,
    enabled: Boolean = true,
    onClick: () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = Space.s5, vertical = Space.s5),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            modifier = Modifier
                .size(Dimens.stepBadge)
                .clip(CircleShape)
                .background(
                    if (done) MaterialTheme.colorScheme.primary
                    else MaterialTheme.colorScheme.surfaceVariant
                ),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                text = if (done) "✓" else index.toString(),
                fontSize = TypeScale.t5,
                fontWeight = FontWeight.SemiBold,
                color = if (done) MaterialTheme.colorScheme.onPrimary
                else MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Spacer(Modifier.size(Space.s4))
        Column(Modifier.weight(1f)) {
            Text(
                text = title,
                fontSize = if (done) TypeScale.t4 else TypeScale.t3,
                fontWeight = if (done) FontWeight.Normal else FontWeight.Medium,
                // 做完的事縮小、變灰；沒做的事變大。
                color = if (done) MaterialTheme.colorScheme.onSurfaceVariant
                else MaterialTheme.colorScheme.onSurface,
            )
            if (state != null) {
                Text(
                    text = state,
                    fontSize = TypeScale.t5,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        if (action != null) {
            if (primary) {
                Button(onClick = onClick, enabled = enabled) { Text(action) }
            } else {
                OutlinedButton(onClick = onClick, enabled = enabled) { Text(action) }
            }
        }
    }
}

/* ─────────────────────── 狀態 3：準備中 ─────────────────────── */

/**
 * 把那十幾秒換成唯一值得問的那個問題。
 *
 * 這一屏不放轉圈圈。它做兩件事：告訴你還在整理、順便讓你把**唯一一個真的
 * 因人而異的決定**做掉。第一張卡已經預選好，所以什麼都不做也是正確答案 ——
 * 這是這一屏不算「多一步」的關鍵。
 */
@Composable
private fun PreparingBody(phase: RimeRuntime.Phase) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    Text(
        text = stringResource(R.string.preparing_title),
        fontSize = TypeScale.t1,
        fontWeight = FontWeight.SemiBold,
        modifier = Modifier.semantics { heading() },
    )
    Spacer(Modifier.height(Space.s1))
    Text(
        // 秒數只有一個來源，見 [DeployEstimate]。
        text = stringResource(R.string.preparing_body, DeployEstimate.TYPICAL_SECONDS),
        fontSize = TypeScale.t3,
        lineHeight = TypeScale.t3Line,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )

    Spacer(Modifier.height(Space.s5))
    DeployBar()

    Spacer(Modifier.height(Space.s7))

    // 部署完成的那一刻方案清單會從檔案旁路換成引擎的清單，所以要跟著 phase 重算。
    //
    // ⚠ [phase] **一定要是參數**，不可以在這裡直接讀 `RimeRuntime.phase`。
    // 那是一個普通的 `@Volatile` 欄位，不是 Compose 狀態：讀它不會登記任何讀取
    // 關係；而一個沒有參數的 Composable 是可跳過的，上層重組時 Compose 會直接
    // 略過它。上一版的「整理字詞失敗」那句文案就是這樣一輩子畫不出來的。
    val all = remember(phase) { starterKeyboards(availableKeyboards(context)) }
    var picked by remember { mutableStateOf<KeyboardType?>(currentKeyboardOf(context, all)) }
    val selectedKey = picked?.key ?: all.firstOrNull()?.key
    if (all.isEmpty()) {
        Text(
            text = stringResource(R.string.preparing_no_keyboards),
            fontSize = TypeScale.t4,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    } else {
        SectionLabel(stringResource(R.string.preparing_pick))
        KeyboardGrid(
            types = all,
            selectedKey = selectedKey,
            onPick = { t ->
                picked = t
                scope.launch { applyKeyboardChoice(context, t) }
            },
        )
        Spacer(Modifier.height(Space.s5))
        Text(
            text = stringResource(R.string.preparing_more_hint),
            fontSize = TypeScale.t5,
            lineHeight = TypeScale.t5Line,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

/* ─────────────────────── 狀態 3b：失敗 ─────────────────────── */

/**
 * 整理字詞失敗。
 *
 * ── 為什麼它是一個帶參數的獨立 Composable ───────────────────────────────
 * 上一版把這件事寫在 [PreparingBody] 裡面，用 `RimeRuntime.phase == FAILED`
 * 分岔。那是**畫不出來的**：[PreparingBody] 當時沒有參數（可跳過），
 * 而 `RimeRuntime.phase` / `initError` 是普通的 `@Volatile` 欄位（讀了不登記）。
 * 兩件事湊在一起，等於這段分支永遠停在第一次組合時的值。
 *
 * 現在失敗是 [SetupStage.FAILED] 這一格，由上層的 `stage` 換屏、訊息由參數
 * 帶進來 —— 兩者都是 Compose 追得到的東西。
 *
 * ── 為什麼這一屏一定要有出路 ────────────────────────────────────────────
 * 引導頁只有 [ReadyBody] 那條路會呼叫 `onFinished`。所以在加這一屏之前，
 * 首次啟動一旦部署失敗，使用者就被**關在引導頁裡**：出不去、也走不到
 * 「進階與問題回報 → 重新整理字詞」。
 *
 * ⚠ **出路不等於那顆實心按鈕。** 五條失敗路徑裡有四條（.so 載不起來、
 * ABI 不符、隨附資料解不開、`rs_init` 回 false）根本按不動「重新整理字詞」
 * —— 那顆按鈕走 `DeployGate`，而它的第一行就會因為 `RimeCore.isInitialized`
 * 是 false 而回頭。所以按鈕由 [actionOf] 決定畫不畫，說明文字由
 * [failedBodyRes] 決定講什麼；那四條路上留下的是「進設定」這條真的走得通的路。
 */
@Composable
private fun FailedBody(
    error: String?,
    failure: RimeRuntime.Failure,
    onRefreshWords: () -> Unit,
    onFinished: () -> Unit,
) {
    Text(
        text = stringResource(failedTitleRes(failure)),
        fontSize = TypeScale.t1,
        fontWeight = FontWeight.SemiBold,
        modifier = Modifier.semantics { heading() },
    )
    Spacer(Modifier.height(Space.s1))
    Text(
        // 每一種失敗講的是使用者做得到的那一件事，見 [failedBodyRes]。
        text = stringResource(failedBodyRes(failure)),
        fontSize = TypeScale.t3,
        lineHeight = TypeScale.t3Line,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    // 引擎給的原因（英文的故障載荷，見 RimeRuntime 的註解）。排在使用者
    // 做得到的事後面 —— 他先知道自己該做什麼，這一行是給問題回報用的。
    if (!error.isNullOrBlank()) {
        Spacer(Modifier.height(Space.s3))
        Text(
            text = error,
            fontSize = TypeScale.t5,
            lineHeight = TypeScale.t5Line,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
    Spacer(Modifier.height(Space.s7))
    // ⚠ 畫不畫這顆按鈕**不是寫在這裡的 if**，是 [actionOf] 的結論 ——
    // 寫在 Composable 裡的 if 沒有人驗得到，那正是上一版的漏法。
    if (actionOf(SetupStage.FAILED, failure) == NotReadyAction.REFRESH_WORDS) {
        // 這一屏唯一的實心按鈕。按的是首頁那顆同一個 redeploy()，進度與結果由
        // StoreOverlays 統一畫（它畫在引導頁之外，所以這裡看得到）。
        Button(
            onClick = onRefreshWords,
            modifier = Modifier.fillMaxWidth().heightIn(min = Dimens.touchTarget),
        ) { Text(stringResource(R.string.not_ready_refresh_words), fontSize = TypeScale.t4) }
        Spacer(Modifier.height(Space.s4))
    }
    // 另一條路：不想在這裡耗（或那顆按鈕根本按不動），就直接進設定 ——
    // 首頁那條橫幅會把同一件事再說一次，而且那裡有診斷與問題回報。
    TextButton(
        onClick = onFinished,
        modifier = Modifier.heightIn(min = Dimens.touchTarget),
    ) { Text(stringResource(R.string.ready_more_settings), fontSize = TypeScale.t4) }
}

/* ─────────────────────── 狀態 4：好了 ─────────────────────── */

/**
 * 「好了」——**而且這裡也要問「注音還是拼音」**。
 *
 * ── 這一格原本只長在「準備中」那一屏 ────────────────────────────────────
 * 唯一值得問的那個問題（[PreparingBody] 的那一排 [KeyboardGrid]）本來只在
 * [SetupStage.PREPARING] 出現。而 `PREPARING` 的定義是「系統那兩步都做完了，
 * 引擎還在整理字詞」—— 也就是**只有跑得比部署快的人才看得到它**。
 *
 * 反過來說：先在系統設定裡設好、隔了一會兒才打開 App 的人（那是常見的走法，
 * 引導頁自己就是這樣叫他去的），一進來就是 `READY`，於是**從來沒有被問過**
 * 他要注音還是拼音。他拿到的是預設的朙月拼音全鍵盤，而注音使用者要自己
 * 摸到鍵盤上的地球鍵才換得掉。
 *
 * 三個環節做了兩個：問題想清楚了、元件也寫好了（[KeyboardGrid] 是現成的），
 * 就是沒有人在這一屏呼叫它。
 *
 * ── 為什麼放在試打框**之前** ────────────────────────────────────────────
 * 試打框是「驗收」，選鍵盤是「設定」。順序反過來的話，使用者會先用錯的鍵盤
 * 打一次字、覺得怪，才發現原來可以換。
 */
@Composable
private fun ReadyBody(phase: RimeRuntime.Phase, onFinished: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    Text(
        text = stringResource(R.string.ready_title),
        fontSize = TypeScale.t1,
        fontWeight = FontWeight.SemiBold,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.semantics { heading() },
    )
    Spacer(Modifier.height(Space.s1))
    Text(
        text = stringResource(R.string.ready_body),
        fontSize = TypeScale.t3,
        lineHeight = TypeScale.t3Line,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )

    // ⚠ [phase] 一定要是參數，理由與 [PreparingBody] 完全相同：
    // `RimeRuntime.phase` 是普通的 @Volatile 欄位，讀它不會登記任何讀取關係，
    // 而沒有參數的 Composable 是可跳過的。上一版有一段文案就是這樣一輩子
    // 畫不出來的。
    val all = remember(phase) { starterKeyboards(availableKeyboards(context)) }
    var picked by remember { mutableStateOf<KeyboardType?>(currentKeyboardOf(context, all)) }
    val selectedKey = picked?.key ?: all.firstOrNull()?.key
    if (all.isNotEmpty()) {
        Spacer(Modifier.height(Space.s7))
        SectionLabel(stringResource(R.string.preparing_pick))
        KeyboardGrid(
            types = all,
            selectedKey = selectedKey,
            onPick = { t ->
                picked = t
                scope.launch { applyKeyboardChoice(context, t) }
            },
        )
    }

    Spacer(Modifier.height(Space.s7))
    TryField()
    Spacer(Modifier.height(Space.s7))
    Text(
        text = stringResource(R.string.ready_hint),
        fontSize = TypeScale.t5,
        lineHeight = TypeScale.t5Line,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Spacer(Modifier.height(Space.s4))
    TextButton(
        onClick = onFinished,
        modifier = Modifier.heightIn(min = Dimens.touchTarget),
    ) { Text(stringResource(R.string.ready_more_settings), fontSize = TypeScale.t4) }
}

/* ─────────────────────── 進度 ─────────────────────── */

@Composable
private fun DeployLine(hint: String) {
    if (RimeRuntime.phase == RimeRuntime.Phase.READY) return
    Column {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = stringResource(R.string.deploy_line_title),
                fontSize = TypeScale.t5,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.weight(1f),
            )
            Text(
                text = stringResource(R.string.deploy_line_once),
                fontSize = TypeScale.t5,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Spacer(Modifier.height(Space.s3))
        DeployBar()
        Spacer(Modifier.height(Space.s4))
        Text(
            text = hint,
            fontSize = TypeScale.t5,
            lineHeight = TypeScale.t5Line,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

/**
 * 3dp 高的一條線。
 *
 * ⚠ 這是**估計**，不是真的進度：`rime_shell` 的 C ABI 沒有暴露部署進度，
 * 我們只知道開始的時間點。所以它封頂在 95%，絕不自己走到 100% ——
 * 一條走到底卻還沒好的進度條比沒有進度條更傷。
 */
@Composable
private fun DeployBar() {
    var tick by remember { mutableLongStateOf(0L) }
    LaunchedEffect(Unit) {
        while (RimeRuntime.phase != RimeRuntime.Phase.READY) {
            tick++
            delay(200)
        }
    }
    val elapsed = RimeRuntime.deployElapsedMillis
    @Suppress("UNUSED_EXPRESSION") tick
    val fraction = when {
        RimeRuntime.phase == RimeRuntime.Phase.READY -> 1f
        elapsed < 0 -> 0.04f
        // 分母只有一個來源，見 [DeployEstimate]（量測來源也記在那裡）。
        else -> (elapsed / DeployEstimate.TYPICAL_MS.toFloat()).coerceIn(0.04f, 0.95f)
    }
    Box(
        Modifier
            .fillMaxWidth()
            .height(Dimens.progress)
            .clip(RoundedCornerShape(Radius.small))
            .background(MaterialTheme.colorScheme.surfaceVariant)
    ) {
        Box(
            Modifier
                .fillMaxWidth(fraction)
                .height(Dimens.progress)
                .clip(RoundedCornerShape(Radius.small))
                .background(MaterialTheme.colorScheme.primary)
        )
    }
}

/* ─────────────────────── 兩顆按鈕 ─────────────────────── */

/**
 * 第 1 步：開系統的輸入法設定。
 *
 * 這一顆開的是**真的 Activity**，所以我們的 `onResume()` 會正常觸發。
 * 與下面那一顆不同 —— 見 [rememberImeSystemState] 的說明。
 */
fun openImeSettings(context: Context) {
    runCatching {
        context.startActivity(
            Intent(Settings.ACTION_INPUT_METHOD_SETTINGS)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        )
    }
}

/**
 * 第 2 步：叫出系統的輸入法選擇器。
 *
 * ⚠ 這一顆開的**不是** Activity，是一個系統對話框視窗。呼叫端全程維持
 * `RESUMED`，`onPause()` / `onResume()` 一次都不會被呼叫。任何把「重新檢查
 * 完成狀態」寫在 onResume 裡的引導頁，在使用者從這個對話框選好鍵盤之後
 * 就會永遠停在「還差一步」。解法見 [rememberImeSystemState]。
 */
fun showImePicker(context: Context) {
    val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
    imm?.showInputMethodPicker()
}
