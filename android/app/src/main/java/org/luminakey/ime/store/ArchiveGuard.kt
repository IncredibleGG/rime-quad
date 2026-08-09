package org.luminakey.ime.store

import org.luminakey.ime.R
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.RandomAccessFile
import java.util.zip.ZipFile

/**
 * ArchiveGuard —— zip 解壓前的安全檢查（docs/schema-store.md §4）。
 *
 * 這個檔案的每一行都在假設「zip 是攻擊者給的」。市集下載的套件經過 sha256
 * 驗證，但**使用者自帶的 zip 沒有任何憑據**，而兩者走同一條解壓路徑，
 * 所以檢查一律以最壞情況為準。
 *
 * ── 為什麼自己讀中央目錄，而不是只用 ZipInputStream ──────────────────────
 * zip 有兩份中繼資料：每個檔案前面的 local file header，以及檔案尾端的
 * central directory。**兩者可以不一致**，而且解壓工具通常只信其中一份。
 * `ZipInputStream` 讀的是 local header，`ZipFile` 讀的是 central directory。
 * 若檢查讀 local header、解壓讀 central directory，攻擊者只要讓兩份不一致
 * 就能繞過檢查。
 *
 * 因此本模組：
 *   · 檢查階段自己解析 **central directory**（[ZipCentralDirectory]）；
 *   · 解壓階段用 `java.util.zip.ZipFile`，它同樣以 central directory 為準；
 *   · 兩邊看的是同一份中繼資料，不存在「檢查的和解壓的不是同一個東西」。
 *
 * 另一個必須自己解析的理由：`java.util.zip.ZipEntry` **沒有暴露 external file
 * attributes**，而 Unix 的符號連結旗標就藏在那裡（mode & 0xF000 == 0xA000）。
 * 少了它就無法實作規範 §4 第 2 條。
 *
 * ── 宣告的大小會說謊 ────────────────────────────────────────────────────
 * central directory 裡的 uncompressed size 是攻擊者可控的欄位。所以解壓炸彈
 * 的防線有兩層：先用宣告值擋掉明目張膽的，再在**實際複製位元組時**硬性
 * 計數，超過上限立刻中止並刪除半成品。只做前者等於沒做。
 */

/** 各項上限。全部可覆寫，測試會用極小的值來逼出邊界行為。 */
data class ArchiveLimits(
    /** entry 數量上限。RIME 方案最多幾十個檔案，2000 已經非常寬鬆。 */
    val maxEntries: Int = 2_000,
    /** 單一檔案解壓後的位元組上限。最大的合法檔案是語言模型，約 6MB。 */
    val maxEntryBytes: Long = 64L * 1024 * 1024,
    /** 整包解壓後的位元組上限。 */
    val maxTotalBytes: Long = 256L * 1024 * 1024,
    /**
     * 壓縮比上限（解壓後 / 壓縮前）。zip 炸彈的本質就是極高的壓縮比；
     * 文字類詞典大約 3–5 倍，200 倍留了非常大的餘裕。
     * 只在壓縮前大小 >= [ratioFloorBytes] 時才判定，避免小檔誤判。
     */
    val maxCompressionRatio: Long = 200,
    val ratioFloorBytes: Long = 4 * 1024,
    /** 路徑長度與目錄深度上限。 */
    val maxPathLength: Int = 255,
    val maxDepth: Int = 4,
    /**
     * 副檔名白名單（小寫，不含點）。
     *
     * `bin` 刻意**不在**白名單裡：`.table.bin` / `.prism.bin` / `.reverse.bin`
     * 是 librime 部署時自己產生的，套件夾帶它們沒有意義，而且是最適合藏
     * 二進位酬載的位置。
     */
    val allowedExtensions: Set<String> = setOf(
        "yaml", "yml",   // schema / dict / 配置
        "txt",           // essay.txt、UPSTREAM.txt
        "ocd2",          // opencc 詞典（規範說不該打包，但夾帶了也不是安全問題）
        "gram",          // octagram 語言模型
        "json",          // opencc 設定
        "md",            // 說明文件
        // lua：librime 已編入 librime-lua 外掛（見 third_party/prebuilt/
        // manifest.json 的 plugins.lua），雾凇拼音、萬象等現代方案沒有 lua/
        // 就只是一個「部署成功卻打不出字」的空殼。
        //
        // 這一條**確實是在放行會被執行的程式碼**，所以其他防線一項都不能少：
        // 市集下載的套件有 sha256 與伺服器側的部署＋輸入探針驗證；使用者自帶的
        // zip 沒有這些憑據，但 lua 是跑在 librime 自己的沙盒 Lua state 裡，
        // 權限不高於同一包 zip 裡的 yaml 能造成的影響（兩者都能改變輸入行為）。
        // 路徑穿越、符號連結、解壓炸彈這幾條照樣擋在前面。
        "lua",
    ),
    /** 無副檔名但允許的檔名（比對時區分大小寫，這些是慣例全大寫）。 */
    val allowedBareNames: Set<String> = setOf(
        "LICENSE", "LICENCE", "COPYING", "NOTICE", "README", "AUTHORS", "CHANGELOG",
    ),
)

