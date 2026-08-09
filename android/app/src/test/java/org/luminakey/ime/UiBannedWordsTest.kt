package org.luminakey.ime

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory

/**
 * 使用者看得到的字串裡，**一個引擎內部的詞都不准出現**（`docs/ui-design.md` §6.7 第一層）。
 *
 * ── 為什麼這一條值得寫 ──────────────────────────────────────────────────
 * 它第一次跑就抓到真的東西，而且其中一條**已經在出貨的 APK 裡**：
 * 一個方案都沒有的時候，使用者唯一會看到的那句話是
 * 「尚無可用方案（`rs_schema_list` 回傳空）」—— 一個 C ABI 函式名，
 * 出現在使用者最無助的那一刻。
 *
 * 這也是四端裡唯一一條「寫一次，之後每一次 commit 都在保護你」的檢查。
 *
 * ── 這個掃描器最可能怎麼失效（§2 的 G 段）────────────────────────────────
 * **不是漏抓，是掃到零個檔案然後全綠。** 本專案的產品識別碼守門腳本正是這樣
 * 在六項全錯的情況下 6/6 全綠的（`find` 的範圍不含放那六項的目錄）。
 * 所以這裡有三道自我防衛，缺一不可：
 *
 *   · **G2 範圍非空**：掃到的檔案要涵蓋 [REQUIRED_FILES] 的每一個，
 *     而且字串總數 ≥ [MIN_STRINGS]。範圍寫錯時的行為是**紅**，不是零個違規。
 *   · **G3 允許清單不可以活得比它的對象久**：清單裡的每一項都必須**還指得到**
 *     那個字串、而且那個字串裡**還真的有**那個違規字。別人把它修好之後，
 *     這條測試會紅，逼下一個人把清單項刪掉 —— 而不是讓一張永遠不會縮短的
 *     允許清單累積下去。
 *   · **G1 反向測試**：[`植入一個違規會被抓到`] 直接餵一段合成的 XML 進同一支
 *     掃描器，確認它真的會紅。沒有做過這一步的檢查，一律當作沒有。
 *
 * ── 為什麼讀 XML 的文字節點，不是 grep 整行 ──────────────────────────────
 * 這是實際踩到的：用 `grep -E 'rs_[a-z_]+'` 掃整行的話，
 * `<string name="summary_chars_follow">` 這個 **key 名**會命中
 * （`cha` + `rs_follow`），而它的內容完全乾淨。一個會叫錯的檢查會被關掉，
 * 所以這裡（1）只看文字內容，不看屬性；（2）西文詞一律加詞界。
 */
class UiBannedWordsTest {

    /* ─────────────── 1. 真的掃一次 ─────────────── */

    @Test
    fun `使用者看得到的字串裡沒有引擎內部的詞`() {
        val scanned = scanAll()

        // G2：範圍非空，而且涵蓋每一個該掃的檔案。
        val missing = REQUIRED_FILES.filterNot { it in scanned.files }
        assertTrue(
            "掃描範圍漏了這幾個檔案 —— 範圍寫錯的時候必須是紅，不是零個違規：$missing",
            missing.isEmpty(),
        )
        assertTrue(
            "只掃到 ${scanned.strings} 條字串，比基準 $MIN_STRINGS 少。" +
                "這通常代表範圍寫錯或檔案被搬走，而不是真的少了那麼多字串。",
            scanned.strings >= MIN_STRINGS,
        )

        // 真正的斷言。
        val unexpected = scanned.violations.filterNot { it.key() in ALLOWED }
        assertTrue(
            buildString {
                appendLine("這幾條使用者看得到的字串裡有引擎內部的詞（docs/ui-design.md §6.7 第一層）：")
                unexpected.forEach { appendLine("  ${it.locale}/${it.id}  「${it.hit}」  ← ${it.rule}") }
                appendLine("要嘛改字串，要嘛（若它真的只出現在診斷區塊）把 id 加進 ALLOWED 並寫清楚理由。")
            },
            unexpected.isEmpty(),
        )
    }

    /**
     * G3：允許清單裡的每一項都必須還指得到它的對象。
     *
     * 少了這一條，允許清單就會變成一張只增不減的名單：別人把
     * `keyboard_no_schema` 修好之後，那一項會**靜靜地繼續存在**，
     * 而下一個把 `rs_schema_list` 寫回去的人不會被擋下來。
     */
    @Test
    fun `允許清單裡的每一項都還指得到一個真的違規`() {
        val actual = scanAll().violations.map { it.key() }.toSet()
        val stale = ALLOWED.filterNot { it in actual }
        assertEquals(
            "允許清單裡這幾項已經沒有對應的違規了 —— 代表有人把它修好了。" +
                "請把它們從 ALLOWED 刪掉（允許清單不可以活得比它的對象久）：$stale",
            emptyList<String>(),
            stale,
        )
    }

