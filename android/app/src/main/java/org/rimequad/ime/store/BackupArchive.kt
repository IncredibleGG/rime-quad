package org.rimequad.ime.store

import java.io.File
import java.io.OutputStream
import java.security.MessageDigest
import java.util.zip.Deflater
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream

/**
 * 備份容器的讀寫。**只用 `java.*`，不碰 Android**，所以整段在 JVM 單元測試裡跑得動。
 *
 * ── 為什麼不直接用 [ArchiveGuard] ───────────────────────────────────────
 * 那個檔案守的是**方案套件**：副檔名白名單（yaml / txt / lua…）、
 * 不接受 `.bin`。備份裡裝的是 LevelDB 的 `CURRENT`、`MANIFEST-000002`、
 * `000003.log`、`000005.ldb` —— 一個都不會通過那份白名單。
 *
 * 所以這裡換一套**更嚴**的模型，而不是把白名單放寬：
 *
 *   1. **manifest 驅動。** 只有 `rimequad-backup.json` 裡列出來的路徑會被
 *      解出來。容器裡多出來的東西一律不落地（也不報錯 —— 那是未來版本
 *      新增的東西，前向相容）。副檔名因此完全不必管：能不能寫出來，
 *      由「我們自己寫的清單上有沒有它」決定。
 *   2. **逐檔 sha256。** manifest 記的摘要與實際解出來的內容必須相符。
 *      傳輸途中被截斷、被雲端硬碟「最佳化」過、被人動過手腳，都會在
 *      這一關現形，而不是變成一個開不起來的詞庫。
 *   3. 路徑穿越與符號連結仍然照 [ArchiveGuard] 那一套走 —— 那兩條與內容
 *      無關，是 zip 本身的攻擊面，重寫一次只會多一份會分岔的程式碼。
 *      符號連結一樣自己讀 central directory（`java.util.zip.ZipEntry`
 *      拿不到 external file attributes，見 [ZipCentralDirectory] 的說明）。
 */
object BackupArchive {

    /** 整包解壓後的上限。備份主要是詞庫與 yaml，256MB 已經非常寬鬆。 */
    const val MAX_TOTAL_BYTES = 256L * 1024 * 1024

    /** 單一檔案上限。 */
    const val MAX_ENTRY_BYTES = 64L * 1024 * 1024

    /** 路徑檢查沿用套件那一套，只放寬深度：`dict/x.userdb/000003.log` 有三層。 */
    private val PATH_LIMITS = ArchiveLimits(maxDepth = 4)

    /* ═══════════════════════════ 寫 ═══════════════════════════ */

    /**
     * 打包。
     *
     * ⚠ **摘要是在寫入的同時算的，不是事先算好再寫。** 事先算會留下一個
     * 空窗：來源檔案在「算完摘要」與「真的寫進去」之間被改動（LevelDB
     * 隨時可能壓實），產生的備份就會自己對不起自己 —— 而且要等到使用者
     * 匯入時才會被 sha256 抓到，那時已經來不及了。
     *
     * 因此 manifest 是**最後**一個 entry。讀取端用 `ZipFile`（讀 central
     * directory），entry 的順序對它沒有影響。
     */
    fun pack(
        entries: List<BackupPlan.Entry>,
        out: OutputStream,
        manifestOf: (List<BackupFile>) -> BackupManifest,
    ): BackupManifest {
        val written = ArrayList<BackupFile>(entries.size)
        val zip = ZipOutputStream(out)
        zip.setLevel(Deflater.BEST_SPEED)   // 詞庫的 .ldb 本來就壓過了，慢慢壓不划算
        return zip.use { z ->
            for (e in entries) {
                require(BackupFormat.isAllowedEntry(e.path)) {
                    "備份項目 ${e.path} 不在允許的前綴底下 —— 這是程式錯誤，不是使用者的檔案問題"
                }
                z.putNextEntry(ZipEntry(e.path))
                val digest = MessageDigest.getInstance("SHA-256")
                var size = 0L
                e.source.inputStream().use { input ->
                    val buf = ByteArray(64 * 1024)
                    while (true) {
                        val n = input.read(buf)
                        if (n < 0) break
                        digest.update(buf, 0, n)
                        z.write(buf, 0, n)
                        size += n
                    }
                }
                z.closeEntry()
                written += BackupFile(e.path, size, digest.digest().toHex())
            }

            val manifest = manifestOf(written)
            z.putNextEntry(ZipEntry(BackupFormat.MANIFEST_NAME))
            z.write(BackupManifestJson.encode(manifest).toByteArray(Charsets.UTF_8))
            z.closeEntry()
            manifest
        }
    }

    /* ═══════════════════════════ 讀 ═══════════════════════════ */

    /**
     * 讀 manifest 並做版本判定。**任何失敗都是一個 [BackupIssue]，不丟例外。**
     *
     * 使用者拿來的檔案可能是：別的 app 的 zip、下載到一半的檔案、
     * 一張照片被改了副檔名、或是新版 App 寫出來的備份。四種要給四種說法。
     */
    fun readManifest(file: File): BackupManifestJson.Result<BackupManifest> {
        val text = try {
            ZipFile(file).use { zf ->
                val entry = zf.getEntry(BackupFormat.MANIFEST_NAME)
                    ?: return BackupManifestJson.Result.failure(
                        BackupIssue(BackupProblem.NOT_A_BACKUP, listOf(file.name))
                    )
                if (entry.size > 4L * 1024 * 1024) {
                    return BackupManifestJson.Result.failure(
                        BackupIssue(BackupProblem.MANIFEST_BROKEN, listOf("too large"))
                    )
                }
                zf.getInputStream(entry).use { it.readBytes().toString(Charsets.UTF_8) }
            }
        } catch (e: Exception) {
            // 不是 zip、中央目錄壞掉、檔案被截斷 —— 對使用者而言都是同一件事。
            return BackupManifestJson.Result.failure(
                BackupIssue(BackupProblem.NOT_A_BACKUP, listOf(e.message ?: "$e"))
            )
        }
        return BackupManifestJson.decode(text)
    }

