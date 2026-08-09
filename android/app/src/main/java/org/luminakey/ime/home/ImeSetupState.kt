package org.luminakey.ime.home

import android.content.Context
import android.provider.Settings
import android.view.inputmethod.InputMethodManager
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.repeatOnLifecycle
import kotlinx.coroutines.delay
import org.luminakey.ime.core.RimeRuntime

/**
 * 「我現在能不能用它打字」—— 引導頁只回答這一個問題。
 *
 * ── 為什麼是四種狀態而不是三種 ──────────────────────────────────────────
 * 直覺上是三種：沒啟用 / 啟用了但不是預設 / 好了。實測推翻了這個直覺 ——
 * 首次啟動時 librime 要編三本詞庫加一份 5.7MB 語言模型，耗時見
 * [org.luminakey.ime.core.DeployEstimate]（量測來源也記在那裡）。也就是說
 * 「已啟用 + 已是預設」**不等於**「能打字」。
 *
 * 少了 [SetupStage.PREPARING] 這一格，使用者會在那段時間裡點進試打框、
 * 打字沒反應，然後認定它壞了。**這一格的存在本身就是這一版最重要的修正之一。**
 *
 * ── 為什麼「失敗」是第五格，而不是併進「準備中」──────────────────────
 * 併進去的版本出貨過，後果是：部署一失敗，首頁就永遠停在「正在整理字詞」，
 * 而那一格**刻意不給按鈕**（理由寫在原始碼裡：「沒有人能加速它」——
 * 這句話對準備中成立，對已經失敗是錯的）。使用者看著一句永遠不會結束的
 * 請稍候，唯一的自救路徑藏在首頁最底下的灰字列裡。
 * **等待與壞掉必須長得不一樣，而且壞掉必須看得見出路。**
 *
 * ── 為什麼完成狀態不存旗標 ──────────────────────────────────────────────
 * 兩步的完成與否都可以即時從系統問到，兩者都不需要任何權限。競品的三步精靈
 * 記自己的旗標，於是使用者若先從系統設定裡設好，回到精靈裡仍停在第三步，
 * 而且完全無法自救（見 docs/competitive-review.md §4.1）。
 *
 * 這裡的每一次組合都重新問一次系統，所以「中途離開、隔天再回來、先從系統
 * 設定弄好」全部自然成立 —— 不是多寫了幾條路徑，而是根本沒有路徑可寫。
 */
enum class SetupStage {
    /** 系統的輸入法清單裡還沒有我們。 */
    NOT_ENABLED,

    /** 在清單裡了，但現在打字用的是別人。 */
    ENABLED_NOT_DEFAULT,

    /** 兩步都完成了，但詞庫還在整理 —— **還不能打字**，而且等就會好。 */
    PREPARING,

    /**
     * 整理字詞失敗了 —— **等下去不會變好**。
     *
     * 與 [PREPARING] 的差別不在文案，在**有沒有出路**：這一格一定要給一顆
     * 看得見的「重新整理字詞」，並把 `RimeRuntime.initError` 一起帶出來。
     */
    FAILED,

    /** 真的可以打字了。 */
    READY,
}

/**
 * 從系統即時查到的兩件事，外加一句「那現在用的是誰」。
 *
 * [currentImeLabel] 只在我們不是預設時才有意義：使用者要知道自己在跟什麼比較，
 * 「現在打字用的還是 Gboard」比「尚未設為預設」好懂得多。
 */
data class ImeSystemState(
    val enabled: Boolean,
    val isDefault: Boolean,
    val currentImeLabel: String?,
)

/* ───────────────────────── 純函式（可直接單元測試）───────────────────────── */

/**
 * `com.google.android.inputmethod.latin/com.android.inputmethod.latin.LatinIME` → 套件名。
 *
 * ⚠ **一律比對套件名，不要比對完整 id。** service 的類別名日後只要被重構
 * （換套件路徑、改類別名），寫死的完整 id 就會對不上，使用者會被困在引導頁裡
 * 出不來，而且完全無法自救 —— 他做的每一件事都是對的，畫面就是不承認。
 */
internal fun packageOfImeId(id: String?): String? =
    id?.substringBefore('/')?.takeIf { it.isNotEmpty() }

