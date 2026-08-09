package org.luminakey.ime.store

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import org.luminakey.ime.keyboard.LayoutRemap
import org.luminakey.ime.keyboard.LayoutRemapJson
import org.luminakey.ime.keyboard.RemapOp
import org.luminakey.ime.keyboard.UserLayoutStore
import java.io.File
import java.security.MessageDigest
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/**
 * 產品改名（RimeQuad → LuminaKey）之後，**磁碟上與備份容器裡的舊名字**還在。
 *
 * ── 這一份測試在防哪一種失敗 ───────────────────────────────────────────
 * `docs/backup-format.md` §1 與 `docs/coordination.md` §5 的相容條款是規範性的：
 * **寫出端只寫新名字，讀取端兩個名字都要認得。** 少了後半句的下場，
 * 是這個專案最怕的那一種 —— **沒有錯誤訊息**：
 *
 *   · 清單檔只認新名字 → 使用者改名前匯出的備份被判成 `NOT_A_BACKUP`。
 *     那句話會叫他去找一個壞掉的檔案，可是檔案是好的、壞掉的是我們。
 *   · 安裝帳本只認新名字 → 「裝過哪些方案」變成空清單，而檔案還在磁碟上，
 *     於是下一次安裝會撞到已經存在的檔案。
 *   · 自訂鍵位只認新名字 → 使用者調過很久的鍵位靜靜地回到原樣。
 *
 * 三種都不會留下任何紀錄，畫面上只是一片乾淨。所以這裡**各匯入一次**：
 * 一份舊名容器、一份新名容器，兩份都必須成功，而且落地之後的檔名必須是
 * 現在的名字。
 *
 * ── 它會不會在該紅的時候安靜地不跑 ─────────────────────────────────────
 * [兩個名字都不是的容器仍然是_不是備份] 是反向對照：如果 `readManifest`
 * 變成「任何 zip 都當成備份」，前面那兩條會照樣通過。有了這一條，
 * 「兩份都成功」才是一個有內容的命題。
 */
class BackupLegacyNameTest {

    @get:Rule
    val tmp = TemporaryFolder()

    /* ═════════════════ 落地檔名的對照表 ═════════════════ */

    /**
     * `BackupFormat` 刻意不 import `keyboard/`（那一層只用 kotlin 與 java.*），
     * 所以「容器裡的舊名字換成什麼」與「那兩個 store 現在叫什麼」是靠這一條
     * 測試釘在一起的，不是靠編譯期相依。兩邊分岔的話這裡會紅。
     */
    @Test
    fun `落地檔名與兩個 store 現在的檔名一致`() {
        assertEquals(
            InstalledRegistry.FILE_NAME,
            BackupFormat.landingPath(BackupFormat.LEGACY_REGISTRY_ENTRY),
        )
        assertEquals(
            UserLayoutStore.FILE_NAME,
            BackupFormat.landingPath(BackupFormat.LEGACY_LAYOUT_ENTRY),
        )
        // 新名字的那一份當然也要落在同一個地方。
        assertEquals(InstalledRegistry.FILE_NAME, BackupFormat.landingPath(BackupFormat.REGISTRY_ENTRY))
        assertEquals(UserLayoutStore.FILE_NAME, BackupFormat.landingPath(BackupFormat.LAYOUT_ENTRY))
    }

    /** 目錄那一段要原樣保留 —— 方案套件會解出 `schema/opencc` 底下的 json。 */
    @Test
    fun `landingPath 只去前綴與換檔名，不把子目錄壓平`() {
        assertEquals("opencc/t2s.json", BackupFormat.landingPath("schema/opencc/t2s.json"))
        assertEquals("default.custom.yaml", BackupFormat.landingPath("config/default.custom.yaml"))
        // 不在任何前綴底下的路徑原樣回傳（呼叫端另有白名單擋它）。
        assertEquals("whatever.txt", BackupFormat.landingPath("whatever.txt"))
    }

