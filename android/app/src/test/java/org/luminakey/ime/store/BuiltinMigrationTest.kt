package org.luminakey.ime.store

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/**
 * 內建方案遷移（[BuiltinMigration]）。
 *
 * 真機回報的 bug：舊使用者升級後看不到九宮格，因為 user 目錄的
 * `default.custom.yaml` 已存在、永遠不會被覆蓋。這裡把三件事釘住：
 *   · 升級的人拿得到新增的內建方案
 *   · 使用者刻意停用過的方案**不可以**被塞回來
 *   · 隨附清單說有、但裝置上沒有的方案不會被硬塞進去（會害部署失敗）
 * 第二件是最重要的 —— 修 bug 修到推翻使用者的決定，是更糟的 bug。
 */
class BuiltinMigrationTest {

    @get:Rule
    val tmp = TemporaryFolder()

    /** 新版隨附的 default.custom.yaml（collect_data.sh 的真實輸出，四個方案）。 */
    private val shippedNew = """
        # 由 scripts/collect_data.sh 產生。
        #
        # t9_pinyin 是本專案自撰的九宮格方案，不在上游 default.yaml 裡，一併在此列入。
        patch:
          schema_list:
            - schema: luna_pinyin_tw    # 拼音（臺灣字形）
            - schema: bopomofo_tw       # 注音（臺灣字形）
            - schema: luna_pinyin       # 拼音（原版）
            - schema: t9_pinyin         # 九宮格拼音（本專案自撰，共用 luna_pinyin 詞典）
    """.trimIndent() + "\n"

    /** 舊版種在裝置上的那一份，只有三個方案。 */
    private val onDeviceOld = """
        # 由 scripts/collect_data.sh 產生。
        #
        # 上游 rime-prelude 的 default.yaml 列出的方案多於本專案實際打包的。
        patch:
          schema_list:
            - schema: luna_pinyin_tw    # 拼音（臺灣字形）
            - schema: bopomofo_tw       # 注音（臺灣字形）
            - schema: luna_pinyin       # 拼音（原版）
    """.trimIndent() + "\n"

    private val allFour =
        listOf("luna_pinyin_tw", "bopomofo_tw", "luna_pinyin", "t9_pinyin")

    /** 相當於 APK 解出來的 shared 目錄：四個方案檔都在。 */
    private lateinit var shared: File

    private fun newShared(ids: List<String> = allFour): File {
        val d = tmp.newFolder()
        ids.forEach { File(d, "$it.schema.yaml").writeText("schema:\n  schema_id: $it\n") }
        return d
    }

    private fun userDir(text: String? = null): File {
        val d = tmp.newFolder()
        if (text != null) File(d, SchemaListPatch.FILE_NAME).writeText(text)
        return d
    }

    /** 產品端就是這樣呼叫的：user 在前、shared 在後（librime 的搜尋順序）。 */
    private fun migrate(user: File, shippedText: String?, sharedDir: File = shared) =
        BuiltinMigration.migrate(user, shippedText, listOf(user, sharedDir))

    @org.junit.Before
    fun setUp() {
        shared = newShared()
    }

    /* ─────────────────── 情境 (a)：全新安裝 ─────────────────── */

    @Test
    fun `全新安裝：隨附清單已經齊全，不動 schema_list，但帳本要記滿`() {
        val d = userDir(shippedNew)
        val before = SchemaListPatch.file(d).readText()

        val r = migrate(d, shippedNew)

        assertEquals(emptyList<String>(), r.added)
        assertFalse(r.changed)
        // 一個位元組都不該動。
        assertEquals(before, SchemaListPatch.file(d).readText())
        assertEquals(allFour.toSet(), BuiltinMigration.readLedger(d))
    }

    /* ─────────────────── 情境 (b)：升級 ─────────────────── */

    @Test
    fun `升級：舊的三方案檔案補上 t9_pinyin`() {
        val d = userDir(onDeviceOld)

        val r = migrate(d, shippedNew)

        assertEquals(listOf("t9_pinyin"), r.added)
        assertTrue(r.changed)
        assertEquals(allFour, SchemaListPatch.read(d))
    }

