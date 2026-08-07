package org.rimequad.ime

import android.content.Intent
import android.content.res.Configuration
import android.inputmethodservice.InputMethodService
import android.util.Log
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.EditorInfo
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.rimequad.ime.core.AndroidKeyMap
import org.rimequad.ime.core.RimeCore
import org.rimequad.ime.core.RimeRuntime
import org.rimequad.ime.keyboard.ConfigRepository
import org.rimequad.ime.keyboard.KeyboardEvent
import org.rimequad.ime.keyboard.KeyboardUiState
import org.rimequad.ime.keyboard.LayoutHost
import org.rimequad.ime.keyboard.RimeKeyboard
import org.rimequad.ime.theme.ActionVerb
import org.rimequad.ime.theme.KeyAction
import org.rimequad.ime.theme.Keysym
import org.rimequad.ime.theme.SendSpec
import org.rimequad.ime.ui.RimeTheme

/**
 * IME 本體。
 *
 * 執行緒：rime_shell.h 要求同一 session 的呼叫序列化在同一條執行緒上。
 * 本類別所有 RimeCore 呼叫都發生在 IME 主執行緒 —— Compose 事件回呼、
 * onKeyDown、以及 RimeRuntime 切回主執行緒後的 phase 回呼，全都是主執行緒。
 *
 * 軟鍵盤的每一個鍵都來自 `core/layouts` 下的 yaml（見 [LayoutHost]）：
 * 這個類別只負責把佈局描述的 `send` / `tap` 翻成 rime_shell 呼叫，
 * 不再知道「QWERTY 長什麼樣」。
 */
class RimeInputMethodService : InputMethodService() {

    private companion object {
        const val TAG = "RimeIME"
    }

    private var session: Long = RimeCore.INVALID_SESSION
    private var host: ComposeKeyboardHost? = null

    private lateinit var config: ConfigRepository
    private lateinit var layoutHost: LayoutHost

    private var uiState by mutableStateOf(KeyboardUiState())

    /** 上一次看到的方案 id，用來偵測方案變動並套用 §9.1.1 的自動換佈局。 */
    private var lastSchemaId: String = ""

    /** onKeyDown 消費掉的 keycode，onKeyUp 要對應吃掉，否則宿主會收到落單的 up。 */
    private val consumedKeys = HashSet<Int>()

    private val phaseListener: (RimeRuntime.Phase) -> Unit = { phase -> onPhase(phase) }

    override fun onCreate() {
        super.onCreate()
        uiState = uiState.copy(isStub = RimeCore.isStub())
        // 解壓在背景執行緒，rs_init 之後才切回主執行緒，這裡不會阻塞。
        RimeRuntime.start(applicationContext)

        // 佈局與主題不依賴 librime，可以先載起來 —— 鍵盤在部署完成前就畫得出來。
        config = ConfigRepository(applicationContext)
        layoutHost = LayoutHost(config)
        layoutHost.ensureLoaded()
        layoutHost.applyAppearance(systemInDarkMode())
        syncConfigToUi()

        RimeRuntime.addListener(phaseListener)
        Log.i(TAG, "onCreate: stub=${RimeCore.isStub()} phase=${RimeRuntime.phase}")
    }

    override fun onCreateInputView(): View {
        host?.onDestroy()
        val newHost = ComposeKeyboardHost(this)
        host = newHost
        // decorView 必須傳進去，否則 Compose 找不到 ViewTreeLifecycleOwner
        // 而在 attach 當下崩潰。詳見 ComposeKeyboardHost 的註解。
        return newHost.createView(window?.window?.decorView) {
            RimeTheme {
                RimeKeyboard(state = uiState, onEvent = ::handleEvent)
            }
        }
    }

    override fun onStartInputView(info: EditorInfo?, restarting: Boolean) {
        super.onStartInputView(info, restarting)
        host?.onStartInput()
        ensureSession()
        refreshFromRime(consumeCommit = false)
    }