    /* ─────────────── 2. 反向測試（G1）─────────────── */

    /**
     * 植入一個違規，確認它會紅。
     *
     * 餵的是合成的 XML，走的是**同一支** [scanText]。這一條要是不會叫，
     * 上面那條全綠就沒有任何意義。
     */
    @Test
    fun `植入一個違規會被抓到`() {
        val xml = """
            <resources>
              <string name="fake_ok">Pick how you want to type</string>
              <string name="fake_bad">Candidates per page (page_size)</string>
            </resources>
        """.trimIndent()
        val found = scanText("synthetic", xml)
        assertEquals(
            "植入 page_size 之後掃描器沒有叫 —— 那上面那條全綠不代表任何事",
            listOf("fake_bad"),
            found.map { it.id },
        )
        assertEquals("page_size", found.single().hit)
    }

    /** 反向測試的另一半：**乾淨的輸入不可以叫**。會亂叫的檢查一樣會被關掉。 */
    @Test
    fun `key 名裡的巧合不算違規`() {
        // `summary_chars_follow` 的 key 名含有 "cha|rs_follow"，用 grep 掃整行會命中。
        val xml = """
            <resources>
              <string name="summary_chars_follow">Characters follow the keyboard</string>
              <string name="hardware_note">Runs on ordinary microprocessors</string>
            </resources>
        """.trimIndent()
        assertEquals(
            "乾淨的字串被誤判成違規 —— 一個會叫錯的檢查會被關掉，那比沒有更糟",
            emptyList<String>(),
            scanText("synthetic", xml).map { it.id },
        )
    }

    /** 診斷載荷是規範明講的例外（§4.11）：它不是介面文字，是要被貼進 issue 的東西。 */
    @Test
    fun `diag_ 前綴的字串不受管`() {
        val xml = """
            <resources>
              <string name="diag_abi">librime ABI %1${'$'}s</string>
            </resources>
        """.trimIndent()
        assertEquals(emptyList<String>(), scanText("synthetic", xml).map { it.id })
    }

    /* ─────────────── 掃描器本體 ─────────────── */

    private class Violation(
        val locale: String,
        val id: String,
        val rule: String,
        val hit: String,
    ) {
        /** 允許清單的鍵：**id + 實際命中的那個字**。 */
        fun key() = "$id::$hit"
    }

    private class Scan(
        val files: Set<String>,
        val strings: Int,
        val violations: List<Violation>,
    )

    private fun scanAll(): Scan {
        val files = resRoot.listFiles()
            .orEmpty()
            .filter { it.isDirectory && it.name.startsWith("values") }
            .flatMap { dir ->
                dir.listFiles().orEmpty()
                    .filter { it.name.startsWith("strings") && it.name.endsWith(".xml") }
            }
            .sortedBy { it.path }

        val seen = LinkedHashSet<String>()
        var count = 0
        val out = ArrayList<Violation>()
        for (f in files) {
            val rel = "${f.parentFile.name}/${f.name}"
            seen += rel
            val doc = DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(f)
            val nodes = doc.getElementsByTagName("string")
            for (i in 0 until nodes.length) {
                val e = nodes.item(i)
                val id = e.attributes?.getNamedItem("name")?.nodeValue ?: continue
                count++
                // ⚠ textContent，不是整行。屬性（key 名）不參與比對，見類別註解。
                out += violationsIn(f.parentFile.name, id, e.textContent.orEmpty())
            }
        }
        return Scan(seen, count, out)
    }

    /** 給反向測試用的入口：同一套規則，輸入換成一段字串。 */
    private fun scanText(locale: String, xml: String): List<Violation> {
        val doc = DocumentBuilderFactory.newInstance().newDocumentBuilder()
            .parse(xml.byteInputStream())
        val nodes = doc.getElementsByTagName("string")
        val out = ArrayList<Violation>()
        for (i in 0 until nodes.length) {
            val e = nodes.item(i)
            val id = e.attributes?.getNamedItem("name")?.nodeValue ?: continue
            out += violationsIn(locale, id, e.textContent.orEmpty())
        }
        return out
    }

    private fun violationsIn(locale: String, id: String, text: String): List<Violation> {
        if (id.startsWith(DIAGNOSTIC_PREFIX)) return emptyList()
        val out = ArrayList<Violation>()
        for ((rule, regex) in RULES) {
            val m = regex.find(text) ?: continue
            out += Violation(locale, id, rule, m.value)
        }
        return out
    }

