package org.rimequad.ime.home

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
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.rimequad.ime.core.RimeRuntime
import org.rimequad.ime.keyboard.KeyboardType
import org.rimequad.ime.prefs.PrefsStore

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
 * 首次部署實測 8.0 秒（模擬器）／12.5 秒（三星 S24U）。這段藏不掉，但可以
 * 跟人的動作**並行**：字詞整理在 App 一打開就開始跑，而使用者這時要去系統
 * 設定按兩趟 —— 人走這兩趟的時間本來就超過那十幾秒。
 *
 * **這個前提是實測過的**：App 被切到背景（使用者人在系統的輸入法設定頁）
 * 期間，部署照樣跑完，phase 從 DEPLOYING 走到 READY 全程發生在背景。
 * 所以進度條出現在畫面最下面、只有 3dp 高，旁邊寫著「不用等它」——
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
    onFinished: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    // D 線的結論：持久化的東西只有這一個布林，而且它**只**決定 app 冷啟時
    // 落在哪一頁，絕不代表「輸入法可以用」。寫入時機是「第一次觀察到系統
    // 說我們已經是預設輸入法」，而不是「使用者按了完成」—— 以系統事實為準。
    LaunchedEffect(stage) {
        if (stage == SetupStage.READY || stage == SetupStage.PREPARING) {
            val store = PrefsStore.get(context)
            if (store.current.onboardingDone != true) {
                store.update { it.copy(onboardingDone = true) }
            }
        }
    }

    // 「準備中」那一屏有一排鍵盤卡，長度不固定，所以只有它捲動；
    // 另外三種狀態一律**不捲動** —— 引導頁一旦要捲，「一屏就是全部」就破了。
    val base = modifier.fillMaxSize().padding(horizontal = 22.dp)
    when (stage) {
        SetupStage.NOT_ENABLED, SetupStage.ENABLED_NOT_DEFAULT ->
            Column(base) {
                Spacer(Modifier.height(28.dp))
                TwoStepBody(stage = stage, system = system)
            }

        SetupStage.PREPARING ->
            Column(base.verticalScroll(rememberScrollState())) {
                Spacer(Modifier.height(28.dp))
                PreparingBody()
                Spacer(Modifier.height(28.dp))
            }

        SetupStage.READY ->
            Column(base) {
                Spacer(Modifier.height(28.dp))
                ReadyBody(onFinished = onFinished)
            }
    }
}

/* ─────────────────────── 狀態 1 與 2：兩步 ─────────────────────── */

