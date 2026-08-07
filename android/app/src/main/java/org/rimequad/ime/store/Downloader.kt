package org.rimequad.ime.store

import java.io.File
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * 索引與套件的下載。零依賴，`HttpURLConnection` 直上。
 *
 * ── sha256 為什麼邊下載邊算 ─────────────────────────────────────────────
 * 規範 §1：「sha256 行動端**必須**驗證。不符即整包丟棄，**不可先解壓再說**。」
 * 這裡把摘要計算放在複製迴圈裡，下載結束的同時就知道結果；不符時檔案
 * 立刻刪掉，連 [ArchiveGuard] 都不會看到它。
 *
 * ── 為什麼還要 maxBytes ──────────────────────────────────────────────
 * 索引宣告的 size 也是遠端給的。若遠端（或中間人）送來一個無窮長的回應，
 * 光是「下載完再驗 sha256」就足以塞爆使用者的儲存空間。所以下載本身
 * 也要有硬上限。
 */
object Downloader {

    const val CONNECT_TIMEOUT_MS = 15_000
    const val READ_TIMEOUT_MS = 30_000
    private const val MAX_REDIRECTS = 5
    private const val USER_AGENT = "rimequad-android"

    sealed class Result<out T> {
        data class Ok<T>(val value: T) : Result<T>()
        data class Err(val message: String) : Result<Nothing>()
    }

    /** 取文字（索引檔）。上限 8MB —— 索引是一份清單，不該比這更大。 */
    fun fetchText(url: String, maxBytes: Long = 8L * 1024 * 1024): Result<String> {
        val tmp = File.createTempFile("rime-index", ".json")
        try {
            return when (val r = download(url, tmp, maxBytes, null)) {
                is Result.Err -> r
                is Result.Ok -> Result.Ok(tmp.readText(Charsets.UTF_8))
            }
        } catch (e: Exception) {
            return Result.Err(e.message ?: "$e")
        } finally {
            tmp.delete()
        }
    }

    data class Downloaded(val file: File, val sha256: String, val bytes: Long)

    /**
     * 下載到 [dest] 並回報 sha256。**呼叫端負責比對**，因為「該不該接受」
     * 是政策問題，不是傳輸問題。
     */
    fun download(
        url: String,
        dest: File,
        maxBytes: Long,
        onProgress: ((read: Long, total: Long) -> Unit)?,
    ): Result<Downloaded> {
        var current = url
        var redirects = 0
        while (true) {
            val parsed = try {
                URL(current)
            } catch (e: Exception) {
                return Result.Err("網址無效：$current")
            }
            if (parsed.protocol != "http" && parsed.protocol != "https") {
                return Result.Err("只支援 http/https，收到 ${parsed.protocol}")
            }

            val conn = try {
                (parsed.openConnection() as HttpURLConnection).apply {
                    connectTimeout = CONNECT_TIMEOUT_MS
                    readTimeout = READ_TIMEOUT_MS
                    requestMethod = "GET"
                    // 跨協定的轉址（https → http 或反之）HttpURLConnection 不會自動跟，
                    // 所以一律關掉自動轉址，自己處理，行為才可預期。
                    instanceFollowRedirects = false
                    setRequestProperty("User-Agent", USER_AGENT)
                    setRequestProperty("Accept-Encoding", "identity")
                }
            } catch (e: Exception) {
                return Result.Err("連線失敗：${e.message}")
            }

            try {
                val code = conn.responseCode
                if (code in 300..399) {
                    val loc = conn.getHeaderField("Location")
                        ?: return Result.Err("HTTP $code 但沒有 Location 標頭")
                    if (++redirects > MAX_REDIRECTS) return Result.Err("轉址次數過多")
                    current = URL(parsed, loc).toString()
                    continue
                }
                if (code != 200) {
                    return Result.Err("HTTP $code ${conn.responseMessage ?: ""}".trim())
                }

                val declared = conn.contentLengthLong
                if (declared in 1..Long.MAX_VALUE && declared > maxBytes) {
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
                                return Result.Err("下載量超過上限 $maxBytes 位元組，已中止")
                            }
                            digest.update(buf, 0, n)
                            out.write(buf, 0, n)
                            onProgress?.invoke(read, if (declared > 0) declared else -1L)
                        }
                    }
                }
                return Result.Ok(Downloaded(dest, digest.digest().toHex(), read))
            } catch (e: IOException) {
                dest.delete()
                return Result.Err("下載失敗：${e.message ?: e.toString()}")
            } finally {
                conn.disconnect()
            }
        }
    }

    /** 供本機檔案（SAF 匯入）計算摘要用。 */
    fun sha256Of(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buf = ByteArray(64 * 1024)
            while (true) {
                val n = input.read(buf)
                if (n < 0) break
                digest.update(buf, 0, n)
            }
        }
        return digest.digest().toHex()
    }

    /**
     * 把套件的 `file` 欄位解析成完整 URL。
     *
     * 規範 §1 說 `base_url` 是絕對網址，但索引也可能被鏡像到別的位置
     * （§5 講的正是「換掉 base_url」這件事）。所以三段回落：
     *   1. `file` 本身就是完整 URL → 直接用
     *   2. 有 `base_url` → 以它為基底解析
     *   3. 都沒有 → 以**索引檔自己的位置**為基底
     * 第 3 條讓「把整個目錄搬到別的主機」不必改索引內容，本機測試也靠它。
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
