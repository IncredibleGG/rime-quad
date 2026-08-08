package org.rimequad.ime.net

import androidx.annotation.StringRes
import org.rimequad.ime.R
import java.io.File

/**
 * 連網紀錄的資料模型與本機儲存。**沒有任何 Android 相依**，所以整份邏輯
 * 都由 JVM 單元測試守著（見 NetworkLogTest）。
 *
 * ── 這一頁為什麼值得做 ──────────────────────────────────────────────────
 * 「相信我們不連網」是一句沒有辦法驗證的話。這份紀錄把它換成「你自己看」：
 * 每一次真的建立的連線都留一筆，開關從沒開過就是空的。很多標榜隱私的 app
 * 都沒有這一頁，因為它會讓自己被抓包 —— 那正是它的價值。
 *
 * ── 刻意不記錄什麼 ──────────────────────────────────────────────────────
 *   · **使用者輸入的任何內容**。一個字都不記。輸入內容不會離開 librime，
 *     也不會流進 [NetworkGate]。
 *   · URL 的**路徑與查詢字串**。只留主機名 + 我們自己給的原因標籤。
 *     為了「透明」而把使用者行為的細節寫進本機檔案，是自打嘴巴。
 *   · 被開關擋下來的嘗試。那不是一次連網，記了會讓「沒開過就是空的」
 *     這句話不成立。
 */

/**
 * 這一次連網是為了什麼。全部由呼叫端在編譯期指定，沒有自由字串。
 *
 * ── 為什麼同時帶 [zh] 與 [labelRes] ─────────────────────────────────────
 * 兩個消費端，語言需求相反：
 *   · [labelRes] 給**畫面**（「連網」頁的每一筆紀錄）。跟著系統語言走。
 *   · [zh] 給 [NetworkGate] 擋下連線時丟出的**例外訊息**。那句話會進 logcat
 *     與問題回報，讀者是我們，不是使用者；而且它必須在沒有 Context 的地方
 *     組得出來 —— NetworkGate 是純邏輯，拿不到資源。
 * 兩者不會漂移，因為它們描述的是同一組編譯期常數，加一個列舉值時編譯器會
 * 逼你兩個都填。
 */
enum class NetworkPurpose(val zh: String, @get:StringRes val labelRes: Int) {
    STORE_INDEX("瀏覽方案市集（取索引）", R.string.network_purpose_store_index),
    STORE_PACKAGE("下載方案套件", R.string.network_purpose_store_package),
    UPDATE_MANIFEST("檢查更新（取版本資訊）", R.string.network_purpose_update_manifest),
    UPDATE_APK("下載更新（APK）", R.string.network_purpose_update_apk),
    ;

    companion object {
        fun of(name: String): NetworkPurpose? = entries.firstOrNull { it.name == name }
    }
}

/** 結果。轉址單獨一種：它既不是成功也不是失敗，但它是一次真的連線。 */
enum class NetworkOutcome(val zh: String, @get:StringRes val labelRes: Int) {
    OK("成功", R.string.network_outcome_ok),
    FAILED("失敗", R.string.network_outcome_failed),
    REDIRECTED("轉址", R.string.network_outcome_redirected),
    ;

    companion object {
        fun of(name: String): NetworkOutcome? = entries.firstOrNull { it.name == name }
    }
}

/**
 * 一次連線。
 *
 * @param atMillis  發生時間（epoch 毫秒）
 * @param host      連到哪一台主機。**不含路徑與查詢字串。**
 * @param purpose   為什麼連
 * @param label     人看得懂的補充，例如套件名稱。可為空字串。
 * @param outcome   結果
 * @param bytes     實際傳輸的位元組數（只有 [NetworkOutcome.OK] 有意義）
 * @param detail    一句話的補充，例如 `HTTP 404`。可為空字串。
 */
data class NetworkLogEntry(
    val atMillis: Long,
    val host: String,
    val purpose: NetworkPurpose,
    val label: String,
    val outcome: NetworkOutcome,
    val bytes: Long,
    val detail: String,
) {
    /**
     * 「下載方案套件：萬象」。
     *
     * ⚠ 這一版**只給診斷與測試用**，畫面上那一行由
     * `NetworkScreen` 用 [NetworkPurpose.labelRes] 現組（見上面的說明）。
     */
    val reasonText: String
        get() = purpose.zh + if (label.isEmpty()) "" else "：$label"
}