internal fun isDefaultImePackage(defaultImeId: String?, myPackage: String): Boolean =
    packageOfImeId(defaultImeId) == myPackage

internal fun isEnabledPackage(enabledImePackages: List<String>, myPackage: String): Boolean =
    enabledImePackages.contains(myPackage)

/**
 * 兩個系統事實 + 引擎階段 → 五種狀態。
 *
 * [RimeRuntime.Phase.FAILED] 自己一格。它與 [SetupStage.PREPARING] 唯一的共通點
 * 是「都不能打字」，而畫面上該說的話、該不該給按鈕完全相反 ——
 * 上一版把兩者併在一起，於是失敗的人被畫成還在等。把失敗當成可用是最不能犯的
 * 錯；把失敗畫成等待是第二不能犯的。
 *
 * ⚠ 系統那兩步仍然排在最前面：沒啟用／不是預設的時候先講那兩步，因為它們是
 * **前置條件** —— 字詞整理好了他還是打不了字。他做完那兩步之後，下一次輪詢
 * 就會看到 [SetupStage.FAILED]。
 */
fun stageOf(system: ImeSystemState, phase: RimeRuntime.Phase): SetupStage = when {
    !system.enabled -> SetupStage.NOT_ENABLED
    !system.isDefault -> SetupStage.ENABLED_NOT_DEFAULT
    phase == RimeRuntime.Phase.FAILED -> SetupStage.FAILED
    phase != RimeRuntime.Phase.READY -> SetupStage.PREPARING
    else -> SetupStage.READY
}

/**
 * 首頁那條「還不能用」的橫幅上，**該給哪一顆按鈕**。
 *
 * ── 為什麼要一個純函式，而不是寫在 Composable 裡的一個 if ────────────────
 * 因為原本的缺陷就是寫在 Composable 裡的那一行：
 * `if (stage == NOT_ENABLED || stage == ENABLED_NOT_DEFAULT)`。它安靜地把失敗態
 * 排除在外，而**沒有任何一條測試看得見它** —— UI 的分支不是可以斷言的東西，
 * 純函式才是。搬到這裡之後，「失敗態有沒有出路」變成一行就能驗的事實。
 */
enum class NotReadyAction {
    /** 不給按鈕。 */
    NONE,

    /** 開系統的輸入法設定（第 1 步）。 */
    OPEN_IME_SETTINGS,

    /** 叫出系統的輸入法選擇器（第 2 步）。 */
    SWITCH_IME,

    /** 重新整理字詞（`StoreController.redeploy()`）。 */
    REFRESH_WORDS,
}

fun actionOf(stage: SetupStage): NotReadyAction = when (stage) {
    SetupStage.NOT_ENABLED -> NotReadyAction.OPEN_IME_SETTINGS
    SetupStage.ENABLED_NOT_DEFAULT -> NotReadyAction.SWITCH_IME
    // 失敗態唯一的出路。原本它只存在於「進階與問題回報」頁最底下的灰字列裡。
    SetupStage.FAILED -> NotReadyAction.REFRESH_WORDS
    // 準備中真的沒有人能加速它，給一顆按鈕只會讓人按了以為有用。
    SetupStage.PREPARING -> NotReadyAction.NONE
    SetupStage.READY -> NotReadyAction.NONE
}

/* ───────────────────────── Android 查詢 ───────────────────────── */

fun readImeSystemState(context: Context): ImeSystemState {
    val pkg = context.packageName
    val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager
        ?: return ImeSystemState(enabled = false, isDefault = false, currentImeLabel = null)

    // enabledInputMethodList 只列「已啟用」的，不列「已安裝未啟用」的 ——
    // 所以它問的正好是第 1 步。使用者在系統設定裡打勾之後這份清單**立刻**就變
    // （IPC 即時查詢，不必重啟行程），實測成立。
    val enabledPackages = runCatching { imm.enabledInputMethodList.map { it.packageName } }
        .getOrDefault(emptyList())
    val defaultId = runCatching {
        Settings.Secure.getString(context.contentResolver, Settings.Secure.DEFAULT_INPUT_METHOD)
    }.getOrNull()

    val isDefault = isDefaultImePackage(defaultId, pkg)
    return ImeSystemState(
        enabled = isEnabledPackage(enabledPackages, pkg),
        isDefault = isDefault,
        currentImeLabel = if (isDefault) null else labelOfIme(context, imm, defaultId),
    )
}

