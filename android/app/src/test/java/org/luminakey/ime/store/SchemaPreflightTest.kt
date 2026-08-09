package org.luminakey.ime.store

import org.junit.Assert.assertEquals
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
        assertTrue(missing[0].humanMessage().contains("terra_pinyin.dict.yaml"))
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
    fun `反查詞典與 dependencies 也算在內`() {
        // 注音方案的真實形狀：translator 用 terra_pinyin，另外要 stroke 做筆畫反查。
        val dirs = setup("terra_pinyin.dict.yaml")
        val missing = SchemaPreflight.checkText(
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
            missing.map { it.fileName }.toSet(),
        )
    }

    @Test
    fun `import_preset 與 __include 指向的配置檔也要在`() {
        val dirs = setup("default.yaml")
        val missing = SchemaPreflight.checkText(
            "x",
            """
            punctuator:
              import_preset: default
            speller:
              __include: pinyin:/speller
            """.trimIndent(),
            dirs,
        )
        assertEquals(listOf("pinyin.yaml"), missing.map { it.fileName })
    }

    @Test
    fun `同檔案內的 __include 不算外部相依`() {
        assertNull(SchemaPreflight.includeTarget("/speller/algebra"))
        assertEquals("pinyin.yaml", SchemaPreflight.includeTarget("pinyin:/speller"))
        assertEquals("symbols.yaml", SchemaPreflight.includeTarget("symbols.yaml:/symbols"))
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
