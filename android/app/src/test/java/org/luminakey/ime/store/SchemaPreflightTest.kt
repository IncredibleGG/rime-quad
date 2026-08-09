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
 * 規範 §4：「錯誤訊息必須明確告訴使用者缺少哪一個詞典，而不是只說部署失敗」。
 *
 * 這是預檢存在的唯一理由 —— `rs_last_error()` 給不出這個資訊。
 *
 * 而預檢自己的紅線是**不准比 librime 嚴**。它曾經比 librime 嚴，代價是市集
 * 98 個方案裡 20 個按下「啟用」直接被自己人擋死。下面每一條 WARNING 的斷言
 * 都對應 librime 原始碼裡一個「找不到也照樣往下走」的位置，改動時請一併查證。
 */
class SchemaPreflightTest {

    @get:Rule
    val tmp = TemporaryFolder()

    private lateinit var user: File
    private lateinit var shared: File

    private fun setup(vararg present: String): List<File> {
        user = tmp.newFolder("user")
        shared = tmp.newFolder("shared")
        present.forEach { File(shared, it).writeText("# stub\n") }
        return listOf(user, shared)
    }

    private fun report(id: String, text: String, dirs: List<File>) =
        SchemaPreflight.Report(id, SchemaPreflight.checkText(id, text, dirs))

    @Test
    fun `缺少的詞典會被指名`() {
        val dirs = setup()
        val missing = SchemaPreflight.checkText(
            "bopomofo_tw",
            """
            schema:
              schema_id: bopomofo_tw
            translator:
              dictionary: terra_pinyin
            """.trimIndent(),
            dirs,
        )
        assertEquals(1, missing.size)
        assertEquals(SchemaPreflight.Kind.DICTIONARY, missing[0].kind)
        assertEquals("terra_pinyin.dict.yaml", missing[0].fileName)
        // 檔名要真的流進訊息的參數裡 —— 規範 §4 要的是「缺哪一本」，
        // 而不是一句「部署失敗」。
        assertTrue(
            "缺的檔名沒有帶進訊息參數：${missing[0].uiMessage()}",
            missing[0].uiMessage().args.contains("terra_pinyin.dict.yaml"),
        )
    }

    @Test
    fun `主詞典缺了就是擋 —— 放寬之後這條仍然要成立`() {
        // librime `SchemaUpdate::Run`：GetString("translator/dictionary") 拿得到，
        // 就一定會去 Compile；編不起來直接 return false，部署真的失敗。
        val r = report(
            "rq_nodict",
            "schema:\n  schema_id: rq_nodict\ntranslator:\n  dictionary: rq_absent\n",
            setup(),
        )
        assertFalse("主詞典不在卻放行，等於預檢整支變成裝飾品", r.ok)
        assertEquals(listOf("rq_absent.dict.yaml"), r.blocking.map { it.fileName })
    }

    @Test
    fun `詞典在 shared 目錄裡就不算缺`() {
        val dirs = setup("terra_pinyin.dict.yaml")
        assertTrue(
            SchemaPreflight.checkText(
                "x", "translator:\n  dictionary: terra_pinyin\n", dirs
            ).isEmpty()
        )
    }

    @Test
    fun `反查詞典與 dependencies 照樣回報，但不擋啟用`() {
        // 注音方案的真實形狀：translator 用 terra_pinyin，另外要 stroke 做筆畫反查。
        //
        // librime 對這兩者的態度：
        //   dependencies → WorkspaceUpdate「skipped unsatisfied dependency」，只是 warning
        //   reverse_lookup/dictionary → SchemaUpdate 從頭到尾沒讀過這個鍵
        // 所以少了 stroke，注音照樣打得出字，只是筆畫反查沒作用。
        val dirs = setup("terra_pinyin.dict.yaml")
        val r = report(
            "bopomofo_tw",
            """
            schema:
              schema_id: bopomofo_tw
              dependencies:
                - stroke
            translator:
              dictionary: terra_pinyin
            reverse_lookup:
              dictionary: stroke
            """.trimIndent(),
            dirs,
        )
        assertEquals(
            setOf("stroke.schema.yaml", "stroke.dict.yaml"),
            r.missing.map { it.fileName }.toSet(),
        )
        assertTrue("少了反查詞典不該擋住啟用", r.ok)
        assertEquals(2, r.warnings.size)
        // ⚠ 不比對譯文（「照常」兩個字），比對**指到哪一份樣板**：
        // 資源名的前綴就是嚴重度。擋的那一條指到放行的樣板時這裡會紅，
        // 而那正是使用者會被一句「不擋你」騙過去的情形。
        assertEquals(
            "放行的那幾條指到了 preflight_warn_ 以外的樣板",
            emptyList<String>(),
            r.warnings.map { resourceNameOf(it.uiMessage().id) }.filterNot {
                it.startsWith("preflight_warn_")
            },
        )
        assertEquals(
            "擋人的那幾條指到了 preflight_block_ 以外的樣板",
            emptyList<String>(),
            r.blocking.map { resourceNameOf(it.uiMessage().id) }.filterNot {
                it.startsWith("preflight_block_")
            },
        )
    }

