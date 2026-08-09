package org.luminakey.ime.theme

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * 診斷模型的三道防線（docs/theme-format.md §6.5 / §6.5.1）。
 *
 *   1. **`severity` 由 `code` 決定，不由產生點決定。**
 *      規範說得很清楚為什麼：只要有一端把同一件事記成 INFO，「四端報一樣多則
 *      WARNING」就失守，而且失守得無聲無息。這裡守兩層 ——
 *      逐 code 與規範碼表的分組比對，以及**掃原始碼**確認沒有任何產生點
 *      自己挑等級。
 *   2. **同一個 `(severity, code, path)` 不重複報。**
 *      重複的那一則沒有帶任何新資訊給使用者，卻會讓四端的診斷序列對不上。
 *   3. **enum 與規範碼表逐項對得上。**
 *      碼表是從 `docs/theme-format.md` **當場讀出來的**，不是抄一份常數 ——
 *      抄的那一份會腐爛，而腐爛的方式正好是「規範改了，測試還是綠的」。
 *
 * ── 這些測試會不會在該紅的時候安靜地不跑 ─────────────────────────────
 * 三個地方會：碼表解析不到任何一列、佈局／主題夾具是空的、原始碼掃描掃不到
 * 檔案。三者都各有一條斷言把「掃到 0 個」判成失敗。另外每一條檢查都寫成
 * 純函式，並且**當場餵一份刻意違規的資料進去確認它會紅**（見各測試末段的
 * 「反向」斷言）—— 這比「我看它綠了」有意義。
 */
class DiagnosticCodeSpecTest {

    /* ─────────────────────── 1. severity 由 code 決定 ─────────────────────── */

    @Test
    fun `每一個 code 的 severity 與規範碼表的分組一致`() {
        val spec = specTable()
        val wrong = spec.mapNotNull { (id, row) ->
            val code = DiagnosticCode.byId(id) ?: return@mapNotNull "$id 不在 DiagnosticCode 裡"
            if (code.severity != row.severity) {
                "$id：規範說 ${row.severity}，程式碼說 ${code.severity}"
            } else null
        }
        assertTrue(wrong.joinToString("\n  ", prefix = "\n  "), wrong.isEmpty())

        // 反向：把一個 code 的規範等級改掉，同一段判斷必須抓到。
        val tampered = spec.toMutableMap()
        tampered["bad_color"] = tampered.getValue("bad_color").copy(severity = Severity.INFO)
        val caught = tampered.any { (id, row) ->
            DiagnosticCode.byId(id)?.severity != row.severity
        }
        assertTrue("把 bad_color 改成 INFO 之後這條檢查還是綠的，它沒有在檢查", caught)
    }

    /**
     * **沒有任何產生點自己挑 severity。**
     *
     * 這條抓的是舊模型留下來的形狀：`diag.error(...)` / `diag.warn(...)` /
     * `Diagnostic(Severity.ERROR, ...)`。它們在改成 code 之後應該一個都不剩 ——
     * 留一個，那一處就有機會把同一件事記成別的等級。
     *
     * 真的發生過：`input_mode:<未知>` 被記成 `diag.error(... "F10" ...)`，
     * 而 §6.3 說那是 WARNING。一顆鍵上的錯字因此讓整份佈局載不起來。
     */
    @Test
    fun `產生端不得自己選擇嚴重度`() {
        val sources = diagnosticSources()
        assertTrue("掃不到任何原始碼，這條測試已經失效", sources.size >= 8)

        val offenders = mutableListOf<String>()
        for (f in sources) {
            f.readLines().forEachIndexed { i, raw ->
                val line = raw.substringBefore("//")
                for (pattern in FORBIDDEN) {
                    if (pattern.containsMatchIn(line)) {
                        offenders += "${f.name}:${i + 1}  ${raw.trim()}"
                    }
                }
            }
        }
        assertTrue(
            "嚴重度只能由 DiagnosticCode.severity 決定。這幾行自己選了一個等級：\n  " +
                offenders.joinToString("\n  "),
            offenders.isEmpty(),
        )

        // 反向：同一組樣式必須抓得到一行刻意寫壞的程式碼。
        val planted = "        diag.warn(path, \"oops\")"
        assertTrue(
            "掃描樣式抓不到 diag.warn(...)，它沒有在掃",
            FORBIDDEN.any { it.containsMatchIn(planted) },
        )
    }

    /* ─────────────────────── 2. (code, path) 不重複 ─────────────────────── */