private fun labelOfIme(
    context: Context,
    imm: InputMethodManager,
    imeId: String?,
): String? {
    val id = imeId ?: return null
    val all = runCatching { imm.enabledInputMethodList }.getOrDefault(emptyList()) +
        runCatching { imm.inputMethodList }.getOrDefault(emptyList())
    val info = all.firstOrNull { it.id == id } ?: return null
    return runCatching { info.loadLabel(context.packageManager).toString() }
        .getOrNull()
        ?.takeIf { it.isNotBlank() }
}

/* ───────────────────────── Compose 接線 ───────────────────────── */

/**
 * 輪詢間隔。兩次系統查詢很便宜（兩個 IPC，沒有 IO），而**慢半秒沒關係、
 * 永遠不更新才會出事**。
 */
private const val POLL_MS = 450L

/**
 * 系統狀態，會自己跟上。
 *
 * ── 這裡藏著整個引導頁能不能用的關鍵 ────────────────────────────────────
 * 直覺的做法是在 `onResume()` 重新檢查一次。**那是錯的**，而且極可能就是
 * 競品「卡在第三步」的真正成因：
 *
 *   `InputMethodManager.showInputMethodPicker()` 開的是一個**系統對話框視窗**，
 *   不是 Activity。實測（API 35）它蓋上來的時候，呼叫端的 Activity 仍然是
 *   `RESUMED`，只有 `mCurrentFocus` 移走了 —— 所以 `onPause()` / `onResume()`
 *   **一次都不會被呼叫**，寫在 onResume 裡的重新檢查永遠不會跑。
 *
 * 對照組：第 1 步那顆按鈕開的 `Settings.ACTION_INPUT_METHOD_SETTINGS` 是**真的**
 * Activity，`onResume()` 會正常觸發。**兩顆按鈕的回程機制不一樣**，這一點
 * 只有實際跑過才會知道。
 *
 * 所以這裡兩條路都走：
 *   1. [refreshKey] 由 Activity 的 `onWindowFocusChanged` 推進 —— 那個回呼**會**
 *      被呼叫（焦點確實移走又回來），給的是「立刻」。
 *   2. 畫面在 STARTED 以上時每 [POLL_MS] 輪詢一次 —— 給的是「無論從哪條路
 *      回來都算數」，包含從系統設定 App 一路繞回來、被電話打斷、隔天再打開。
 */
@Composable
fun rememberImeSystemState(refreshKey: Any = Unit): ImeSystemState {
    val context = LocalContext.current
    val owner = LocalLifecycleOwner.current
    var state by remember { mutableStateOf(readImeSystemState(context)) }
    LaunchedEffect(owner, refreshKey) {
        // 焦點剛回來的那一刻先查一次，不要等下一個輪詢週期。
        state = readImeSystemState(context)
        owner.lifecycle.repeatOnLifecycle(Lifecycle.State.STARTED) {
            while (true) {
                state = readImeSystemState(context)
                delay(POLL_MS)
            }
        }
    }
    return state
}

/**
 * 失敗訊息，跟著 [rememberRimePhase] 一起更新。
 *
 * ⚠ `RimeRuntime.initError` 是一個普通的 `@Volatile` 欄位，**不是 Compose 狀態**：
 * 在 Composable 裡直接讀它不會登記任何讀取關係，Compose 也就沒有任何理由因為
 * 它變了而重組。所以這裡拿 `phase` 當 `remember` 的鍵 —— `RimeRuntime` 的每一條
 * 失敗路徑都是**先寫 initError 再 setPhase(FAILED)**（見該檔），
 * 所以 phase 一到手，訊息一定已經寫好了。
 */
@Composable
fun rememberRimeInitError(phase: RimeRuntime.Phase): String? =
    remember(phase) { RimeRuntime.initError }

/** librime 的初始化階段，會自己跟上。 */
@Composable
fun rememberRimePhase(): RimeRuntime.Phase {
    var phase by remember { mutableStateOf(RimeRuntime.phase) }
    DisposableEffect(Unit) {
        val listener: (RimeRuntime.Phase) -> Unit = { phase = it }
        RimeRuntime.addListener(listener)
        onDispose { RimeRuntime.removeListener(listener) }
    }
    return phase
}