    @Test
    fun `市集裡真的存在的四種形狀，一個都不准被擋下`() {
        // 這四份是從 build/schema-store 的套件裡抄出來的最小形狀。放寬之前，
        // 市集 98 個方案有 20 個長這樣 —— 使用者下載完、按啟用、被自己的預檢擋死。
        val dirs = setup("wubi86.dict.yaml", "cangjie5_express.dict.yaml", "moran.dict.yaml")

        // 五筆 / 鄭碼：拿 pinyin_simp 當反查
        val wubi = report(
            "wubi86",
            """
            schema:
              schema_id: wubi86
              dependencies:
                - pinyin_simp
            translator:
              dictionary: wubi86
            reverse_lookup:
              dictionary: pinyin_simp
            """.trimIndent(),
            dirs,
        )
        assertTrue("五筆被擋住了", wubi.ok)

        // 倉頡 / 速成 / 行列 / 快速：拿 luna_quanpin 當反查
        val cangjie = report(
            "cangjie5_express",
            """
            schema:
              schema_id: cangjie5_express
              dependencies:
                - luna_quanpin
            translator:
              dictionary: cangjie5_express
            """.trimIndent(),
            dirs,
        )
        assertTrue("倉頡被擋住了", cangjie.ok)

        // 魔然 / 粵拼：拿 cangjie5 當輔助碼
        val moran = report(
            "moran",
            """
            schema:
              schema_id: moran
            translator:
              dictionary: moran
            reverse_lookup:
              dictionary: cangjie5
            """.trimIndent(),
            dirs,
        )
        assertTrue("魔然被擋住了", moran.ok)

        // 帶語言模型的整句方案：.gram 動輒上百 MB，不隨套件走是常態
        val essay = report(
            "rq_gram",
            """
            schema:
              schema_id: rq_gram
            translator:
              dictionary: moran
            grammar:
              language: zh-hans-t-essay-bgw
            """.trimIndent(),
            dirs,
        )
        assertTrue("語言模型不在不該擋住啟用", essay.ok)
        assertEquals(listOf("zh-hans-t-essay-bgw.gram"), essay.warnings.map { it.fileName })
    }

    @Test
    fun `translator 以外的 dictionary 不算主詞典`() {
        // `translator@melt_eng` 這種掛載式次翻譯器，librime 的部署期不編它的詞典。
        val r = report(
            "rq_mount",
            """
            schema:
              schema_id: rq_mount
            translator:
              dictionary: rq_main
            translator@melt_eng:
              dictionary: melt_eng
            custom_phrase:
              dictionary: custom_phrase
            """.trimIndent(),
            setup("rq_main.dict.yaml"),
        )
        assertTrue(r.ok)
        assertEquals(
            setOf("melt_eng.dict.yaml", "custom_phrase.dict.yaml"),
            r.warnings.map { it.fileName }.toSet(),
        )
    }

    @Test
    fun `import_preset 與 __include 指向的配置檔缺了要擋`() {
        // librime `ConfigCompiler`：`resource not found` → Resolve 回 false
        // → ConfigFileUpdate 回 false → SchemaUpdate 回 false。真的會失敗。
        val dirs = setup("default.yaml")
        val r = report(
            "x",
            """
            punctuator:
              import_preset: default
            speller:
              __include: pinyin:/speller
            """.trimIndent(),
            dirs,
        )
        assertEquals(listOf("pinyin.yaml"), r.missing.map { it.fileName })
        assertFalse(r.ok)
    }

    @Test
    fun `有沒有冒號才是分界線 —— 同檔案內的 __include 不算外部相依`() {
        // librime `config_compiler.cc` 的 ParseInclude 註解就是這條規則：
        //   __include: path/to/local/node
        //   __include: filename[.yaml]:/path/to/external/node
        // 沒有冒號時 CreateReference() 把 resource_id 取成 current_resource_id()。
        assertNull(SchemaPreflight.includeTarget("/speller/algebra"))
        // ↓ 這一行是真的踩過的：倉頡 array30 與魔然 moran 都用這種寫法，
        //   而它們各自的 yaml 裡就有這個頂層鍵。當成檔案 = 誤報 = 使用者啟不動。
        assertNull(SchemaPreflight.includeTarget("array30_format"))
        assertNull(SchemaPreflight.includeTarget("reverse_format"))
        assertEquals("pinyin.yaml", SchemaPreflight.includeTarget("pinyin:/speller")?.fileName)
        assertEquals(
            "symbols.yaml",
            SchemaPreflight.includeTarget("symbols.yaml:/symbols")?.fileName,
        )
        // 結尾的 `?` = librime 的 optional，缺了不算失敗
        assertEquals(true, SchemaPreflight.includeTarget("melt_eng:/translator?")?.optional)
        assertEquals(false, SchemaPreflight.includeTarget("melt_eng:/translator")?.optional)
    }