    override fun onFinishInputView(finishingInput: Boolean) {
        super.onFinishInputView(finishingInput)
        if (session != RimeCore.INVALID_SESSION) {
            RimeCore.clearComposition(session)
        }
        currentInputConnection?.finishComposingText()
        uiState = uiState.copy(schemaPickerOpen = false)
        host?.onFinishInput()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        // §8.2 執行期規則第 2 條：跟隨系統時切到 counterpart。
        layoutHost.applyAppearance(
            (newConfig.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
                Configuration.UI_MODE_NIGHT_YES
        )
        syncConfigToUi()
    }

    override fun onDestroy() {
        RimeRuntime.removeListener(phaseListener)
        host?.onDestroy()
        host = null
        if (session != RimeCore.INVALID_SESSION) {
            RimeCore.sessionDestroy(session)
            session = RimeCore.INVALID_SESSION
        }
        super.onDestroy()
    }

    private fun systemInDarkMode(): Boolean =
        (resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
            Configuration.UI_MODE_NIGHT_YES

    /* ─────────────────── 初始化狀態 ─────────────────── */

    private fun onPhase(phase: RimeRuntime.Phase) {
        when (phase) {
            RimeRuntime.Phase.IDLE,
            RimeRuntime.Phase.EXTRACTING,
            -> uiState = uiState.copy(busyMessage = "正在準備輸入法資料…", fatalMessage = null)

            RimeRuntime.Phase.DEPLOYING ->
                uiState = uiState.copy(
                    busyMessage = "首次啟動：正在編譯詞庫，需要一到兩分鐘…",
                    fatalMessage = null,
                )

            RimeRuntime.Phase.READY -> {
                uiState = uiState.copy(
                    busyMessage = null,
                    fatalMessage = null,
                    schemas = RimeCore.schemaList(),
                )
                // 部署會讓舊 session 失效，這裡一律重建。
                rebuildSession()
                refreshFromRime(consumeCommit = false)
            }

            RimeRuntime.Phase.FAILED ->
                uiState = uiState.copy(
                    busyMessage = null,
                    fatalMessage = RimeRuntime.initError ?: "rime 初始化失敗",
                )
        }
        Log.i(TAG, "phase=$phase session=$session")
    }

    private fun rebuildSession() {
        if (session != RimeCore.INVALID_SESSION) {
            RimeCore.sessionDestroy(session)
            session = RimeCore.INVALID_SESSION
        }
        session = RimeCore.sessionCreate()
        if (session == RimeCore.INVALID_SESSION) {
            Log.e(TAG, "建立 session 失敗: ${RimeCore.lastError()}")
        }
    }

    /** 部署後舊 session 會失效（見 rime_shell.h），這裡負責重建。 */
    private fun ensureSession() {
        if (!RimeRuntime.isReady) return
        if (!RimeCore.sessionAlive(session)) {
            rebuildSession()
            Log.i(TAG, "session 已失效，重建為 $session")
        }
    }

    /** 把 [LayoutHost] 的當前佈局／層／主題搬進 UI 狀態。 */
    private fun syncConfigToUi() {
        uiState = uiState.copy(
            theme = layoutHost.theme,
            layout = layoutHost.layout,
            layerId = layoutHost.layerId,
            configProblem = layoutHost.problem,
        )
    }

    /* ─────────────────── 實體鍵盤 ───────────────────
     *
     * `adb shell input keyevent` 與真正的實體鍵盤走的都是這條路徑
     * （宿主 → InputMethodService.onKeyDown → rs_process_key），
     * 和軟鍵盤點擊完全不同。scripts/verify_rime_compose.sh 就是靠這條
     * 路徑證明 librime 真的在工作。
     */

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (processHardwareKey(event, release = false)) {
            consumedKeys.add(keyCode)
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        if (consumedKeys.remove(keyCode)) {
            // key-up 也送給 librime（有些方案靠 RS_MOD_RELEASE 做上屏），
            // 但無論它吃不吃，這個 up 都不該再交給宿主。
            processHardwareKey(event, release = true)
            return true
        }
        return super.onKeyUp(keyCode, event)
    }

    private fun processHardwareKey(event: KeyEvent, release: Boolean): Boolean {
        ensureSession()
        if (session == RimeCore.INVALID_SESSION) return false

        val keysym = AndroidKeyMap.keysymOf(event)
        if (keysym == 0) return false

        val mods = AndroidKeyMap.modifiersOf(event, release)
        val consumed = RimeCore.processKey(session, keysym, mods)
        if (consumed) refreshFromRime()
        return consumed
    }

    /* ─────────────────── 軟鍵盤事件 ─────────────────── */

    private fun handleEvent(event: KeyboardEvent) {
        when (event) {
            is KeyboardEvent.Send -> handleSend(event.spec)
            is KeyboardEvent.Act -> handleAction(event.action)

            is KeyboardEvent.Candidate -> {
                RimeCore.selectCandidate(session, event.indexOnPage)
                refreshFromRime(allowAutoCommit = true)
            }

            is KeyboardEvent.CandidateLongPress -> {
                RimeCore.deleteCandidate(session, event.indexOnPage)
                refreshFromRime()
            }

            is KeyboardEvent.Page -> {
                RimeCore.changePage(session, event.backward)
                refreshFromRime()
            }

            is KeyboardEvent.OpenSchemaPicker ->
                uiState = uiState.copy(
                    schemaPickerOpen = true,
                    schemas = RimeCore.schemaList(),
                )

            is KeyboardEvent.CloseSchemaPicker ->
                uiState = uiState.copy(schemaPickerOpen = false)

            is KeyboardEvent.SelectSchema -> {
                selectSchema(event.id)
                uiState = uiState.copy(schemaPickerOpen = false)
            }
        }
    }

    /** §9.4：`send` 有且僅有兩種形態，互斥。 */
    private fun handleSend(spec: SendSpec) {
        ensureSession()
        when (spec) {
            is SendSpec.Keysym -> {
                // 靜態表查得到就用它；查不到本應回落到 RimeGetKeycodeByName()，
                // 但 rime_shell 的 C ABI 目前沒有暴露那個函式（見回報中的 ABI 缺口），
                // 所以這裡只能讓該鍵變成 noop。
                val code = spec.code
                if (code == Keysym.VOID_SYMBOL) {
                    Log.w(TAG, "keysym '${spec.name}' 無法解析，該鍵視為 noop")
                    return
                }
                val consumed = RimeCore.processKey(session, code, spec.modifiers)
                if (!consumed) fallbackKey(code)
                layoutHost.afterKeySent()
                refreshFromRime()
            }

            is SendSpec.Text -> {
                // 形態 B：**繞過 librime**，直接上屏。
                currentInputConnection?.commitText(spec.text, 1)
                layoutHost.afterKeySent()
                syncConfigToUi()
            }
        }
    }

    /** §9.5 的 action 分派。 */
    private fun handleAction(action: KeyAction) {
        val ic = currentInputConnection
        when (action.verb) {
            ActionVerb.NOOP -> Unit

            ActionVerb.LAYER -> action.arg?.let { layoutHost.setLayer(it) }
            ActionVerb.LAYER_ONCE -> action.arg?.let { layoutHost.setLayerOnce(it) }
            ActionVerb.LAYER_LOCK -> action.arg?.let { layoutHost.lockLayer(it) }
            ActionVerb.SWITCH_LAYOUT -> action.arg?.let { layoutHost.switchLayout(it) }

            ActionVerb.TOGGLE_OPTION -> action.arg?.let { opt ->
                RimeCore.setOption(session, opt, !RimeCore.getOption(session, opt))
                refreshFromRime()
            }

            ActionVerb.SET_OPTION -> {
                val opt = action.args.getOrNull(0)
                val on = action.args.getOrNull(1) == "on"
                if (opt != null) {
                    RimeCore.setOption(session, opt, on)
                    refreshFromRime()
                }
            }

            ActionVerb.SCHEMA_NEXT -> cycleSchema(1)
            ActionVerb.SCHEMA_PREV -> cycleSchema(-1)
            ActionVerb.SCHEMA_PICKER ->
                uiState = uiState.copy(schemaPickerOpen = true, schemas = RimeCore.schemaList())

            ActionVerb.SCHEMA_SELECT -> action.arg?.let { selectSchema(it) }

            ActionVerb.CANDIDATE_SELECT -> {
                val n = action.arg?.toIntOrNull() ?: return
                RimeCore.selectCandidate(session, n)
                refreshFromRime(allowAutoCommit = true)
            }

            ActionVerb.CANDIDATE_DELETE -> {
                val n = action.arg?.toIntOrNull() ?: return
                RimeCore.deleteCandidate(session, n)
                refreshFromRime()
            }

            ActionVerb.CANDIDATE_NEXT_PAGE -> {
                RimeCore.changePage(session, false)
                refreshFromRime()
            }

            ActionVerb.CANDIDATE_PREV_PAGE -> {
                RimeCore.changePage(session, true)
                refreshFromRime()
            }

            // rime_shell 沒有「移動高亮」的函式，只有換頁。見回報中的 ABI 缺口。
            ActionVerb.CANDIDATE_NEXT, ActionVerb.CANDIDATE_PREV ->
                Log.w(TAG, "action ${action.raw} 尚無對應的 rime_shell 呼叫，忽略")

            ActionVerb.CURSOR_LEFT -> sendHostKey(KeyEvent.KEYCODE_DPAD_LEFT)
            ActionVerb.CURSOR_RIGHT -> sendHostKey(KeyEvent.KEYCODE_DPAD_RIGHT)
            ActionVerb.CURSOR_HOME -> sendHostKey(KeyEvent.KEYCODE_MOVE_HOME)
            ActionVerb.CURSOR_END -> sendHostKey(KeyEvent.KEYCODE_MOVE_END)

            ActionVerb.CLEAR -> {
                RimeCore.clearComposition(session)
                ic?.finishComposingText()
                refreshFromRime()
            }

            ActionVerb.HIDE_KEYBOARD -> requestHideSelf(0)

            ActionVerb.SETTINGS -> startActivity(
                Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            )

            // 表情面板尚未實作，維持 noop 而不是假裝有效。
            ActionVerb.EMOJI -> Log.i(TAG, "emoji 面板尚未實作")
        }
        syncConfigToUi()
    }

    private fun sendHostKey(keyCode: Int) {
        val ic = currentInputConnection ?: return
        ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, keyCode))
        ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_UP, keyCode))
    }

    /* ─────────────────── 方案切換 ─────────────────── */

    private fun cycleSchema(delta: Int) {
        val list = RimeCore.schemaList()
        if (list.isEmpty()) return
        val cur = list.indexOfFirst { it.id == uiState.status.schemaId }
        val next = ((if (cur < 0) 0 else cur) + delta + list.size) % list.size
        selectSchema(list[next].id)
    }

    /**
     * 切換方案，並依 §9.1.1 連帶換掉鍵盤佈局。
     * 注音方案因此會自動換上 `bopomofo-dachen`，不需要使用者再設定一次。
     */
    private fun selectSchema(schemaId: String) {
        ensureSession()
        if (!RimeCore.selectSchema(session, schemaId)) {
            Log.e(TAG, "切換方案 $schemaId 失敗: ${RimeCore.lastError()}")
            return
        }
        Log.i(TAG, "方案 → $schemaId")
        refreshFromRime(consumeCommit = false)
    }

    private fun fallbackKey(keysym: Int) {
        val ic = currentInputConnection ?: return
        when {
            keysym == AndroidKeyMap.BACKSPACE -> ic.deleteSurroundingText(1, 0)

            keysym == AndroidKeyMap.RETURN -> {
                if (!sendDefaultEditorAction(true)) {
                    ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_ENTER))
                    ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_ENTER))
                }
            }

            // X11 規則：Latin-1 範圍內 keysym == 碼位。
            keysym in 0x20..0xFF -> ic.commitText(keysym.toChar().toString(), 1)

            keysym and 0x01000000 != 0 ->
                ic.commitText(String(Character.toChars(keysym and 0x00FFFFFF)), 1)
        }
    }

    /**
     * 從 rime 取快照並同步到 UI 與 InputConnection。
     *
     * rime_shell.h 明訂「每個輸入事件只 acquire 一次」，且 commit 在 acquire
     * 當下就被消費。所以整條路徑上只有這個函式會取快照，自動確認也遞迴回
     * 這裡而不是另外開一次 acquire —— 否則待讀取的 commit_text 會被吃掉。
     */
    private fun refreshFromRime(
        consumeCommit: Boolean = true,
        allowAutoCommit: Boolean = false,
    ) {
        val ic = currentInputConnection
        val snapshot = RimeCore.snapshot(session)

        if (snapshot == null) {
            uiState = uiState.copy(
                preedit = "",
                candidates = emptyList(),
                highlighted = -1,
                isStub = RimeCore.isStub(),
                theme = layoutHost.theme,
                layout = layoutHost.layout,
                layerId = layoutHost.layerId,
                configProblem = layoutHost.problem,
            )
            return
        }

        if (consumeCommit) {
            snapshot.commitText?.takeIf { it.isNotEmpty() }?.let { text ->
                ic?.commitText(text, 1)
            }
        }

        // 「選字」不等於「上屏」。判別條件是 menu.count，見 rime_shell.h：
        //   count > 0               → 還有段落待選，繼續選，不要 commit
        //   count == 0 && composing → 轉換完成待確認，呼叫 rs_commit_composition
        //   count == 0 && !composing→ 已經結束
        if (allowAutoCommit &&
            snapshot.status.isComposing &&
            snapshot.menu.candidates.isEmpty()
        ) {
            Log.i(TAG, "自動確認：count=0 且仍在組字，呼叫 rs_commit_composition")
            if (RimeCore.commitComposition(session)) {
                refreshFromRime(consumeCommit = true, allowAutoCommit = false)
                return
            }
        }

        // 組字串以 composing text 呈現，讓宿主應用看得到未上屏的內容。
        if (ic != null) {
            if (snapshot.composition.preedit.isNotEmpty()) {
                ic.setComposingText(snapshot.composition.preedit, 1)
            } else {
                ic.finishComposingText()
            }
        }

        // §9.1.1：方案變了就換佈局。放在這裡而不是 selectSchema，
        // 是因為方案也可能由 librime 自己切（例如 schema 的 switcher）。
        val schemaId = snapshot.status.schemaId
        if (schemaId.isNotEmpty() && schemaId != lastSchemaId) {
            lastSchemaId = schemaId
            layoutHost.applySchema(schemaId)
            Log.i(TAG, "方案 $schemaId → 佈局 ${layoutHost.layout?.id}")
        }

        uiState = uiState.copy(
            preedit = snapshot.composition.preedit,
            candidates = snapshot.menu.candidates,
            highlighted = snapshot.menu.highlighted,
            pageNo = snapshot.menu.pageNo,
            isLastPage = snapshot.menu.isLastPage,
            status = snapshot.status,
            isStub = RimeCore.isStub(),
            theme = layoutHost.theme,
            layout = layoutHost.layout,
            layerId = layoutHost.layerId,
            configProblem = layoutHost.problem,
            schemas = if (uiState.schemas.isEmpty()) RimeCore.schemaList() else uiState.schemas,
        )
    }
}