/** 一項拒絕理由。[entry] 為 null 代表問題出在整個檔案而不是某個 entry。 */
data class ArchiveRejection(
    val kind: Kind,
    val entry: String?,
    val detail: String,
) {
    enum class Kind {
        MALFORMED,          // 根本不是合法 zip
        PATH_TRAVERSAL,     // §4.1
        SYMLINK,            // §4.2
        ZIP_BOMB,           // §4.3
        EXTENSION,          // §4.4
        EMPTY,
    }

    override fun toString(): String =
        if (entry == null) "[$kind] $detail" else "[$kind] $entry: $detail"

    /**
     * 給使用者看的一行說明。
     *
     * ⚠ 回傳 [UiMessage]（資源 id + 參數）而不是字串：這些話會直接畫在
     * 匯入失敗的對話框上，而預設語系是英文。理由見 [UiMessage] 的檔頭。
     * [detail] 是英文的故障載荷，當參數帶出去。
     */
    fun uiMessage(): UiMessage = when (kind) {
        Kind.MALFORMED -> UiMessage.of(R.string.archive_err_malformed, detail)
        Kind.PATH_TRAVERSAL -> UiMessage.of(R.string.archive_err_path, entry.orEmpty())
        Kind.SYMLINK -> UiMessage.of(R.string.archive_err_symlink, entry.orEmpty())
        Kind.ZIP_BOMB -> UiMessage.of(R.string.archive_err_bomb, detail)
        Kind.EXTENSION -> UiMessage.of(R.string.archive_err_extension, entry.orEmpty(), detail)
        Kind.EMPTY -> UiMessage(R.string.archive_err_empty)
    }
}

/** 通過檢查的一項。[name] 已保證是安全的相對路徑。 */
data class SafeEntry(
    val name: String,
    val compressedSize: Long,
    val uncompressedSize: Long,
)

data class ArchiveReport(
    val entries: List<SafeEntry>,
    val rejections: List<ArchiveRejection>,
) {
    val isSafe: Boolean get() = rejections.isEmpty()
    val totalUncompressed: Long get() = entries.sumOf { it.uncompressedSize }
}

sealed class ExtractResult {
    data class Ok(val files: List<String>, val bytes: Long) : ExtractResult()
    data class Rejected(val report: ArchiveReport) : ExtractResult()
    data class Failed(val message: String) : ExtractResult()
}

object ArchiveGuard {