    @Test
    fun `相容條款本身還在_舊名沒有被安靜地拿掉`() {
        assertEquals(
            listOf(BackupFormat.MANIFEST_NAME, BackupFormat.LEGACY_MANIFEST_NAME),
            BackupFormat.MANIFEST_NAMES,
        )
        assertEquals(setOf(BackupFormat.KIND, BackupFormat.LEGACY_KIND), BackupFormat.ACCEPTED_KINDS)
        assertTrue(BackupFormat.LAYOUT_ENTRIES.contains(BackupFormat.LEGACY_LAYOUT_ENTRY))
        assertTrue(BackupFormat.REGISTRY_ENTRIES.contains(BackupFormat.LEGACY_REGISTRY_ENTRY))
        // 寫出端只寫新名字：第一個永遠是現在的名字。
        assertEquals(BackupFormat.MANIFEST_NAME, BackupFormat.MANIFEST_NAMES.first())
        assertEquals(BackupFormat.LAYOUT_ENTRY, BackupFormat.LAYOUT_ENTRIES.first())
        assertEquals(BackupFormat.REGISTRY_ENTRY, BackupFormat.REGISTRY_ENTRIES.first())
    }

    /* ═════════════════ 靶心：兩份容器各匯入一次 ═════════════════ */

    @Test
    fun `舊名容器匯入得起來`() {
        assertImports(legacy = true)
    }

    @Test
    fun `新名容器匯入得起來`() {
        assertImports(legacy = false)
    }

    /**
     * 反向對照。少了它，前面兩條在「什麼 zip 都當成備份」的實作下也會綠。
     */
    @Test
    fun `兩個名字都不是的容器仍然是_不是備份`() {
        val zip = File(tmp.newFolder("neither-${System.nanoTime()}"), "b.zip")
        val payload = "{}".toByteArray(Charsets.UTF_8)
        ZipOutputStream(zip.outputStream()).use { z ->
            z.putNextEntry(ZipEntry("dict/x.userdb/CURRENT"))
            z.write(payload)
            z.closeEntry()
            // 名字對不上任何一個已知的清單檔。
            z.putNextEntry(ZipEntry("someoneelse-backup.json"))
            z.write(
                manifestText(
                    kind = BackupFormat.KIND,
                    files = listOf(Triple("dict/x.userdb/CURRENT", payload.size.toLong(), sha(payload))),
                ).toByteArray(Charsets.UTF_8)
            )
            z.closeEntry()
        }
        val r = BackupArchive.readManifest(zip)
        assertTrue("名字不對的容器不可以被當成備份", r is BackupManifestJson.Result.Err)
        assertEquals(
            BackupProblem.NOT_A_BACKUP,
            (r as BackupManifestJson.Result.Err).issue.problem,
        )
    }

    /** `kind` 是第二道身分檢查，同樣要新舊都認、而且只認這兩個。 */
    @Test
    fun `kind 新舊都認，別人的 kind 仍然是壞掉的 manifest`() {
        assertEquals(
            BackupProblem.MANIFEST_BROKEN,
            err(BackupManifestJson.decode(manifestText(kind = "someone-else-backup"))).problem,
        )
        assertEquals(
            BackupProblem.MANIFEST_BROKEN,
            err(BackupManifestJson.decode("""{"format_version":1}""")).problem,
        )
        // 新舊兩個 kind 都讀得動。
        for (k in listOf(BackupFormat.KIND, BackupFormat.LEGACY_KIND)) {
            val m = BackupManifestJson.decode(manifestText(kind = k))
            assertTrue("kind=$k 應該讀得動", m is BackupManifestJson.Result.Ok)
        }
    }

    /* ═════════════════ 磁碟上的兩份帳本 ═════════════════ */

    @Test
    fun `安裝帳本_讀不到新名時退回舊名，寫入時遷移過去`() {
        val dir = tmp.newFolder("user-reg-${System.nanoTime()}")
        val legacy = File(dir, InstalledRegistry.LEGACY_FILE_NAME)
        legacy.writeText(REGISTRY_JSON, Charsets.UTF_8)

        // 讀：新名字不在，退回舊名字。
        val reg = InstalledRegistry.load(dir)
        assertEquals(setOf("demo"), reg.ids)
        assertEquals("示範", reg.get("demo")?.name)

        // 寫：一律寫新名字，而且寫完之後舊的那一份不再留著。
        reg.put(
            InstalledPackage(
                id = "another",
                name = "另一個",
                sha256 = "00",
                installedAt = 1L,
                schemas = listOf(StoreSchemaRef("s", "s", null)),
                files = listOf("s.schema.yaml"),
                requires = emptyList(),
                recommendedLayout = null,
                layoutNote = null,
                source = "store",
            )
        )
        val current = File(dir, InstalledRegistry.FILE_NAME)
        assertTrue("寫入必須落在現在的檔名上", current.isFile)
        assertFalse("遷移之後不可以留下兩份各說各話的帳本", legacy.exists())

        // 重讀一次：兩個套件都在。
        assertEquals(setOf("demo", "another"), InstalledRegistry.load(dir).ids)
    }