@Composable
private fun ColumnScope.TwoStepBody(stage: SetupStage, system: ImeSystemState) {
    val context = LocalContext.current
    val enabled = stage != SetupStage.NOT_ENABLED

    Text(
        text = if (enabled) "剩最後一步" else "打字前，還差兩步",
        fontSize = 27.sp,
        fontWeight = FontWeight.SemiBold,
        lineHeight = 34.sp,
    )
    Spacer(Modifier.height(10.dp))
    Text(
        text = if (enabled) {
            val now = system.currentImeLabel
            if (now != null) "現在打字用的還是「$now」。\n按「切換」會跳出系統的鍵盤清單，在裡面點一下我們就好。"
            else "按「切換」會跳出系統的鍵盤清單，在裡面點一下我們就好。"
        } else {
            "這兩步 Android 規定要你親手做，我們只能帶路。\n做完就不會再出現。"
        },
        fontSize = 15.5.sp,
        lineHeight = 23.sp,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )

    Spacer(Modifier.height(34.dp))

    PlainCard {
        StepRow(
            index = 1,
            done = enabled,
            title = if (enabled) "已在系統設定裡打開" else "在系統設定裡打開",
            state = if (enabled) null else "還沒打開",
            // 畫面上任何時候**只有一顆實心按鈕** —— 不必讀字也知道該點哪裡。
            action = if (enabled) null else "前往",
            primary = !enabled,
            onClick = { openImeSettings(context) },
        )
        RowDivider()
        StepRow(
            index = 2,
            done = false,
            title = "設成你平常用的鍵盤",
            state = if (enabled) "還沒設定" else "等上一步完成",
            action = "切換",
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
    DeployLine(hint = if (enabled) "不用等它，先去做上面那一步就好。" else "不用等它，先去做上面那兩步就好。")
    Spacer(Modifier.height(28.dp))
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
            .padding(horizontal = 16.dp, vertical = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            modifier = Modifier
                .size(24.dp)
                .clip(CircleShape)
                .background(
                    if (done) MaterialTheme.colorScheme.primary
                    else MaterialTheme.colorScheme.surfaceVariant
                ),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                text = if (done) "✓" else index.toString(),
                fontSize = 13.sp,
                fontWeight = FontWeight.SemiBold,
                color = if (done) MaterialTheme.colorScheme.onPrimary
                else MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Spacer(Modifier.size(14.dp))
        Column(Modifier.weight(1f)) {
            Text(
                text = title,
                fontSize = if (done) 14.5.sp else 16.sp,
                fontWeight = if (done) FontWeight.Normal else FontWeight.Medium,
                // 做完的事縮小、變灰；沒做的事變大。
                color = if (done) MaterialTheme.colorScheme.onSurfaceVariant
                else MaterialTheme.colorScheme.onSurface,
            )
            if (state != null) {
                Text(
                    text = state,
                    fontSize = 12.5.sp,
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
private fun PreparingBody() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val failed = RimeRuntime.phase == RimeRuntime.Phase.FAILED
    val error = RimeRuntime.initError

    Text(
        text = if (failed) "字詞整理沒成功" else "還在整理字詞",
        fontSize = 27.sp,
        fontWeight = FontWeight.SemiBold,
    )
    Spacer(Modifier.height(10.dp))
    Text(
        text = if (failed) {
            "鍵盤還是打得開，但可能打不出字。到「進階與問題回報」按一次" +
                "「重新整理字詞」通常就好了。\n\n$error"
        } else {
            "只有第一次要做，大概十幾秒。\n趁現在挑一個順手的就好，不挑也可以，會用第一個。"
        },
        fontSize = 15.5.sp,
        lineHeight = 23.sp,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )

    if (!failed) {
        Spacer(Modifier.height(18.dp))
        DeployBar()
    }

    Spacer(Modifier.height(30.dp))

    // 部署完成的那一刻方案清單會從檔案旁路換成引擎的清單，所以要跟著 phase 重算。
    val all = remember(RimeRuntime.phase) { starterKeyboards(availableKeyboards(context)) }
    var picked by remember { mutableStateOf<KeyboardType?>(currentKeyboardOf(context, all)) }
    val selectedKey = picked?.key ?: all.firstOrNull()?.key
    if (all.isEmpty()) {
        Text(
            text = "還讀不到可用的鍵盤清單。等字詞整理完，到「設定 › 鍵盤」也能挑。",
            fontSize = 13.5.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    } else {
        SectionLabel("挑一個鍵盤")
        KeyboardGrid(
            types = all,
            selectedKey = selectedKey,
            onPick = { t ->
                picked = t
                scope.launch { applyKeyboardChoice(context, t) }
            },
        )
        Spacer(Modifier.height(16.dp))
        Text(
            text = "還有更多鍵盤可以選。之後在鍵盤上點候選字那一列右邊的 ⚙ 就能換，不用回到這裡。",
            fontSize = 12.5.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

/* ─────────────────────── 狀態 4：好了 ─────────────────────── */

@Composable
private fun ReadyBody(onFinished: () -> Unit) {
    Text(
        text = "好了，打字看看",
        fontSize = 27.sp,
        fontWeight = FontWeight.SemiBold,
        color = MaterialTheme.colorScheme.primary,
    )
    Spacer(Modifier.height(10.dp))
    Text(
        text = "下面這格可以隨便打，打壞也沒關係。",
        fontSize = 15.5.sp,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Spacer(Modifier.height(22.dp))
    TryField(placeholder = "在這裡打字看看")
    Spacer(Modifier.height(22.dp))
    Text(
        text = "換鍵盤、調高度、換深色：打字時點候選字那一列右邊的 ⚙。",
        fontSize = 13.sp,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
    Spacer(Modifier.height(14.dp))
    TextButton(onClick = onFinished) { Text("其他設定 ›") }
}

/* ─────────────────────── 進度 ─────────────────────── */

/** 首次部署的估計耗時。實測：模擬器 8.0 秒、三星 S24U 12.5 秒。 */
private const val DEPLOY_ESTIMATE_MS = 12_500f

@Composable
private fun DeployLine(hint: String) {
    if (RimeRuntime.phase == RimeRuntime.Phase.READY) return
    Column {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(
                text = "正在整理字詞",
                fontSize = 12.5.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.weight(1f),
            )
            Text(
                text = "只有第一次要做",
                fontSize = 12.5.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Spacer(Modifier.height(8.dp))
        DeployBar()
        Spacer(Modifier.height(10.dp))
        Text(text = hint, fontSize = 12.5.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
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
        else -> (elapsed / DEPLOY_ESTIMATE_MS).coerceIn(0.04f, 0.95f)
    }
    Box(
        Modifier
            .fillMaxWidth()
            .height(3.dp)
            .clip(RoundedCornerShape(2.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant)
    ) {
        Box(
            Modifier
                .fillMaxWidth(fraction)
                .height(3.dp)
                .clip(RoundedCornerShape(2.dp))
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