    /**
     * 只檢查、不寫任何檔案。
     *
     * 一旦有任何一項不合格就整包拒絕（規範用語：「整包拒絕」），但仍會把
     * 找到的**所有**問題列出來 —— 使用者自帶的檔案通常不只錯一處，
     * 一次只報一個會讓人來回好幾趟。
     */
    fun inspect(file: File, limits: ArchiveLimits = ArchiveLimits()): ArchiveReport {
        val cd = try {
            ZipCentralDirectory.read(file)
        } catch (e: Exception) {
            return ArchiveReport(
                emptyList(),
                listOf(ArchiveRejection(ArchiveRejection.Kind.MALFORMED, null, e.message ?: "$e")),
            )
        }

        val rejections = ArrayList<ArchiveRejection>()
        val safe = ArrayList<SafeEntry>()

        if (cd.entries.size > limits.maxEntries) {
            rejections.add(
                ArchiveRejection(
                    ArchiveRejection.Kind.ZIP_BOMB, null,
                    "entry count ${cd.entries.size} exceeds the limit of ${limits.maxEntries}",
                )
            )
        }

        var total = 0L
        for (e in cd.entries) {
            // ── §4.1 路徑穿越 ──────────────────────────────────────────
            val pathProblem = pathProblemOf(e.name, limits)
            if (pathProblem != null) {
                rejections.add(
                    ArchiveRejection(ArchiveRejection.Kind.PATH_TRAVERSAL, e.name, pathProblem)
                )
                continue
            }

            // ── §4.2 符號連結 ──────────────────────────────────────────
            if (e.isSymlink) {
                rejections.add(
                    ArchiveRejection(
                        ArchiveRejection.Kind.SYMLINK, e.name,
                        "unix mode in external attributes is " +
                            "0${e.unixMode.toString(8)} (S_IFLNK)",
                    )
                )
                continue
            }

            if (e.isDirectory) continue

            // ── §4.4 副檔名白名單 ──────────────────────────────────────
            val extProblem = extensionProblemOf(e.name, limits)
            if (extProblem != null) {
                rejections.add(ArchiveRejection(ArchiveRejection.Kind.EXTENSION, e.name, extProblem))
                continue
            }

            // ── §4.3 解壓炸彈（宣告值這一層）──────────────────────────
            if (e.uncompressedSize > limits.maxEntryBytes) {
                rejections.add(
                    ArchiveRejection(
                        ArchiveRejection.Kind.ZIP_BOMB, e.name,
                        "declared uncompressed size ${e.uncompressedSize} exceeds the " +
                            "per-entry limit of ${limits.maxEntryBytes}",
                    )
                )
                continue
            }
            if (e.compressedSize >= limits.ratioFloorBytes && e.compressedSize > 0) {
                val ratio = e.uncompressedSize / e.compressedSize
                if (ratio > limits.maxCompressionRatio) {
                    rejections.add(
                        ArchiveRejection(
                            ArchiveRejection.Kind.ZIP_BOMB, e.name,
                            "compression ratio ${ratio}:1 exceeds the limit of " +
                                "${limits.maxCompressionRatio}:1",
                        )
                    )
                    continue
                }
            }

            total += e.uncompressedSize
            if (total > limits.maxTotalBytes) {
                rejections.add(
                    ArchiveRejection(
                        ArchiveRejection.Kind.ZIP_BOMB, null,
                        "declared uncompressed total exceeds the limit of ${limits.maxTotalBytes}",
                    )
                )
                break
            }

            safe.add(SafeEntry(e.name, e.compressedSize, e.uncompressedSize))
        }

        if (rejections.isEmpty() && safe.isEmpty()) {
            rejections.add(ArchiveRejection(ArchiveRejection.Kind.EMPTY, null, "no entry passed the checks"))
        }
        return ArchiveReport(safe, rejections)
    }

