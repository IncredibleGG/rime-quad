package org.luminakey.ime

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory

/**
 * 使用者看得到的字**走資源，不是 Kotlin 字面值**。
 *
 * ── 為什麼這一份必須存在 ────────────────────────────────────────────────
 * 預設語系是英文（`res/values/` 是英文），所以一句寫死的中文會**原樣上畫面**
 * ——一個法國使用者第一次打開鍵盤，看到的是一行中文。
 * [UiBannedWordsTest] 自己在 G4 那段寫得很清楚：它只掃在地化資源，
 * **擋不住有人在 Kotlin 裡寫死一句中文**。這一份就是補那個洞。
 *
 * ── ⚠ 上一版的範圍太窄，而且窄得剛好漏掉自己新開的那條路 ───────────────
 * 上一版只掃 `RimeInputMethodService.onPhase()`，理由是「只補在證明得了
 * 這些字會上畫面的那一個函式上」。同一個 commit 卻在 `DeployGate` 裡寫下
 * `NotStarted("librime 尚未初始化")` —— 而那句話正是首頁失敗態那顆按鈕
 * 按下去會拿到的東西。**守門綠著，卻抓不到它宣稱要抓的那一類問題。**
 *
 * 所以範圍改成一張明列的「會上畫面的地方」清單（[SURFACES]），而且用
 * [REQUIRED_HITS] 保證每一個檔案都真的被讀到 —— 掃到零個檔案然後全綠是
 * 這個專案踩過的失效方式。
 *
 * ── 為什麼 Log 不算 ────────────────────────────────────────────────────
 * `Log` 的讀者是我們，不是使用者，而這個專案的規矩是註解與 log 用中文。
 * 掃描器把 `Log.x(…)` 整段挖掉再看（[stripLogCalls]），這樣既擋得住
 * 「訊息寫死中文」，又不會逼人把 log 翻成英文。
 *
 * ── 它抓不到什麼（誠實說明）────────────────────────────────────────────
 * · 只認漢字（U+4E00–U+9FFF）。有人寫死一句英文，這份不會叫 —— 但那至少
 *   不會讓非中文使用者看到看不懂的字。也不認全形標點：`"、"` 這種分隔符
 *   逃得掉（`SchemaStore.joined()` 因此刻意改用 `", "`）。
 * · [SURFACES] 是一張**人維護的清單**。新增一支會上畫面的引擎層檔案而沒有
 *   把它加進來，這份不會叫。加檔案時請一起加進去。
 * · 它證明不了「資源的內容是對的」—— 那由 [StringCatalogTest]、
 *   [org.luminakey.ime.keyboard.KbdStringsTest]（掃 `values/` 底下每一份
 *   `strings*.xml` 的三語形狀）與 [UiBannedWordsTest] 從別的角度接住。
 */
class ImeNoticeStringsTest {

    /* ─────────────── 1. 真的掃一次 ─────────────── */

    @Test
    fun `onPhase 裡沒有寫死的中文`() {
        val body = onPhaseBody()
        // G2：範圍非空 —— 函式改名或搬走時必須是紅，不是零個違規。
        assertTrue("onPhase() 的函式體只有 ${body.length} 個字元，大概是抓錯了", body.length >= 300)
        assertTrue(
            "onPhase() 裡找不到 uiState.copy —— 這條測試已經對不上實作了",
            body.contains("uiState = uiState.copy"),
        )

        assertEquals(
            "這幾句會被畫在候選列上，卻是寫死的中文。預設語系是英文，" +
                "非中文使用者第一次打開鍵盤就會看到它們：",
            emptyList<String>(),
            cjkLiteralsIn(body),
        )
    }

    /**
     * **本版新增的重點。** 引擎層那幾支檔案裡不可以有會上畫面的中文字面值。
     *
     * 每一支的「為什麼它會上畫面」寫在 [SURFACES] 的註解裡。
     */
    @Test
    fun `引擎層沒有寫死的中文`() {
        val offenders = LinkedHashMap<String, List<String>>()
        for (surface in SURFACES) {
            val src = readSurface(surface)
            val hits = cjkLiteralsIn(src)
            if (hits.isNotEmpty()) offenders[surface] = hits
        }
        assertEquals(
            buildString {
                appendLine("這幾句寫死的中文會走到使用者畫面上（預設語系是英文）：")
                offenders.forEach { (f, hits) -> appendLine("  $f  ${hits.joinToString("  ")}") }
                appendLine("要嘛改走資源（見 store/UiMessage.kt），")
                appendLine("要嘛它其實是給我們看的故障載荷 —— 那就寫成英文。")
            },
            emptyMap<String, List<String>>(),
            offenders,
        )
    }