    sealed class Extract {
        data class Ok(val staging: File, val files: List<String>, val bytes: Long) : Extract()
        data class Err(val issue: BackupIssue) : Extract()
    }

    /**
     * 把 manifest 列出來的每一個檔案解到 [stagingDir]，逐檔驗 sha256。
     *
     * **全有全無**：任何一項不合格就整包拒絕並清掉暫存，一個位元組都不會
     * 進到使用者的資料目錄。理由與 [ArchiveGuard.extract] 相同 ——
     * 半套的檔案不是「少一點功能」，一個寫到一半的 `MANIFEST` 會讓
     * LevelDB 從此開不起來。
     */
    fun extract(file: File, manifest: BackupManifest, stagingDir: File): Extract {
        if (manifest.isEmpty) {
            return Extract.Err(BackupIssue(BackupProblem.EMPTY))
        }

        // 符號連結只能從 central directory 的 external attributes 看得出來。
        val cd = try {
            ZipCentralDirectory.read(file)
        } catch (e: Exception) {
            return Extract.Err(BackupIssue(BackupProblem.NOT_A_BACKUP, listOf(e.message ?: "$e")))
        }
        cd.entries.firstOrNull { it.isSymlink }?.let {
            return Extract.Err(BackupIssue(BackupProblem.UNSAFE_PATH, listOf(it.name)))
        }

        var declared = 0L
        for (f in manifest.files) {
            if (!BackupFormat.isAllowedEntry(f.path)) {
                return Extract.Err(BackupIssue(BackupProblem.UNSAFE_PATH, listOf(f.path)))
            }
            ArchiveGuard.pathProblemOf(f.path, PATH_LIMITS)?.let {
                return Extract.Err(BackupIssue(BackupProblem.UNSAFE_PATH, listOf(f.path)))
            }
            if (f.size > MAX_ENTRY_BYTES) {
                return Extract.Err(BackupIssue(BackupProblem.UNSAFE_PATH, listOf(f.path)))
            }
            declared += maxOf(f.size, 0L)
        }
        if (declared > MAX_TOTAL_BYTES) {
            return Extract.Err(BackupIssue(BackupProblem.UNSAFE_PATH, listOf("total")))
        }

        if (!stagingDir.exists() && !stagingDir.mkdirs()) {
            return Extract.Err(BackupIssue(BackupProblem.IO, listOf(stagingDir.name)))
        }
        val root = stagingDir.canonicalFile
        val names = ArrayList<String>(manifest.files.size)
        var total = 0L

        try {
            ZipFile(file).use { zf ->
                for (f in manifest.files) {
                    val ze = zf.getEntry(f.path)
                        ?: return fail(stagingDir, BackupProblem.MISSING_ENTRY, f.path)

                    val target = File(root, f.path).canonicalFile
                    // 第二道路徑檢查，機制與第一道獨立（字串比對 vs 正規化）。
                    if (!ArchiveGuard.isInside(target, root)) {
                        return fail(stagingDir, BackupProblem.UNSAFE_PATH, f.path)
                    }
                    target.parentFile?.mkdirs()

                    val digest = MessageDigest.getInstance("SHA-256")
                    var size = 0L
                    zf.getInputStream(ze).use { input ->
                        target.outputStream().use { o ->
                            val buf = ByteArray(64 * 1024)
                            while (true) {
                                val n = input.read(buf)
                                if (n < 0) break
                                size += n
                                total += n
                                // 宣告的大小會說謊，實際位元組才算數（同 ArchiveGuard）。
                                if (size > MAX_ENTRY_BYTES || total > MAX_TOTAL_BYTES) {
                                    o.flush()
                                    return fail(stagingDir, BackupProblem.UNSAFE_PATH, f.path)
                                }
                                digest.update(buf, 0, n)
                                o.write(buf, 0, n)
                            }
                        }
                    }
                    val actual = digest.digest().toHex()
                    if (actual != f.sha256) {
                        return fail(stagingDir, BackupProblem.CONTENT_MISMATCH, f.path)
                    }
                    names += f.path
                }
            }
        } catch (e: Exception) {
            stagingDir.deleteRecursively()
            return Extract.Err(BackupIssue(BackupProblem.IO, listOf(e.message ?: "$e")))
        }

        return Extract.Ok(stagingDir, names, total)
    }

    private fun fail(staging: File, problem: BackupProblem, arg: String): Extract.Err {
        staging.deleteRecursively()
        return Extract.Err(BackupIssue(problem, listOf(arg)))
    }

    /* ── 摘要 ── */

    internal fun ByteArray.toHex(): String {
        val chars = "0123456789abcdef"
        val sb = StringBuilder(size * 2)
        for (b in this) {
            val v = b.toInt() and 0xFF
            sb.append(chars[v ushr 4]).append(chars[v and 0x0F])
        }
        return sb.toString()
    }
}