    /**
     * 檢查通過才解壓。
     *
     * 解壓先寫進 [stagingParent] 底下的暫存目錄，全部成功之後才搬進
     * [targetDir]。理由是規範 §3 的回滾：半套的檔案雖然「無害」，但
     * 一個寫到一半被截斷的 dict.yaml 會讓之後每次部署都失敗，那就不無害了。
     */
    fun extract(
        zip: File,
        targetDir: File,
        stagingParent: File,
        limits: ArchiveLimits = ArchiveLimits(),
    ): ExtractResult {
        val report = inspect(zip, limits)
        if (!report.isSafe) return ExtractResult.Rejected(report)

        val staging = File(stagingParent, "unzip-${System.nanoTime()}")
        if (!staging.mkdirs()) return ExtractResult.Failed("could not create the staging directory $staging")
        val stagingRoot = staging.canonicalFile

        var written = 0L
        val names = ArrayList<String>(report.entries.size)
        try {
            ZipFile(zip).use { zf ->
                for (entry in report.entries) {
                    val ze = zf.getEntry(entry.name)
                        ?: return ExtractResult.Failed(
                            "central directory lists ${entry.name} but ZipFile cannot open it"
                        )
                    val out = File(stagingRoot, entry.name)

                    // 第二道路徑檢查。前面已經用字串比對擋過一次，這裡再用
                    // canonical path 確認一次 —— 兩種機制互相獨立，
                    // 任一種被繞過另一種還在。
                    val canonical = out.canonicalFile
                    if (!isInside(canonical, stagingRoot)) {
                        return ExtractResult.Rejected(
                            ArchiveReport(
                                emptyList(),
                                listOf(
                                    ArchiveRejection(
                                        ArchiveRejection.Kind.PATH_TRAVERSAL, entry.name,
                                        "normalises to $canonical, which is outside $stagingRoot",
                                    )
                                ),
                            )
                        )
                    }
                    canonical.parentFile?.mkdirs()

                    zf.getInputStream(ze).use { input ->
                        written += copyCapped(
                            input, canonical,
                            entryCap = limits.maxEntryBytes,
                            totalRemaining = limits.maxTotalBytes - written,
                            name = entry.name,
                        )
                    }
                    names.add(entry.name)
                }
            }
        } catch (e: ZipBombException) {
            staging.deleteRecursively()
            return ExtractResult.Rejected(
                ArchiveReport(
                    emptyList(),
                    listOf(ArchiveRejection(ArchiveRejection.Kind.ZIP_BOMB, e.entry, e.message ?: "")),
                )
            )
        } catch (e: Exception) {
            staging.deleteRecursively()
            return ExtractResult.Failed(e.message ?: "$e")
        }

        // 全部寫成功了才搬進使用者資料目錄。
        try {
            if (!targetDir.exists() && !targetDir.mkdirs()) {
                throw IOException("could not create $targetDir")
            }
            for (name in names) {
                val src = File(stagingRoot, name)
                val dst = File(targetDir, name)
                dst.parentFile?.mkdirs()
                if (dst.exists()) dst.delete()
                if (!src.renameTo(dst)) {
                    src.copyTo(dst, overwrite = true)
                    src.delete()
                }
            }
        } catch (e: Exception) {
            return ExtractResult.Failed("could not move into ${targetDir.name}: ${e.message}")
        } finally {
            staging.deleteRecursively()
        }

        return ExtractResult.Ok(names, written)
    }

    /* ───────────────────────── 個別檢查 ───────────────────────── */

    private class ZipBombException(val entry: String, message: String) : IOException(message)

    /**
     * 解壓時的硬性計數。central directory 宣告的大小是攻擊者可控的，
     * 這裡只信實際讀到的位元組數。
     */
    private fun copyCapped(
        input: InputStream,
        dst: File,
        entryCap: Long,
        totalRemaining: Long,
        name: String,
    ): Long {
        val buf = ByteArray(64 * 1024)
        var n = 0L
        dst.outputStream().use { out ->
            while (true) {
                val read = input.read(buf)
                if (read < 0) break
                n += read
                if (n > entryCap) {
                    out.flush()
                    dst.delete()
                    throw ZipBombException(
                        name, "$name inflates past the per-entry limit $entryCap " +
                            "(the declared size lied)"
                    )
                }
                if (n > totalRemaining) {
                    out.flush()
                    dst.delete()
                    throw ZipBombException(name, "inflated total exceeds the limit (the declared sizes lied)")
                }
                out.write(buf, 0, read)
            }
        }
        return n
    }

