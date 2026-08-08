package org.rimequad.ime.store

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream

/**
 * 打包 → 解包的往返，以及**最重要的那一條**：
 * 匯出的備份裡真的裝著使用者自己造的詞。
 *
 * ── 為什麼「檔案產生得出來」不算驗過 ────────────────────────────────────
 * 這個功能最可能的失敗長這樣：備份存出來了、大小看起來也對、匯入也成功，
 * 使用者三個月後才發現他的詞庫少了一截。整條路徑上沒有任何一個錯誤訊息。
 *
 * 少的那一截，多半就是 LevelDB 的 **WAL（`000003.log`）** —— 使用者最近打的
 * 字全在那裡面，而它的檔名長得跟「日誌」一模一樣，是最容易被一條
 * 「排除所有 *.log」的規則順手刪掉的東西（見 [BackupPlan] 檔頭）。
 *
 * 所以這裡不驗「有沒有產生檔案」，而是**把一個具體的詞放進假的詞庫，
 * 走完整條匯出流程，再從產物裡把那個詞找回來**。
 *
 * ── 它會不會在該紅的時候安靜地不跑 ─────────────────────────────────────
 * [核心斷言會紅嗎_把會丟資料的打包器餵給同一組斷言] 這條測試就是在問這件事：
 * 把一個「刻意漏掉 *.log」的打包器餵給同一個斷言函式，斷言**必須**失敗。
 * 失敗了才證明前面那條測試真的在測東西，而不是在測一個永遠成立的命題。
 */
class BackupRoundTripTest {

    @get:Rule
    val tmp = TemporaryFolder()

    companion object {
        /**
         * 「使用者自己造的詞」。挑一個絕不可能出現在任何內建詞典裡的字串，
         * 這樣在產物裡找到它就只有一個解釋：它是從我們放進去的那份詞庫來的。
         */
        const val USER_WORD = "皮卡丘鹹酥雞測試詞"

        /** 只出現在 LevelDB 自己的除錯輸出 `LOG` 裡。備份**不該**有它。 */
        const val LOG_ONLY_MARKER = "COMPACTION-DEBUG-SHOULD-NOT-BE-BACKED-UP"

        const val DB_NAME = "luna_pinyin"
    }

    /**
     * 一份長得像 librime 使用者詞典的目錄。
     *
     * 檔案名稱照 LevelDB 真實的樣子擺：`CURRENT` 指向 `MANIFEST-*`，
     * 未壓實的寫入在 `*.log`（WAL），壓實過的在 `*.ldb`，
     * 另外有行程鎖 `LOCK` 與除錯輸出 `LOG`。
     */
    private fun fakeUserDb(parent: File, name: String = DB_NAME): File {
        val dir = File(parent, "$name${BackupPlan.USERDB_SUFFIX}").apply { mkdirs() }
        File(dir, "CURRENT").writeText("MANIFEST-000002\n")
        File(dir, "MANIFEST-000002").writeBytes(byteArrayOf(1, 2, 3, 4, 5))
        // ⚠ 使用者剛學到的詞住在 WAL 裡。這一行是整條測試的靶心。
        File(dir, "000003.log").writeText("pika $USER_WORD c=3 d=0.9", Charsets.UTF_8)
        File(dir, "000005.ldb").writeText("older 你好 c=1", Charsets.UTF_8)
        File(dir, "LOCK").writeBytes(ByteArray(0))
        File(dir, "LOG").writeText("$LOG_ONLY_MARKER\ncompacted 2 files\n")
        return dir
    }

    private fun manifestOf(files: List<BackupFile>, dbFlushed: Boolean = true) = BackupManifest(
        formatVersion = BackupFormat.FORMAT_VERSION,
        createdAt = 1_754_000_000L,
        producer = BackupProducer("android", "1.0", 1L, 1),
        userDbs = listOf(
            BackupUserDb(
                DB_NAME,
                BackupFormat.ENCODING_LEVELDB_DIR,
                "${BackupFormat.DIR_DICT}$DB_NAME${BackupPlan.USERDB_SUFFIX}",
                dbFlushed,
            )
        ),
        schemas = listOf(BackupSchemaRef("luna_pinyin_tw", "拼音", null, true)),
        enabledSchemas = listOf("luna_pinyin_tw"),
        files = files,
        omitted = BackupPlan.DECLARED_OMISSIONS,
    )

    /* ═════════════════ 靶心：詞真的在裡面嗎 ═════════════════ */

