package org.luminakey.ime.store

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/**
 * 規範 §4 的三道安全檢查（路徑穿越 / 符號連結 / 解壓炸彈）加副檔名白名單。
 *
 * 這些測試的斷言刻意分成兩層：
 *   1. **有沒有被拒絕**（report.isSafe == false，且拒絕理由的種類正確）
 *   2. **有沒有真的沒寫出去**（目標目錄之外一個檔案都不能多）
 * 只驗第 1 層是不夠的：拒絕了但已經寫了一半，跟沒擋一樣糟。
 */
class ArchiveGuardTest {

    @get:Rule
    val tmp = TemporaryFolder()

    private fun dirs(): Triple<File, File, File> {
        val root = tmp.newFolder("root")
        val target = File(root, "user").apply { mkdirs() }
        val staging = File(root, "cache").apply { mkdirs() }
        return Triple(root, target, staging)
    }

    /** 目標目錄之外（root 底下、user 以外）有沒有多出東西。 */
    private fun strayFiles(root: File, target: File, staging: File): List<String> =
        root.walkTopDown()
            .filter { it.isFile }
            .filterNot { it.absolutePath.startsWith(target.absolutePath + File.separator) }
            .filterNot { it.absolutePath.startsWith(staging.absolutePath + File.separator) }
            .map { it.absolutePath }
            .toList()

    /* ───────────────────── §4.1 路徑穿越 ───────────────────── */

    @Test
    fun `含 dotdot 的 entry 整包拒絕且不寫出任何檔案`() {
        val (root, target, staging) = dirs()
        val zip = StoreFixtures.malicious("evil-traversal.zip")

        val report = ArchiveGuard.inspect(zip)
        assertFalse("含 ../../ 的 zip 必須被拒絕", report.isSafe)
        assertTrue(report.rejections.any { it.kind == ArchiveRejection.Kind.PATH_TRAVERSAL })

        val result = ArchiveGuard.extract(zip, target, staging)
        assertTrue("必須是 Rejected", result is ExtractResult.Rejected)
        assertEquals("目標目錄不該有任何檔案", 0, target.listFiles()?.size ?: 0)
        assertEquals("目標目錄之外不該多出檔案", emptyList<String>(), strayFiles(root, target, staging))
        // 這是這條測試真正的重點：`../../evil.yaml` 若沒被擋，會落在 root 的上兩層。
        assertFalse(File(root.parentFile, "evil.yaml").exists())
        assertFalse(File(root, "evil.yaml").exists())
    }

    @Test
    fun `絕對路徑的 entry 被拒絕`() {
        val (_, target, staging) = dirs()
        val zip = StoreFixtures.malicious("evil-absolute.zip")
        val report = ArchiveGuard.inspect(zip)
        assertFalse(report.isSafe)
        assertTrue(report.rejections.any { it.kind == ArchiveRejection.Kind.PATH_TRAVERSAL })
        assertTrue(ArchiveGuard.extract(zip, target, staging) is ExtractResult.Rejected)
        assertFalse(File("/tmp/evil-absolute.yaml").exists())
    }

    @Test
    fun `各種穿越寫法都認得出來`() {
        val limits = ArchiveLimits()
        val bad = listOf(
            "../evil.yaml",
            "../../evil.yaml",
            "a/../../evil.yaml",
            "/etc/passwd",
            "//etc/passwd",
            "C:/windows/system32/evil.yaml",
            "..\\..\\evil.yaml",
            "dir\\evil.yaml",
            "a//b.yaml",
            "./evil.yaml",
            "a/./b.yaml",
            "evil.yaml\u0000.txt",
            "a/b/c/d/e/f/deep.yaml",
        )
        for (name in bad) {
            assertNotNull("「$name」應該被判定為不安全", ArchiveGuard.pathProblemOf(name, limits))
        }
        val good = listOf("luna_pinyin.schema.yaml", "opencc/t2s.json", "LICENSE", "UPSTREAM.txt")
        for (name in good) {
            assertNull("「$name」應該是安全的", ArchiveGuard.pathProblemOf(name, limits))
        }
    }