    /** 回傳問題描述，null 代表沒問題。**這是 §4.1 的主要防線。** */
    internal fun pathProblemOf(rawName: String, limits: ArchiveLimits): String? {
        if (rawName.isEmpty()) return "entry name is empty"
        if (rawName.length > limits.maxPathLength) {
            return "path length ${rawName.length} exceeds the limit of ${limits.maxPathLength}"
        }
        if (rawName.any { it.code < 0x20 || it.code == 0x7F }) return "path contains control characters"
        // Windows 的分隔符。java.io.File 在 Linux 上不把 '\' 當分隔符，
        // 但 zip 規格說名稱一律用 '/'，出現 '\' 本身就可疑。
        if (rawName.contains('\\')) return "path contains a backslash"
        if (rawName.startsWith("/")) return "path starts with / (absolute)"
        if (rawName.length >= 2 && rawName[1] == ':' && rawName[0].isLetter()) {
            return "path contains a drive letter (absolute)"
        }

        val segments = rawName.split('/')
        // 尾端的空段是目錄標記（"foo/"），合法；其餘位置的空段代表 "//"。
        segments.forEachIndexed { idx, seg ->
            when {
                seg == ".." -> return "path contains a .. segment"
                seg == "." -> return "path contains a . segment"
                seg.isEmpty() && idx != segments.lastIndex -> return "path contains consecutive slashes"
                seg.endsWith(" ") || seg.endsWith(".") ->
                    return "path segment '$seg' ends with a space or a dot"
            }
        }
        val depth = segments.count { it.isNotEmpty() }
        if (depth > limits.maxDepth) return "directory depth $depth exceeds the limit of ${limits.maxDepth}"
        return null
    }

    internal fun extensionProblemOf(name: String, limits: ArchiveLimits): String? {
        val base = name.substringAfterLast('/')
        if (base.isEmpty()) return "file name is empty"
        if (base.startsWith(".")) return "hidden files (names starting with a dot) are not accepted"
        val dot = base.lastIndexOf('.')
        if (dot < 0) {
            return if (base in limits.allowedBareNames) null
            else "no extension and not in the allowed bare names " +
                    "(${limits.allowedBareNames.joinToString()})"
        }
        val ext = base.substring(dot + 1).lowercase()
        if (ext in limits.allowedExtensions) return null
        return "extension .$ext is not on the allowlist " +
            "(allowed: ${limits.allowedExtensions.sorted().joinToString()})"
    }

