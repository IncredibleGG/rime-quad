package org.luminakey.ime

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory

/**
 * 鍵盤上那幾句狀態訊息**走資源，不是 Kotlin 字面值**。
 *
 * ── 為什麼單獨守 onPhase() ──────────────────────────────────────────────
 * `RimeInputMethodService.onPhase()` 寫進 `uiState.busyMessage` /
 * `fatalMessage` 的字**一定會上畫面**：`KeyboardView` 的候選列無條件把
 * `fatalMessage ?: busyMessage` 當提示畫出來。而這四句原本全是 Kotlin 裡的
 * 中文字面值 —— 預設語系是英文（`res/values/` 是英文），所以一個法國使用者
 * 第一次打開鍵盤，看到的是一行中文。
 *
 * [UiBannedWordsTest] 自己在 G4 那段寫得很清楚：它只掃在地化資源，
 * **擋不住有人在 Kotlin 裡寫死一句中文**。這一份就是補那個洞 ——
 * 但只補在證明得了「這些字會上畫面」的那一個函式上，不是整個檔案：
 * 同一支檔案裡的 `Log` 訊息用中文是對的（讀者是我們，不是使用者）。
 *
 * ── 它抓不到什麼（誠實說明）──────────────────────────────────────────────
 * · 只看 `onPhase()`。同一支檔案別處、以及 `store/SchemaStore.kt` 那些
 *   `Outcome.Ok("已安裝 N 個套件")` 一樣會上畫面，而且一樣是中文字面值 ——
 *   那些**沒有**被這份守到，也不歸這條線修。
 * · 只認漢字。有人在這裡寫死一句英文，這份不會叫（但那至少不會讓非中文
 *   使用者看到看不懂的字）。
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

    /** 反向測試的另一半：**寫對了不可以叫**。 */
    @Test
    fun `走資源與註解裡的中文都不算`() {
        val good = """
            // 這一句會上畫面，所以走資源。
            uiState = uiState.copy(
                busyMessage = getString(R.string.ime_notice_preparing),
                fatalMessage = null,
            )
            Log.i(TAG, "phase=${'$'}phase")
        """.trimIndent()
        assertEquals(emptyList<String>(), cjkLiteralsIn(good))
    }

    /* ─────────────── 掃描器本體 ─────────────── */

    /** 從 `private fun onPhase(` 之後的第一個 `{` 起，括號配對到結束。 */
    private fun onPhaseBody(): String {
        val f = File("src/main/java/org/luminakey/ime/RimeInputMethodService.kt")
        assertTrue("找不到 ${f.path}", f.isFile)
        val src = f.readText()
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
     */
    private fun cjkLiteralsIn(src: String): List<String> =
        LITERAL.findAll(stripComments(src))
            .map { it.value }
            .filter { s -> s.any { it.code in 0x4E00..0x9FFF } }
            .toList()

    private fun stripComments(src: String): String =
        src.lineSequence().joinToString("\n") { line ->
            val i = line.indexOf("//")
            // 只砍**整行都是註解**的情況，避免把字串裡的 `//`（網址）誤砍。
            if (i >= 0 && line.take(i).isBlank()) "" else line
        }.replace(BLOCK_COMMENT, "")

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

        private val LITERAL = Regex(""""([^"\\\n]|\\.)*"""")
        private val BLOCK_COMMENT = Regex("""/\*[\s\S]*?\*/""")
    }
}
