package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * 工單 #107 的第二半:**每一個畫方案名的地方都問過 [SchemaVariantLabel]**。
 *
 * ── 為什麼 [SchemaVariantLabelTest] 不夠 ────────────────────────────────
 * 那一份驗的是判準本身,加上**空白鍵那一個**呼叫端。而覆核實測抓到的是:
 * 抽象是對的、空白鍵是對的,而畫同一個方案名的地方一共有五個,只接了兩個 ——
 *
 *   · `KeyboardView.kt`   鍵盤上方案選單那張卡的副標
 *   · `KeyboardChoice.kt` 設定頁卡片的副標
 *   · `KeyboardChoice.kt` 設定頁的分組小標
 *
 * 三處照舊畫原始的 `type.subtitle`。其中**第一處與已經修好的空白鍵同框**
 * (方案選單是浮層,底列的鍵露出來),所以原本那個「畫面說臺灣正體、引擎吐
 * 簡體」的失敗模式在修完之後**仍然能用一張截圖重現**,只是換了個地方。
 *
 * ── 這一份守的不是「現在這三處對了」──────────────────────────────────
 * 逐一斷言那三處,下一個人新增第四個呼叫端時它照樣全綠 —— 而那正是這條
 * 缺陷這一次的長法。所以判準反過來寫:
 *
 *     **`src/main` 裡每一次讀 `.subtitle`,都必須是
 *       `SchemaVariantLabel.display(…)` 的第一個引數。**
 *
 * 沒有允許清單。真的需要原始值的人(例如拿它當 map 的鍵)必須**動這條測試**,
 * 而那一刻正是該想一想「這個字串會不會被畫出來」的時候。
 * `availableKeyboardGroups()` 原本就是那種用法,這一版把分組鍵改成
 * `schemaId` —— 拿一個會隨顯示而變的字串當身分,本來就是錯的。
 *
 * ── 它抓不到什麼(誠實說明)────────────────────────────────────────────
 * · 只認 `.subtitle` 這個**字面形狀**。有人先 `val s = type.subtitle` 再畫 `s`,
 *   仍然會被抓到(那一行就是違規);但有人繞道 `type.schemaName` 自己組一次,
 *   這份不會叫。那是另一個形狀,現在沒有守門,已寫進「發現但沒做」。
 * · 它證明不了畫出來好不好看、位置對不對 —— 那要人看。
 *   它證明得了的是:**把線拆掉、或新增一條沒接的線,就會紅。**
 */
class SchemaVariantWiringTest {

    /* ─────────────── 1. 真的掃一次 ─────────────── */

    @Test
    fun `每一次讀 subtitle 都經過判準`() {
        val files = mainSources()

        // G2:範圍非空。這個專案的守門腳本正是在「範圍不含那個目錄」的情況下
        //     六項全錯而 6/6 全綠的。掃到零個檔案必須是紅,不是零個違規。
        assertTrue("只掃到 ${files.size} 支 .kt,範圍大概錯了", files.size >= MIN_FILES)
        val total = files.sumOf { occurrencesIn(it.readText()).size }
        assertTrue(
            "整棵樹一次 `.subtitle` 都沒讀到 —— 判準改名或檔案搬走了,這條測試已經對不上實作",
            total >= MIN_OCCURRENCES,
        )
        // G2 的第二半:那三個**真的出過事**的檔案必須在範圍內。
        for (rel in REQUIRED_FILES) {
            assertTrue("掃描範圍漏了 $rel", files.any { it.path.endsWith(rel) })
        }

        val offenders = files.flatMap { f ->
            rawUsesIn(f.readText()).map { "${f.name}: $it" }
        }
        assertEquals(
            "這幾行直接畫了方案名的原始副標。方案名裡的「臺灣正體」是那個方案的" +
                "**預設**字集,不是現在生效的那一個 —— 使用者切成簡體之後,這一行" +
                "會與同一個畫面上的其他地方互相矛盾。請改走 " +
                "SchemaVariantLabel.display(…):",
            emptyList<String>(),
            offenders,
        )
    }

