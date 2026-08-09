package org.luminakey.ime

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.core.DeployEstimate
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory

/**
 * 「整理字詞要多久」**全專案只有一個數字**。
 *
 * ── 為什麼這件事值得一條測試 ────────────────────────────────────────────
 * 稽核之前它散在七個地方而且互相矛盾：鍵盤上那句以分鐘計、首頁與引導頁的文案
 * 以十秒計、引導頁的進度條分母又是另一個值、市集的說明再一個。使用者在同一次
 * 首次啟動裡至少看得到其中三句 —— 那不只是不一致，是讓他**無法判斷自己是不是
 * 卡住了**。而這種矛盾沒有任何自動化看得見：每一句單獨讀都很合理。
 *
 * 現在數字只有 [DeployEstimate] 一個來源（量測來源也記在那裡），文案裡那個數字
 * 一律是 placeholder。這支測試守的就是「不要有人又自己寫一個」。
 *
 * ── 這個掃描器最可能怎麼失效 ────────────────────────────────────────────
 * **不是漏抓，是掃到零個檔案然後全綠。** 所以：
 *   · G2 範圍非空：[REQUIRED] 的每一個都要在掃描範圍裡，而且檔案總數 ≥ [MIN_FILES]。
 *   · G1 反向測試：植入幾種真的出現過的寫法，確認同一支掃描器會叫；再餵乾淨的
 *     輸入，確認它不會亂叫 —— 會亂叫的檢查會被關掉，那比沒有更糟。
 *
 * ── 它抓不到什麼（誠實說明）──────────────────────────────────────────────
 * · 只認「數字 + 秒／分鐘／second／minute」。模糊的說法（「十幾秒」「好幾秒」
 *   「數十秒」）**刻意放行** —— 它們不是一個會過期的數字。
 * · 毫秒不管（`delay(200)`、`POLL_MS` 這些是實作參數，不是講給使用者聽的預期）。
 * · 掃描範圍不含另外三端（apple / windows / core）。四端之間的一致性沒有人守。
 * · 本檔自己被排除在掃描之外 —— 它必須含有反向測試用的樣本。
 */
class DeployEstimateTest {

    /* ─────────────── 1. 常數本身 ─────────────── */

    /**
     * 文案上的秒數**永遠不會少講**。
     *
     * 少講的代價是使用者在最後那一下認定它壞了；多講的代價只是他提早拿到好消息。
     * 所以是無條件進位，不是四捨五入。
     */
    @Test
    fun `文案的秒數是實測值的無條件進位`() {
        assertTrue(
            "文案說 ${DeployEstimate.TYPICAL_SECONDS}，實測是 ${DeployEstimate.TYPICAL_MS} ms —— 說少了",
            DeployEstimate.TYPICAL_SECONDS * 1000L >= DeployEstimate.TYPICAL_MS,
        )
        assertTrue(
            "進位過頭了：多講了一整格以上，那就不是同一個數字了",
            (DeployEstimate.TYPICAL_SECONDS - 1) * 1000L < DeployEstimate.TYPICAL_MS,
        )
    }

    /* ─────────────── 2. 真的掃一次 ─────────────── */

    @Test
    fun `除了 DeployEstimate 以外沒有人自己寫死一個時間`() {
        val (files, hits) = scan()

        // G2：範圍非空，而且涵蓋每一個該掃的檔案。
        val missing = REQUIRED.filterNot { r -> files.any { it.endsWith(r) } }
        assertTrue(
            "掃描範圍漏了這幾個 —— 範圍寫錯的時候必須是紅，不是零個違規：$missing",
            missing.isEmpty(),
        )
        assertTrue(
            "只掃到 ${files.size} 個檔案，比基準 $MIN_FILES 少，通常代表範圍寫錯或檔案被搬走",
            files.size >= MIN_FILES,
        )

        assertTrue(
            buildString {
                appendLine("這幾處自己寫死了一個時間數字。")
                appendLine("本專案對「整理字詞要多久」只留一個數字，見 core/DeployEstimate.kt：")
                hits.forEach { appendLine("  ${it.where}  「${it.text}」  ← ${it.rule}") }
                appendLine("文案請改用 placeholder（值傳 DeployEstimate.TYPICAL_SECONDS），")
                appendLine("註解請改成引用 DeployEstimate，不要在那裡再抄一次數字。")
            },
            hits.isEmpty(),
        )
    }

    /* ─────────────── 3. 反向測試（G1）─────────────── */

    @Test
    fun `植入一個寫死的時間會被抓到`() {
        assertEquals(listOf("10 seconds"), texts("it takes about 10 seconds"))
        assertEquals(listOf("ten seconds"), texts("takes about ten seconds."))
        assertEquals(listOf("一到兩分鐘"), texts("首次啟動：正在編譯詞庫，需要一到兩分鐘…"))
        assertEquals(listOf("12.5 秒"), texts("使用者的 S24U 上 12.5 秒"))
        assertEquals(listOf("7 秒"), texts("實測 3 個方案約 7 秒"))
    }

