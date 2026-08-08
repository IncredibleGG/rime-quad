package org.rimequad.ime

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.rimequad.ime.theme.Diagnostic
import org.rimequad.ime.theme.DiagnosticCode
import org.rimequad.ime.theme.DiagnosticTerm
import org.rimequad.ime.theme.DiagnosticText
import java.io.File
import java.util.IllegalFormatException
import java.util.Locale
import javax.xml.parsers.DocumentBuilderFactory

/**
 * 每一個診斷 code 都要有三份樣板（英／繁／簡），**缺一份就紅**。
 *
 * ── 為什麼不能靠回退 ────────────────────────────────────────────────
 * 少一條翻譯時，Android 會安靜地回落到 `values/` 的英文，畫面不會壞、log 不會
 * 叫、build 不會紅 —— 一個繁體使用者看到一句英文夾在中文裡，只會覺得這個 app
 * 做得很隨便。這是「不會有人發現」等級的缺陷，正是自動化該接手的地方。
 * [DiagnosticText] 執行期確實會回退（§6.5 規定診斷**不得**因為查不到而消失），
 * 但那是最後一道保險，不是「翻譯可以漏」的許可。這個檔案就是那條紀律。
 *
 * ── 為什麼直接讀 xml 而不是讀 R ─────────────────────────────────────
 * JVM 單元測試拿不到編譯後的資源表，而且就算拿得到，aapt 早就把「這個語系缺
 * 這個 key」回落成預設值了 —— 到那時已經看不出少了什麼。要抓的正是源頭。
 * （`R` 這個類別本身還是用得上：它的欄位名就是資源名，拿來反查
 * `DiagnosticStrings` 的對照表有沒有指錯。）
 *
 * ⚠ [StringCatalogTest] **不涵蓋這個檔案** —— 它的路徑寫死 `strings.xml`。
 * 已回報 docs/coordination.md §5：每一條支線新增的 `strings_<支線>.xml`
 * 都要自己帶一份形狀測試，否則就是一個安靜地不檢查的檢查。
 */
class DiagnosticStringsTest {

    private val default = "values"
    private val translations = listOf("values-b+zh+Hant", "values-b+zh+Hans")

    /** 期待存在的資源名：由 code 與 term **純函式推導**，不是另抄一份清單。 */
    private val expected: List<String>
        get() = (
            DiagnosticCode.values().flatMap { it.resourceNames } +
                DiagnosticTerm.ALL_RESOURCE_NAMES
            ).sorted()

    @Test
    fun `三份 strings_diag 的 key 集合都恰好等於所有 code 與代號`() {
        assertTrue("推導不出任何資源名，這條測試已經失效", expected.size >= 50)
        for (dir in listOf(default) + translations) {
            assertEquals(
                "$dir/strings_diag.xml 的 key 與 DiagnosticCode 對不上（左＝應有，右＝實際）。" +
                    "少一條的下場是那一則診斷在該語言下顯示英文開發訊息。",
                expected,
                load(dir).keys.sorted(),
            )
        }
    }

    @Test
    fun `同一個 key 的位置參數在每一種語言都一樣`() {
        val base = load(default)
        for (dir in translations) {
            val other = load(dir)
            for ((key, text) in base) {
                assertEquals(
                    "$dir 的 $key 位置參數與英文不一致 —— " +
                        "少一個會在帶參數呼叫時丟 MissingFormatArgumentException",
                    placeholdersOf(text),
                    placeholdersOf(other.getValue(key)),
                )
            }
        }
    }

    /**
     * 樣板用到的參數個數必須等於該 code 宣告的 arity。
     *
     * 這條同時抓兩種手滑：樣板寫少了（畫面上少一段資訊），以及
     * [DiagnosticStrings.resIdFor] 把某個 code 指到了別人的樣板 —— 只要兩者的
     * 參數個數不同就會露餡。個數相同的那種由下面的反射比對接手。
     */
    @Test
    fun `樣板的參數個數與 code 宣告的一致`() {
        for (dir in listOf(default) + translations) {
            val strings = load(dir)
            val wrong = mutableListOf<String>()
            for (code in DiagnosticCode.values()) {
                for (n in code.arity) {
                    val name = code.resourceName(n)
                    val used = placeholdersOf(strings.getValue(name))
                    val highest = used.mapNotNull { indexOf(it) }.maxOrNull() ?: 0
                    if (highest != n) {
                        wrong += "$dir/$name：用到 %$highest，但 ${code.id} 的參數是 $n 個"
                    }
                }
            }
            for (term in DiagnosticTerm.values()) {
                val used = placeholdersOf(strings.getValue(term.resourceName))
                if (used.isNotEmpty()) wrong += "$dir/${term.resourceName} 不該有位置參數"
            }
            assertTrue(wrong.joinToString("\n  ", prefix = "\n  "), wrong.isEmpty())
        }
    }