    /**
     * 三個呼叫端**各自**都要在。
     *
     * 上一條是「沒有違規」,它在有人把整行刪掉時也是綠的。這一條反過來要求
     * 那三處的接線真的存在 —— 兩條合起來才擋得住「拆掉」與「新增沒接的」。
     */
    @Test
    fun `三個呼叫端都接上了`() {
        val kv = read("keyboard/KeyboardView.kt")
        val kc = read("home/KeyboardChoice.kt")

        assertTrue(
            "鍵盤上方案選單那張卡沒有接判準 —— 它與空白鍵同框,會同框自打嘴巴",
            codeOnly(kv).contains("SchemaVariantLabel.display(shown.subtitle"),
        )
        assertTrue(
            "設定頁卡片副標沒有接判準",
            codeOnly(kc).contains("SchemaVariantLabel.display(type.subtitle, simplified)"),
        )
        assertTrue(
            "設定頁分組小標沒有接判準",
            codeOnly(kc).contains("SchemaVariantLabel.display(types.first().subtitle, simplified)"),
        )
        // 那張卡拿到的必須是**引擎當下的字集**,不是寫死的一邊。
        assertTrue(
            "方案選單那張卡沒有拿到引擎的當下字集,那就只是換個地方寫死",
            codeOnly(kv).contains("simplified = state.status.isSimplified"),
        )
    }

    /* ─────────────── 2. 反向測試(G1)─────────────── */

    /**
     * **這一條才是這份測試存在的理由。**
     *
     * 覆核那三處各自還原成原本的寫法,以及**新增一個第四呼叫端**,
     * 每一種都必須被抓到。植入用的是真的原始碼,而且先斷言它真的變了 ——
     * 錨點對不上的話樹是沒改過的,「規則沒叫」會被誤讀成「規則沒用」。
     */
    @Test
    fun `三處還原與新增第四處,每一種都要被抓到`() {
        val kv = read("keyboard/KeyboardView.kt")
        val kc = read("home/KeyboardChoice.kt")
        // 前提:拆之前是乾淨的。否則下面的紅是本來就紅。
        assertEquals("KeyboardView 拆之前就已經有違規", emptyList<String>(), rawUsesIn(kv))
        assertEquals("KeyboardChoice 拆之前就已經有違規", emptyList<String>(), rawUsesIn(kc))

        val sabotages = listOf(
            Sabotage(
                "(1) 鍵盤方案選單那張卡還原成原始副標",
                kv,
                kv.replaceFirst(
                    "text = SchemaVariantLabel.display(shown.subtitle, simplified),",
                    "text = shown.subtitle,",
                ),
            ),
            Sabotage(
                "(2) 設定頁卡片副標還原成原始副標",
                kc,
                kc.replaceFirst(
                    "text = SchemaVariantLabel.display(type.subtitle, simplified),",
                    "text = type.subtitle,",
                ),
            ),
            Sabotage(
                "(3) 分組小標還原成原本用方案名當鍵的寫法",
                kc,
                kc.replaceFirst(
                    "for (t in all) out.getOrPut(t.schemaId) { ArrayList() } += t",
                    "for (t in all) out.getOrPut(t.subtitle) { ArrayList() } += t",
                ),
            ),
            // ★ 新增一個呼叫端而忘了接。這是這條缺陷這一次真正的長法 ——
            //   逐一斷言「現在這三處對了」的測試,對這一種是全綠的。
            Sabotage(
                "(4) 新增第四個呼叫端而忘了接判準",
                kc,
                kc + """

@Composable
private fun SchemaBadge(type: KeyboardType) {
    Text(text = type.subtitle)
}
""",
            ),
        )

        for (s in sabotages) {
            assertNotEquals(
                "${s.what}:這個拆法根本沒有植入成功,這一條反向測試等於沒做",
                s.original,
                s.mutated,
            )
            assertTrue(
                "${s.what}:拆掉之後規則沒有叫 —— 那上面那些綠燈不代表任何事",
                rawUsesIn(s.mutated).isNotEmpty(),
            )
        }
    }

    /** 反向測試的另一半:**接對了不可以叫**。會亂叫的檢查一樣會被關掉。 */
    @Test
    fun `接對了不算違規`() {
        val good = """
            Text(text = SchemaVariantLabel.display(type.subtitle, simplified))
            val name = SchemaVariantLabel.display(types.first().subtitle, simplified)
            // 註解裡提到 type.subtitle 不算
            /* 區塊註解裡的 shown.subtitle 也不算 */
        """.trimIndent()
        assertEquals(emptyList<String>(), rawUsesIn(good))
    }

