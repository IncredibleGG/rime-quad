package org.rimequad.ime.net

import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.util.Log
import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * ═══════════════════════════════════════════════════════════════════════════
 *  全 app **唯一**的連網出口。
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * 這個檔案是為了「經得起審計」而存在的。想確認這個輸入法有沒有偷偷連網，
 * 不必讀完整個專案 —— 讀這一個檔案就夠了，理由如下。
 *
 * ── 為什麼光有一顆開關不夠 ──────────────────────────────────────────────
 * `android.permission.INTERNET` 是**安裝時**權限，執行期無法取消。只要 app
 * 帶著方案市集與應用內更新，權限清單上就一定看得到它，**開關關掉也一樣
 * 看得到**。任何一個懷疑我們的人，第一件事就是去查權限，然後發現「你說
 * 你不連網，但你有網路權限」。
 *
 * 所以我們不宣稱「本 app 沒有網路權限」—— 那是假的。我們改成把「你要相信
 * 我們」換成「你自己查」，做法是三層：
 *
 *   1. **單一出口**（本檔）。整個專案只有這裡碰得到 `java.net`。
 *      驗證方式是一行 grep，不必讀程式碼：
 *
 *          grep -rn 'URL(\|HttpURLConnection\|openConnection\|Socket\|OkHttp' \
 *              android/app/src --include=*.kt
 *
 *      結果**必須**只出現在本檔。多一處就是這個定位破功，CI 與 code review
 *      都該把它擋下來。
 *
 *   2. **預設拒絕**（[policy] 的初值是 `{ false }`）。不是「忘了設定就放行」，
 *      而是「沒有人明確安裝過政策就一律拒絕」。安裝政策的地方是
 *      [NetworkAudit.install]，由 `RimeApp.onCreate` 呼叫。就算那一行被
 *      誤刪，行為也是**完全離線**而不是完全開放。
 *
 *   3. **連網紀錄**（[NetworkLog]）。每一次真的建立的 HTTP 連線都會留下
 *      一筆：時間、主機、原因、結果、位元組數。使用者在「連網」分頁自己看。
 *      轉址的每一跳都各記一筆 —— 「它到底連了哪些主機」不能被一次轉址藏起來。
 *
 * ── 這裡**不**記錄什麼 ──────────────────────────────────────────────────
 * 不記錄使用者輸入的任何內容 —— 一個字都不記。輸入內容根本不會離開
 * librime 與鍵盤，也不會進入本檔。紀錄裡連 URL 的**路徑與查詢字串**都不存，
 * 只存主機名加一個我們自己給的原因標籤（例如「下載方案套件：萬象」）。
 * 一個標榜隱私的 app 若為了「透明」而把使用者的行為細節寫進一份本機檔案，
 * 那是自打嘴巴。
 *
 * ── 執行緒 ──────────────────────────────────────────────────────────────
 * 所有函式都是**同步阻塞**的，一律由呼叫端放到背景執行緒上跑
 * （[org.rimequad.ime.store.StoreController] 與
 *  [org.rimequad.ime.update.UpdateController] 各有一條 worker）。
 * 本物件不呼叫任何 `rs_*`，與 rime_shell.h 的執行緒約定無關。
 */
object NetworkGate {

    /**
     * logcat 標籤。
     *
     * 為什麼是**常設**而不是除錯用的臨時程式碼：這讓「開關關掉時真的沒連網」
     * 變成任何人都能在自己的機器上重現的一件事 ——
     *
     *     adb logcat -s NetworkGate
     *
     * 開關關著時去逛市集，會看到一行「拒絕連網（開關關閉）」而**不會**有
     * 任何一行「連線」。這比我們寫在 README 上的保證有用得多。
     *
     * logcat 裡只寫主機名與用途，與連網紀錄同一套規則：不含任何輸入內容。
     */
    private const val TAG = "NetworkGate"

    const val CONNECT_TIMEOUT_MS = 15_000
    const val READ_TIMEOUT_MS = 30_000

    /** 索引是一份清單，不該比這更大。 */
    const val MAX_TEXT_BYTES = 8L * 1024 * 1024

    private const val MAX_REDIRECTS = 5