    /**
     * 預設那一份**不可以有漢字**。
     *
     * 抓的是最容易犯的錯：新增樣板時順手寫了中文，於是一個法國使用者看到的
     * 就是中文，而三份的 key 集合完全一致，上面每一條都不會叫。
     */
    @Test
    fun `英文預設裡不出現漢字`() {
        val offenders = load(default)
            .filterValues { v -> v.any { it.code in 0x4E00..0x9FFF } }
            .keys.sorted()
        assertTrue(
            "values/strings_diag.xml 是回落語系，必須是英文。這幾個 key 帶漢字：$offenders",
            offenders.isEmpty(),
        )
    }

    /**
     * `DiagnosticStrings` 的 `when` 沒有指錯資源。
     *
     * `R.string` 的欄位名就是 xml 裡的 `name`（aapt 生的），所以「欄位名等於
     * 推導出來的資源名」就等於「指對了」。沒有這一條，
     * `BAD_COLOR -> R.string.diag_bad_bool` 這種手滑會一路活到使用者眼前。
     */
    @Test
    fun `每一個 code 都對到同名的資源`() {
        val byName = R.string::class.java.fields.associate { it.name to it.getInt(null) }
        assertTrue("R.string 讀不到任何欄位，這條測試已經失效", byName.size >= 50)

        val wrong = mutableListOf<String>()
        for (code in DiagnosticCode.values()) {
            for (n in code.arity) {
                val name = code.resourceName(n)
                val want = byName[name]
                if (want == null) {
                    wrong += "R.string.$name 不存在"
                } else if (DiagnosticStrings.resIdFor(code, n) != want) {
                    wrong += "${code.id}（$n 個參數）指到的不是 R.string.$name"
                }
            }
        }
        for ((term, id) in DiagnosticStrings.TERM_IDS) {
            val want = byName[term.resourceName]
            if (want == null) {
                wrong += "R.string.${term.resourceName} 不存在"
            } else if (id != want) {
                wrong += "${term.id} 指到的不是 R.string.${term.resourceName}"
            }
        }
        assertTrue(wrong.joinToString("\n  ", prefix = "\n  "), wrong.isEmpty())

        // 反向：比對邏輯必須抓得到一個刻意指錯的對照。
        val planted = byName.getValue("diag_bad_bool") != byName.getValue("diag_bad_color")
        assertTrue("兩個不同的資源竟然是同一個 id，比對沒有鑑別力", planted)
    }

    /**
     * 每一個 `%` 都必須是**帶編號的**（`%1${'$'}s`），一個裸的 `%s` 都不行。
     *
     * ── 為什麼這條要單獨守 ──────────────────────────────────────────
     * 譯者把 `%2${'$'}s` 打成 `%2s` 的時候，Java 的 Formatter **不會丟例外**：
     * 它把那個當成「寬度 2 的無編號轉換」，於是安靜地取了**下一個**參數。
     * 畫面上該出現第二個參數的地方出現第一個，長度、標點、語氣全都正常，
     * 只是內容錯了。這正是「不會有人發現」的形狀。
     *
     * 實測過：只靠「位置參數集合相同」與「填得起來」兩條，`%2s` 兩條都過。
     */
    @Test
    fun `樣板裡的每一個百分號都帶編號`() {
        val offenders = mutableListOf<String>()
        for (dir in listOf(default) + translations) {
            for ((name, text) in load(dir)) {
                if (hasLoosePercent(text)) offenders += "$dir/$name：$text"
            }
        }
        assertTrue(
            "這幾條樣板裡有沒帶編號的 %，Formatter 會安靜地取錯參數：\n  " +
                offenders.joinToString("\n  "),
            offenders.isEmpty(),
        )

        // 反向：檢查樣式必須抓得到一句真的寫壞的樣板。
        assertTrue(
            "抓不到 %2s，這條檢查沒有鑑別力",
            hasLoosePercent("同時設「%1${'$'}s」與「%2s」"),
        )
        assertTrue(
            "把好的樣板誤判成壞的",
            !hasLoosePercent("同時設「%1${'$'}s」與「%2${'$'}s」，100%% 確定"),
        )
    }