/**
 * 檔案格式的編解碼。
 *
 * 用 TSV 而不是 JSON：這份檔案的目的是「使用者（或任何人）能自己驗證」，
 * `adb shell run-as ... cat` 出來要一眼看得懂。專案裡的 [MiniJson] 只做讀，
 * 為了這件事再寫一個 writer 不划算。
 *
 * 欄位裡的 tab 與換行一律換成空白 —— 遠端可以控制 host（轉址目的地），
 * 所以它是**不可信輸入**，不能讓它把一行拆成兩行。
 */
object NetworkLogCodec {

    private const val SEP = '\t'
    const val MAX_HOST = 120
    const val MAX_LABEL = 80
    const val MAX_DETAIL = 120

    fun encode(e: NetworkLogEntry): String = listOf(
        e.atMillis.toString(),
        clean(e.host, MAX_HOST),
        e.purpose.name,
        clean(e.label, MAX_LABEL),
        e.outcome.name,
        e.bytes.coerceAtLeast(0).toString(),
        clean(e.detail, MAX_DETAIL),
    ).joinToString(SEP.toString())

    /** 解不開的行一律回 null 並被丟掉 —— 一行壞資料不該把整份紀錄拖垮。 */
    fun decode(line: String): NetworkLogEntry? {
        val f = line.split(SEP)
        if (f.size != 7) return null
        val at = f[0].toLongOrNull() ?: return null
        val purpose = NetworkPurpose.of(f[2]) ?: return null
        val outcome = NetworkOutcome.of(f[4]) ?: return null
        val bytes = f[5].toLongOrNull() ?: return null
        return NetworkLogEntry(at, f[1], purpose, f[3], outcome, bytes, f[6])
    }

    private fun clean(s: String, max: Int): String =
        s.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ').trim().take(max)
}

/**
 * 紀錄的本機檔案。附加寫入、有上限、可清除。
 *
 * ── 為什麼有上限 ────────────────────────────────────────────────────────
 * 沒有上限的日誌檔遲早會變成一個沒人注意的、慢慢長大的檔案。而且對這一頁
 * 而言，一千筆以前的紀錄沒有人會捲到。[maxEntries] 到了就丟最舊的。
 *
 * ── 為什麼整份重寫而不是真的 append ─────────────────────────────────────
 * 因為有上限，滿了就得裁頭；而且整份檔案最多幾十 KB，重寫的成本遠低於
 * 維護一份「什麼時候該壓縮」的邏輯。寫入先寫暫存檔再 rename，
 * 中途斷電不會留下半行。
 */
class NetworkLogStore(
    private val file: File,
    private val maxEntries: Int = DEFAULT_MAX_ENTRIES,
) {

    /** 由舊到新。讀不到檔案（還沒有任何連網）就是空清單。 */
    @Synchronized
    fun read(): List<NetworkLogEntry> {
        if (!file.isFile) return emptyList()
        return try {
            file.readLines(Charsets.UTF_8).mapNotNull { line ->
                if (line.isBlank()) null else NetworkLogCodec.decode(line)
            }
        } catch (e: Exception) {
            emptyList()
        }
    }

    /** 回傳寫入後的完整內容（由舊到新），呼叫端可以直接拿去更新 UI。 */
    @Synchronized
    fun append(entry: NetworkLogEntry): List<NetworkLogEntry> {
        val next = (read() + entry).let {
            if (it.size > maxEntries) it.subList(it.size - maxEntries, it.size) else it
        }
        writeAll(next)
        return next
    }

    @Synchronized
    fun clear() {
        try {
            file.delete()
        } catch (e: Exception) {
            // 刪不掉就算了，下一次寫入會蓋掉它。
        }
    }

    private fun writeAll(entries: List<NetworkLogEntry>) {
        try {
            file.parentFile?.mkdirs()
            val tmp = File(file.parentFile, file.name + ".tmp")
            tmp.writeText(
                entries.joinToString("\n") { NetworkLogCodec.encode(it) } + "\n",
                Charsets.UTF_8,
            )
            if (!tmp.renameTo(file)) {
                // 有些檔案系統上 rename 到既存檔會失敗，退一步直接覆寫。
                file.writeText(tmp.readText(Charsets.UTF_8), Charsets.UTF_8)
                tmp.delete()
            }
        } catch (e: Exception) {
            // 寫不進去不該讓下載失敗（見 NetworkGate.record 的 catch）。
        }
    }

    companion object {
        const val DEFAULT_MAX_ENTRIES = 500
    }
}
