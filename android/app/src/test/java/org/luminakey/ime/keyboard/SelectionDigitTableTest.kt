package org.luminakey.ime.keyboard

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.theme.LayoutLoader
import org.luminakey.ime.theme.Platform
import org.luminakey.ime.theme.RepoFixtures
import org.luminakey.ime.theme.SendSpec

/**
 * §10 第 43 條:序號判準 × **repo 裡真正會被載入的那幾份佈局** ＋ **真的那一份實測表**。
 *
 * ── 為什麼非有這一組不可 ────────────────────────────────────────────────
 * 上一輪的守門（`CandidateDensityTest` 的「有數字列的層畫序號」）是這樣寫的：
 *
 *     val numrow = layer("t9", "1", "2", …, "9", "0")   // 手搓的 fixture
 *     assertTrue(labelVisible(true, layerSendsSelectionDigit(numrow)))
 *
 * 它有三個獨立的理由永遠不會紅：(一) 它是同義反覆 —— 輸入照著實作的分支條件
 * 造出來，兩邊是同一句話寫兩遍；(二) 那個層叫 `"t9"` 只是字串，**它從來沒有
 * 碰過 `core/layouts/cn-t9-pinyin-numrow.yaml`**，真實 YAML 改成什麼樣它都
 * 不會紅；(三) fixture 寫死 `popup = null`、`swipe = emptyMap()`，反向誤判
 * 從構造上被排除掉了。
 *
 * 同一個坑 `RepoFixtures` 的 KDoc 已經記過一次（「這裡一度也是手寫的四個 id，
 * 於是十二份主題有八份從來沒有被載入過」）—— 序號那一條原封不動地重挖了一次。
 *
 * ── 這一組守什麼、不守什麼 ──────────────────────────────────────────────
 * **守得住**：真實 YAML 的每一層、每一格 (佈局, 方案) 的判準結果；實測表本身
 * 的格式與可信度（`yes` 必須帶得出量測的機器與日期）。
 *
 * **守不住**：「按下去到底選不選得到」。那件事只有真機答得出來，由
 * `scripts/verify_selection_digit.sh` 負責 —— 它同時是這份表的產生器與斷言者。
 * 這一組是 CI 上（沒有裝置）的那一半。
 */
class SelectionDigitTableTest {

    private val tsv: File by lazy { File(RepoFixtures.coreDir, "selection-digit.tsv") }

    private data class Row(
        val layout: String,
        val schema: String,
        val verdict: String,
        val compose: String,
        val measuredOn: String,
        val line: Int,
    )

    private val rows: List<Row> by lazy {
        tsv.readLines(Charsets.UTF_8).mapIndexedNotNull { i, raw ->
            val t = raw.trim()
            if (t.isEmpty() || t.startsWith("#")) return@mapIndexedNotNull null
            val c = raw.split('\t').map { it.trim() }
            if (c.size < 5) error("selection-digit.tsv 第 ${i + 1} 行欄位不足（要 5 欄以上）：$raw")
            Row(c[0], c[1], c[2].lowercase(), c[3], c[4], i + 1)
        }
    }

    private fun layout(id: String) =
        LayoutLoader.load(id, RepoFixtures.layouts, Platform.ANDROID).value ?: error("佈局 $id 載不起來")

    /** 依表把 [SelectionDigits] 設成 repo 的那一份。 */
    private fun installTable() {
        SelectionDigits.setForTest(
            rows.filter { it.verdict == SelectionDigits.VERDICT_YES }.map { it.layout to it.schema }
        )
    }

    @Test
    fun `實測表存在而且解析得出東西`() {
        assertTrue("找不到 ${tsv.absolutePath}", tsv.isFile)
        assertTrue("表裡一列資料都沒有 —— 那它守不住任何東西", rows.isNotEmpty())
        val (yes, count) = SelectionDigits.parse(tsv.readText(Charsets.UTF_8))
        assertEquals("執行期的解析器與這支測試的解析器對不上", rows.size, count)
        assertEquals(
            rows.count { it.verdict == SelectionDigits.VERDICT_YES },
            yes.size,
        )
    }