    /**
     * 每一份樣板都要能**真的被填**。
     *
     * `Context.getString(id, *args)` 底下就是 `String.format`，所以這裡直接拿
     * xml 讀出來的樣板做一次格式化。抓的是那種只在某一種語言下才會炸的錯：
     * 譯者把 `%2${'$'}s` 打成 `%2s`、或不小心留下一個裸的 `%`。那時使用者看到的
     * 不是一句翻壞的話，是輸入法在解析壞主題的當下**丟例外**。
     *
     * ⚠ 這條**不能**取代真機驗證：它證明的是「填得起來」，不是「Android 在
     * zh-Hant 環境下真的挑到這一份」。後者只有裝上去才算數。
     */
    @Test
    fun `每一份樣板都填得起來`() {
        for (dir in listOf(default) + translations) {
            val strings = load(dir)
            for (code in DiagnosticCode.values()) {
                for (n in code.arity) {
                    val name = code.resourceName(n)
                    val args = Array<Any>(n) { "arg${it + 1}" }
                    val text = try {
                        String.format(Locale.ROOT, strings.getValue(name), *args)
                    } catch (e: IllegalFormatException) {
                        throw AssertionError("$dir/$name 填不起來：$e", e)
                    }
                    for (i in 1..n) {
                        assertTrue("$dir/$name 沒有用到第 $i 個參數", text.contains("arg$i"))
                    }
                }
            }
        }
    }

    /* ── 樣板挑選：arity 變體與回退 ────────────────────────────────── */

    @Test
    fun `有猜測時用兩個參數的樣板，沒有時用一個`() {
        val seen = mutableListOf<String>()
        DiagnosticText.install { _, name, _ -> seen += name; "ok" }
        try {
            DiagnosticText.render(
                Diagnostic(DiagnosticCode.UNKNOWN_FIELD, listOf("bakcground"), "keyboard")
            )
            DiagnosticText.render(
                Diagnostic(
                    DiagnosticCode.UNKNOWN_FIELD, listOf("bakcground", "background"), "keyboard"
                )
            )
        } finally {
            DiagnosticText.uninstall()
        }
        assertEquals(listOf("diag_unknown_field", "diag_unknown_field_2"), seen)
    }

    @Test
    fun `查不到樣板時退化成開發者訊息，而不是讓診斷消失`() {
        DiagnosticText.install { _, _, _ -> null }
        try {
            val d = Diagnostic(DiagnosticCode.BAD_COLOR, listOf("#ZZZ"), "palette.bg", 7)
            val text = DiagnosticText.render(d)
            assertTrue(text, text.contains("bad_color"))
            assertTrue(text, text.contains("palette.bg"))
        } finally {
            DiagnosticText.uninstall()
        }
    }

    /* ────────────────────────────── 讀檔 ────────────────────────────── */

    private fun load(dir: String): Map<String, String> {
        val f = File("src/main/res/$dir/strings_diag.xml")
        assertTrue("找不到 ${f.path}", f.isFile)
        val doc = DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(f)
        val out = LinkedHashMap<String, String>()
        val nodes = doc.getElementsByTagName("string")
        for (i in 0 until nodes.length) {
            val e = nodes.item(i)
            val name = e.attributes?.getNamedItem("name")?.nodeValue ?: continue
            out[name] = e.textContent.orEmpty()
        }
        assertTrue("$dir/strings_diag.xml 一條字串都沒有", out.isNotEmpty())
        return out
    }

    /** 有沒有「不帶編號、也不是 `%%`」的百分號。 */
    private fun hasLoosePercent(text: String): Boolean =
        PERCENT_TOKEN.findAll(text).any { it.value != "%%" && it.groupValues[1].isEmpty() }

    /** `%1$s`、`%2$d`… 收斂成集合：語序由譯者決定，出現的**參數**不能少。 */
    private fun placeholdersOf(s: String): Set<String> =
        PLACEHOLDER.findAll(s).map { it.value }.toSet()

    /** `%3${'$'}s` → 3；沒有編號的（`%s`）回 null。 */
    private fun indexOf(placeholder: String): Int? =
        INDEX.find(placeholder)?.groupValues?.get(1)?.toIntOrNull()

    private companion object {
        private val PLACEHOLDER = Regex("""%(\d+\$)?[sdf]""")
        private val INDEX = Regex("""%(\d+)[${'$'}]""")

        /**
         * 逐個 `%` 掃過去：`%%` 是跳脫，`%<n>${'$'}` 是帶編號的，其餘都是裸的。
         *
         * 不用單一個 lookahead 的樣式 —— 那會把 `100%%` 的第二個 `%` 當成裸的，
         * 這裡先踩過一次。
         */
        private val PERCENT_TOKEN = Regex("""%%|%(\d+[${'$'}])?""")
    }
}