    /**
     * User-Agent。**刻意不含本專案的名字。**
     *
     * 原本送的是 `rimequad-android`。在有審查的網路環境下那一行就足以讓
     * 使用者被標記為「正在使用這個輸入法」—— 被動觀察者連解密都不必，
     * 明文的 HTTP 請求標頭裡就寫著。使用者可能因為「裝了哪個 app」被關注，
     * 這是我們自己送出去的，不是他選的。
     *
     * ── 為什麼是這個值，而不是拿掉 ──────────────────────────────────────
     * 拿掉不會變成「沒有 User-Agent」：`HttpURLConnection` 會自己補一個
     * `Dalvik/2.1.0 (Linux; U; Android 15; <裝置型號> Build/<版本>)`。
     * 那反而多送了 Android 版本與裝置型號 —— 熵更高，不是更低。
     * 所以送一個**固定、不含任何裝置資訊**的常數值。
     *
     * ── 這不是偽裝成瀏覽器 ──────────────────────────────────────────────
     * 我們沒有要冒充 Chrome：TLS 的握手特徵（JA3）本來就跟瀏覽器不一樣，
     * 硬湊一個瀏覽器 UA 只會讓「UA 與 TLS 指紋對不上」變成新的特徵。
     * 目標只有一個 —— **不要自報家門**。
     *
     * ── 瞞不了的部分 ────────────────────────────────────────────────────
     * DNS 查詢與 TLS 的 SNI 都是明文，網路上的觀察者一定看得到我們連的是
     * 哪個網域。改 User-Agent 改變不了這件事，我們也沒有在說明裡假裝
     * 改得了（見 NetworkUi 的 NetworkRequiredCard 與首次說明）。
     */
    private const val USER_AGENT = "Mozilla/5.0"

    /* ═══════════════════════ 開關與紀錄的接線 ═══════════════════════ */

    /**
     * 「現在可以連網嗎」。**預設拒絕**，見檔頭第 2 點。
     *
     * 刻意是一個函式而不是一個布林欄位：使用者可能在下載進行到一半時把
     * 開關關掉，每次連線前重新問一次才會立刻生效。
     */
    fun interface Policy {
        fun isEnabled(): Boolean
    }

    /** 連網紀錄的接收端。null = 沒接（單元測試的預設狀態）。 */
    fun interface Recorder {
        fun record(entry: NetworkLogEntry)
    }

    @Volatile
    var policy: Policy = Policy { false }

    @Volatile
    var recorder: Recorder? = null

    /**
     * 政策自己爆掉時視為**關閉**。fail-closed：讀不到偏好、DataStore 壞掉、
     * 行程正在被回收 —— 任何一種意外都不該變成「那就連網吧」。
     */
    val isEnabled: Boolean
        get() = try {
            policy.isEnabled()
        } catch (e: Throwable) {
            false
        }

    /**
     * 連網前的閘門。**這是整個定位的兌現處**：開關關閉時這裡直接拋例外，
     * 一個 socket 都不會開。
     */
    @Throws(NetworkDisabledException::class)
    fun requireEnabled(purpose: NetworkPurpose, label: String = "") {
        if (!isEnabled) throw NetworkDisabledException(purpose, label)
    }

    /* ═══════════════════════════ 結果型別 ═══════════════════════════ */

    sealed class Result<out T> {
        data class Ok<T>(val value: T) : Result<T>()

        /**
         * @param blocked true = 被開關擋下來的，不是網路失敗。UI 必須分得出來：
         *   「連不上伺服器」要顯示重試，「開關是關的」要顯示一顆開啟開關的按鈕。
         */
        data class Err(val message: String, val blocked: Boolean = false) : Result<Nothing>()
    }

    data class Downloaded(val file: File, val sha256: String, val bytes: Long)

    /* ═══════════════════════════ 公開 API ═══════════════════════════ */

    /** 取一份文字（索引 index.json、版本資訊 version.json）。 */
    fun fetchText(
        url: String,
        purpose: NetworkPurpose,
        label: String = "",
        maxBytes: Long = MAX_TEXT_BYTES,
    ): Result<String> {
        // 開關關閉時連暫存檔都不要建 —— 什麼都不做才是「離線」。
        blockedOrNull(purpose, label)?.let { return it }

        val tmp = try {
            File.createTempFile("rime-net", ".txt")
        } catch (e: Exception) {
            return Result.Err("無法建立暫存檔：${e.message}")
        }
        try {
            return when (val r = download(url, tmp, maxBytes, purpose, label, null)) {
                is Result.Err -> r
                is Result.Ok -> Result.Ok(tmp.readText(Charsets.UTF_8))
            }
        } catch (e: Exception) {
            return Result.Err(e.message ?: "$e")
        } finally {
            tmp.delete()
        }
    }

