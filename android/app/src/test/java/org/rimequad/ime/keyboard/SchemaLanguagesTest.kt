package org.rimequad.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.rimequad.ime.core.RimeSchema
import org.rimequad.ime.theme.LayoutKind

/**
 * 選單分組改讀語言標記之後，守兩件事：
 *
 *   1. **有標記就用標記**，不再靠 id 後綴與方案名的字面去猜；
 *   2. **沒標記仍然分得了組** —— 回落到原本的啟發式，方案不會從選單裡消失。
 *
 * 第 2 條比第 1 條重要。標記是資料，而資料一定有涵蓋不到的一天：索引比 app 新、
 * 使用者自己把方案丟進 user_data_dir。那一天使用者可以接受「分組沒那麼準」，
 * 不能接受「我裝的方案不見了」。
 */
class SchemaLanguagesTest {

    private fun schema(id: String, name: String) = RimeSchema(id, name)

    private fun qwerty() = LayoutBrief(
        id = "qwerty",
        name = "QWERTY 英數",
        kind = LayoutKind.ALPHABETIC,
        forSchema = listOf("*"),
        autoForSchema = emptyList(),
        primary = true,
    )

    private val table = LanguageTable(
        tags = mapOf(
            "luna_pinyin_tw" to "zh-Hant-TW",
            "luna_pinyin" to "zh-Hant",
            "rime_ice" to "zh-Hans",
            "jyut6ping3" to "yue-Hant-HK",
            "KappaJP" to "ja",
            // und 是「不是語言」或「判不出來」，等同沒有標記。
            "ipa_xsampa" to "und",
        ),
        names = mapOf(
            "zh-Hant-TW" to "中文（臺灣正體）",
            "zh-Hant" to "中文（繁體）",
            "zh-Hans" to "中文（简体）",
            "yue-Hant-HK" to "粵語（香港）",
            "ja" to "日本語",
        ),
        order = mapOf(
            "zh-Hant-TW" to 1, "zh-Hant" to 3, "zh-Hans" to 4,
            "yue-Hant-HK" to 10, "ja" to 40,
        ),
    )

    /* ── 有標記：讀資料，不猜 ────────────────────────────────────────── */

    @Test
    fun groupsComeFromTheLanguageTagRatherThanTheSchemaName() {
        val groups = KeyboardTypes.build(
            listOf(
                schema("luna_pinyin_tw", "朙月拼音·臺灣正體"),
                schema("rime_ice", "雾凇拼音"),
                schema("jyut6ping3", "粵語拼音"),
            ),
            listOf(qwerty()),
            table,
        )
        assertEquals(
            listOf("中文（臺灣正體）", "中文（简体）", "粵語（香港）"),
            groups.map { it.title },
        )
    }

    /**
     * 這一則是整批測試的重點。`KappaJP`（河童五筆，日文）的方案名裡有漢字，
     * 舊的啟發式 `looksChinese` 會把它算成中文 —— 使用者在「中文」底下看到一個
     * 打日文的鍵盤。有了語言標記就不會。
     */
    @Test
    fun aJapaneseSchemaWithHanCharactersInItsNameIsNoLongerCalledChinese() {
        val japanese = schema("KappaJP", "河童五筆")

        // 沒有標記時，啟發式確實會判錯 —— 這正是要修的東西。
        assertEquals(KeyboardTypes.ZH, KeyboardTypes.groupTitleOf(japanese))

        val groups = KeyboardTypes.build(listOf(japanese), listOf(qwerty()), table)
        assertEquals(listOf("日本語"), groups.map { it.title })
    }

    @Test
    fun groupsAreOrderedByTheLanguageTableNotByTheSchemaListOrder() {
        val groups = KeyboardTypes.build(
            listOf(
                schema("KappaJP", "河童五筆"),
                schema("jyut6ping3", "粵語拼音"),
                schema("luna_pinyin_tw", "朙月拼音·臺灣正體"),
            ),
            listOf(qwerty()),
            table,
        )
        assertEquals(
            listOf("中文（臺灣正體）", "粵語（香港）", "日本語"),
            groups.map { it.title },
        )
    }

    /* ── 沒標記：回落，但不消失 ──────────────────────────────────────── */

