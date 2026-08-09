package org.luminakey.ime

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.prefs.PrefLevels
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory

/**
 * 三份 strings.xml 的**形狀**必須一模一樣。
 *
 * ── 為什麼這件事要用測試守，而不是靠人看 ────────────────────────────────
 * 少一個 key 的下場是：那一句在該語言下**靜靜地顯示英文**。畫面不會壞、
 * log 不會叫、build 不會紅 —— 而繁體使用者看到一句英文夾在中文裡，只會覺得
 * 這個 app 做得很隨便。這是「不會有人發現」等級的缺陷，正是自動化該接手的地方。
 *
 * 這裡不看**內容**（那要人來讀），只看四件形狀上的事：
 *   1. `<string>` 的 key 集合完全相同；
 *   2. `<plurals>` / `<string-array>` 的 key 集合完全相同；
 *   3. 同一個 key 的 **placeholder 集合**相同 —— 翻譯漏掉 `%1$s` 不只是少一段
 *      資訊，帶格式參數呼叫時還會直接丟 `MissingFormatArgumentException`；
 *   4. `<string-array>` 的**項數**相同，而且與 [PrefLevels] 的檔位數對得上 ——
 *      分段控制的第 n 格對應 `indexOf*()` 回傳的 n，數量錯開的畫面看起來完全
 *      正常，只是選「中」得到「小」。
 *
 * ── 為什麼直接讀檔而不是讀 R ─────────────────────────────────────────────
 * JVM 單元測試裡拿不到編譯後的資源表，而且就算拿得到，aapt 早就把「這個語系
 * 缺這個 key」回落成預設值了 —— 到那時已經看不出少了什麼。要抓的正是源頭。
 */
class StringCatalogTest {

    /** 預設語系是**英文**：找不到使用者語系時 Android 回落到這裡。 */
    private val default = "values"
    private val translations = listOf("values-b+zh+Hant", "values-b+zh+Hans")

    @Test
    fun `每一份翻譯的 key 集合都與英文預設完全相同`() {
        val base = load(default)
        for (locale in translations) {
            val other = load(locale)
            assertEquals(
                "$locale 的 <string> key 與 $default 不一致（左＝預設，右＝$locale）",
                base.strings.keys.sorted(),
                other.strings.keys.sorted(),
            )
            assertEquals(
                "$locale 的 <plurals> key 與 $default 不一致",
                base.plurals.sorted(),
                other.plurals.sorted(),
            )
            assertEquals(
                "$locale 的 <string-array> key 與 $default 不一致",
                base.arrays.keys.sorted(),
                other.arrays.keys.sorted(),
            )
        }
    }

    @Test
    fun `同一個 key 的 placeholder 在每一種語言都一樣`() {
        val base = load(default)
        for (locale in translations) {
            val other = load(locale)
            for ((key, text) in base.strings) {
                val expected = placeholdersOf(text)
                val actual = placeholdersOf(other.strings[key] ?: continue)
                assertEquals(
                    "$locale 的 $key placeholder 不一致 —— " +
                        "少一個會在帶參數呼叫時丟 MissingFormatArgumentException",
                    expected,
                    actual,
                )
            }
        }
    }

    @Test
    fun `string-array 的項數在每一種語言都一樣`() {
        val base = load(default)
        for (locale in translations) {
            val other = load(locale)
            for ((key, items) in base.arrays) {
                assertEquals(
                    "$locale 的 $key 項數與 $default 不一致 —— " +
                        "分段控制的第 n 格會對到別的值",
                    items,
                    other.arrays[key],
                )
            }
        }
    }

    @Test
    fun `檔位陣列的長度與 PrefLevels 的檔位數相同`() {
        val base = load(default)
        // PrefLevels 是純函式（數字），標籤在 strings.xml（字）。兩邊都由這裡對齊。
        assertEquals(PrefLevels.SOUND_LABELS.size, base.arrays["levels_sound"])
        assertEquals(PrefLevels.HAPTIC_LABELS.size, base.arrays["levels_haptic"])
        assertEquals(PrefLevels.LONG_PRESS_LABELS.size, base.arrays["levels_long_press"])
        assertEquals(PrefLevels.CANDIDATE_COUNT_LABELS.size, base.arrays["levels_candidate_count"])
        assertEquals(PrefLevels.CANDIDATE_SIZE_LABELS.size, base.arrays["levels_candidate_size"])
    }

    /**
     * 預設那一份**不可以有中文**。
     *
     * 這條看起來多餘，其實抓的是最容易犯的錯：改動時順手把新字串加進
     * `values/strings.xml`，但寫的是中文 —— 於是一個法國使用者看到的就是中文，
     * 而且三份的 key 集合完全一致，上面那幾個測試一個都不會叫。
     */
    @Test
    fun `英文預設裡不出現漢字`() {
        val offenders = load(default).strings
            .filterValues { v -> v.any { it.code in 0x4E00..0x9FFF } }
            .keys
            .sorted()
        assertTrue(
            "values/strings.xml 是回落語系，必須是英文。這幾個 key 帶漢字：$offenders",
            offenders.isEmpty(),
        )
    }

    /* ────────────────────────────── 讀檔 ────────────────────────────── */

    private class Catalog(
        val strings: Map<String, String>,
        val plurals: List<String>,
        /** key → item 數 */
        val arrays: Map<String, Int>,
    )

    private fun load(dir: String): Catalog {
        val f = File(resRoot, "$dir/strings.xml")
        assertTrue("找不到 ${f.path}", f.isFile)
        val doc = DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(f)

        val strings = LinkedHashMap<String, String>()
        val nodes = doc.getElementsByTagName("string")
        for (i in 0 until nodes.length) {
            val e = nodes.item(i)
            val name = e.attributes?.getNamedItem("name")?.nodeValue ?: continue
            strings[name] = e.textContent.orEmpty()
        }

        val plurals = ArrayList<String>()
        val pn = doc.getElementsByTagName("plurals")
        for (i in 0 until pn.length) {
            pn.item(i).attributes?.getNamedItem("name")?.nodeValue?.let { plurals += it }
        }

        val arrays = LinkedHashMap<String, Int>()
        val an = doc.getElementsByTagName("string-array")
        for (i in 0 until an.length) {
            val e = an.item(i)
            val name = e.attributes?.getNamedItem("name")?.nodeValue ?: continue
            var count = 0
            val kids = e.childNodes
            for (k in 0 until kids.length) if (kids.item(k).nodeName == "item") count++
            arrays[name] = count
        }
        return Catalog(strings, plurals, arrays)
    }

    /** `%1$s`、`%2$d`… 一律收斂成集合：順序由譯者決定，出現的**參數**不能少。 */
    private fun placeholdersOf(s: String): Set<String> =
        PLACEHOLDER.findAll(s).map { it.value }.toSet()

    companion object {
        private val PLACEHOLDER = Regex("""%(\d+\$)?[sdf]""")

        /**
         * 單元測試的工作目錄是模組目錄（`android/app`），所以資源就在腳下。
         * 這一行若哪天壞了，第一個測試會以「找不到檔案」失敗，不會靜默通過。
         */
        private val resRoot = File("src/main/res")
    }
}
