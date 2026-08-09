package org.luminakey.ime.core

import android.util.Log
import org.luminakey.ime.theme.Modifiers
import java.util.concurrent.CopyOnWriteArrayList

/**
 * `rime_shell.h` 的 Kotlin 門面。
 *
 * ── 執行緒約定（照抄 rime_shell.h）────────────────────────────────────
 * 除 [deploy] 外，同一 session 的所有呼叫必須序列化在同一條執行緒上，
 * 行動端即 IME 主執行緒。不要從動畫／背景執行緒呼叫。
 *
 * ── 記憶體約定 ────────────────────────────────────────────────────────
 * [snapshot] 在 JNI 內完成 acquire → 複製 → release，回傳的
 * [RimeSnapshot] 是純 Kotlin 值物件，可以安全長期持有。
 *
 * ── 類別名 ────────────────────────────────────────────────────────────
 * 原生端以 RegisterNatives 綁定，類別名由 gradle.properties 的
 * `rime.jniClass` 決定。**改動本類別的套件或名稱時，必須同步改那個屬性。**
 */
object RimeCore {

    private const val TAG = "RimeCore"

    /** 上層編譯期認定的 ABI 版本，必須與 `RIME_SHELL_ABI_VERSION` 一致。 */
    const val EXPECTED_ABI_VERSION: Int = 3

    /** so 載入結果。載入失敗時整個 IME 應該降級而不是崩潰。 */
    @Volatile
    var libraryLoaded: Boolean = false
        private set

    @Volatile
    var libraryLoadError: String? = null
        private set

    @Volatile
    private var initialized: Boolean = false

    private val deployListeners = CopyOnWriteArrayList<(RimeDeployStatus) -> Unit>()

    @Volatile
    var lastDeployStatus: RimeDeployStatus = RimeDeployStatus.IDLE
        private set

    init {
        try {
            System.loadLibrary("rime_jni")
            libraryLoaded = true
        } catch (t: Throwable) {
            libraryLoaded = false
            libraryLoadError = t.message ?: t.toString()
            Log.e(TAG, "載入 librime_jni.so 失敗", t)
        }
    }

    /* ───────────────── 版本與模式 ───────────────── */

    /** 執行期 so 回報的 ABI 版本；so 沒載到時為 -1。 */
    fun abiVersion(): Int = if (libraryLoaded) nativeAbiVersion() else -1

    /** so 編譯時的 `RIME_SHELL_ABI_VERSION`。 */
    fun compiledAbiVersion(): Int = if (libraryLoaded) nativeExpectedAbiVersion() else -1

    /** true 代表目前連的是 `rime_shell_stub.cc`，候選字全是假的。 */
    fun isStub(): Boolean = if (libraryLoaded) nativeIsStub() else true

    /** 啟動時的版本閘門，見 rime_shell.h 對 ABI 版本的說明。 */
    fun abiCompatible(): Boolean = libraryLoaded && abiVersion() == EXPECTED_ABI_VERSION

    /* ───────────────── 生命週期 ───────────────── */

    val isInitialized: Boolean get() = initialized

    @Synchronized
    fun init(
        userDataDir: String,
        sharedDataDir: String,
        logDir: String?,
        appName: String,
    ): Boolean {
        if (!libraryLoaded) return false
        if (initialized) return true
        if (!abiCompatible()) {
            Log.e(
                TAG,
                "ABI 不相符：so=${abiVersion()} 上層=$EXPECTED_ABI_VERSION，拒絕初始化",
            )
            return false
        }
        initialized = nativeInit(userDataDir, sharedDataDir, logDir, appName)
        if (!initialized) Log.e(TAG, "rs_init 失敗: ${lastError()}")
        return initialized
    }

    @Synchronized
    fun finalizeRime() {
        if (!libraryLoaded || !initialized) return
        nativeFinalize()
        initialized = false
    }