    @Test
    fun `同檔案內的 __include 不會被算成缺檔`() {
        // 整份倉頡的形狀：頂層有 array30_format，底下四處 __include 它。
        val r = report(
            "array30",
            """
            schema:
              schema_id: array30
            translator:
              dictionary: array30
            array30_format:
              comment_format:
                - "xlit|abc|ABC|"
            simplifier:
              __include: array30_format
            reverse_lookup:
              __include: array30_format
            """.trimIndent(),
            setup("array30.dict.yaml"),
        )
        assertTrue("同檔案內的節點被當成缺檔了", r.ok)
        assertEquals(emptyList<String>(), r.missing.map { it.fileName })
    }

    @Test
    fun `同一個檔案被兩處引用時取比較嚴的那一個`() {
        // 主詞典與反查用同一本的方案存在（例如某些單一碼表方案）。
        // 收集順序不該決定它是擋還是放行。
        val dirs = setup()
        val a = report(
            "rq_same",
            "translator:\n  dictionary: rq_x\nreverse_lookup:\n  dictionary: rq_x\n",
            dirs,
        )
        val b = report(
            "rq_same",
            "reverse_lookup:\n  dictionary: rq_x\ntranslator:\n  dictionary: rq_x\n",
            dirs,
        )
        assertFalse(a.ok)
        assertFalse("換個書寫順序就從擋變成放行，那是排序決定的結果，不是規則", b.ok)
    }

    @Test
    fun `假索引裡缺詞典的套件會被抓出來，正常的不會`() {
        val dirs = setup()
        // rq-nodict 的 schema 指向 rq_absent，詞典根本不存在。
        val bad = SchemaPreflight.checkText(
            "rq_nodict", NODICT_SCHEMA, dirs,
        )
        assertEquals(listOf("rq_absent.dict.yaml"), bad.map { it.fileName })

        File(shared, "rq_demo.dict.yaml").writeText("# stub\n")
        assertTrue(SchemaPreflight.checkText("rq_demo", DEMO_SCHEMA, dirs).isEmpty())
    }

    @Test
    fun `解析不了的方案檔不亂猜`() {
        // 誤報會擋住合法套件，比漏報更糟；解析不了就交給 librime 自己報錯。
        val dirs = setup()
        assertTrue(SchemaPreflight.checkText("x", "\t這不是合法 YAML: [", dirs).isEmpty())
    }

    /**
     * 八種（嚴重度 × 種類）各自指到自己的樣板，一個都不准共用。
     *
     * ── 為什麼用反射比對資源**名稱** ────────────────────────────────────
     * `R.string.*` 只是 int，`preflight_block_schema` 與 `preflight_warn_schema`
     * 兩個常數長得一模一樣，寫錯一個字母編譯照過。名稱是 aapt 從 xml 的
     * `name` 生出來的，所以「欄位名對得上」就等於「指對了樣板」。
     * 同一招見 `DiagnosticStringsTest`。
     *
     * 這條抓的是最傷的手滑：把 BLOCKING 指到 `preflight_warn_*`，
     * 於是「這個方案裝不起來」被寫成「不影響你打字」，使用者按下啟用之後
     * 得到一個編不起來的鍵盤，而畫面上剛剛才跟他說不要緊。
     */
    @Test
    fun `每一種缺檔都指到自己那一份樣板`() {
        val seen = LinkedHashMap<String, String>()
        for (severity in SchemaPreflight.Severity.values()) {
            for (kind in SchemaPreflight.Kind.values()) {
                val m = SchemaPreflight.Missing(kind, "a.dict.yaml", "schema-x", severity)
                val name = resourceNameOf(m.uiMessage().id)
                val expected =
                    if (severity == SchemaPreflight.Severity.BLOCKING) "preflight_block_"
                    else "preflight_warn_"
                assertTrue(
                    "$severity/$kind 指到了 $name，應該是 $expected 開頭",
                    name.startsWith(expected),
                )
                assertEquals(
                    "$severity/$kind 的參數應該是（方案名, 檔名）",
                    listOf<Any>("schema-x", "a.dict.yaml"),
                    m.uiMessage().args,
                )
                seen[name] = "$severity/$kind"
            }
        }
        assertEquals("八種組合共用了樣板：$seen", 8, seen.size)
    }

    /** `R.string` 的欄位名。找不到就直接紅 —— 那代表那個 id 根本不是字串資源。 */
    private fun resourceNameOf(id: Int): String {
        val f = org.luminakey.ime.R.string::class.java.fields
            .firstOrNull { it.getInt(null) == id }
        assertTrue("R.string 裡找不到 id=$id 的欄位", f != null)
        return f!!.name
    }

    private companion object {
        const val DEMO_SCHEMA = """
schema:
  schema_id: rq_demo
engine:
  translators:
    - table_translator
translator:
  dictionary: rq_demo
"""
        const val NODICT_SCHEMA = """
schema:
  schema_id: rq_nodict
translator:
  dictionary: rq_absent
"""
    }
}
