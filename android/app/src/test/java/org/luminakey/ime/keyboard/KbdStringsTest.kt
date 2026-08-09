package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory

/**
 * `res` 底下每一個 `values…` 目錄裡的每一份 `strings….xml`，三種語系的**形狀**
 * 必須一致 —— 每一份檔案，不是某一份。
 *
 * ── ⚠ 為什麼不是只驗自己新增的那一份 ────────────────────────────────────
 * 專案既有的 `StringCatalogTest` 把檔名寫死成 `strings.xml`：
 *
 *     private fun load(dir: String) = File(resRoot, "$dir/strings.xml")
 *
 * 於是每一條支線照規矩開的 `strings_<支線>.xml` **一份都沒有被檢查過**。
 * `store/DictStringsTest.kt` 已經記錄了這個坑，但它的解法是「再抄一份、只看
 * 自己那一份」—— 那條路走下去，第 N 條支線就有第 N 份幾乎一樣的測試，而**下一份
 * 新檔案仍然是沒人看的**。缺一個 key 的下場是那一句在該語言下靜靜地顯示英文：
 * 畫面不會壞、build 不會紅，正是本專案一再踩到的「測試會安靜地跳過自己」。
 *
 * 所以這一支改成**掃目錄**：`values/` 底下叫 `strings*.xml` 的每一個檔案都要驗。
 * 新開一份 `strings_xxx.xml` 不必也不該再寫一支測試 —— 它自動被涵蓋。
 * （`StringCatalogTest` 與 `DictStringsTest` 是別條線的檔案，本支線不動它們；
 * 覆蓋範圍重疊沒有害處，會出事的是**邏輯**重複，而這裡只有一份邏輯。）
 *
 * 只看形狀，不看內容（內容要人來讀）：
 *   1. `<string>` 的 key 集合完全相同；
 *   2. 同一個 key 的 placeholder 集合相同（少一個 `%1$s` 會在帶參數呼叫時直接
 *      丟 `MissingFormatArgumentException`）；
 *   3. 每一份翻譯檔都得**存在** —— 整份漏掉是最容易發生、也最容易被忽略的一種；
 *   4. 回落語系（`values/`）不得出現漢字。
 */
class KbdStringsTest {

    private val default = "values"
    private val translations = listOf("values-b+zh+Hant", "values-b+zh+Hans")

    /** 掃出來的檔名清單本身要先被檢查：掃到 0 個檔案的測試會全部「通過」。 */
    @Test
    fun `掃得到每一份 strings 檔`() {
        val files = stringFiles()
        assertTrue("在 ${File(resRoot, default).absolutePath} 底下掃不到任何 strings*.xml", files.isNotEmpty())
        assertTrue("至少該掃到 strings.xml 與本支線的 strings_kbd.xml，實際：$files", files.size >= 2)
        assertTrue("本支線的 strings_kbd.xml 不見了：$files", files.contains("strings_kbd.xml"))
    }

    @Test
    fun `每一份翻譯的 key 集合都與英文預設完全相同`() {
        for (name in stringFiles()) {
            val base = load(default, name)
            assertTrue("$default/$name 是空的", base.isNotEmpty())
            for (locale in translations) {
                val f = File(resRoot, "$locale/$name")
                assertTrue(
                    "$locale 少了整份 $name —— 那一整組字串會靜靜地顯示英文",
                    f.isFile,
                )
                assertEquals(
                    "$locale/$name 的 key 與 $default 不一致（左＝預設）",
                    base.keys.sorted(),
                    load(locale, name).keys.sorted(),
                )
            }
        }
    }

    @Test
    fun `同一個 key 的 placeholder 在每一種語言都一樣`() {
        for (name in stringFiles()) {
            val base = load(default, name)
            for (locale in translations) {
                val other = load(locale, name)
                for ((key, text) in base) {
                    assertEquals(
                        "$locale/$name 的 $key placeholder 不一致 —— " +
                            "少一個會在帶參數呼叫時丟 MissingFormatArgumentException",
                        placeholdersOf(text),
                        placeholdersOf(other[key].orEmpty()),
                    )
                }
            }
        }
    }

    /**
     * 回落語系必須是英文。
     *
     * 抓的是最容易犯的錯：新增字串時順手寫了中文放進 `values/`，於是法國使用者
     * 看到中文 —— 而三份的 key 完全一致，上面那幾條一個都不會叫。
     */
    @Test
    fun `英文預設裡不出現漢字`() {
        for (name in stringFiles()) {
            val offenders = load(default, name)
                .filterValues { v -> v.any { it.code in 0x4E00..0x9FFF } }
                .keys.sorted()
            assertTrue("$default/$name 是回落語系，必須是英文。這幾個 key 帶漢字：$offenders", offenders.isEmpty())
        }
    }

    /* ────────────────────────────── 讀檔 ────────────────────────────── */

    private fun stringFiles(): List<String> =
        File(resRoot, default).listFiles().orEmpty()
            .filter { it.isFile && it.name.startsWith("strings") && it.name.endsWith(".xml") }
            .map { it.name }
            .sorted()

    private fun load(dir: String, name: String): Map<String, String> {
        val f = File(resRoot, "$dir/$name")
        assertTrue("找不到 ${f.path}", f.isFile)
        val doc = DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(f)
        val out = LinkedHashMap<String, String>()
        val nodes = doc.getElementsByTagName("string")
        for (i in 0 until nodes.length) {
            val e = nodes.item(i)
            val key = e.attributes?.getNamedItem("name")?.nodeValue ?: continue
            out[key] = e.textContent.orEmpty()
        }
        return out
    }

    private fun placeholdersOf(s: String): Set<String> =
        PLACEHOLDER.findAll(s).map { it.value }.toSet()

    private companion object {
        val PLACEHOLDER = Regex("""%(\d+\$)?[sdf]""")

        /** 單元測試的工作目錄是模組目錄（`android/app`），資源就在腳下。 */
        val resRoot = File("src/main/res")
    }
}