    /**
     * ⛔ **`yes` 必須說得出是在哪一台機器、哪一天量到的。**
     *
     * 手寫一個 `yes` 是這份表唯一的腐爛方式，而腐爛的症狀是「畫面上有序號、
     * 按下去把使用者打好的組字毀掉」。要求 `<avd>@<YYYY-MM-DD>` 不能杜絕
     * 手寫，但可以讓手寫變成一句寫得出來的謊 —— 而 `--bless` 只要一行指令。
     */
    @Test
    fun `說得可用的那幾格都量過`() {
        val bad = rows.filter { it.verdict == SelectionDigits.VERDICT_YES }
            .filter { !Regex("""^[A-Za-z0-9_.-]+@\d{4}-\d{2}-\d{2}$""").matches(it.measuredOn) }
        assertTrue(
            "這幾列說 yes 卻沒有量測出處（要 `<avd 名>@<YYYY-MM-DD>`，" +
                "跑 scripts/verify_selection_digit.sh --bless 產生）：\n  " +
                bad.joinToString("\n  ") { "第 ${it.line} 行：${it.layout} × ${it.schema} → ${it.measuredOn}" },
            bad.isEmpty(),
        )
    }

    /** 表裡的佈局與方案要真的存在，`compose` 的每一顆鍵也要在那份佈局上找得到。 */
    @Test
    fun `表裡的佈局與按鍵都真的存在`() {
        val known = RepoFixtures.layoutIds.toSet()
        val problems = ArrayList<String>()
        for (r in rows) {
            if (r.layout !in known) {
                problems += "第 ${r.line} 行：core/layouts 裡沒有 ${r.layout}"
                continue
            }
            val lay = layout(r.layout)
            if (lay.forSchema != listOf("*") && r.schema !in lay.forSchema) {
                problems += "第 ${r.line} 行：${r.layout} 的 for_schema 不含 ${r.schema}"
            }
            if (r.compose.isBlank()) {
                problems += "第 ${r.line} 行：compose 是空的 —— 守門腳本不知道要打什麼"
                continue
            }
            val sendable = HashSet<String>()
            for (layer in lay.layers) {
                for (row in layer.rows) {
                    for (k in row.keys) {
                        val s = k.send
                        if (s is SendSpec.Keysym) sendable += s.name
                    }
                }
            }
            for (name in r.compose.split(' ').filter { it.isNotBlank() }) {
                if (name !in sendable) {
                    problems += "第 ${r.line} 行：${r.layout} 上沒有送得出 `$name` 的鍵"
                }
            }
        }
        assertTrue(problems.joinToString("\n  ", prefix = "\n  "), problems.isEmpty())
    }

    /**
     * **真實佈局 × 真實實測表 → 判準的答案。**
     *
     * 這一條會在三種情況下紅，每一種都是該紅的時候：
     *   · 有人改了判準（`selectionDigitUsable`）
     *   · 有人改了 `core/layouts` 底下的數字列
     *   · 有人改了 `core/selection-digit.tsv`
     */
    @Test
    fun `每一份佈局的每一層都照實測表決定畫不畫`() {
        installTable()
        val yesPairs = rows.filter { it.verdict == SelectionDigits.VERDICT_YES }
            .map { it.layout to it.schema }.toSet()
        val schemas = rows.map { it.schema }.toSortedSet()
        val drawn = ArrayList<String>()
        for (id in RepoFixtures.layoutIds) {
            val lay = layout(id)
            for (layer in lay.layers) {
                val hasRow = CandidateDensity.layerHasSelectionDigitRow(layer)
                for (schema in schemas) {
                    val usable = CandidateDensity.selectionDigitUsable(layer, id, schema)
                    val want = hasRow && (id to schema) in yesPairs
                    if (usable != want) {
                        drawn += "$id/${layer.id} × $schema：判準說 $usable，" +
                            "而（整排數字=$hasRow、實測表=${(id to schema) in yesPairs}）說 $want"
                    }
                }
            }
        }
        assertTrue(drawn.joinToString("\n  ", prefix = "\n  "), drawn.isEmpty())
    }

    /**
     * ⛔ **表裡沒有的組合一律不畫** —— fail-closed 的那一半。
     *
     * `qwerty` 是這一條最重要的一格：它的 `q..p` 有 `hint` 1..0、有
     * `swipe.up`（本端**沒有實作**）也有 `popup`（活的），而判準只認直接按得到
     * 的整排 `send`。照 `swipe` 判就會做出一個「要使用者上滑、上滑卻沒反應」
     * 的序號。
     */
    @Test
    fun `沒有量過的組合一概不畫`() {
        installTable()
        for (id in RepoFixtures.layoutIds) {
            val lay = layout(id)
            for (layer in lay.layers) {
                assertFalse(
                    "$id/${layer.id} 在一個沒有列進實測表的方案上畫了序號",
                    CandidateDensity.selectionDigitUsable(layer, id, "some_third_party_schema"),
                )
            }
        }
        val qwerty = layout("qwerty")
        for (layer in qwerty.layers) {
            assertFalse(
                "qwerty/${layer.id} 被判成「按得到整排數字」—— 它只有 hint、swipe（未實作）與 popup",
                CandidateDensity.layerHasSelectionDigitRow(layer),
            )
        }
    }
}