    @Test
    fun `隨附的主題與佈局不產生重複的診斷身分`() {
        assertTrue("core/themes 夾具是空的", RepoFixtures.themeIds.isNotEmpty())
        assertTrue("core/layouts 夾具是空的", RepoFixtures.layoutIds.isNotEmpty())

        val dup = mutableListOf<String>()
        for (id in RepoFixtures.themeIds) {
            dup += duplicatesIn(id, ThemeLoader.load(id, RepoFixtures.themes).diagnostics)
        }
        for (id in RepoFixtures.layoutIds) {
            dup += duplicatesIn(id, LayoutLoader.load(id, RepoFixtures.layouts).diagnostics)
        }
        assertTrue(dup.joinToString("\n  ", prefix = "\n  "), dup.isEmpty())
    }

    @Test
    fun `一份到處都是錯的文件也不產生重複的診斷身分`() {
        val r = LayoutLoader.load("zoo", MapDocumentSource(mapOf("zoo" to ZOO)))
        assertNotNull("這份夾具應該還載得起來（全部都是可回復錯誤）", r.value)
        assertTrue("這份夾具沒有產生任何診斷，它已經不是夾具了", r.diagnostics.size >= 8)
        assertTrue(
            duplicatesIn("zoo", r.diagnostics).joinToString("\n  ", prefix = "\n  "),
            duplicatesIn("zoo", r.diagnostics).isEmpty(),
        )
    }

    @Test
    fun `累積器擋掉重複，但不同的路徑各留一則`() {
        val d = Diagnostics()
        d.add(DiagnosticCode.BAD_COLOR, "a.b", 1, listOf("#ZZZ"))
        d.add(DiagnosticCode.BAD_COLOR, "a.b", 9, listOf("#YYY"))
        assertEquals("同一個 (severity, code, path) 只該留一則", 1, d.items.size)

        d.add(DiagnosticCode.BAD_COLOR, "a.c", 2, listOf("#ZZZ"))
        assertEquals("路徑不同就是不同的診斷", 2, d.items.size)

        // 反向：去重若壞掉（例如改成只比 code），第二則就會被吃掉。
        assertEquals(listOf("a.b", "a.c"), d.items.map { it.path })
    }

    /**
     * §6.3 說「某 row 的 width 總和 ≠ units」每一列各一則。三列都錯的時候
     * 必須是**三則**，不能因為去重變成一則 —— 那是路徑沒帶到列號的症狀。
     */
    @Test
    fun `三列寬度都不對就報三則`() {
        val doc = """
            format: rime-layout/1
            id: widths
            layers:
              - id: main
                units: 3.0
                rows:
                  - keys: [{ label: "a" }]
                  - keys: [{ label: "b" }]
                  - keys: [{ label: "c" }]
        """.trimIndent()
        val r = LayoutLoader.load("widths", MapDocumentSource(mapOf("widths" to doc)))
        val rows = r.diagnostics.filter { it.code == DiagnosticCode.ROW_WIDTH_MISMATCH }
        assertEquals(rows.map { it.path }.toString(), 3, rows.size)
        assertEquals(3, rows.map { it.identity }.toSet().size)
    }

    /* ─────────────────────── 3. 與規範碼表對得上 ─────────────────────── */

    @Test
    fun `規範碼表裡的每一個 code 都實作了，參數個數也對得上`() {
        val spec = specTable()
        assertTrue(
            "從 docs/theme-format.md §6.5.1 只解析到 ${spec.size} 個 code —— " +
                "碼表的格式改了，這條測試正在空轉",
            spec.size >= 40,
        )

        val problems = mutableListOf<String>()
        for ((id, row) in spec) {
            val code = DiagnosticCode.byId(id)
            if (code == null) {
                problems += "$id 在規範裡有，程式碼裡沒有"
                continue
            }
            if (code.arity != row.arity) {
                problems += "$id：規範的參數個數是 ${row.arity}，程式碼是 ${code.arity}"
            }
        }
        assertTrue(problems.joinToString("\n  ", prefix = "\n  "), problems.isEmpty())
    }

    /**
     * 程式碼裡多出來的 code **只能**是登記過的暫定碼。
     *
     * 這條是雙向的門：
     *   * 隨手發明一個 code → 紅（要嘛進規範，要嘛登記成暫定並回報 §5）；
     *   * macOS 端把某個暫定碼寫進規範 → 也紅，提醒把 `provisional` 拿掉。
     *     那一刻正是最容易忘記的時候。
     */
    @Test
    fun `程式碼裡多出來的 code 恰好是登記過的暫定碼`() {
        val spec = specTable().keys
        val extra = DiagnosticCode.values().map { it.id }.toSet() - spec
        val declared = DiagnosticCode.values().filter { it.provisional }.map { it.id }.toSet()
        assertEquals(
            "左＝規範沒有的 code，右＝標成 provisional 的 code。" +
                "兩邊要一模一樣：規範收編了就把 provisional 拿掉，新加的就標上。",
            extra.sorted(),
            declared.sorted(),
        )
    }