    /**
     * 下載到 [dest] 並回報 sha256。**呼叫端負責比對**，因為「該不該接受」
     * 是政策問題，不是傳輸問題。
     *
     * ── sha256 為什麼邊下載邊算 ─────────────────────────────────────────
     * docs/schema-store.md §1：「sha256 行動端**必須**驗證。不符即整包丟棄，
     * **不可先解壓再說**。」摘要計算放在複製迴圈裡，下載結束的同時就知道
     * 結果；不符時檔案立刻刪掉，連 ArchiveGuard 都不會看到它。
     *
     * ── 為什麼還要 maxBytes ────────────────────────────────────────────
     * 索引宣告的 size 也是遠端給的。若遠端（或中間人）送來一個無窮長的回應，
     * 光是「下載完再驗 sha256」就足以塞爆使用者的儲存空間。傳輸本身要有硬牆。
     */
    fun download(
        url: String,
        dest: File,
        maxBytes: Long,
        purpose: NetworkPurpose,
        label: String = "",
        onProgress: ((read: Long, total: Long) -> Unit)? = null,
    ): Result<Downloaded> {
        blockedOrNull(purpose, label)?.let { return it }

        var current = url
        var redirects = 0
        while (true) {
            // 每一跳都重新問一次開關：使用者可能在下載途中把它關掉，
            // 那一下必須真的中斷後續連線，而不是等這一輪跑完。
            blockedOrNull(purpose, label)?.let {
                dest.delete()
                return it
            }

            val parsed = try {
                URL(current)
            } catch (e: Exception) {
                return Result.Err("網址無效：$current")
            }
            val host = parsed.host ?: "?"
            if (parsed.protocol != "http" && parsed.protocol != "https") {
                // 還沒連線，所以不記錄 —— 紀錄裡只該有真的發生過的連線。
                return Result.Err("只支援 http/https，收到 ${parsed.protocol}")
            }

            Log.i(TAG, "連線 $host（${purpose.name}）")
            val conn = try {
                (parsed.openConnection() as HttpURLConnection).apply {
                    connectTimeout = CONNECT_TIMEOUT_MS
                    readTimeout = READ_TIMEOUT_MS
                    requestMethod = "GET"
                    // 跨協定的轉址（https → http 或反之）HttpURLConnection 不會自動跟，
                    // 所以一律關掉自動轉址，自己處理。行為可預期，而且每一跳
                    // 都進得了連網紀錄 —— 交給它自動跟就沒得記了。
                    instanceFollowRedirects = false
                    setRequestProperty("User-Agent", USER_AGENT)
                    setRequestProperty("Accept-Encoding", "identity")
                }
            } catch (e: Exception) {
                record(host, purpose, label, NetworkOutcome.FAILED, 0, "建立連線失敗")
                return Result.Err("連線失敗：${e.message}")
            }

            try {
                val code = conn.responseCode
                if (code in 300..399) {
                    val loc = conn.getHeaderField("Location")
                    if (loc == null) {
                        record(host, purpose, label, NetworkOutcome.FAILED, 0, "HTTP $code 無 Location")
                        return Result.Err("HTTP $code 但沒有 Location 標頭")
                    }
                    if (++redirects > MAX_REDIRECTS) {
                        record(host, purpose, label, NetworkOutcome.FAILED, 0, "轉址過多")
                        return Result.Err("轉址次數過多")
                    }
                    current = URL(parsed, loc).toString()
                    // 轉址也是一次真的連線，而且下一跳可能是完全不同的主機。
                    // 不記的話，紀錄就會漏掉使用者最想知道的那一件事。
                    record(host, purpose, label, NetworkOutcome.REDIRECTED, 0, "HTTP $code → ${hostOf(current)}")
                    continue
                }
                if (code != 200) {
                    record(host, purpose, label, NetworkOutcome.FAILED, 0, "HTTP $code")
                    return Result.Err("HTTP $code ${conn.responseMessage ?: ""}".trim())
                }

                val declared = conn.contentLengthLong
                if (declared in 1..Long.MAX_VALUE && declared > maxBytes) {
                    record(host, purpose, label, NetworkOutcome.FAILED, 0, "宣告大小超過上限")
                    return Result.Err("回應宣告 $declared 位元組，超過上限 $maxBytes")
                }

                dest.parentFile?.mkdirs()
                val digest = MessageDigest.getInstance("SHA-256")
                var read = 0L
                conn.inputStream.use { input ->
                    dest.outputStream().use { out ->
                        val buf = ByteArray(64 * 1024)
                        while (true) {
                            val n = input.read(buf)
                            if (n < 0) break
                            read += n
                            if (read > maxBytes) {
                                dest.delete()
                                record(host, purpose, label, NetworkOutcome.FAILED, read, "超過大小上限，已中止")
                                return Result.Err("下載量超過上限 $maxBytes 位元組，已中止")
                            }
                            digest.update(buf, 0, n)
                            out.write(buf, 0, n)
                            onProgress?.invoke(read, if (declared > 0) declared else -1L)
                        }
                    }
                }
                record(host, purpose, label, NetworkOutcome.OK, read, "")
                return Result.Ok(Downloaded(dest, digest.digest().toHex(), read))
            } catch (e: IOException) {
                dest.delete()
                record(host, purpose, label, NetworkOutcome.FAILED, 0, shortReason(e))
                return Result.Err("下載失敗：${e.message ?: e.toString()}")
            } finally {
                conn.disconnect()
            }
        }
    }