    @Test
    fun `升級：使用者原有的順序與行末註解不能被洗掉`() {
        // 使用者自己調過順序、也自己加了註解。
        val mine = """
            # 我自己改過的設定，不要動我的東西。
            patch:
              schema_list:
                - schema: bopomofo_tw       # 我最常用這個，放第一
                - schema: luna_pinyin_tw    # 拼音（臺灣字形）
              menu/page_size: 9
        """.trimIndent() + "\n"
        val d = userDir(mine)

        migrate(d, shippedNew)

        val text = SchemaListPatch.file(d).readText()
        assertTrue("使用者的檔頭註解不見了：\n$text", text.contains("不要動我的東西"))
        assertTrue("行末註解不見了：\n$text", text.contains("# 我最常用這個，放第一"))
        assertTrue("schema_list 以外的設定不見了：\n$text", text.contains("menu/page_size: 9"))
        // 使用者排在前面的仍在前面，補的排在後面。
        assertEquals(
            listOf("bopomofo_tw", "luna_pinyin_tw", "luna_pinyin", "t9_pinyin"),
            SchemaListPatch.read(d),
        )
    }

    @Test
    fun `升級：user 目錄根本沒有 default_custom_yaml 時會建出完整清單`() {
        val d = userDir(null)

        val r = migrate(d, shippedNew)

        assertEquals(allFour, r.added)
        assertEquals(allFour, SchemaListPatch.read(d))
    }

    /* ─────────────── 核心：停用過的不可以被塞回來 ─────────────── */

    @Test
    fun `使用者在市集停用內建方案後，重啟不可以被塞回來`() {
        val d = userDir(onDeviceOld)

        // 第一次啟動：遷移補上 t9_pinyin，帳本記下四個。
        migrate(d, shippedNew)
        assertTrue("t9_pinyin" in SchemaListPatch.read(d))

        // 使用者到市集把九宮格停用（走的就是 SchemaStore 用的那支）。
        assertEquals(listOf("t9_pinyin"), SchemaListPatch.disable(d, listOf("t9_pinyin")))

        // 之後每一次啟動都再跑一次遷移 —— 都不可以動它。
        repeat(3) {
            val again = migrate(d, shippedNew)
            assertEquals(emptyList<String>(), again.added)
            assertFalse(
                "停用過的 t9_pinyin 又被塞回 schema_list",
                "t9_pinyin" in SchemaListPatch.read(d),
            )
        }
    }

    @Test
    fun `使用者停用的是舊版就有的內建方案，一樣不可以被塞回來`() {
        val d = userDir(onDeviceOld)
        migrate(d, shippedNew)                            // 帳本記滿四個
        SchemaListPatch.disable(d, listOf("luna_pinyin"))

        val again = migrate(d, shippedNew)

        assertEquals(emptyList<String>(), again.added)
        assertFalse("luna_pinyin" in SchemaListPatch.read(d))
    }

    /* ─────────────── 預檢：裝置上沒有的方案不硬塞 ─────────────── */

    @Test
    fun `方案檔不在裝置上時不加進 schema_list，也不記帳`() {
        // shared 目錄少了 t9_pinyin（實測撞到的情形：ASSET_REVISION 忘了遞增，
        // shared 沒有重新解壓）。
        val d = userDir(onDeviceOld)

        val r = migrate(d, shippedNew, sharedDir = newShared(allFour - "t9_pinyin"))

        assertEquals(emptyList<String>(), r.added)
        assertEquals(listOf("t9_pinyin"), r.skipped.map { it.id })
        assertTrue(r.skipped.single().reasons.single().contains("t9_pinyin.schema.yaml"))
        // schema_list 一個字都沒動 → 部署不會因此失敗。
        assertEquals(listOf("luna_pinyin_tw", "bopomofo_tw", "luna_pinyin"), SchemaListPatch.read(d))
        // 帳本**不可以**記它，否則打包端修好之後使用者永遠拿不到。
        assertFalse("t9_pinyin" in BuiltinMigration.readLedger(d))
    }