    @Test
    fun `資源名是純函式推導的，而且不會撞名`() {
        val names = DiagnosticCode.values().flatMap { it.resourceNames } +
            DiagnosticTerm.ALL_RESOURCE_NAMES
        assertEquals("有兩個 code 推導出同一個資源名", names.size, names.toSet().size)
        assertEquals("diag_fatal_yaml_syntax", DiagnosticCode.FATAL_YAML_SYNTAX.resourceName())
        assertEquals("diag_unknown_field", DiagnosticCode.UNKNOWN_FIELD.resourceName(1))
        assertEquals("diag_unknown_field_2", DiagnosticCode.UNKNOWN_FIELD.resourceName(2))
        assertNull("不該有一個叫 unknown 的 code", DiagnosticCode.byId("unknown"))
    }

    /* ─────────────────────── 產生點的具體行為 ─────────────────────── */

    /**
     * `input_mode:<未知>` 曾經是**致命錯誤**（`diag.error(... "F10" ...)`）。
     * §6.2 的致命清單沒有這一條，§6.3 明寫「已知 verb、參數不合法 → noop + WARNING」。
     */
    @Test
    fun `未知的 input_mode 參數是可回復的，不是致命的`() {
        val doc = """
            format: rime-layout/1
            id: badmode
            layers:
              - id: main
                rows:
                  - keys: [{ id: "m", label: "中", tap: "input_mode:explode" }]
        """.trimIndent()
        val r = LayoutLoader.load("badmode", MapDocumentSource(mapOf("badmode" to doc)))
        assertNotNull("一顆鍵上的錯字不該讓整份佈局載不起來", r.value)
        assertEquals(emptyList<Diagnostic>(), r.errors)
        assertEquals(
            listOf(DiagnosticCode.BAD_ACTION_ARGUMENT),
            r.diagnostics.map { it.code },
        )
    }

    /**
     * §6.2 的致命清單（F1–F10）沒有「layer 缺 id」，而 §6.2 明寫
     * 「超出此清單者一律為可回復錯誤」—— 所以丟掉那一層，不是拒絕整份文件。
     */
    @Test
    fun `缺 id 的層被丟掉，其餘照常；全部丟光才是 F8`() {
        val partial = """
            format: rime-layout/1
            id: noid
            default_layer: main
            layers:
              - rows:
                  - keys: [{ label: "x" }]
              - id: main
                rows:
                  - keys: [{ label: "a" }]
        """.trimIndent()
        val r = LayoutLoader.load("noid", MapDocumentSource(mapOf("noid" to partial)))
        assertNotNull(RepoFixtures.describe(r.diagnostics), r.value)
        assertEquals(listOf("main"), r.value!!.layers.map { it.id })
        assertTrue(r.diagnostics.any { it.code == DiagnosticCode.ENTRY_DROPPED })

        val allBad = """
            format: rime-layout/1
            id: noid2
            layers:
              - rows:
                  - keys: [{ label: "x" }]
        """.trimIndent()
        val r2 = LayoutLoader.load("noid2", MapDocumentSource(mapOf("noid2" to allBad)))
        assertNull(r2.value)
        assertEquals(
            listOf(DiagnosticCode.FATAL_LAYERS_MISSING),
            r2.errors.map { it.code },
        )
    }

    /** `auto_for_schema` 曾經被取值兩次，型別錯時就是兩則一模一樣的 WARNING。 */
    @Test
    fun `auto_for_schema 型別錯只報一則`() {
        val doc = """
            format: rime-layout/1
            id: autobad
            auto_for_schema:
              a: 1
            layers:
              - id: main
                rows:
                  - keys: [{ label: "a" }]
        """.trimIndent()
        val r = LayoutLoader.load("autobad", MapDocumentSource(mapOf("autobad" to doc)))
        val mismatches = r.diagnostics.filter {
            it.code == DiagnosticCode.TYPE_MISMATCH && it.path == "auto_for_schema"
        }
        assertEquals(mismatches.toString(), 1, mismatches.size)
    }