    /** 掃描器本身要看得出「差一點點」的寫法:引數之間隔了逗號就不算接上。 */
    @Test
    fun `隔了一個引數就不算接上`() {
        val bad = """
            Text(text = SchemaVariantLabel.display(other, type.subtitle))
        """.trimIndent()
        assertEquals(1, rawUsesIn(bad).size)
    }

    /**
     * 而第一個引數裡**有括號**是完全正常的寫法,不可以叫。
     * 這一條是實跑之後補的:第一版把 `types.first().subtitle` 判成違規。
     */
    @Test
    fun `第一個引數裡有括號不算違規`() {
        val good = """
            val name = SchemaVariantLabel.display(types.first().subtitle, simplified)
        """.trimIndent()
        assertEquals(emptyList<String>(), rawUsesIn(good))
    }

    /* ─────────────── 掃描器本體 ─────────────── */

    private data class Sabotage(val what: String, val original: String, val mutated: String)

    private fun read(rel: String): String {
        val f = File(mainRoot, rel)
        assertTrue("找不到 ${f.path}", f.isFile)
        return f.readText()
    }

    private fun mainSources(): List<File> =
        mainRoot.walkTopDown().filter { it.isFile && it.extension == "kt" }.toList()

    /** `.subtitle` 在**程式碼**裡出現的每一個位置。 */
    private fun occurrencesIn(src: String): List<Int> {
        val code = codeOnly(src)
        val out = mutableListOf<Int>()
        var i = code.indexOf(NEEDLE)
        while (i >= 0) {
            out += i
            i = code.indexOf(NEEDLE, i + 1)
        }
        return out
    }

    /**
     * 沒有經過判準的那幾次。
     *
     * 判準:往前找最近的 `SchemaVariantLabel.display(`,而 `.subtitle` 必須落在
     * 它的**第一個引數**裡 —— 中間的括號要配平、而且不可以有頂層的逗號。
     *
     * ⚠ 括號要**配平**而不是「一個都不准有」:第一版寫成後者,而
     *   `display(types.first().subtitle, …)` 這種完全正確的寫法會被判成違規。
     *   會亂叫的檢查一樣會被關掉,所以這一條是實跑之後改的。
     *   頂層逗號那個限制留著:`display(other, type.subtitle)` 把方案名放在了
     *   `simplified` 的位置上,那不算接上。
     */
    private fun rawUsesIn(src: String): List<String> {
        val code = codeOnly(src)
        return occurrencesIn(src).filterNot { at -> approved(code, at) }.map { at ->
            val from = code.lastIndexOf('\n', at) + 1
            val to = code.indexOf('\n', at).let { if (it < 0) code.length else it }
            code.substring(from, to).trim()
        }
    }

    /** `.subtitle` 落在最近那個 `display(` 的第一個引數裡嗎。 */
    private fun approved(code: String, at: Int): Boolean {
        val head = code.lastIndexOf(MARK, at)
        if (head < 0) return false
        var depth = 0
        for (i in head + MARK.length until at) {
            when (code[i]) {
                '(' -> depth++
                ')' -> {
                    depth--
                    if (depth < 0) return false
                }
                ',' -> if (depth == 0) return false
            }
        }
        return depth == 0
    }

    /**
     * 只留下程式碼。整行註解與 `/* */` 區塊一律挖掉 —— 註解會提到它在講的
     * 每一個識別字,拿它當程式碼掃,結論一定是錯的(這個專案踩過)。
     */
    private fun codeOnly(src: String): String =
        src.replace(BLOCK_COMMENT, "")
            .lineSequence()
            .joinToString("\n") { line ->
                val i = line.indexOf("//")
                if (i >= 0 && line.take(i).isBlank()) "" else line
            }

    companion object {
        /** 單元測試的工作目錄是模組目錄(`android/app`)。 */
        private val mainRoot = File("src/main/java/org/luminakey/ime")

        private const val NEEDLE = ".subtitle"
        private const val MARK = "SchemaVariantLabel.display("

        private val BLOCK_COMMENT = Regex("""/\*[\s\S]*?\*/""")

        /** 掃到比這個少,一定是路徑錯了而不是真的只有這麼多檔案。 */
        private const val MIN_FILES = 40

        /** 三個呼叫端 + 首頁摘要 + 分組小標。少於這個數就是判準改名了。 */
        private const val MIN_OCCURRENCES = 4

        private val REQUIRED_FILES = listOf(
            "keyboard/KeyboardView.kt",
            "home/KeyboardChoice.kt",
        )
    }
}