    @Test
    fun anUntaggedSchemaFallsBackToTheHeuristicInsteadOfDisappearing() {
        val groups = KeyboardTypes.build(
            listOf(
                schema("luna_pinyin_tw", "朙月拼音·臺灣正體"),   // 有標記
                schema("bopomofo_tw", "注音·臺灣正體"),          // 表裡沒有 → 啟發式
                schema("mystery", "Mystery Method"),             // 兩邊都認不得 → 其他
            ),
            listOf(qwerty()),
            table,
        )
        // 啟發式對 bopomofo_tw 的答案「中文（臺灣正體）」和標記表的顯示名相同，
        // 所以兩者會併進同一組 —— 這是刻意的，回落不該把同一種語言拆成兩堆。
        assertEquals(listOf("中文（臺灣正體）", "其他"), groups.map { it.title })
        assertEquals(
            listOf("luna_pinyin_tw", "bopomofo_tw"),
            groups[0].types.map { it.schemaId },
        )
        assertEquals(listOf("mystery"), groups[1].types.map { it.schemaId })
    }

    @Test
    fun undIsTreatedAsNoTagAtAll() {
        assertNull("und 不是語言，等同沒標記", table.tagOf("ipa_xsampa"))
        val groups = KeyboardTypes.build(
            listOf(schema("ipa_xsampa", "X-SAMPA")),
            listOf(qwerty()),
            table,
        )
        // 落到啟發式：名字裡沒有漢字、id 不在中文族裡 → 其他。**不猜**。
        assertEquals(listOf(KeyboardTypes.OTHER), groups.map { it.title })
    }

    @Test
    fun anEmptyTableBehavesExactlyLikeBeforeTheChange() {
        val schemas = listOf(
            schema("luna_pinyin_tw", "朙月拼音·臺灣正體"),
            schema("luna_pinyin", "朙月拼音"),
        )
        val groups = KeyboardTypes.build(schemas, listOf(qwerty()), LanguageTable.EMPTY)
        assertEquals(listOf(KeyboardTypes.ZH_TW, KeyboardTypes.ZH), groups.map { it.title })
    }

    /* ── 對照表的解析 ────────────────────────────────────────────────── */

    @Test
    fun theShippedTableParsesTagsAndDisplayNames() {
        val parsed = SchemaLanguages.parse(
            """
            {
              "format_version": 1,
              "languages": [
                {"tag": "zh-Hant-TW", "name": "中文（臺灣正體）", "order": 1},
                {"tag": "ja", "name": "日本語", "order": 40}
              ],
              "schemas": {"luna_pinyin_tw": "zh-Hant-TW", "KappaJP": "ja"}
            }
            """.trimIndent()
        )
        requireNotNull(parsed)
        assertEquals("zh-Hant-TW", parsed.tagOf("luna_pinyin_tw"))
        assertEquals("日本語", parsed.titleOf("ja"))
        assertEquals(40, parsed.orderOf("ja"))
        assertNull(parsed.tagOf("沒收錄的方案"))
        // 沒列到的標記不該讓分組炸掉，顯示名退回標記本身。
        assertEquals("ko", parsed.titleOf("ko"))
        assertEquals(LanguageTable.UNRANKED, parsed.orderOf("ko"))
    }

    @Test
    fun aBrokenTableIsIgnoredRatherThanCrashingTheKeyboard() {
        assertNull(SchemaLanguages.parse("{ 這不是 json"))
        assertNull("空表沒有意義，視同讀不到", SchemaLanguages.parse("""{"format_version": 1}"""))
    }

    /**
     * 帳本疊在隨 APK 出貨的那一份之上。
     *
     * 為什麼需要這一層：方案 id 不是全域唯一的 —— `double_pinyin` 在
     * `double-pinyin` 套件裡借的是朙月拼音的繁體詞庫，在 `ice` 套件裡是簡體。
     * 扁平的出貨表沒辦法表達兩者，只有知道「是哪個套件裝的」的帳本分得出來。
     */
    @Test
    fun theInstalledLedgerOverridesTheShippedTable() {
        val shipped = LanguageTable(
            tags = mapOf("luna_pinyin" to "zh-Hant"),
            names = mapOf("zh-Hant" to "中文（繁體）", "zh-Hans" to "中文（简体）"),
            order = mapOf("zh-Hant" to 3, "zh-Hans" to 4),
        )
        val merged = shipped.overlay(mapOf("double_pinyin" to "zh-Hans"))
        assertEquals("zh-Hans", merged.tagOf("double_pinyin"))
        assertEquals("zh-Hant", merged.tagOf("luna_pinyin"))
        assertTrue("顯示名沿用出貨表", merged.titleOf("zh-Hans") == "中文（简体）")
    }
}
