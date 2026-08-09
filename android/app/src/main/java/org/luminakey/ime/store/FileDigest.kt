package org.luminakey.ime.store

import java.io.File
import java.security.MessageDigest

/**
 * 本機檔案的 sha256。
 *
 * ── 為什麼單獨一個檔案 ──────────────────────────────────────────────────
 * 這段程式碼原本住在 `store/Downloader.kt` 裡。那個檔案已經整個搬進
 * [org.luminakey.ime.net.NetworkGate] —— 全 app 的連網集中到單一出口，
 * 「grep 一次就能確認沒有第二個出口」是這個專案對使用者的承諾之一。
 *
 * 摘要計算本身**不碰網路**（SAF 匯入的檔案、快取裡等著安裝的 APK 都要算），
 * 硬塞進 NetworkGate 只會讓那個要被外人審計的檔案變長。所以留在這裡，
 * 而且刻意不叫 Downloader —— 一個叫 Downloader 卻不下載的檔案會誤導審計者。
 */
object FileDigest {

    fun sha256(file: File): String {
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