    @Test
    fun `模糊的說法與 placeholder 不算`() {
        // 沒有一個會過期的數字 —— 這些刻意放行。
        assertEquals(emptyList<String>(), texts("只有第一次要做，大概十幾秒。"))
        assertEquals(emptyList<String>(), texts("解壓在模擬器上要好幾秒"))
        assertEquals(emptyList<String>(), texts("大方案數十秒起跳"))
        assertEquals(emptyList<String>(), texts("卡住半秒以上"))
        assertEquals(emptyList<String>(), texts("從幾十毫秒變成好幾秒"))
        // placeholder：數字是從 DeployEstimate 傳進去的，正是我們**要**的寫法。
        assertEquals(emptyList<String>(), texts("takes about %1${S}d seconds"))
        assertEquals(emptyList<String>(), texts("已耗時 %1${S}d 秒。一般大約 %2${S}d 秒"))
        assertEquals(emptyList<String>(), texts("Words sorted (took %1${S}s seconds)"))
        // 序數不是時間。
        assertEquals(emptyList<String>(), texts("Second key"))
    }

    /* ─────────────── 掃描器本體 ─────────────── */

    private class Hit(val where: String, val rule: String, val text: String)

    private fun scan(): Pair<List<String>, List<Hit>> {
        val files = ArrayList<String>()
        val hits = ArrayList<Hit>()

        for (base in listOf(File("src/main/java"), File("src/test/java"))) {
            base.walkTopDown()
                .filter { it.isFile && it.name.endsWith(".kt") && it.name !in EXCLUDED }
                .sortedBy { it.path }
                .forEach { f ->
                    files += f.path
                    hitsIn(f.readText()).forEach { hits += Hit(f.path, it.first, it.second) }
                }
        }

        File("src/main/res").listFiles().orEmpty()
            .filter { it.isDirectory && it.name.startsWith("values") }
            .flatMap { d ->
                d.listFiles().orEmpty()
                    .filter { it.name.startsWith("strings") && it.name.endsWith(".xml") }
            }
            .sortedBy { it.path }
            .forEach { f ->
                files += f.path
                val doc = DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(f)
                val nodes = doc.getElementsByTagName("string")
                for (i in 0 until nodes.length) {
                    val e = nodes.item(i)
                    val id = e.attributes?.getNamedItem("name")?.nodeValue ?: continue
                    hitsIn(e.textContent.orEmpty()).forEach {
                        hits += Hit("${f.parentFile.name}/${f.name}:$id", it.first, it.second)
                    }
                }
            }
        return files to hits
    }

    /** 給反向測試用的入口：同一套規則，輸入換成一段字串。 */
    private fun texts(s: String): List<String> = hitsIn(s).map { it.second }

    private fun hitsIn(text: String): List<Pair<String, String>> {
        // placeholder 先拿掉：帶 placeholder 的文案，數字是從 DeployEstimate 傳進來的，
        // 那正是我們**要**的寫法，不可以被自己的規則判成違規。
        val t = PLACEHOLDER.replace(text, "")
        return RULES.flatMap { (rule, rx) -> rx.findAll(t).map { rule to it.value }.toList() }
    }

    companion object {
        /** 錢字號。直接寫在 Kotlin 字串裡會被當成字串模板的開頭。 */
        private const val S = '$'

        private val EXCLUDED = setOf(
            // 唯一可以寫那個數字的地方。
            "DeployEstimate.kt",
            // 本檔含有反向測試用的樣本，掃自己一定會叫。
            "DeployEstimateTest.kt",
        )

        /** G2：這幾個一定要在掃描範圍裡，否則就是範圍寫錯了。 */
        private val REQUIRED = listOf(
            "home/AppScreen.kt",
            "home/Onboarding.kt",
            "home/ImeSetupState.kt",
            "ime/RimeInputMethodService.kt",
            "store/StoreController.kt",
            "store/DeployGate.kt",
            "values/strings.xml",
            "values-b+zh+Hant/strings.xml",
            "values-b+zh+Hans/strings.xml",
        )

        /** 2026-08-10 實測 157 個。取 120 當下界，留一點正常增減的空間。 */
        private const val MIN_FILES = 120

        /** `%1$s`、`%2$d`…。`\x24` 就是錢字號，寫成 escape 是為了不必跟 Kotlin 的模板打架。 */
        private val PLACEHOLDER = Regex("""%(\d+\x24)?[sdf]""")

        /**
         * 「一個會過期的時間數字」的四種長法。
         *
         * ⚠ 中文那一條有一個 negative lookbehind：`數十秒`／`好幾秒`／`十幾秒`
         * 都不是確切數字，放行。少了它，滿滿的合理註解會被判成違規，
         * 然後整條檢查會被關掉。
         */
        private val RULES: List<Pair<String, Regex>> = listOf(
            "數字+秒/分鐘" to Regex("""\d+(\.\d+)?\s*(秒|分鐘|分钟)"""),
            "digits+seconds/minutes" to
                Regex("""\d+(\.\d+)?\s*(seconds?|minutes?)\b""", RegexOption.IGNORE_CASE),
            "中文數字+秒/分鐘" to Regex(
                """(?<![數数好幾几])[一二三四五六七八九十兩两]+""" +
                    """(\s*到\s*[一二三四五六七八九十兩两]+)?\s*(秒|分鐘|分钟)"""
            ),
            "number word+seconds/minutes" to Regex(
                """\b(one|two|three|four|five|six|seven|eight|nine|ten|""" +
                    """eleven|twelve|fifteen|twenty|thirty|forty|fifty|sixty)""" +
                    """\s+(seconds?|minutes?)\b""",
                RegexOption.IGNORE_CASE,
            ),
        )
    }
}