    /**
     * G2：範圍非空。
     *
     * 這個專案的產品識別碼守門腳本正是在「`find` 的範圍不含那個目錄」的情況下
     * 六項全錯而 6/6 全綠的。所以每一支檔案都要證明自己**真的被讀進來過**：
     * 讀得到、夠長、而且含有一句只有那支檔案才有的字。
     */
    @Test
    fun `掃描範圍涵蓋每一支該掃的檔案`() {
        for ((surface, needle) in REQUIRED_HITS) {
            val src = readSurface(surface)
            assertTrue("$surface 只讀到 ${src.length} 個字元，路徑大概錯了", src.length >= 1000)
            assertTrue("$surface 裡找不到「$needle」—— 這條測試已經對不上實作了", src.contains(needle))
        }
        assertEquals(
            "SURFACES 與 REQUIRED_HITS 必須是同一張清單",
            SURFACES.sorted(),
            REQUIRED_HITS.keys.sorted(),
        )
    }

    /** 三句都真的接上了資源。少接一句，那一句就會變回沒有人翻譯的狀態。 */
    @Test
    fun `三句狀態訊息都走資源`() {
        val body = onPhaseBody()
        val missing = REQUIRED_IDS.filterNot { body.contains("R.string.$it") }
        assertEquals("onPhase() 沒有用到這幾條資源：$missing", emptyList<String>(), missing)
    }

    /** 英／繁／簡三份都要有，而且不可以是空的。 */
    @Test
    fun `三種語言都有這幾句`() {
        val blanks = mutableListOf<String>()
        for (dir in listOf("values", "values-b+zh+Hant", "values-b+zh+Hans")) {
            val table = load(dir)
            for (id in REQUIRED_IDS) {
                val v = table[id]
                if (v.isNullOrBlank()) blanks += "$dir/$id"
            }
        }
        assertEquals(
            "少一份的下場是那一句在該語言下靜靜地顯示英文（或空白）：$blanks",
            emptyList<String>(),
            blanks,
        )
    }

    /* ─────────────── 2. 反向測試（G1）─────────────── */

    @Test
    fun `植入一句寫死的中文會被抓到`() {
        val old = """
            RimeRuntime.Phase.DEPLOYING ->
                uiState = uiState.copy(
                    busyMessage = "首次啟動：正在編譯詞庫…",
                    fatalMessage = null,
                )
        """.trimIndent()
        assertEquals(listOf("\"首次啟動：正在編譯詞庫…\""), cjkLiteralsIn(old))
    }

    /**
     * G1 的第二半：**餵上一版 DeployGate 真正的寫法**。
     *
     * 這一條就是上一輪漏掉的那句話。它要是不會叫，`引擎層沒有寫死的中文`
     * 那條全綠就不代表任何事。
     */
    @Test
    fun `上一版 DeployGate 那句話會被抓到`() {
        val old = """
            fun deployAndWait(): Outcome {
                if (!RimeCore.isInitialized) return Outcome.NotStarted("librime 尚未初始化")
                if (!RimeCore.deploy()) {
                    return Outcome.NotStarted(
                        "rs_deploy() 拒絕啟動（多半是已有一個部署在進行中）：${'$'}{RimeCore.lastError()}"
                    )
                }
                Log.i(TAG, "部署結束，耗時 ${'$'}{elapsed}ms")
                return Outcome.Timeout(0)
            }
        """.trimIndent()
        val hits = cjkLiteralsIn(old)
        assertEquals("兩句都要被抓到，Log 那句不算", 2, hits.size)
        assertTrue("漏了「librime 尚未初始化」", hits.any { it.contains("尚未初始化") })
        assertTrue("漏了 rs_deploy 那一句", hits.any { it.contains("拒絕啟動") })
        assertTrue("把 Log 也算進去了 —— 會亂叫的檢查一樣會被關掉", hits.none { it.contains("部署結束") })
    }

    /** 上一版 SchemaStore 的 `Outcome.Ok("已安裝 N 個套件")` 同樣要被抓到。 */
    @Test
    fun `上一版 SchemaStore 那幾句會被抓到`() {
        val old = """
            return Outcome.Ok("已安裝 ${'$'}{installed.size} 個套件")
        """.trimIndent()
        assertEquals(listOf("\"已安裝 \${installed.size} 個套件\""), cjkLiteralsIn(old))
    }

    /** 反向測試的另一半：**寫對了不可以叫**。 */
    @Test
    fun `走資源與註解裡的中文都不算`() {
        val good = """
            // 這一句會上畫面，所以走資源。
            uiState = uiState.copy(
                busyMessage = getString(R.string.ime_notice_preparing),
                fatalMessage = null,
            )
            /* 區塊註解裡的中文也不算。 */
            val m = UiMessage.of(R.string.store_err_download, pkg.name, dl.message)
            Log.i(TAG, "phase=${'$'}phase 已經就緒")
        """.trimIndent()
        assertEquals(emptyList<String>(), cjkLiteralsIn(good))
    }

    /* ─────────────── 掃描器本體 ─────────────── */

    private fun readSurface(rel: String): String {
        val f = File("src/main/java/org/luminakey/ime/$rel")
        assertTrue("找不到 ${f.path}", f.isFile)
        return f.readText()
    }