    /** `type_mismatch` 的參數是代號，不是給人看的字（不然翻譯會漏在參數上）。 */
    @Test
    fun `type_mismatch 的參數是可翻譯的代號`() {
        val doc = """
            format: rime-theme/1
            id: kinds
            palette: 5
        """.trimIndent()
        val r = ThemeLoader.load("kinds", MapDocumentSource(mapOf("kinds" to doc)))
        val d = r.diagnostics.first { it.code == DiagnosticCode.TYPE_MISMATCH }
        val known = DiagnosticTerm.values().map { it.id }
        assertTrue("$d 的參數不是 DiagnosticTerm 代號", d.args.all { it in known })
    }

    /* ────────────────────────────── 夾具 ────────────────────────────── */

    private data class SpecRow(val severity: Severity, val arity: IntRange)

    private fun duplicatesIn(id: String, diags: List<Diagnostic>): List<String> =
        diags.groupBy { it.identity }
            .filterValues { it.size > 1 }
            .map { (identity, group) -> "$id：$identity 出現 ${group.size} 次" }

    /**
     * 從 `docs/theme-format.md` §6.5.1 讀出碼表。
     *
     * 只讀規範，不抄規範 —— 抄一份常數表的話，macOS 端改了碼表這裡不會紅，
     * 而「規範改了但某一端沒跟上」正是這整件事要防的。
     */
    private fun specTable(): Map<String, SpecRow> {
        val md = File(RepoFixtures.coreDir.parentFile, "docs/theme-format.md")
        assertTrue("找不到 ${md.path}", md.isFile)

        val out = LinkedHashMap<String, SpecRow>()
        var severity: Severity? = null
        var inSection = false
        for (raw in md.readLines()) {
            val line = raw.trim()
            if (line.startsWith("#### 6.5.1")) {
                inSection = true
                continue
            }
            if (inSection && line.startsWith("## ")) break
            if (!inSection) continue

            when {
                line.startsWith("**致命") -> severity = Severity.ERROR
                line.startsWith("**可回復") -> severity = Severity.WARNING
                line.startsWith("**INFO") -> severity = Severity.INFO
            }
            if (!line.startsWith("|")) continue
            val cells = line.split("|").map { it.trim() }
            if (cells.size < 4) continue
            val code = CODE_CELL.matchEntire(cells[1])?.groupValues?.get(1) ?: continue
            val counts = ARGS_CELL.findAll(cells[cells.size - 2]).map { m ->
                val body = m.groupValues[1].trim()
                if (body.isEmpty()) 0 else body.split(",").size
            }.toList()
            if (counts.isEmpty()) continue
            out[code] = SpecRow(
                severity ?: error("$code 出現在任何分組標題之前"),
                counts.min()..counts.max(),
            )
        }
        return out
    }

    /** 會產生診斷的原始碼。掃到 0 個要失敗，不能安靜地通過。 */
    private fun diagnosticSources(): List<File> =
        listOf("src/main/java/org/luminakey/ime/theme", "src/main/java/org/luminakey/ime/keyboard")
            .flatMap { File(it).listFiles().orEmpty().toList() }
            .filter { it.isFile && it.name.endsWith(".kt") && it.name != "Diagnostics.kt" }

    private companion object {
        private val CODE_CELL = Regex("`([a-z][a-z0-9_.]*)`")
        private val ARGS_CELL = Regex("`\\[([^]]*)]`")

        /** 舊模型留下來的形狀：呼叫點自己挑一個等級。 */
        private val FORBIDDEN = listOf(
            Regex("""\bdiag(nostics)?\.(warn|info|error)\("""),
            Regex("""Diagnostic\(\s*Severity\."""),
            Regex("""Severity\.(ERROR|WARNING|INFO)\s*,"""),
        )

        /**
         * 一份到處都是可回復錯誤的佈局。刻意讓多種 code 同時出現在**不同的**
         * 路徑上，用來確認去重不會把不同的問題吃掉。
         */
        private val ZOO = """
            format: rime-layout/1
            id: zoo
            blahblah: 1
            direction: sideways
            metrics:
              row_spacing: "wide"
              height_scale: 9.0
            layers:
              - id: main
                rows:
                  - keys:
                      - { id: "a", label: "a", icon: "nosuchicon", send: { keysym: "a" } }
                      - { id: "b", label: "b", tap: "explode:now" }
                      - { id: "c", label: "c", tap: "layer:ghost" }
                      - { id: "d", label: "d", send: { keysym: "a" }, tap: "clear" }
                      - { id: "e", label: "e", send: { text: "" } }
                      - { id: "f", label: "f", send: { keysym: "a", modifiers: ["Nope"] } }
                      - { id: "g", label: "g", swipe: { sideways: { label: "z" } } }
        """.trimIndent()
    }
}