    /* ───────────────────── §4.2 符號連結 ───────────────────── */

    @Test
    fun `zip 內的符號連結被拒絕`() {
        val (root, target, staging) = dirs()
        val zip = StoreFixtures.malicious("evil-symlink.zip")

        val report = ArchiveGuard.inspect(zip)
        assertFalse(report.isSafe)
        val sym = report.rejections.firstOrNull { it.kind == ArchiveRejection.Kind.SYMLINK }
        assertNotNull("必須以 SYMLINK 為由拒絕，而不是靠副檔名誤打誤撞", sym)
        assertEquals("passwd.yaml", sym!!.entry)

        assertTrue(ArchiveGuard.extract(zip, target, staging) is ExtractResult.Rejected)
        assertEquals(0, target.listFiles()?.size ?: 0)
        assertEquals(emptyList<String>(), strayFiles(root, target, staging))
    }

    /* ───────────────────── §4.3 解壓炸彈 ───────────────────── */

    @Test
    fun `壓縮比過高的 entry 被拒絕`() {
        val (_, target, staging) = dirs()
        val zip = StoreFixtures.malicious("evil-bomb.zip")
        val report = ArchiveGuard.inspect(zip)
        assertFalse(report.isSafe)
        assertTrue(report.rejections.any { it.kind == ArchiveRejection.Kind.ZIP_BOMB })
        assertTrue(ArchiveGuard.extract(zip, target, staging) is ExtractResult.Rejected)
        assertEquals(0, target.listFiles()?.size ?: 0)
    }

    @Test
    fun `宣告的大小說謊時由解壓時的硬性計數擋下`() {
        val (root, target, staging) = dirs()
        val zip = StoreFixtures.malicious("evil-lying-size.zip")

        // 中央目錄宣告只有 10 bytes，所以事前檢查一定會放行 ——
        // 這正是這條測試存在的意義：只信宣告值等於沒檢查。
        val report = ArchiveGuard.inspect(zip)
        assertTrue("事前檢查會被騙過（宣告值說謊）", report.isSafe)

        val limits = ArchiveLimits(maxEntryBytes = 64 * 1024)
        val result = ArchiveGuard.extract(zip, target, staging, limits)
        assertTrue("解壓時的硬性計數必須擋下它", result is ExtractResult.Rejected)
        assertTrue(
            (result as ExtractResult.Rejected).report.rejections
                .any { it.kind == ArchiveRejection.Kind.ZIP_BOMB }
        )
        assertEquals("被中止的解壓不可以留下半個檔案", 0, target.listFiles()?.size ?: 0)
        assertEquals(emptyList<String>(), strayFiles(root, target, staging))
    }

    @Test
    fun `entry 數量超過上限被拒絕`() {
        val (_, target, staging) = dirs()
        val zip = File(tmp.root, "many.zip")
        ZipOutputStream(zip.outputStream()).use { z ->
            repeat(20) { i ->
                z.putNextEntry(ZipEntry("f$i.yaml"))
                z.write("x".toByteArray())
                z.closeEntry()
            }
        }
        val limits = ArchiveLimits(maxEntries = 5)
        val report = ArchiveGuard.inspect(zip, limits)
        assertFalse(report.isSafe)
        assertTrue(report.rejections.any { it.kind == ArchiveRejection.Kind.ZIP_BOMB })
        assertTrue(ArchiveGuard.extract(zip, target, staging, limits) is ExtractResult.Rejected)
    }

