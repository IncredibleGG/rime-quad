package org.rimequad.ime

import android.inputmethodservice.InputMethodService
import android.util.Log
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.EditorInfo
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
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
 * 這裡所有 RimeCore 呼叫都發生在 IME 主執行緒（Compose 事件回呼也在主執行緒），
 * 符合約定。唯一的例外是 rs_deploy 的回呼，那是原生端主動打上來的。
 */
class RimeInputMethodService : InputMethodService() {

    private companion object {
        const val TAG = "RimeIME"
    }

    private var session: Long = RimeCore.INVALID_SESSION
    private var host: ComposeKeyboardHost? = null

    private var uiState by mutableStateOf(KeyboardUiState())

    override fun onCreate() {
        super.onCreate()
        val ok = RimeRuntime.ensureInitialized(applicationContext)
        if (ok) {
            session = RimeCore.sessionCreate()
        }
        uiState = uiState.copy(
            isStub = RimeCore.isStub(),
            fatalMessage = when {
                !ok -> RimeRuntime.initError ?: "rime 初始化失敗"
                session == RimeCore.INVALID_SESSION -> "無法建立 rime session: ${RimeCore.lastError()}"
                else -> null
            },
        )
        Log.i(TAG, "onCreate: init=$ok session=$session stub=${RimeCore.isStub()}")
    }

    override fun onCreateInputView(): View {
        host?.onDestroy()
        val newHost = ComposeKeyboardHost(this)
        host = newHost
        return newHost.createView {
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
        host?.onDestroy()
        host = null
        if (session != RimeCore.INVALID_SESSION) {
            RimeCore.sessionDestroy(session)
            session = RimeCore.INVALID_SESSION
        }
        super.onDestroy()
    }

    /** 部署後舊 session 會失效（見 rime_shell.h），這裡負責重建。 */
    private fun ensureSession() {
        if (!RimeRuntime.isInitialized) return
        if (!RimeCore.sessionAlive(session)) {
            session = RimeCore.sessionCreate()
            Log.i(TAG, "session 已失效，重建為 $session")
        }
    }

    /* ─────────────────── 事件處理 ─────────────────── */

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
                refreshFromRime()
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
     * [RimeCore.snapshot] 在 JNI 內就完成了 acquire → 複製 → release，
     * 這裡拿到的已經是純 Kotlin 值物件，可以安全存進 Compose state。
     */
    private fun refreshFromRime(consumeCommit: Boolean = true) {
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
