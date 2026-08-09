package org.luminakey.ime.home

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * App 畫面上不出現裸的間距與字級數字（`docs/ui-design.md` §3.1 / §3.2、檢核表 F1 / F2）。
 *
 * ── 為什麼這件事值得用測試守 ────────────────────────────────────────────
 * 規範動工前實測過：App 側用了 **26 種 `.dp`、17 種 `.sp`**。
 * 26 種間距不是「比較有彈性」，是**沒有人決定過**。24 與 26 之間沒有人看得出
 * 差別，但它保證每一個新畫面都會再長出一個新數值，而且永遠收不回來 ——
 * 這正是「UI 不對」最具體、也最容易量的那一部分。
 *
 * 收斂之後要**守得住**，否則下一個 commit 就會加回第 27 種。
 *
 * ── ⚠ 這條檢查最容易失效的方式：誤傷，然後被關掉 ─────────────────────────
 * `.dp` 不是全都是間距。把 1dp 的分隔線、48dp 的觸控目標跟間距混在一起掃，
 * 這條檢查會從第一天就是紅的，然後被某個趕時間的人加上 `@Ignore` ——
 * **一條永遠紅的檢查比沒有這條檢查更糟**。
 *
 * 所以這裡的做法是：**不分類，直接禁止任何裸字面值**。
 * 間距、圓角、尺寸各自有自己的 object（[Space] / [Radius] / [Dimens]），
 * 要用哪一個由寫的人決定，測試只管「有沒有繞過它們」。
 * 這樣就沒有「這個 8.dp 到底是間距還是尺寸」的判斷題，也就沒有誤傷。
 *
 * 唯一的例外是 `0.dp`：它表達的是「不要陰影」，不是一個間距（§3.1）。
 */
class DesignTokenTest {

    /* ─────────────── 1. 真的掃一次 ─────────────── */

    @Test
    fun `畫面程式碼裡沒有裸的 dp 與 sp 字面值`() {
        val files = scanScope()

        // G2：範圍非空。掃到零個檔案然後全綠，是本專案發生過的事故形狀。
        val names = files.map { it.name }.toSet()
        val missing = REQUIRED_FILES.filterNot { it in names }
        assertTrue(
            "掃描範圍漏了這幾個檔案，範圍寫錯必須是紅而不是零個違規：$missing",
            missing.isEmpty(),
        )
        assertTrue("只掃到 ${files.size} 個檔案，比下界 $MIN_FILES 少", files.size >= MIN_FILES)

        // ⚠ DesignTokens.kt 是**階梯本身的定義**，它當然要寫出那些數字。
        //   把它排除掉不是開後門：它的內容由下面那條「八階六階」的測試逐值比對，
        //   守得比這條字面值掃描更緊。
        val offenders = files.filterNot { it.name == LADDER_FILE }.flatMap { f ->
            findLiterals(f.readText()).map { "${f.name}:${it.line}  ${it.text}" }
        }
        assertTrue(
            buildString {
                appendLine("這幾行寫了裸的 dp／sp 數字，請改用 Space / Radius / Dimens / TypeScale：")
                offenders.forEach { appendLine("  $it") }
                appendLine()
                appendLine("真的需要一個階梯上沒有的值時，改 DesignTokens.kt 並通知四端，")
                appendLine("不要在單一畫面上開例外 —— 那正是 26 種間距的長法。")
            },
            offenders.isEmpty(),
        )
    }

    /**
     * 階梯本身沒有長出第九階。
     *
     * 這一條守的是**規範文件與程式碼不漂移**：DesignTokens.kt 是 §3.1／§3.2
     * 的 Kotlin 版本，有人往裡面多加一階的時候，這裡會叫。
     */
    @Test
    fun `間距八階、字級六階，值與規範一致`() {
        assertEquals(
            listOf(2, 4, 8, 12, 16, 20, 24, 40),
            listOf(Space.s1, Space.s2, Space.s3, Space.s4, Space.s5, Space.s6, Space.s7, Space.s8)
                .map { it.value.toInt() },
        )
        assertEquals(
            listOf(27f, 17f, 16f, 13.5f, 12.5f, 12f),
            listOf(
                TypeScale.t1, TypeScale.t2, TypeScale.t3,
                TypeScale.t4, TypeScale.t5, TypeScale.t6,
            ).map { it.value },
        )
        assertEquals(
            listOf(16, 11, 9, 8),
            listOf(Radius.large, Radius.medium, Radius.mediumInner, Radius.small)
                .map { it.value.toInt() },
        )
    }

    /* ─────────────── 2. 反向測試（G1）─────────────── */

