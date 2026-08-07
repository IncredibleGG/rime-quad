package org.rimequad.ime.net

import android.content.Context
import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.rimequad.ime.prefs.PrefsStore
import java.io.File
import java.util.concurrent.Executors

/**
 * 把 [NetworkGate] 這個純邏輯的閘門接到 Android 上：偏好（開關）在哪、
 * 紀錄寫到哪、UI 怎麼看到它。
 *
 * ── 為什麼要分成兩個檔案 ────────────────────────────────────────────────
 * [NetworkGate] 必須是「幾分鐘讀完就能確認」的那一份，所以它裡面不該有
 * DataStore、Compose、Context 這些會把讀者帶去別的地方的東西。接線的
 * 髒活全部在本檔，本檔則刻意沒有任何 `java.net`。
 *
 * ── 安裝時機 ────────────────────────────────────────────────────────────
 * [install] 由 `RimeApp.onCreate` 呼叫，也就是**行程一起來就裝好**，早於
 * 任何 Activity 與 IME 服務。就算它沒被呼叫，[NetworkGate.policy] 的初值
 * 是「拒絕」，行為仍然是完全離線。
 */
object NetworkAudit {

    private val main = Handler(Looper.getMainLooper())
    private val io = Executors.newSingleThreadExecutor { r -> Thread(r, "rime-netlog") }

    @Volatile
    private var store: NetworkLogStore? = null

    @Volatile
    private var installed = false

    /** 紀錄，**由新到舊**（畫面上最新的在最上面）。 */
    var entries by mutableStateOf<List<NetworkLogEntry>>(emptyList())
        private set

    /** 給「連網」分頁顯示的檔案位置 —— 使用者要自己去 adb pull 也拿得到。 */
    @Volatile
    var logFilePath: String = "(尚未初始化)"
        private set

    @Synchronized
    fun install(context: Context) {
        if (installed) return
        installed = true

        val app = context.applicationContext
        val file = File(File(app.filesDir, "net"), "connections.tsv")
        val s = NetworkLogStore(file)
        store = s
        logFilePath = file.absolutePath

        // 開關：每次連線前重新問一次偏好（PrefsStore.current 是同步的快取讀取）。
        // `== true` 不是囉嗦 —— UserPrefs 的 null 代表「使用者沒設定過」，
        // 而這一項未設定的行為必須是**關**。
        val prefs = PrefsStore.get(app)
        NetworkGate.policy = NetworkGate.Policy { prefs.current.networkEnabled == true }

        NetworkGate.recorder = NetworkGate.Recorder { entry ->
            // 呼叫端在 worker 執行緒上，檔案 IO 再丟到自己的執行緒，
            // 免得一次 fsync 卡住正在跑的下載進度回報。
            io.execute {
                val all = s.append(entry)
                main.post { entries = all.asReversed() }
            }
        }

        // 冷啟動時先把既有紀錄讀進來。這一步**不會**連網，只是讀本機檔案。
        io.execute {
            val all = s.read()
            main.post { entries = all.asReversed() }
        }
    }

    fun clear() {
        val s = store ?: return
        io.execute {
            s.clear()
            main.post { entries = emptyList() }
        }
    }

    /** 開關現在是開的嗎。非 Compose 的呼叫端（controller）用這個。 */
    val networkEnabled: Boolean get() = NetworkGate.isEnabled

    /**
     * 改開關。suspend 是因為 [PrefsStore] 的寫入是 suspend 的；
     * 呼叫端一律是 Compose 的 `rememberCoroutineScope()`。
     *
     * @param on true = 開；false = 關。**沒有「回復預設」** —— 這一項的預設
     *   就是關，使用者按「關」與「從沒設定過」在行為上完全相同，多一顆
     *   回復預設的按鈕只會讓人以為還有第三種狀態。
     */
    suspend fun setEnabled(context: Context, on: Boolean) {
        PrefsStore.get(context.applicationContext).update { it.copy(networkEnabled = on) }
    }

    /** 首次說明看過了。只講一次，見 [FirstRunNoticeHost]。 */
    suspend fun markNoticeSeen(context: Context) {
        PrefsStore.get(context.applicationContext).update { it.copy(offlineNoticeSeen = true) }
    }
}
