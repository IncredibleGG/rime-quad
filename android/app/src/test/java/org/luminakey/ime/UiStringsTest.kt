package org.luminakey.ime

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory

/**
 * `strings_ui.xml` 三份的形狀一致（`StringCatalogTest` 對 `strings.xml` 做的同一件事）。
 *
 * ── 為什麼要再寫一份，而不是把 StringCatalogTest 的範圍改大 ────────────────
 * `StringCatalogTest` 寫死只讀 `$dir/strings.xml`，而 `strings_diag.xml` 與
 * `strings_dict.xml` 各自有自己的測試、各自有自己的規則（診斷那一份**刻意**
 * 只有英文）。把範圍粗暴地改成 `strings*.xml` 會讓那兩份被套上不適用的規則。
 *
 * 但更重要的是：**新開一個字串檔而沒有人守它，是這種缺陷最典型的長法。**
 * 少一個 key 的下場是那一句在該語言下**靜靜地顯示英文**——畫面不會壞、
 * log 不會叫、build 不會紅。所以這個檔案一出生就配一份守門。
 */
class UiStringsTest {

    private val default = "values"
    private val translations = listOf("values-b+zh+Hant", "values-b+zh+Hans")

    @Test
    fun `三份的 key 集合完全相同`() {
        val base = load(default)
        // G2：範圍非空。檔名打錯、檔案沒被放進去時，這裡先紅。
        assertTrue("values/strings_ui.xml 只讀到 ${base.size} 條，大概是路徑錯了", base.size >= MIN)
        for (locale in translations) {
            assertEquals(
                "$locale/strings_ui.xml 的 key 與 $default 不一致（左＝預設，右＝$locale）",
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
                    PLACEHOLDER.findAll(text).map { it.value }.toSet(),
                    PLACEHOLDER.findAll(other[key] ?: "").map { it.value }.toSet(),
                )
            }
        }
    }

    /**
     * 預設那一份**不可以有中文**。
     *
     * 抓的是最容易犯的錯：新增字串時順手寫進 `values/`，但寫的是中文 ——
     * 於是一個法國使用者看到的就是中文，而三份的 key 集合完全一致，
     * 上面那兩個測試一個都不會叫。
     */
    @Test
    fun `英文預設裡不出現漢字`() {
        val offenders = load(default)
            .filterValues { v -> v.any { it.code in 0x4E00..0x9FFF } }
            .keys.sorted()
        assertTrue(
            "values/strings_ui.xml 是回落語系，必須是英文。這幾個 key 帶漢字：$offenders",
            offenders.isEmpty(),
        )
    }

    /**
     * 無障礙用的字串**三種語言都要有**，而且不可以是空的。
     *
     * 空字串在 XML 裡完全合法，key 集合也一樣 —— 但 TalkBack 念到的是一片沉默，
     * 和沒有 `contentDescription` 的效果一模一樣。這是「形狀對、內容空」的
     * 典型漏法，所以單獨守一條。
     */
    @Test
    fun `無障礙字串每一種語言都有內容`() {
        val blanks = mutableListOf<String>()
        for (locale in listOf(default) + translations) {
            load(locale).filterKeys { it.startsWith("a11y_") }
                .filterValues { it.isBlank() }
                .keys.forEach { blanks += "$locale/$it" }
        }
        assertTrue("這幾條無障礙字串是空的，TalkBack 會念一片沉默：$blanks", blanks.isEmpty())
        assertTrue(
            "一條 a11y_ 字串都沒有 —— 這代表檔案讀錯了，不代表不需要無障礙",
            load(default).keys.count { it.startsWith("a11y_") } >= MIN_A11Y,
        )
    }

    private fun load(dir: String): Map<String, String> {
        val f = File(resRoot, "$dir/strings_ui.xml")
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

    companion object {
        private val resRoot = File("src/main/res")
        private val PLACEHOLDER = Regex("""%(\d+\$)?[sdf]""")
        private const val MIN = 20
        private const val MIN_A11Y = 6
    }
}