    /** 從 `private fun onPhase(` 之後的第一個 `{` 起，括號配對到結束。 */
    private fun onPhaseBody(): String {
        val src = readSurface("RimeInputMethodService.kt")
        val at = src.indexOf("private fun onPhase(")
        assertTrue("RimeInputMethodService.kt 裡找不到 onPhase() —— 它被改名或搬走了", at >= 0)
        val open = src.indexOf('{', at)
        assertTrue(open >= 0)
        var depth = 0
        for (i in open until src.length) {
            when (src[i]) {
                '{' -> depth++
                '}' -> {
                    depth--
                    if (depth == 0) return src.substring(open + 1, i)
                }
            }
        }
        throw AssertionError("onPhase() 的大括號沒有配對成功")
    }

    /**
     * 找出含漢字的字串字面值。
     *
     * ⚠ 只看字面值，不看註解 —— 註解本來就是中文的（這個專案的規矩）。
     * 一個會把註解判成違規的檢查會被關掉，那比沒有更糟。
     * 同理 `Log.x(…)` 整段挖掉：log 的讀者是我們。
     */
    private fun cjkLiteralsIn(src: String): List<String> =
        LITERAL.findAll(stripLogCalls(stripComments(src)))
            .map { it.value }
            .filter { s -> s.any { it.code in 0x4E00..0x9FFF } }
            .toList()

    private fun stripComments(src: String): String =
        src.lineSequence().joinToString("\n") { line ->
            val i = line.indexOf("//")
            // 只砍**整行都是註解**的情況，避免把字串裡的 `//`（網址）誤砍。
            if (i >= 0 && line.take(i).isBlank()) "" else line
        }.replace(BLOCK_COMMENT, "")

    /**
     * 把 `Log.i(…)` / `Log.e(…)` 整個呼叫（含跨行的參數）挖掉。
     *
     * 用括號配對而不是「砍到行尾」：這個專案的 log 訊息常常跨兩三行，
     * 砍到行尾會留下後面那幾行的中文，掃描器就會對著 log 亂叫。
     */
    private fun stripLogCalls(src: String): String {
        val out = StringBuilder(src)
        while (true) {
            val m = LOG_CALL.find(out) ?: break
            val open = out.indexOf("(", m.range.first)
            var depth = 0
            var end = -1
            var i = open
            while (i < out.length) {
                when (out[i]) {
                    '(' -> depth++
                    ')' -> {
                        depth--
                        if (depth == 0) {
                            end = i
                            i = out.length
                        }
                    }
                }
                i++
            }
            if (end < 0) {
                out.delete(m.range.first, out.length)
                break
            }
            out.delete(m.range.first, end + 1)
        }
        return out.toString()
    }

    private fun load(dir: String): Map<String, String> {
        val f = File("src/main/res/$dir/strings.xml")
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
        private val REQUIRED_IDS = listOf(
            "ime_notice_preparing",
            "ime_notice_deploying",
            "ime_notice_failed",
        )

        /**
         * 會上畫面的引擎層檔案。每一支後面那句話是「它憑什麼在這張清單上」。
         *
         * · `store/DeployGate.kt`      → `Outcome` 被 `StoreController.redeploy()`
         *                                 與 `SchemaStore.setEnabled()` 直接畫成對話框。
         * · `store/SchemaStore.kt`     → `Outcome.message` / `details` 就是對話框內容。
         * · `store/ArchiveGuard.kt`    → 拒絕理由進 `Outcome.details`。
         * · `store/SchemaPreflight.kt` → 缺檔說明進 `Outcome.details`。
         * · `core/RimeRuntime.kt`      → `initError` 印在首頁與引導頁的失敗那一屏上。
         *                                 那裡刻意是**英文**的故障載荷（見該檔開頭），
         *                                 所以這條掃描順便守住「別人把它翻成中文」。
         */
        private val SURFACES = listOf(
            "store/DeployGate.kt",
            "store/SchemaStore.kt",
            "store/ArchiveGuard.kt",
            "store/SchemaPreflight.kt",
            "core/RimeRuntime.kt",
        )

        /** G2：每一支檔案都要含有這句只屬於它的字，證明真的讀對了檔案。 */
        private val REQUIRED_HITS = mapOf(
            "store/DeployGate.kt" to "fun deployAndWait(",
            "store/SchemaStore.kt" to "fun setEnabled(",
            "store/ArchiveGuard.kt" to "fun pathProblemOf(",
            "store/SchemaPreflight.kt" to "fun uiMessage(",
            "core/RimeRuntime.kt" to "private fun fail(",
        )

        private val LITERAL = Regex(""""([^"\\\n]|\\.)*"""")
        private val BLOCK_COMMENT = Regex("""/\*[\s\S]*?\*/""")
        private val LOG_CALL = Regex("""\bLog\.[a-z]+\(""")
    }
}