    /** rime_shell.h 中唯一允許跨執行緒呼叫的函式。 */
    fun deploy(): Boolean = if (libraryLoaded && initialized) nativeDeploy() else false

    fun lastError(): String = if (libraryLoaded) nativeLastError() else (libraryLoadError ?: "")

    fun addDeployListener(listener: (RimeDeployStatus) -> Unit) {
        deployListeners.add(listener)
        listener(lastDeployStatus)
    }

    fun removeDeployListener(listener: (RimeDeployStatus) -> Unit) {
        deployListeners.remove(listener)
    }

    /**
     * 由原生 `rs_deploy_callback` 回呼。可能來自任意執行緒，
     * 監聽者自行負責切到需要的執行緒。
     */
    @JvmStatic
    private fun onDeployStatusFromNative(status: Int) {
        val s = RimeDeployStatus.fromNative(status)
        lastDeployStatus = s
        Log.i(TAG, "部署狀態: $s")
        deployListeners.forEach { runCatching { it(s) } }
    }

    /* ───────────────── Session ───────────────── */

    fun sessionCreate(): Long =
        if (libraryLoaded && initialized) nativeSessionCreate() else INVALID_SESSION

    fun sessionDestroy(session: Long) {
        if (libraryLoaded && session != INVALID_SESSION) nativeSessionDestroy(session)
    }

    fun sessionAlive(session: Long): Boolean =
        libraryLoaded && session != INVALID_SESSION && nativeSessionAlive(session)

    /* ───────────────── 輸入 ───────────────── */

    fun processKey(session: Long, keysym: Int, modifiers: Int = Modifiers.NONE): Boolean =
        libraryLoaded && session != INVALID_SESSION && nativeProcessKey(session, keysym, modifiers)

    fun selectCandidate(session: Long, indexOnPage: Int): Boolean =
        libraryLoaded && session != INVALID_SESSION && nativeSelectCandidate(session, indexOnPage)

    fun deleteCandidate(session: Long, indexOnPage: Int): Boolean =
        libraryLoaded && session != INVALID_SESSION && nativeDeleteCandidate(session, indexOnPage)

    fun changePage(session: Long, backward: Boolean): Boolean =
        libraryLoaded && session != INVALID_SESSION && nativeChangePage(session, backward)

    fun clearComposition(session: Long): Boolean =
        libraryLoaded && session != INVALID_SESSION && nativeClearComposition(session)

    /**
     * 把目前組字內容直接上屏。
     *
     * 「選字」不等於「上屏」：拼音類方案選字當下就 commit，注音類方案選字後
     * 仍停留在組字狀態，必須明確呼叫本函式。回傳 true 代表下一次 [snapshot]
     * 的 commitText 會有內容。
     */
    fun commitComposition(session: Long): Boolean =
        libraryLoaded && session != INVALID_SESSION && nativeCommitComposition(session)

    /* ───────────────── 輸入串的直接改寫（九宮格音節消歧）───────────────── */

    /**
     * 直接改寫「引擎目前正在打的那一串」，並讓它重新切分、重新算候選。
     *
     * ── 為什麼九宮格非用它不可 ─────────────────────────────────────────
     * 一顆鍵三四個字母，`MG` 可能是 ni 也可能是 mi。使用者點了 ni 之後，引擎
     * 必須知道「第一個音節確定是 ni，後面仍然模糊」。librime **沒有**「選擇
     * 某個拼寫」的 API（上游作者明講過，rime/librime#123）：那是前端的工作，
     * 做法就是把輸入串改寫成 `ni` + 剩下的模糊碼。
     *
     * [selectCandidate] 做不到 —— 它確定的是**字**，不是**音節**。
     *
     * ⚠ 這是「重打一次」，不是「附加」。新字串必須整串落在方案的
     *   `speller/alphabet` 裡，否則引擎會靜靜地丟掉不認得的部分。回傳 false
     *   代表引擎拒絕了，**呼叫端必須當成沒發生**，不可以照樣更新畫面。
     */
    fun setInput(session: Long, input: String): Boolean =
        libraryLoaded && session != INVALID_SESSION && nativeSetInput(session, input)

