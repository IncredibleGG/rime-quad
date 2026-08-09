package org.luminakey.ime.store

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/**
 * 「什麼進備份、什麼刻意不進」。
 *
 * 這一組守的是兩種**都是靜默的**錯誤：
 *   · 多帶一個檔案 → 使用者的隱私跟著跑到另一台機器上；
 *   · 少帶一個檔案 → 他換手機之後少一截詞庫，而且沒有任何錯誤訊息。
 *
 * 所以每一條排除規則都各有一條測試，而且**同時斷言該收的有收到** ——
 * 只驗「有排除」的話，一個把所有東西都排除掉的 bug 會全綠。
 */
class BackupPlanTest {

    @get:Rule
    val tmp = TemporaryFolder()

    /* ═══════════ .userdb/ 裡面 ═══════════ */

    /**
     * ⚠ 這是本功能最容易寫錯的一行。
     *
     * `000003.log` 是 LevelDB 的**寫入前紀錄（WAL）**，使用者最近打的字全在
     * 那裡面。一條看起來很合理的「排除所有 *.log」會讓備份大小幾乎不變、
     * 匯入也成功，而使用者最近學的詞全沒了。
     */
    @Test
    fun `userdb 裡的 log 是 WAL 不是日誌，一定要收`() {
        assertTrue(BackupPlan.userDbFileIncluded("000003.log"))
        assertTrue(BackupPlan.userDbFileIncluded("000123.log"))
        assertTrue(BackupPlan.userDbFileIncluded("CURRENT"))
        assertTrue(BackupPlan.userDbFileIncluded("MANIFEST-000002"))
        assertTrue(BackupPlan.userDbFileIncluded("000005.ldb"))
        assertTrue(BackupPlan.userDbFileIncluded("000005.sst"))
    }

    @Test
    fun `userdb 裡只排除 LOCK 與 LevelDB 自己的 LOG`() {
        assertFalse(BackupPlan.userDbFileIncluded("LOCK"))
        assertFalse(BackupPlan.userDbFileIncluded("LOG"))
        assertFalse(BackupPlan.userDbFileIncluded("LOG.old"))
        // 大小寫敏感：LevelDB 的除錯輸出一定是大寫 LOG，小寫的不是它。
        assertTrue(BackupPlan.userDbFileIncluded("log"))
    }

    /* ═══════════ user_data_dir 裡面（.userdb/ 之外）═══════════ */

    @Test
    fun `連網紀錄不進備份`() {
        assertEquals(BackupFormat.OMIT_NETWORK_LOG, BackupPlan.omissionReason("net/connections.tsv"))
        assertEquals(BackupFormat.OMIT_NETWORK_LOG, BackupPlan.omissionReason("net"))
    }

    @Test
    fun `跨重裝穩定的 installation_id 不進備份`() {
        assertEquals(
            BackupFormat.OMIT_INSTALLATION_ID,
            BackupPlan.omissionReason("installation.yaml"),
        )
    }

    @Test
    fun `「何時用過哪個方案」不進備份`() {
        assertEquals(BackupFormat.OMIT_SCHEMA_ACCESS_TIME, BackupPlan.omissionReason("user.yaml"))
    }

    @Test
    fun `內建方案的引入帳本不進備份`() {
        assertEquals(
            BackupFormat.OMIT_BUILTIN_LEDGER,
            BackupPlan.omissionReason(BuiltinMigration.FILE_NAME),
        )
    }

    @Test
    fun `部署產物不進備份`() {
        assertEquals(
            BackupFormat.OMIT_DERIVED_BINARIES,
            BackupPlan.omissionReason("luna_pinyin.table.bin"),
        )
        assertEquals(
            BackupFormat.OMIT_DERIVED_BINARIES,
            BackupPlan.omissionReason("luna_pinyin.prism.bin"),
        )
        assertEquals(
            BackupFormat.OMIT_DERIVED_BINARIES,
            BackupPlan.omissionReason("build/luna_pinyin.schema.yaml"),
        )
        assertEquals(
            BackupFormat.OMIT_DERIVED_BINARIES,
            BackupPlan.omissionReason("rimequad-store.json.tmp"),
        )
    }

    @Test
    fun `librime 的同步目錄不進備份`() {
        assertEquals(BackupFormat.OMIT_SYNC_DIR, BackupPlan.omissionReason("sync/ABCDEF/x.userdb.txt"))
    }