    /**
     * 核心斷言，抽成函式是為了能拿另一個（壞掉的）打包器餵它，
     * 證明它在該紅的時候真的會紅。
     *
     * @param pack 收到「一個假的 userdb 目錄」與「要寫到哪個 zip」，
     *             產生一份備份。
     */
    private fun assertUserWordSurvives(pack: (File, File) -> Unit) {
        val userDir = tmp.newFolder("user-${System.nanoTime()}")
        fakeUserDb(userDir)
        val zip = File(tmp.newFolder("out-${System.nanoTime()}"), "backup.zip")

        pack(userDir, zip)

        // ① 直接在容器裡找。不透過我們自己的 manifest —— 那等於拿嫌犯的
        //    證詞當證據；這裡用 java.util.zip 重新讀一遍。
        val inZip = ZipFile(zip).use { zf ->
            entriesOf(zf).any { e ->
                !e.isDirectory &&
                    zf.getInputStream(e).use { it.readBytes() }
                        .toString(Charsets.UTF_8)
                        .contains(USER_WORD)
            }
        }
        assertTrue(
            "備份裡找不到使用者造的詞「$USER_WORD」—— " +
                "最可能的原因是 WAL（*.log）被當成日誌排除掉了",
            inZip,
        )

        // ② 走完整條解包流程，從**還原出來的檔案**裡再找一次。
        //    ① 只證明資料進了容器，② 才證明它拿得回來。
        val manifest = when (val r = BackupArchive.readManifest(zip)) {
            is BackupManifestJson.Result.Ok -> r.value
            is BackupManifestJson.Result.Err -> {
                fail("讀不到 manifest: ${r.issue}")
                return
            }
        }
        val staging = tmp.newFolder("staging-${System.nanoTime()}")
        val extracted = BackupArchive.extract(zip, manifest, staging)
        assertTrue("解包應該成功，實際是 $extracted", extracted is BackupArchive.Extract.Ok)

        val restored = File(staging, BackupFormat.DIR_DICT).walkTopDown()
            .filter { it.isFile }
            .any { it.readText(Charsets.UTF_8).contains(USER_WORD) }
        assertTrue("還原出來的詞庫檔案裡找不到「$USER_WORD」", restored)
    }

    @Test
    fun `匯出的詞庫真的含有使用者造的詞`() {
        assertUserWordSurvives { userDir, zip -> packWith(userDir, zip, dropWal = false) }
    }

    /**
     * 反向測試：把一個「會漏掉 WAL」的打包器餵給**同一組斷言**。
     * 斷言必須失敗 —— 失敗才證明上面那條測試不是在測一個永遠成立的命題。
     */
    @Test
    fun `核心斷言會紅嗎_把會丟資料的打包器餵給同一組斷言`() {
        try {
            assertUserWordSurvives { userDir, zip -> packWith(userDir, zip, dropWal = true) }
        } catch (expected: AssertionError) {
            return  // 正確：它紅了
        }
        fail("漏掉 WAL 的打包器竟然通過了核心斷言 —— 那條測試等於沒在測東西")
    }

    /**
     * [dropWal] = true 時模擬那個經典錯誤：「*.log 是日誌，不用備份」。
     * 其餘與正式流程完全相同（同一個 [BackupPlan]、同一個 [BackupArchive]）。
     */
    private fun packWith(userDir: File, zip: File, dropWal: Boolean) {
        val dbDir = BackupPlan.userDbDirs(userDir).single()
        val entries = BackupPlan.userDbEntries(BackupPlan.userDbName(dbDir), dbDir)
            .filterNot { dropWal && it.path.endsWith(".log") }
        zip.parentFile?.mkdirs()
        zip.outputStream().use { out ->
            BackupArchive.pack(entries, out) { files -> manifestOf(files) }
        }
    }

    /* ═════════════════ 排除項目 ═════════════════ */

    @Test
    fun `LevelDB 自己的 LOG 與 LOCK 不會被打包`() {
        val userDir = tmp.newFolder("user")
        fakeUserDb(userDir)
        val zip = File(tmp.newFolder("out"), "backup.zip")
        packWith(userDir, zip, dropWal = false)

        val names = ZipFile(zip).use { zf -> entriesOf(zf).map { it.name } }
        assertFalse("LOCK 不該進備份：$names", names.any { it.endsWith("/LOCK") })
        assertFalse("LOG 不該進備份：$names", names.any { it.endsWith("/LOG") })

        val everything = ZipFile(zip).use { zf ->
            entriesOf(zf).joinToString("\n") { e ->
                zf.getInputStream(e).use { it.readBytes() }.toString(Charsets.UTF_8)
            }
        }
        assertFalse(
            "LOG 的內容不該以任何形式出現在備份裡",
            everything.contains(LOG_ONLY_MARKER),
        )
        // 同一份產物裡，該有的必須有 —— 否則上面兩條「沒有」的斷言只要
        // 打包器整個壞掉就會全過。
        assertTrue("WAL 必須在", names.any { it.endsWith("/000003.log") })
        assertTrue("CURRENT 必須在", names.any { it.endsWith("/CURRENT") })
    }