    /**
     * 引擎目前持有的輸入串。
     *
     * ⚠ **不是 preedit。** preedit 是給人看的，speller 可能已經插入分隔符
     * （本方案的 `delimiter` 是空白與 `'`）或做過轉寫；拿 preedit 去餵
     * [setInput] 會把那些顯示用的字元一起寫回引擎。
     */
    fun getInput(session: Long): String =
        if (libraryLoaded && session != INVALID_SESSION) nativeGetInput(session) else ""

    /* ───────────────── 快照 ───────────────── */

    /**
     * 取得 UI 狀態快照。session 失效時回傳 null（部署後舊 session 會失效，
     * 呼叫端應據此重建 session）。
     */
    @Suppress("UNCHECKED_CAST")
    fun snapshot(session: Long): RimeSnapshot? {
        if (!libraryLoaded || session == INVALID_SESSION) return null
        val raw = nativeSnapshot(session) ?: return null
        if (raw.size < 2) return null
        val ints = raw[0] as? IntArray ?: return null
        val strs = raw[1] as? Array<*> ?: return null
        return RimeSnapshot.decode(ints, Array(strs.size) { strs[it] as? String })
    }

    /* ───────────────── Schema 與選項 ───────────────── */

    fun schemaList(): List<RimeSchema> {
        if (!libraryLoaded || !initialized) return emptyList()
        val flat = nativeSchemaList() ?: return emptyList()
        return (0 until flat.size / 2).map { RimeSchema(flat[it * 2], flat[it * 2 + 1]) }
    }

    fun selectSchema(session: Long, schemaId: String): Boolean =
        libraryLoaded && session != INVALID_SESSION && nativeSelectSchema(session, schemaId)

    fun setOption(session: Long, option: String, value: Boolean): Boolean =
        libraryLoaded && session != INVALID_SESSION && nativeSetOption(session, option, value)

    fun getOption(session: Long, option: String): Boolean =
        libraryLoaded && session != INVALID_SESSION && nativeGetOption(session, option)

    const val INVALID_SESSION: Long = 0L

    /* ───────────────── native 宣告 ─────────────────
     * 名稱與簽章必須與 jni_bridge.cc 的 kMethods 表逐項對應。
     */

    private external fun nativeAbiVersion(): Int
    private external fun nativeExpectedAbiVersion(): Int
    private external fun nativeIsStub(): Boolean
    private external fun nativeInit(
        userDataDir: String,
        sharedDataDir: String,
        logDir: String?,
        appName: String,
    ): Boolean

    private external fun nativeFinalize()
    private external fun nativeDeploy(): Boolean
    private external fun nativeLastError(): String
    private external fun nativeSessionCreate(): Long
    private external fun nativeSessionDestroy(session: Long)
    private external fun nativeSessionAlive(session: Long): Boolean
    private external fun nativeProcessKey(session: Long, keysym: Int, modifiers: Int): Boolean
    private external fun nativeSelectCandidate(session: Long, index: Int): Boolean
    private external fun nativeDeleteCandidate(session: Long, index: Int): Boolean
    private external fun nativeChangePage(session: Long, backward: Boolean): Boolean
    private external fun nativeClearComposition(session: Long): Boolean
    private external fun nativeCommitComposition(session: Long): Boolean
    private external fun nativeSetInput(session: Long, input: String): Boolean
    private external fun nativeGetInput(session: Long): String
    private external fun nativeSnapshot(session: Long): Array<Any?>?
    private external fun nativeSchemaList(): Array<String>?
    private external fun nativeSelectSchema(session: Long, schemaId: String): Boolean
    private external fun nativeSetOption(session: Long, option: String, value: Boolean): Boolean
    private external fun nativeGetOption(session: Long, option: String): Boolean
}