    @Test
    fun `安裝帳本_新名字存在時不看舊名字`() {
        val dir = tmp.newFolder("user-reg2-${System.nanoTime()}")
        File(dir, InstalledRegistry.LEGACY_FILE_NAME).writeText(REGISTRY_JSON, Charsets.UTF_8)
        File(dir, InstalledRegistry.FILE_NAME).writeText(
            """{"format_version":1,"packages":[{"id":"newer","name":"新的"}]}""",
            Charsets.UTF_8,
        )
        assertEquals(setOf("newer"), InstalledRegistry.load(dir).ids)
    }

    @Test
    fun `自訂鍵位_讀不到新名時退回舊名，寫入時遷移過去`() {
        val dir = tmp.newFolder("user-lay-${System.nanoTime()}")
        val legacy = File(dir, UserLayoutStore.LEGACY_FILE_NAME)
        legacy.writeText(
            LayoutRemapJson.encode(
                listOf(LayoutRemap("qwerty", listOf(RemapOp.Swap("lower", "a", "s"))))
            ),
            Charsets.UTF_8,
        )

        val store = UserLayoutStore.get(dir)
        assertNotNull("改名前調過的鍵位必須還在", store.remapFor("qwerty"))

        // 任何一次寫入都要把它遷到新名字上。
        store.put(LayoutRemap("t9-pinyin", listOf(RemapOp.Swap("t9", "k2", "k3"))))
        assertTrue(File(dir, UserLayoutStore.FILE_NAME).isFile)
        assertFalse("遷移之後不可以留下兩份", legacy.exists())
        assertNotNull(store.remapFor("qwerty"))
        assertNotNull(store.remapFor("t9-pinyin"))
    }

    /* ═════════════════ 工具 ═════════════════ */

    /**
     * 造一份備份容器並走完「讀 manifest → 解壓驗摘要 → 決定落地檔名」。
     *
     * 這三步就是 `BackupController.importNow` 裡與名字有關的**全部** ——
     * 第四步（真的搬進 user_data_dir）需要 `Context`，不在 JVM 測試的範圍，
     * 所以那一步用的 [BackupFormat.landingPath] 在這裡直接驗。
     */
    private fun assertImports(legacy: Boolean) {
        val tag = if (legacy) "舊名" else "新名"
        val manifestName =
            if (legacy) BackupFormat.LEGACY_MANIFEST_NAME else BackupFormat.MANIFEST_NAME
        val kind = if (legacy) BackupFormat.LEGACY_KIND else BackupFormat.KIND
        val registryEntry =
            if (legacy) BackupFormat.LEGACY_REGISTRY_ENTRY else BackupFormat.REGISTRY_ENTRY
        val layoutEntry =
            if (legacy) BackupFormat.LEGACY_LAYOUT_ENTRY else BackupFormat.LAYOUT_ENTRY

        val db = "dict/luna_pinyin.userdb/CURRENT" to "MANIFEST-000002\n".toByteArray(Charsets.UTF_8)
        val reg = registryEntry to REGISTRY_JSON.toByteArray(Charsets.UTF_8)
        val lay = layoutEntry to LayoutRemapJson.encode(
            listOf(LayoutRemap("qwerty", listOf(RemapOp.Swap("lower", "a", "s"))))
        ).toByteArray(Charsets.UTF_8)
        val payload = listOf(db, reg, lay)

        val zip = File(tmp.newFolder("c-$tag-${System.nanoTime()}"), "backup.zip")
        ZipOutputStream(zip.outputStream()).use { z ->
            payload.forEach { (path, bytes) ->
                z.putNextEntry(ZipEntry(path))
                z.write(bytes)
                z.closeEntry()
            }
            z.putNextEntry(ZipEntry(manifestName))
            z.write(
                manifestText(
                    kind = kind,
                    files = payload.map { (p, b) -> Triple(p, b.size.toLong(), sha(b)) },
                ).toByteArray(Charsets.UTF_8)
            )
            z.closeEntry()
        }

        // 1. 認得出這是我們的備份。
        val manifest = when (val r = BackupArchive.readManifest(zip)) {
            is BackupManifestJson.Result.Ok -> r.value
            is BackupManifestJson.Result.Err ->
                throw AssertionError("$tag 容器被判成 ${r.issue} —— 使用者會看到一句「這不是備份」")
        }
        assertEquals(3, manifest.files.size)

        // 2. 解得開、摘要對得上。
        val staging = tmp.newFolder("s-$tag-${System.nanoTime()}")
        val extracted = when (val e = BackupArchive.extract(zip, manifest, staging)) {
            is BackupArchive.Extract.Ok -> e
            is BackupArchive.Extract.Err -> throw AssertionError("$tag 容器解不開:${e.issue}")
        }
        assertEquals(3, extracted.files.size)

        // 3. 落地之後叫的是**現在**的名字。
        val landed = extracted.files
            .filter { it.startsWith(BackupFormat.DIR_SCHEMA) || it.startsWith(BackupFormat.DIR_CONFIG) }
            .map { BackupFormat.landingPath(it) }
        assertTrue(
            "$tag 容器裡的安裝帳本必須落成 ${InstalledRegistry.FILE_NAME}(實際 $landed)",
            landed.contains(InstalledRegistry.FILE_NAME),
        )

        // 4. 自訂鍵位那一份找得到 —— BackupController 是照 LAYOUT_ENTRIES 去找的。
        val layoutFile = BackupFormat.LAYOUT_ENTRIES
            .map { File(staging, it) }
            .firstOrNull { it.isFile }
        assertNotNull("$tag 容器裡的自訂鍵位必須找得到，否則會安靜地一個都沒還原", layoutFile)
        assertEquals(
            1,
            LayoutRemapJson.decode(layoutFile!!.readText(Charsets.UTF_8)).size,
        )

        // 5. 帳本的內容真的讀得懂（落地檔名對了但內容讀不動一樣是空清單）。
        val userDir = tmp.newFolder("u-$tag-${System.nanoTime()}")
        File(staging, registryEntry).copyTo(File(userDir, BackupFormat.landingPath(registryEntry)))
        assertEquals(setOf("demo"), InstalledRegistry.load(userDir).ids)
    }