    /** 反面：這些**必須**收，否則使用者換手機之後設定與方案全沒了。 */
    @Test
    fun `使用者真正在意的東西不會被排除`() {
        assertNull(BackupPlan.omissionReason("default.custom.yaml"))
        assertNull(BackupPlan.omissionReason("luna_pinyin.custom.yaml"))
        assertNull(BackupPlan.omissionReason("rimequad-store.json"))
        assertNull(BackupPlan.omissionReason("rimequad-layouts.json"))
        assertNull(BackupPlan.omissionReason("ice.dict.yaml"))
        assertNull(BackupPlan.omissionReason("lua/expand_translator.lua"))
    }

    @Test
    fun `宣告出來的排除理由與規則對得上`() {
        // manifest 裡宣告了什麼，程式就要真的做到什麼 —— 一份「說有排除
        // 但其實沒排」的清單比沒有清單更糟。
        val exercised = setOf(
            BackupPlan.omissionReason("net/connections.tsv"),
            BackupPlan.omissionReason("installation.yaml"),
            BackupPlan.omissionReason("user.yaml"),
            BackupPlan.omissionReason(BuiltinMigration.FILE_NAME),
            BackupPlan.omissionReason("sync/x"),
            BackupPlan.omissionReason("a.table.bin"),
        ).filterNotNull().toSet()
        assertTrue(
            "DECLARED_OMISSIONS 裡有規則沒實作到：${BackupPlan.DECLARED_OMISSIONS - exercised}",
            // OMIT_LOGS 目前只是宣告（log/ 不在 user_data_dir 底下），其餘都要有規則。
            (BackupPlan.DECLARED_OMISSIONS.toSet() - exercised) == setOf(BackupFormat.OMIT_LOGS),
        )
    }

    /* ═══════════ 走訪目錄 ═══════════ */

    @Test
    fun `userDbDirs 只認 userdb 結尾的目錄`() {
        val user = tmp.newFolder("user")
        File(user, "luna_pinyin.userdb").mkdirs()
        File(user, "bopomofo.userdb").mkdirs()
        File(user, "build").mkdirs()
        File(user, "lua").mkdirs()
        // 一個**檔案**叫 x.userdb —— 不是目錄就不是詞庫，不可以誤收。
        File(user, "not_a_dir.userdb").writeText("x")
        File(user, "default.custom.yaml").writeText("patch: {}")

        val names = BackupPlan.userDbDirs(user).map { it.name }
        assertEquals(listOf("bopomofo.userdb", "luna_pinyin.userdb"), names)
        assertEquals("luna_pinyin", BackupPlan.userDbName(File(user, "luna_pinyin.userdb")))
    }

    @Test
    fun `userDbEntries 產生的路徑都落在 dict 前綴底下`() {
        val user = tmp.newFolder("user")
        val db = File(user, "luna_pinyin.userdb").apply { mkdirs() }
        listOf("CURRENT", "000003.log", "LOCK", "LOG").forEach { File(db, it).writeText("x") }

        val entries = BackupPlan.userDbEntries("luna_pinyin", db)
        assertEquals(
            listOf(
                "dict/luna_pinyin.userdb/000003.log",
                "dict/luna_pinyin.userdb/CURRENT",
            ),
            entries.map { it.path }.sorted(),
        )
        assertTrue(entries.all { BackupFormat.isAllowedEntry(it.path) })
    }

    @Test
    fun `只收 custom 的設定檔`() {
        val user = tmp.newFolder("user")
        File(user, "default.custom.yaml").writeText("patch: {}")
        File(user, "luna_pinyin.custom.yaml").writeText("patch: {}")
        // 部署時從 shared 抄過去的產物，新機器上會自己重新產生。
        File(user, "luna_pinyin.schema.yaml").writeText("schema: {}")

        assertEquals(
            listOf("config/default.custom.yaml", "config/luna_pinyin.custom.yaml"),
            BackupPlan.customConfigEntries(user).map { it.path },
        )
    }

    @Test
    fun `帳本列出來但已經被刪掉的檔案不會讓匯出失敗`() {
        val user = tmp.newFolder("user")
        File(user, "ice.dict.yaml").writeText("---")
        val registry = InstalledRegistry.load(user)
        registry.put(
            InstalledPackage(
                id = "ice",
                name = "雾凇拼音",
                sha256 = "",
                installedAt = 0L,
                schemas = listOf(StoreSchemaRef("ice", "雾凇", "zh-Hans")),
                files = listOf("ice.dict.yaml", "gone.dict.yaml", "ice.table.bin"),
                requires = emptyList(),
                recommendedLayout = null,
                layoutNote = null,
                source = "store",
            )
        )
        val entries = BackupPlan.installedSchemaEntries(user, InstalledRegistry.load(user))
        assertEquals(listOf("schema/ice.dict.yaml"), entries.map { it.path })
    }
}