    /* ═══════════════════════ 不連網的小工具 ═══════════════════════ */

    /**
     * 把套件的 `file` 欄位解析成完整 URL。**純字串運算，不connect**，
     * 但仍然放在本檔：`URL(` 這個 token 只要出現在第二個檔案，檔頭那條
     * grep 就失去了「一眼看完」的價值。
     *
     * 規範 §1 說 `base_url` 是絕對網址，但索引也可能被鏡像到別的位置
     * （§5 講的正是「換掉 base_url」）。所以三段回落：
     *   1. `file` 本身就是完整 URL → 直接用
     *   2. 有 `base_url` → 以它為基底解析
     *   3. 都沒有 → 以**索引檔自己的位置**為基底
     */
    fun resolveUrl(indexUrl: String, baseUrl: String?, file: String): String {
        if (file.startsWith("http://") || file.startsWith("https://")) return file
        val base = baseUrl?.takeIf { it.isNotBlank() }
        return try {
            if (base != null) {
                URL(URL(indexUrl), if (base.endsWith("/")) base else "$base/").let {
                    URL(it, file).toString()
                }
            } else {
                URL(URL(indexUrl), file).toString()
            }
        } catch (e: Exception) {
            file
        }
    }

    /** 給紀錄與 UI 用的主機名。解不開時回原字串的前 60 字，不回 null。 */
    fun hostOf(url: String): String = try {
        URL(url).host?.takeIf { it.isNotEmpty() } ?: url.take(60)
    } catch (e: Exception) {
        url.take(60)
    }

    /**
     * 系統目前有沒有可用的網路。
     *
     * ⚠ 這**只讀系統狀態，不送出任何封包**（`ACCESS_NETWORK_STATE` 權限），
     * 所以它不進連網紀錄。放在本檔是為了讓「跟網路有關的東西都在這裡」
     * 這句話沒有例外 —— 審計時最怕的就是「還有一個檔案也在碰網路」。
     *
     * 呼叫端一律先過 [isEnabled] 再問這個。
     */
    fun systemHasNetwork(context: Context): Boolean = try {
        val cm = context.applicationContext
            .getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val net = cm.activeNetwork
        val caps = if (net == null) null else cm.getNetworkCapabilities(net)
        caps != null && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
    } catch (e: Throwable) {
        false
    }

    /* ═══════════════════════════ 內部 ═══════════════════════════ */

    private fun blockedOrNull(purpose: NetworkPurpose, label: String): Result.Err? = try {
        requireEnabled(purpose, label)
        null
    } catch (e: NetworkDisabledException) {
        Log.i(TAG, "拒絕連網（開關關閉）：${purpose.name}")
        // 刻意**不**記進連網紀錄：被擋下來的嘗試不是一次連網。
        // 若把它記進去，「開關從沒開過 → 紀錄是空的」這句話就不成立了，
        // 而那句話正是使用者驗證我們的方式。
        Result.Err(e.message ?: "連網開關是關閉的", blocked = true)
    }

    private fun record(
        host: String,
        purpose: NetworkPurpose,
        label: String,
        outcome: NetworkOutcome,
        bytes: Long,
        detail: String,
    ) {
        val r = recorder ?: return
        try {
            r.record(
                NetworkLogEntry(
                    atMillis = System.currentTimeMillis(),
                    host = host,
                    purpose = purpose,
                    label = label,
                    outcome = outcome,
                    bytes = bytes,
                    detail = detail,
                )
            )
        } catch (e: Throwable) {
            // 記不起來不該讓下載失敗。
        }
    }

    /** 例外訊息可能很長（含堆疊式的巢狀原因），紀錄只留一句。 */
    private fun shortReason(e: Throwable): String =
        (e.message ?: e.javaClass.simpleName).lineSequence().first().take(80)

    private fun ByteArray.toHex(): String {
        val chars = "0123456789abcdef"
        val sb = StringBuilder(size * 2)
        for (b in this) {
            val v = b.toInt() and 0xFF
            sb.append(chars[v ushr 4]).append(chars[v and 0x0F])
        }
        return sb.toString()
    }
}

/**
 * 開關關閉時從 [NetworkGate.requireEnabled] 拋出。
 *
 * 是 `IOException` 的子類別，因為對呼叫端而言「這條路走不通」的處理方式
 * 與網路失敗同一類；但型別分得出來，UI 才能給出正確的下一步
 * （開啟開關，而不是「請檢查你的網路」）。
 */
class NetworkDisabledException(
    val purpose: NetworkPurpose,
    val label: String = "",
) : IOException(
    "連網開關是關閉的，已拒絕：" + purpose.zh + (if (label.isEmpty()) "" else "（$label）")
)