    private fun err(r: BackupManifestJson.Result<BackupManifest>): BackupIssue = when (r) {
        is BackupManifestJson.Result.Err -> r.issue
        is BackupManifestJson.Result.Ok -> throw AssertionError("預期失敗，卻成功了")
    }

    private fun sha(b: ByteArray): String {
        val d = MessageDigest.getInstance("SHA-256").digest(b)
        val chars = "0123456789abcdef"
        val sb = StringBuilder(d.size * 2)
        for (x in d) {
            val v = x.toInt() and 0xFF
            sb.append(chars[v ushr 4]).append(chars[v and 0x0F])
        }
        return sb.toString()
    }

    /**
     * 手寫 manifest。**刻意不用 [BackupManifestJson.encode]** ——
     * 那一支永遠寫現在的 `kind`，拿它來造舊名容器等於測不到東西。
     */
    private fun manifestText(
        kind: String,
        files: List<Triple<String, Long, String>> = emptyList(),
    ): String {
        val fileList = files.joinToString(",\n                ") { (p, n, h) ->
            """{"path": "$p", "size": $n, "sha256": "$h"}"""
        }
        return """
            {
              "kind": "$kind",
              "format_version": ${BackupFormat.FORMAT_VERSION},
              "created_at": 1754000000,
              "producer": {"platform": "android", "app_version": "1.0",
                           "app_version_code": 1, "rime_shell_abi": 1},
              "user_db": [{"name": "luna_pinyin", "encoding": "leveldb-dir",
                           "root": "dict/luna_pinyin.userdb", "flushed": true}],
              "schemas": [{"id": "luna_pinyin_tw", "name": "拼音",
                           "package": null, "bundled": true}],
              "enabled_schemas": ["luna_pinyin_tw"],
              "omitted": [],
              "files": [$fileList]
            }
        """.trimIndent()
    }

    private companion object {
        /** 一份最小但讀得懂的安裝帳本。 */
        const val REGISTRY_JSON =
            """{"format_version": 1, "packages": [{"id": "demo", "name": "示範",
               "sha256": "aa", "installed_at": 1, "source": "store",
               "files": ["demo.schema.yaml"], "requires": [],
               "schemas": [{"id": "demo", "name": "示範", "language": "zh-Hant"}]}]}"""
    }
}