    internal fun isInside(candidate: File, root: File): Boolean {
        val c = candidate.path
        val r = root.path
        return c == r || c.startsWith(if (r.endsWith(File.separator)) r else r + File.separator)
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * 中央目錄讀取器
 *
 * 只讀我們需要的欄位。刻意不支援 ZIP64：本專案的上限是 256MB，會用到
 * ZIP64 的套件不是打包出了問題就是有意為之，兩種都該擋。
 * ═════════════════════════════════════════════════════════════════════════ */

internal class ZipCentralDirectory(val entries: List<Entry>) {

    class Entry(
        val name: String,
        val compressedSize: Long,
        val uncompressedSize: Long,
        val unixMode: Int,
        val hostOs: Int,
    ) {
        val isDirectory: Boolean
            get() = name.endsWith("/") || (hostOs == HOST_UNIX && (unixMode and S_IFMT) == S_IFDIR)

        /** Unix 的 S_IFLNK。非 Unix 打包的 zip 沒有這個概念，一律視為非連結。 */
        val isSymlink: Boolean
            get() = hostOs == HOST_UNIX && (unixMode and S_IFMT) == S_IFLNK
    }

    companion object {
        private const val EOCD_SIG = 0x06054b50
        private const val CD_SIG = 0x02014b50
        private const val ZIP64_LOCATOR_SIG = 0x07064b50
        private const val MAX_COMMENT = 0xFFFF

        const val HOST_UNIX = 3
        const val S_IFMT = 0xF000
        const val S_IFLNK = 0xA000
        const val S_IFDIR = 0x4000

        fun read(file: File): ZipCentralDirectory {
            if (!file.isFile) throw IOException("file does not exist: $file")
            val len = file.length()
            if (len < 22) throw IOException("file is too small to be a zip (${len} bytes)")

            RandomAccessFile(file, "r").use { raf ->
                val tailLen = minOf(len, (MAX_COMMENT + 22).toLong()).toInt()
                val tailStart = len - tailLen
                val tail = ByteArray(tailLen)
                raf.seek(tailStart)
                raf.readFully(tail)

                var eocd = -1
                for (i in tailLen - 22 downTo 0) {
                    if (u32(tail, i) == EOCD_SIG.toLong()) { eocd = i; break }
                }
                if (eocd < 0) throw IOException("no End Of Central Directory record — not a zip file")

                if (eocd >= 20 && u32(tail, eocd - 20) == ZIP64_LOCATOR_SIG.toLong()) {
                    throw IOException("ZIP64 is not supported (no package here should be that large)")
                }

                val count = u16(tail, eocd + 10)
                val cdSize = u32(tail, eocd + 12)
                val cdOffset = u32(tail, eocd + 16)
                if (count == 0xFFFF || cdSize == 0xFFFFFFFFL || cdOffset == 0xFFFFFFFFL) {
                    throw IOException("the central directory uses ZIP64 fields, which are not supported")
                }
                if (cdOffset + cdSize > len) throw IOException("the central directory offset is past the end of the file")
                if (cdSize > 16L * 1024 * 1024) throw IOException("the central directory is too large")

                val cd = ByteArray(cdSize.toInt())
                raf.seek(cdOffset)
                raf.readFully(cd)

                val out = ArrayList<Entry>(count)
                var p = 0
                while (p + 46 <= cd.size) {
                    if (u32(cd, p) != CD_SIG.toLong()) break
                    val versionMadeBy = u16(cd, p + 4)
                    val nameLen = u16(cd, p + 28)
                    val extraLen = u16(cd, p + 30)
                    val commentLen = u16(cd, p + 32)
                    val compSize = u32(cd, p + 20)
                    val uncompSize = u32(cd, p + 24)
                    val externalAttrs = u32(cd, p + 38)
                    if (p + 46 + nameLen > cd.size) throw IOException("the central directory is truncated")
                    val name = String(cd, p + 46, nameLen, Charsets.UTF_8)
                    out.add(
                        Entry(
                            name = name,
                            compressedSize = compSize,
                            uncompressedSize = uncompSize,
                            unixMode = ((externalAttrs ushr 16) and 0xFFFF).toInt(),
                            hostOs = (versionMadeBy ushr 8) and 0xFF,
                        )
                    )
                    p += 46 + nameLen + extraLen + commentLen
                }
                if (out.size != count) {
                    throw IOException("the central directory declares $count entries but only ${out.size} were read")
                }
                return ZipCentralDirectory(out)
            }
        }

        private fun u16(b: ByteArray, off: Int): Int {
            if (off + 2 > b.size || off < 0) throw IOException("read past the end of the buffer")
            return (b[off].toInt() and 0xFF) or ((b[off + 1].toInt() and 0xFF) shl 8)
        }

        private fun u32(b: ByteArray, off: Int): Long {
            if (off + 4 > b.size || off < 0) throw IOException("read past the end of the buffer")
            return (b[off].toLong() and 0xFF) or
                ((b[off + 1].toLong() and 0xFF) shl 8) or
                ((b[off + 2].toLong() and 0xFF) shl 16) or
                ((b[off + 3].toLong() and 0xFF) shl 24)
        }
    }
}