    /* ═════════════════ 往返 ═════════════════ */

    @Test
    fun `每一個檔案的位元組在往返之後完全相同`() {
        val userDir = tmp.newFolder("user")
        val dbDir = fakeUserDb(userDir)
        val zip = File(tmp.newFolder("out"), "backup.zip")
        packWith(userDir, zip, dropWal = false)

        val manifest = ok(BackupArchive.readManifest(zip))
        val staging = tmp.newFolder("staging")
        assertTrue(BackupArchive.extract(zip, manifest, staging) is BackupArchive.Extract.Ok)

        val expected = dbDir.listFiles()!!
            .filter { it.isFile && BackupPlan.userDbFileIncluded(it.name) }
            .associate { it.name to it.readBytes().toList() }
        val actual = File(staging, "${BackupFormat.DIR_DICT}$DB_NAME${BackupPlan.USERDB_SUFFIX}")
            .listFiles()!!
            .associate { it.name to it.readBytes().toList() }

        assertEquals("往返之後檔案集合必須相同", expected.keys.sorted(), actual.keys.sorted())
        expected.forEach { (name, bytes) ->
            assertEquals("$name 的內容在往返之後變了", bytes, actual[name])
        }
    }

    @Test
    fun `manifest 記下了詞庫名與載體格式`() {
        val userDir = tmp.newFolder("user")
        fakeUserDb(userDir)
        val zip = File(tmp.newFolder("out"), "backup.zip")
        packWith(userDir, zip, dropWal = false)

        val m = ok(BackupArchive.readManifest(zip))
        assertEquals(1, m.userDbs.size)
        assertEquals(DB_NAME, m.userDbs[0].name)
        assertEquals(BackupFormat.ENCODING_LEVELDB_DIR, m.userDbs[0].encoding)
        assertTrue(m.files.isNotEmpty())
        assertTrue("排除理由必須寫進備份，否則沒有人查得到我們刻意漏掉什麼",
            m.omitted.contains(BackupFormat.OMIT_NETWORK_LOG))
    }

    /* ═════════════════ 壞掉的檔案 ═════════════════ */

    @Test
    fun `內容被動過的備份會被 sha256 抓到且不留下半套檔案`() {
        val userDir = tmp.newFolder("user")
        fakeUserDb(userDir)
        val good = File(tmp.newFolder("out"), "backup.zip")
        packWith(userDir, good, dropWal = false)

        // 保留 manifest 原樣，只把 WAL 的內容改掉 —— 這正是「傳輸途中壞了」
        // 或「有人動過手腳」的樣子。
        val tampered = File(tmp.newFolder("out2"), "tampered.zip")
        rezip(good, tampered) { name, bytes ->
            if (name.endsWith("/000003.log")) "完全不同的內容".toByteArray() else bytes
        }

        val m = ok(BackupArchive.readManifest(tampered))
        val staging = tmp.newFolder("staging2")
        val r = BackupArchive.extract(tampered, m, staging)
        assertTrue("必須被拒絕，實際是 $r", r is BackupArchive.Extract.Err)
        assertEquals(
            BackupProblem.CONTENT_MISMATCH,
            (r as BackupArchive.Extract.Err).issue.problem,
        )
        assertFalse("拒絕之後不可以留下半套解出來的檔案", staging.exists())
    }

    @Test
    fun `manifest 說有但容器裡沒有的檔案會被指名`() {
        val userDir = tmp.newFolder("user")
        fakeUserDb(userDir)
        val good = File(tmp.newFolder("out"), "backup.zip")
        packWith(userDir, good, dropWal = false)

        val truncated = File(tmp.newFolder("out2"), "truncated.zip")
        rezip(good, truncated) { name, bytes ->
            if (name.endsWith("/000003.log")) null else bytes   // 整個 entry 抽掉
        }

        val m = ok(BackupArchive.readManifest(truncated))
        val r = BackupArchive.extract(truncated, m, tmp.newFolder("staging3"))
        assertTrue(r is BackupArchive.Extract.Err)
        val issue = (r as BackupArchive.Extract.Err).issue
        assertEquals(BackupProblem.MISSING_ENTRY, issue.problem)
        assertTrue("訊息要指名是哪一個檔案不見了", issue.args.first().endsWith("000003.log"))
    }

    @Test
    fun `隨便一個檔案不會讓 App 崩潰_只會得到一句看得懂的話`() {
        val junk = File(tmp.newFolder("junk"), "photo.jpg")
        junk.writeBytes(ByteArray(4096) { (it % 251).toByte() })

        val r = BackupArchive.readManifest(junk)
        assertTrue(r is BackupManifestJson.Result.Err)
        assertEquals(
            BackupProblem.NOT_A_BACKUP,
            (r as BackupManifestJson.Result.Err).issue.problem,
        )
    }