    @Test
    fun `解壓後總量超過上限被拒絕`() {
        val (_, target, staging) = dirs()
        val zip = File(tmp.root, "total.zip")
        ZipOutputStream(zip.outputStream()).use { z ->
            repeat(4) { i ->
                z.putNextEntry(ZipEntry("f$i.yaml"))
                z.write(ByteArray(4096) { 'a'.code.toByte() })
                z.closeEntry()
            }
        }
        val limits = ArchiveLimits(maxTotalBytes = 8192, maxCompressionRatio = Long.MAX_VALUE)
        assertFalse(ArchiveGuard.inspect(zip, limits).isSafe)
        assertTrue(ArchiveGuard.extract(zip, target, staging, limits) is ExtractResult.Rejected)
    }

    /* ───────────────────── §4.4 副檔名白名單 ───────────────────── */

    @Test
    fun `可執行檔被拒絕`() {
        val (_, target, staging) = dirs()
        val zip = StoreFixtures.malicious("evil-executable.zip")
        val report = ArchiveGuard.inspect(zip)
        assertFalse(report.isSafe)
        val ext = report.rejections.firstOrNull { it.kind == ArchiveRejection.Kind.EXTENSION }
        assertNotNull(ext)
        assertEquals("libpwn.so", ext!!.entry)
        assertTrue(ArchiveGuard.extract(zip, target, staging) is ExtractResult.Rejected)
        assertEquals(0, target.listFiles()?.size ?: 0)
    }

    @Test
    fun `白名單涵蓋方案套件實際會用到的檔案`() {
        val limits = ArchiveLimits()
        listOf(
            "luna_pinyin.schema.yaml", "luna_pinyin.dict.yaml", "pinyin.yaml",
            "essay.txt", "UPSTREAM.txt", "LICENSE", "COPYING",
            "opencc/t2s.json", "opencc/TSCharacters.ocd2", "zh-hans.gram", "README.md",
            // librime-lua 的執行期資料：rime.lua 在根、其餘在 lua/ 底下
            // （package.path 就是這樣設的，攤平就 require 不到）
            "rime.lua", "lua/date_translator.lua", "lua/cold_word_drop/filter.lua",
            "cn_dicts/base.dict.yaml",
        ).forEach { assertNull("$it 應該被接受", ArchiveGuard.extensionProblemOf(it, limits)) }

        listOf(
            "payload.so", "run.sh", "a.exe", "x.dex", "luna_pinyin.table.bin",
            ".hidden.yaml", "noext",
        ).forEach { assertNotNull("$it 應該被拒絕", ArchiveGuard.extensionProblemOf(it, limits)) }
    }

    /* ───────────────────── 正常路徑 ───────────────────── */

    @Test
    fun `合法套件通過檢查並解壓到目標目錄`() {
        val (root, target, staging) = dirs()
        val zip = StoreFixtures.packageZip("rq-demo")

        val report = ArchiveGuard.inspect(zip)
        assertTrue(report.rejections.joinToString(), report.isSafe)

        val result = ArchiveGuard.extract(zip, target, staging)
        assertTrue(result.toString(), result is ExtractResult.Ok)
        val ok = result as ExtractResult.Ok
        assertTrue("rq_demo.schema.yaml" in ok.files)
        assertTrue("LICENSE" in ok.files)
        assertTrue("UPSTREAM.txt" in ok.files)
        assertTrue(File(target, "rq_demo.schema.yaml").isFile)
        assertEquals(emptyList<String>(), strayFiles(root, target, staging))
        assertEquals("暫存目錄要清乾淨", 0, staging.listFiles()?.size ?: 0)
    }

    @Test
    fun `不是 zip 的檔案以 MALFORMED 拒絕而不是拋例外`() {
        val (_, target, staging) = dirs()
        val junk = File(tmp.root, "junk.zip")
        junk.writeText("這根本不是 zip")
        val report = ArchiveGuard.inspect(junk)
        assertFalse(report.isSafe)
        assertEquals(ArchiveRejection.Kind.MALFORMED, report.rejections.first().kind)
        assertTrue(ArchiveGuard.extract(junk, target, staging) is ExtractResult.Rejected)
    }
}