    @Test
    fun `打包端補上檔案之後，下一次啟動就會補進來`() {
        val d = userDir(onDeviceOld)
        migrate(d, shippedNew, sharedDir = newShared(allFour - "t9_pinyin"))
        assertFalse("t9_pinyin" in SchemaListPatch.read(d))

        val r = migrate(d, shippedNew, sharedDir = newShared())

        assertEquals(listOf("t9_pinyin"), r.added)
        assertEquals(allFour, SchemaListPatch.read(d))
    }

    @Test
    fun `缺詞典的方案也會被擋下來`() {
        val bad = tmp.newFolder()
        (allFour - "t9_pinyin").forEach {
            File(bad, "$it.schema.yaml").writeText("schema:\n  schema_id: $it\n")
        }
        File(bad, "t9_pinyin.schema.yaml").writeText(
            "schema:\n  schema_id: t9_pinyin\ntranslator:\n  dictionary: nowhere\n"
        )
        val d = userDir(onDeviceOld)

        val r = migrate(d, shippedNew, sharedDir = bad)

        assertEquals(emptyList<String>(), r.added)
        assertTrue(r.skipped.single().reasons.single().contains("nowhere.dict.yaml"))
    }

    @Test
    fun `沒有搜尋目錄時不做預檢，維持原本的行為`() {
        val d = userDir(onDeviceOld)
        val r = BuiltinMigration.migrate(d, shippedNew, searchDirs = emptyList())
        assertEquals(listOf("t9_pinyin"), r.added)
    }

    /* ─────────────────── 帳本本身 ─────────────────── */

    @Test
    fun `帳本讀寫可以往返`() {
        val d = userDir()
        BuiltinMigration.writeLedger(d, listOf("a", "b", "b", "", "c"))
        assertEquals(setOf("a", "b", "c"), BuiltinMigration.readLedger(d))
        assertTrue(BuiltinMigration.ledgerFile(d).name.endsWith(".json"))
    }

    @Test
    fun `帳本不存在時視為空的`() {
        assertEquals(emptySet<String>(), BuiltinMigration.readLedger(userDir()))
    }

    @Test
    fun `壞掉的帳本視為空的，不可以丟例外`() {
        val d = userDir(onDeviceOld)
        BuiltinMigration.ledgerFile(d).writeText("{ 這不是 JSON")
        assertEquals(emptySet<String>(), BuiltinMigration.readLedger(d))
        // 而且遷移照樣跑得完。
        assertEquals(listOf("t9_pinyin"), migrate(d, shippedNew).added)
    }

    @Test
    fun `帳本內容不變時不重寫檔案`() {
        val d = userDir(shippedNew)
        migrate(d, shippedNew)
        val stamp = BuiltinMigration.ledgerFile(d).readText()
        migrate(d, shippedNew)
        assertEquals(stamp, BuiltinMigration.ledgerFile(d).readText())
    }

    /* ─────────────────── 邊界 ─────────────────── */

    @Test
    fun `讀不到隨附清單時完全不動使用者的檔案`() {
        val d = userDir(onDeviceOld)
        val before = SchemaListPatch.file(d).readText()

        listOf<String?>(null, "", "   ", "# 只有註解\n").forEach { text ->
            val r = migrate(d, text)
            assertEquals(emptyList<String>(), r.added)
        }

        assertEquals(before, SchemaListPatch.file(d).readText())
        assertFalse(BuiltinMigration.ledgerFile(d).exists())
    }

    @Test
    fun `plan 只挑「隨附、帳本沒記過、且目前不在 schema_list」的`() {
        val shipped = listOf("a", "b", "c", "d")
        assertEquals(
            listOf("c"),
            BuiltinMigration.plan(shipped, ledger = setOf("d"), enabled = listOf("a", "b")),
        )
        assertEquals(
            emptyList<String>(),
            BuiltinMigration.plan(shipped, ledger = shipped.toSet(), enabled = emptyList()),
        )
        assertEquals(
            shipped,
            BuiltinMigration.plan(shipped, ledger = emptySet(), enabled = emptyList()),
        )
    }

    @Test
    fun `shippedFrom 認得隨附檔的順序`() {
        assertEquals(allFour, BuiltinMigration.shippedFrom(shippedNew))
        assertEquals(emptyList<String>(), BuiltinMigration.shippedFrom(null))
    }
}