    companion object {
        /** 單元測試的工作目錄是模組目錄（`android/app`）。 */
        private val resRoot = File("src/main/res")

        /** §4.11：診斷區塊是唯一可以出現 id 與引擎詞的地方。 */
        private const val DIAGNOSTIC_PREFIX = "diag_"

        /**
         * G2 的下界。**這幾個檔案必須在掃描範圍裡**，否則就是範圍寫錯了。
         *
         * ⚠ G4：擴大範圍時要問一次「還有誰不在範圍內」。目前的答案是 ——
         * 這裡只掃 `res/values…/strings….xml`，也就是**在地化資源**。
         * 沒被掃到的還有：
         *   · Kotlin 裡寫死的字面字串（例如 `Text("✓")`）。那些幾乎都是符號，
         *     但**這條檢查擋不住有人在 Kotlin 裡寫死一句帶 `schema` 的中文**。
         *   · `core/themes`、`core/layouts` 的 yaml 裡的鍵面與標籤。
         *   · 另外三端的字串表（§6.7 的表列了各端自己的範圍）。
         * 這三項都還沒有人接。
         */
        private val REQUIRED_FILES = listOf(
            "values/strings.xml",
            "values/strings_diag.xml",
            "values/strings_dict.xml",
            "values/strings_ui.xml",
            "values-b+zh+Hant/strings.xml",
            "values-b+zh+Hant/strings_diag.xml",
            "values-b+zh+Hant/strings_dict.xml",
            "values-b+zh+Hant/strings_ui.xml",
            "values-b+zh+Hans/strings.xml",
            "values-b+zh+Hans/strings_diag.xml",
            "values-b+zh+Hans/strings_dict.xml",
            "values-b+zh+Hans/strings_ui.xml",
        )

        /** 2026-08-09 的實測值是 1497。取 §6.7 宣告的基準 1413 當下界。 */
        private const val MIN_STRINGS = 1413

        /**
         * **已知、尚未修好、而且不歸這條線修**的違規。
         *
         * 兩條都住在 `values…/strings.xml`，而且只被 `keyboard/KeyboardView.kt`
         * 引用 —— 那兩個路徑都不屬於這條線（`docs/coordination.md` §2）。
         * 所以這裡先記下來讓測試能綠，並靠上面那條 G3 測試保證：
         * **鍵盤那條線把它修好的那一刻，這張清單就會逼人把它刪掉。**
         *
         * · `keyboard_no_schema`  「尚無可用方案（rs_schema_list 回傳空）」
         *     —— ABI 函式名印在使用者畫面上，而且正好在他最無助的時刻。
         * · `keyboard_stub_notice`「⟦STUB⟧ 未接 librime，候選字為假資料」
         *     —— 開發期佔位訊息走了使用者字串資源。
         */
        private val ALLOWED = setOf(
            "keyboard_no_schema::rs_schema_list",
            "keyboard_stub_notice::STUB",
            "keyboard_stub_notice::librime",
            "keyboard_stub_notice::⟦STUB⟧",
        )

        /**
         * §6.7 第一層：引擎的內部識別字、ABI 函式名、除錯標記。
         *
         * ⚠ 西文詞一律加詞界 `(?<![A-Za-z0-9_])…(?![A-Za-z0-9_])`。
         * 少了它，`chars_follow` 會被 `rs_` 命中、`namespace` 會被
         * 任何含該子字串的英文命中 —— 見類別註解那段實際踩到的事。
         */
        private val RULES: List<Pair<String, Regex>> = buildList {
            val words = listOf(
                "schema_list", "page_size", "simplification", "ascii_punct",
                "full_shape", "half_shape", "speller", "translator", "segmentor",
                "processor", "prism", "opencc", "stabledb", "db_class",
                "table_translator", "user_dict", "custom_phrase", "preedit",
                "langid", "rime_shell", "librime", "applicationId", "namespace",
                "STUB", "TODO", "FIXME",
            )
            for (w in words) {
                add(w to Regex("(?<![A-Za-z0-9_])${Regex.escape(w)}(?![A-Za-z0-9_])"))
            }
            // 所有 ABI 函式名。`rs_` 前綴 + 小寫字母／底線。
            add("rs_*" to Regex("(?<![A-Za-z0-9_])rs_[a-z_]+"))
            // 開發期的佔位標記。
            add("⟦…⟧" to Regex("⟦.*?⟧"))
        }
    }
}