    /**
     * 植入一個違規，確認它會紅。
     *
     * 這一條要是不會叫，上面那條全綠就只代表「我沒有掃到東西」。
     */
    @Test
    fun `植入一個裸數字會被抓到`() {
        val src = """
            Column(Modifier.padding(Space.s5)) {
                Spacer(Modifier.height(26.dp))
                Text("hi", fontSize = 15.5.sp)
            }
        """.trimIndent()
        val found = findLiterals(src)
        assertEquals(
            "植入 26.dp 與 15.5.sp 之後掃描器沒有全抓到 —— 那上面那條全綠不代表任何事",
            listOf("26.dp", "15.5.sp"),
            found.map { it.literal },
        )
    }

    /** 反向測試的另一半：**乾淨的輸入不可以叫**，而 `0.dp` 是規範明講的例外。 */
    @Test
    fun `token 與 0dp 不算違規`() {
        val src = """
            elevation = CardDefaults.cardElevation(defaultElevation = 0.dp),
            Spacer(Modifier.height(Space.s7))
            Text(t, fontSize = TypeScale.t3, lineHeight = TypeScale.t3Line)
            Box(Modifier.height(Dimens.hairline))
        """.trimIndent()
        assertEquals(emptyList<String>(), findLiterals(src).map { it.literal })
    }

    /** 註解與 KDoc 裡談論數值是**說明**，不是版面。它們不該被抓。 */
    @Test
    fun `註解裡的數字不算違規`() {
        val src = """
            // 以前這裡是 26.dp，看不出與 24.dp 的差別
            /** 實測：14.5.sp 在英文下會折行 */
            Spacer(Modifier.height(Space.s7))
        """.trimIndent()
        assertEquals(
            "註解被判成違規 —— 一個會叫錯的檢查會被關掉，那比沒有更糟",
            emptyList<String>(),
            findLiterals(src).map { it.literal },
        )
    }

    /* ─────────────── 掃描器本體 ─────────────── */

    private class Hit(val line: Int, val literal: String, val text: String)

    /**
     * 找出**程式碼**裡的裸 dp／sp 字面值。
     *
     * 逐行處理，先把註解切掉再比對：
     *   · `//` 之後整段丟掉；
     *   · `*` 開頭（KDoc／區塊註解的續行）整行丟掉。
     *
     * 這不是一個完整的 Kotlin 剖析器 —— 字串字面值裡的 `// ` 會被誤切。
     * 但版面程式碼裡不會有那種東西，而**寧可少抓也不要誤傷**：
     * 誤傷會讓這條檢查被關掉，少抓只是少抓。
     */
    private fun findLiterals(source: String): List<Hit> {
        val out = ArrayList<Hit>()
        source.lineSequence().forEachIndexed { i, raw ->
            val trimmed = raw.trim()
            // 註解的三種開頭：`//`、`/*`（含 `/**`）、以及區塊註解的續行 `*`。
            // 少了 `/*` 那一種，一行寫完的 KDoc（`/** 實測 14.5.sp … */`）會被誤判。
            if (trimmed.startsWith("*") ||
                trimmed.startsWith("//") ||
                trimmed.startsWith("/*")
            ) {
                return@forEachIndexed
            }
            val code = raw.substringBefore("//")
            for (m in LITERAL.findAll(code)) {
                if (m.value == "0.dp") continue
                out += Hit(i + 1, m.value, trimmed)
            }
        }
        return out
    }

    private fun scanScope(): List<File> =
        (homeRoot.listFiles().orEmpty().filter { it.name.endsWith(".kt") } +
            listOfNotNull(storeScreen.takeIf { it.isFile }))
            .sortedBy { it.name }

    companion object {
        private val homeRoot = File("src/main/java/org/luminakey/ime/home")
        private val storeScreen = File("src/main/java/org/luminakey/ime/store/StoreScreen.kt")

        private val LITERAL = Regex("""(?<![\w.])\d+(?:\.\d+)?\.(?:dp|sp)\b""")

        /**
         * ⚠ G4：擴大範圍時要問一次「還有誰不在範圍內」。目前的答案是 ——
         *
         * **在範圍內**：`home/` 的全部，加上 `store/StoreScreen.kt`。
         * **不在範圍內，而且是刻意的**：
         *   · `keyboard/` —— 鍵盤自己的尺寸由 `core/themes` 的 yaml 決定，
         *     不吃 App 側的階梯（`docs/ui-design.md` §0 的邊界）。
         *   · `prefs/KeyRemapSection.kt`、`net/`、`update/`、`store/` 的其餘檔案
         *     —— 這幾個路徑屬於別條線，而且 §7.7 的換鍵整頁重做還沒排到。
         *     它們**現在仍然有裸字面值**；不是通過了，是沒被看。
         */
        private val REQUIRED_FILES = listOf(
            "AppScreen.kt", "DesignTokens.kt", "ImeSetupState.kt",
            "KeyboardChoice.kt", "Onboarding.kt", "SettingsPages.kt", "Ui.kt",
            "StoreScreen.kt",
        )

        private const val MIN_FILES = 8

        /** 階梯的定義檔。它**在**掃描範圍裡（必須存在），但不受字面值那條規則管。 */
        private const val LADDER_FILE = "DesignTokens.kt"
    }
}
