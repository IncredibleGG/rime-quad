package org.rimequad.ime

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
import org.rimequad.ime.core.RimeKeysym
import org.rimequad.ime.core.RimeModifier
import org.rimequad.ime.core.RimeRuntime
import org.rimequad.ime.keyboard.KeyLayer
import org.rimequad.ime.keyboard.KeyType
import org.rimequad.ime.keyboard.KeyboardEvent
import org.rimequad.ime.keyboard.KeyboardUiState
import org.rimequad.ime.keyboard.RimeKeyboard
import org.rimequad.ime.ui.RimeTheme

/**
 * IME 本體。
 *
 * 執行緒：rime_shell.h 要求同一 session 的呼叫序列化在同一條執行緒上。
 * 本類別所有 RimeCore 呼叫都發生在 IME 主執行緒 —— Compose 事件回呼、
 * onKeyDown、以及 RimeRuntime 切回主執行緒後的 phase 回呼，全都是主執行緒。
 */
class RimeInputMethodService : InputMethodService() {

    private companion object {
        const val TAG = "RimeIME"
    }

    private var session: Long = RimeCore.INVALID_SESSION
    private var host: ComposeKeyboardHost? = null

    private var uiState by mutableStateOf(KeyboardUiState())

    /** onKeyDown 消費掉的 keycode，onKeyUp 要對應吃掉，否則宿主會收到落單的 up。 */
    private val consumedKeys = HashSet<Int>()

    private val phaseListener: (RimeRuntime.Phase) -> Unit = { phase -> onPhase(phase) }

    override fun onCreate() {
        super.onCreate()
        uiState = uiState.copy(isStub = RimeCore.isStub())
        RimeRuntime.addListener(phaseListener)
        // 解壓在背景執行緒，rs_init 之後才切回主執行緒，這裡不會阻塞。
        RimeRuntime.start(applicationContext)
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
        host?.onFinishInput()
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
                uiState = uiState.copy(busyMessage = null, fatalMessage = null)
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
            is KeyboardEvent.ToggleShift -> {
                uiState = uiState.copy(shift = !uiState.shift)
                return
            }

            is KeyboardEvent.ToggleLayer -> {
                uiState = uiState.copy(
                    layer = if (uiState.layer == KeyLayer.LETTERS) {
                        KeyLayer.SYMBOLS
                    } else {
                        KeyLayer.LETTERS
                    },
                )
                return
            }

            is KeyboardEvent.ToggleLanguage -> {
                val next = !uiState.asciiMode
                RimeCore.setOption(session, "ascii_mode", next)
                uiState = uiState.copy(asciiMode = next)
                refreshFromRime()
                return
            }

            is KeyboardEvent.Candidate -> {
                RimeCore.selectCandidate(session, event.indexOnPage)
                refreshFromRime(allowAutoCommit = true)
                return
            }

            is KeyboardEvent.CandidateLongPress -> {
                RimeCore.deleteCandidate(session, event.indexOnPage)
                refreshFromRime()
                return
            }

            is KeyboardEvent.Page -> {
                RimeCore.changePage(session, event.backward)
                refreshFromRime()
                return
            }

            is KeyboardEvent.Key -> handleKey(event)
        }
    }

    private fun handleKey(event: KeyboardEvent.Key) {
        val key = event.key
        val shift = uiState.shift
        val keysym = key.keysymFor(shift)
        val modifiers = if (shift) RimeModifier.SHIFT else RimeModifier.NONE

        ensureSession()
        val consumed = RimeCore.processKey(session, keysym, modifiers)

        if (!consumed) {
            // rime 沒吃下去 → 由宿主自行處理，維持一般鍵盤的行為。
            fallbackKey(key.type, keysym, shift)
        }

        // 一次性 shift：出一個字後自動彈回。
        val nextShift = if (key.type == KeyType.NORMAL && shift) false else shift
        if (nextShift != shift) uiState = uiState.copy(shift = nextShift)

        refreshFromRime()
    }

    private fun fallbackKey(type: KeyType, keysym: Int, shift: Boolean) {
        val ic = currentInputConnection ?: return
        when {
            keysym == RimeKeysym.BACKSPACE -> {
                ic.deleteSurroundingText(1, 0)
            }

            keysym == RimeKeysym.RETURN -> {
                if (!sendDefaultEditorAction(true)) {
                    ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_ENTER))
                    ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_ENTER))
                }
            }

            type == KeyType.SPACE -> ic.commitText(" ", 1)

            keysym in 0x20..0x7E -> {
                val c = keysym.toChar()
                ic.commitText((if (shift) c.uppercaseChar() else c).toString(), 1)
            }
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

        uiState = uiState.copy(
            preedit = snapshot.composition.preedit,
            candidates = snapshot.menu.candidates,
            highlighted = snapshot.menu.highlighted,
            pageNo = snapshot.menu.pageNo,
            isLastPage = snapshot.menu.isLastPage,
            schemaName = snapshot.status.schemaName,
            asciiMode = snapshot.status.isAsciiMode,
            isStub = RimeCore.isStub(),
        )
    }
}
