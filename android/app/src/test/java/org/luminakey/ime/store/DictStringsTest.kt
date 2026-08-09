package org.luminakey.ime.store

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory

/**
 * `strings_dict.xml` 三份的形狀必須一致。
 *
 * ── ⚠ 為什麼要再寫一份，而不是靠既有的 StringCatalogTest ─────────────────
 * 那一支（`app/src/test/java/org/luminakey/ime/StringCatalogTest.kt`）把檔名
 * **寫死成 `strings.xml`**：
 *
 *     private fun load(dir: String) = File(resRoot, "$dir/strings.xml")
 *
 * 所以每一條支線照專案規矩「開新的 strings_<支線>.xml」之後，那些字串
 * **一個都沒有被檢查過**。少一個 key 的下場是那一句在該語言下靜靜地顯示
 * 英文 —— 畫面不會壞、build 不會紅，正是這個專案一再踩到的
 * 「測試會安靜地跳過自己」。
 *
 * 短期解法是本檔（只看自己這一份）；長期應該把 StringCatalogTest 改成
 * 掃 `strings*.xml`，但那是共用檔案，已寫進 `docs/coordination.md` §5 回報。
 */
class DictStringsTest {

    private val fileName = "strings_dict.xml"
    private val default = "values"
    private val translations = listOf("values-b+zh+Hant", "values-b+zh+Hans")

    @Test
    fun `三份的 key 集合完全相同`() {
        val base = load(default)
        assertTrue("預設那一份不該是空的", base.isNotEmpty())
        for (locale in translations) {
            assertEquals(
                "$locale/$fileName 的 key 與 $default 不一致（左＝預設）",
                base.keys.sorted(),
                load(locale).keys.sorted(),
            )
        }
    }

    @Test
    fun `同一個 key 的 placeholder 在每一種語言都一樣`() {
        val base = load(default)
        for (locale in translations) {
            val other = load(locale)
            for ((key, text) in base) {
                assertEquals(
                    "$locale 的 $key placeholder 不一致 —— " +
                        "少一個會在帶參數呼叫時丟 MissingFormatArgumentException",
                    placeholdersOf(text),
                    placeholdersOf(other.getValue(key)),
                )
            }
        }
    }

    /**
     * 預設那一份是回落語系，**必須是英文**。
     * 順手寫成中文的話，一個法國使用者看到的就是中文，而上面兩條測試全綠。
     */
    @Test
    fun `英文預設裡不出現漢字`() {
        val offenders = load(default)
            .filterValues { v -> v.any { it.code in 0x4E00..0x9FFF } }
            .keys.sorted()
        assertTrue("values/$fileName 是回落語系，必須是英文。這幾個 key 帶漢字：$offenders", offenders.isEmpty())
    }

    /**
     * 這一份的每一個 key 都要以 `backup_` 開頭。
     *
     * 不是潔癖：`values/` 底下所有 xml 會被 aapt **併成同一張表**，
     * 兩條支線各自開檔卻撞名的話，贏的那一個由檔案順序決定 ——
     * 而畫面看起來完全正常，只是有一句話變成別人的。前綴是唯一的防線。
     */
    @Test
    fun `所有 key 都帶著本支線的前綴`() {
        val bad = load(default).keys.filterNot { it.startsWith("backup_") }.sorted()
        assertTrue("這幾個 key 沒有 backup_ 前綴，可能與別條支線撞名：$bad", bad.isEmpty())
    }

    /* ────────────────────────────── 讀檔 ────────────────────────────── */

    private fun load(dir: String): Map<String, String> {
        val f = File(resRoot, "$dir/$fileName")
        assertTrue("找不到 ${f.path}", f.isFile)
        val doc = DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(f)
        val out = LinkedHashMap<String, String>()
        val nodes = doc.getElementsByTagName("string")
        for (i in 0 until nodes.length) {
            val e = nodes.item(i)
            val name = e.attributes?.getNamedItem("name")?.nodeValue ?: continue
            out[name] = e.textContent.orEmpty()
        }
        return out
    }

    private fun placeholdersOf(s: String): Set<String> =
        PLACEHOLDER.findAll(s).map { it.value }.toSet()

    companion object {
        private val PLACEHOLDER = Regex("""%(\d+\$)?[sdf]""")

        /** 單元測試的工作目錄是模組目錄（`android/app`）。 */
        private val resRoot = File("src/main/res")
    }
}
