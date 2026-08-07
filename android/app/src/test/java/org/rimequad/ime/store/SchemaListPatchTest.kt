package org.rimequad.ime.store

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/**
 * `default.custom.yaml` 的 schema_list patch 讀寫與回滾（規範 §3）。
 *
 * 回滾是整個市集裡最要緊的一段：少了它，一次失敗的導入會讓使用者卡在
 * 「每次啟動都部署失敗」而且沒有自救途徑。所以這裡把「還原之後必須
 * **逐位元組**等於導入前」當成硬性斷言，而不只是「schema_list 內容一樣」。
 */
class SchemaListPatchTest {

    @get:Rule
    val tmp = TemporaryFolder()

    /** collect_data.sh 產生的真實內容。 */
    private val shipped = """
        # 由 scripts/collect_data.sh 產生。
        #
        # 上游 rime-prelude 的 default.yaml 列出的方案多於本專案實際打包的，
        # 未打包的方案會在部署時報錯。這裡以 patch 覆寫 schema_list，
        # 只保留確實有詞庫的方案 —— 這樣上游 default.yaml 可以原封不動地更新。
        patch:
          schema_list:
            - schema: luna_pinyin_tw    # 拼音（臺灣字形）
            - schema: bopomofo_tw       # 注音（臺灣字形）
            - schema: luna_pinyin       # 拼音（原版）
    """.trimIndent() + "\n"

    private fun userDir(text: String? = shipped): File {
        val d = tmp.newFolder()
        if (text != null) File(d, SchemaListPatch.FILE_NAME).writeText(text)
        return d
    }

    @Test
    fun `讀得出隨附的 schema_list`() {
        assertEquals(
            listOf("luna_pinyin_tw", "bopomofo_tw", "luna_pinyin"),
            SchemaListPatch.read(userDir()),
        )
    }

    @Test
    fun `加入新方案後其餘內容原封不動`() {
        val d = userDir()
        assertEquals(listOf("rq_demo"), SchemaListPatch.enable(d, listOf("rq_demo")))
        val text = SchemaListPatch.file(d).readText()
        assertEquals(
            listOf("luna_pinyin_tw", "bopomofo_tw", "luna_pinyin", "rq_demo"),
            SchemaListPatch.read(d),
        )
        // 註解是使用者與打包腳本的資產，不能被寫入洗掉。
        assertTrue("開頭註解必須保留", text.contains("由 scripts/collect_data.sh 產生"))
        assertTrue(text.contains("未打包的方案會在部署時報錯"))
        // 行末註解也要在。使用者只是裝了一個方案，不該連帶失去自己檔案裡的說明。
        assertTrue("行末註解必須保留", text.contains("- schema: luna_pinyin_tw    # 拼音（臺灣字形）"))
        assertTrue(text.contains("- schema: bopomofo_tw       # 注音（臺灣字形）"))
    }

    @Test
    fun `重複加入不會產生重複項`() {
        val d = userDir()
        SchemaListPatch.enable(d, listOf("rq_demo"))
        assertEquals(emptyList<String>(), SchemaListPatch.enable(d, listOf("rq_demo")))
        assertEquals(1, SchemaListPatch.read(d).count { it == "rq_demo" })
    }

    @Test
    fun `停用只移除指定項`() {
        val d = userDir()
        assertEquals(listOf("bopomofo_tw"), SchemaListPatch.disable(d, listOf("bopomofo_tw")))
        assertEquals(listOf("luna_pinyin_tw", "luna_pinyin"), SchemaListPatch.read(d))
    }

    @Test
    fun `回滾之後檔案逐位元組等於導入前`() {
        val d = userDir()
        val snapshot = SchemaListPatch.snapshot(d)

        SchemaListPatch.enable(d, listOf("rq_bad", "rq_other"))
        assertTrue("rq_bad" in SchemaListPatch.read(d))
        assertFalse("先確認真的改過了", SchemaListPatch.file(d).readText() == shipped)

        SchemaListPatch.restore(d, snapshot)
        assertEquals(shipped, SchemaListPatch.file(d).readText())
        assertEquals(
            listOf("luna_pinyin_tw", "bopomofo_tw", "luna_pinyin"),
            SchemaListPatch.read(d),
        )
    }

    @Test
    fun `導入前檔案不存在時回滾會把它刪掉`() {
        val d = userDir(text = null)
        val snapshot = SchemaListPatch.snapshot(d)
        SchemaListPatch.enable(d, listOf("rq_demo"))
        assertTrue(SchemaListPatch.file(d).isFile)

        SchemaListPatch.restore(d, snapshot)
        assertFalse("原本沒有這個檔案，回滾後也不該有", SchemaListPatch.file(d).exists())
    }

    @Test
    fun `檔案裡沒有 patch 區塊時會補上`() {
        val d = userDir("# 只有註解\nfoo: bar\n")
        SchemaListPatch.enable(d, listOf("rq_demo"))
        assertEquals(listOf("rq_demo"), SchemaListPatch.read(d))
        assertTrue(SchemaListPatch.file(d).readText().contains("foo: bar"))
    }

    @Test
    fun `有 patch 但沒有 schema_list 時插進去且保留其他 patch 項`() {
        val d = userDir(
            """
            patch:
              "menu/page_size": 9
            """.trimIndent() + "\n"
        )
        SchemaListPatch.enable(d, listOf("rq_demo"))
        val text = SchemaListPatch.file(d).readText()
        assertEquals(listOf("rq_demo"), SchemaListPatch.read(d))
        assertTrue("其他 patch 項不能消失", text.contains("\"menu/page_size\": 9"))
    }

    @Test
    fun `純量寫法的 schema_list 也讀得出來`() {
        // 上游偶爾寫成 `- luna_pinyin` 而不是 `- schema: luna_pinyin`。
        assertEquals(
            listOf("luna_pinyin", "stroke"),
            SchemaListPatch.readFrom("patch:\n  schema_list:\n    - luna_pinyin\n    - stroke\n"),
        )
    }
}