    @Test
    fun `是 zip 但不是我們的備份`() {
        val zip = File(tmp.newFolder("other"), "other.zip")
        ZipOutputStream(zip.outputStream()).use { z ->
            z.putNextEntry(ZipEntry("readme.txt"))
            z.write("hello".toByteArray())
            z.closeEntry()
        }
        val r = BackupArchive.readManifest(zip)
        assertEquals(
            BackupProblem.NOT_A_BACKUP,
            (r as BackupManifestJson.Result.Err).issue.problem,
        )
    }

    @Test
    fun `manifest 指向容器外的路徑一律拒絕`() {
        val zip = File(tmp.newFolder("evil"), "evil.zip")
        val evilPath = "dict/../../escaped.log"
        ZipOutputStream(zip.outputStream()).use { z ->
            z.putNextEntry(ZipEntry(evilPath))
            z.write("x".toByteArray())
            z.closeEntry()
            z.putNextEntry(ZipEntry(BackupFormat.MANIFEST_NAME))
            z.write(
                BackupManifestJson.encode(
                    manifestOf(listOf(BackupFile(evilPath, 1, "00".repeat(32))))
                ).toByteArray()
            )
            z.closeEntry()
        }
        val m = ok(BackupArchive.readManifest(zip))
        val staging = tmp.newFolder("staging4")
        val r = BackupArchive.extract(zip, m, staging)
        assertEquals(
            BackupProblem.UNSAFE_PATH,
            (r as BackupArchive.Extract.Err).issue.problem,
        )
        assertFalse(File(staging.parentFile, "escaped.log").exists())
    }

    @Test
    fun `容器裡多出來的東西不會被解出來`() {
        val userDir = tmp.newFolder("user")
        fakeUserDb(userDir)
        val good = File(tmp.newFolder("out"), "backup.zip")
        packWith(userDir, good, dropWal = false)

        // 加一個 manifest 沒有列的 entry —— 未來版本新增的東西長這樣，
        // 舊版必須忽略它（前向相容），而不是把它寫進使用者的資料目錄。
        val extra = File(tmp.newFolder("out2"), "extra.zip")
        rezipAdding(good, extra, "config/surprise.custom.yaml", "patch: {}".toByteArray())

        val m = ok(BackupArchive.readManifest(extra))
        val staging = tmp.newFolder("staging5")
        assertTrue(BackupArchive.extract(extra, m, staging) is BackupArchive.Extract.Ok)
        assertFalse(
            "manifest 沒列的檔案不可以落地",
            File(staging, "config/surprise.custom.yaml").exists(),
        )
    }

    /* ═════════════════ 工具 ═════════════════ */

    private fun ok(r: BackupManifestJson.Result<BackupManifest>): BackupManifest = when (r) {
        is BackupManifestJson.Result.Ok -> r.value
        is BackupManifestJson.Result.Err -> throw AssertionError("預期成功，得到 ${r.issue}")
    }

    /**
     * `ZipFile.entries()` 回的是 `Enumeration<? extends ZipEntry>`，
     * 在 Kotlin 這一側型別是 `Enumeration<out ZipEntry>`；用最笨的迴圈把它
     * 攤成 `List<ZipEntry>`，省掉一整排型別投影的噪音。
     */
    private fun entriesOf(zf: ZipFile): List<ZipEntry> {
        val out = ArrayList<ZipEntry>()
        val e = zf.entries()
        while (e.hasMoreElements()) out += e.nextElement()
        return out
    }

    /** 重打包，`mutate` 回 null 代表把那個 entry 拿掉。 */
    private fun rezip(src: File, dst: File, mutate: (String, ByteArray) -> ByteArray?) {
        ZipFile(src).use { zf ->
            ZipOutputStream(dst.outputStream()).use { z ->
                entriesOf(zf).forEach { e ->
                    val bytes = zf.getInputStream(e).use { it.readBytes() }
                    val next = mutate(e.name, bytes) ?: return@forEach
                    z.putNextEntry(ZipEntry(e.name))
                    z.write(next)
                    z.closeEntry()
                }
            }
        }
    }

    private fun rezipAdding(src: File, dst: File, name: String, bytes: ByteArray) {
        ZipFile(src).use { zf ->
            ZipOutputStream(dst.outputStream()).use { z ->
                entriesOf(zf).forEach { e ->
                    z.putNextEntry(ZipEntry(e.name))
                    z.write(zf.getInputStream(e).use { s -> s.readBytes() })
                    z.closeEntry()
                }
                z.putNextEntry(ZipEntry(name))
                z.write(bytes)
                z.closeEntry()
            }
        }
    }
}
