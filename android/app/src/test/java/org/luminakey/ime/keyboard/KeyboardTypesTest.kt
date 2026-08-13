package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.core.RimeSchema
import org.luminakey.ime.theme.LayoutKind
import org.luminakey.ime.theme.LayoutLoader
import org.luminakey.ime.theme.Platform
import org.luminakey.ime.theme.RepoFixtures

/**
 * 鍵盤類型選單的資料模型。
 *
 * 這批測試守的是使用者摸得到什麼：**repo 裡每一份佈局都必須在某個方案底下
 * 出現在選單裡**，否則做了也等於沒做 —— 那正是這輪改動要修的缺陷。
 */
class KeyboardTypesTest {

    private fun schema(id: String, name: String) = RimeSchema(id, name)

    private fun brief(
        id: String,
        name: String = id,
        kind: LayoutKind = LayoutKind.ALPHABETIC,
        vararg forSchema: String,
        primary: Boolean = false,
        autoForSchema: List<String>? = null,
    ) = LayoutBrief(
        id, name, kind, forSchema.toList(),
        // §9.1.1 的預設：auto_for_schema 沒寫時 = for_schema 去掉 "*"。
        autoForSchema ?: forSchema.filter { it != "*" },
        primary,
    )

    /* ── 攤平 ─────────────────────────────────────────────────────────── */

    @Test
    fun oneSchemaWithSeveralLayoutsBecomesSeveralEntries() {
        // 這就是三星的「拼音全键盘 / 拼音九键」：同一個方案，兩種鍵盤。
        val groups = KeyboardTypes.build(
            listOf(schema("luna_pinyin", "朙月拼音")),
            listOf(
                brief("qwerty", "QWERTY 英數", forSchema = arrayOf("*")),
                brief("cn-qwerty-numrow", "全鍵盤・數字列", forSchema = arrayOf("luna_pinyin")),
                brief("cn-t9-pinyin", "九宮格拼音", LayoutKind.GRID, "luna_pinyin"),
            ),
        )
        assertEquals(1, groups.size)
        assertEquals(
            listOf("cn-qwerty-numrow", "cn-t9-pinyin", "qwerty"),
            groups[0].types.map { it.layoutId },
        )
        assertTrue("每一項都掛在同一個方案上", groups[0].types.all { it.schemaId == "luna_pinyin" })
    }

    /**
     * **一份打不出字的鍵盤不該出現在選單裡。**
     *
     * 九宮格送的是 `A/D/G/J/M/P/T/W` 八個代表鍵，那套字母表是 `t9_pinyin`
     * 方案的 speller 契約。同一份九宮格配上 `luna_pinyin`，按「abc」送出的
     * 只是字母 a —— 鍵盤畫得出來，一個中文字也打不出來。
     *
     * 欄位拆開之後，這件事由**資料**保證而不是靠 `kind` 猜：九宮格佈局老實
     * 寫 `for_schema: ["t9_pinyin"]`，於是它不可能落到 luna_pinyin 底下。
     */
    @Test
    fun aLayoutOnlyShowsUnderTheSchemasItDeclares() {
        val layouts = listOf(
            brief("qwerty", forSchema = arrayOf("*"), primary = true),
            brief("cn-t9-pinyin", kind = LayoutKind.GRID, forSchema = arrayOf("t9_pinyin")),
        )
        val luna = KeyboardTypes.build(listOf(schema("luna_pinyin", "朙月拼音")), layouts)
        assertEquals(listOf("qwerty"), luna.single().types.map { it.layoutId })
    }

    /**
     * **同一個方案的第二份佈局不必再說謊。**
     *
     * 拆欄位以前，`cn-t9-pinyin-numrow` 為了不搶 `cn-t9-pinyin` 的自動命中，
     * 只能把 `for_schema` 寫成 `"*"`；那句 `"*"` 照字面會把九宮格提供給每一個
     * 方案。現在它寫 `for_schema: ["t9_pinyin"]` + `auto_for_schema: []`：
     * 選單裡有，自動命中沒有，兩件事各自表述。
     */
    @Test
    fun aSecondLayoutForTheSameSchemaNeedsNoWildcardLie() {
        val layouts = listOf(
            brief("qwerty", forSchema = arrayOf("*"), primary = true),
            brief("cn-t9-pinyin", kind = LayoutKind.GRID, forSchema = arrayOf("t9_pinyin")),
            brief(
                "cn-t9-pinyin-numrow",
                kind = LayoutKind.GRID,
                forSchema = arrayOf("t9_pinyin"),
                autoForSchema = emptyList(),
            ),
        )
        val t9 = KeyboardTypes.build(listOf(schema("t9_pinyin", "九宮格拼音")), layouts)
        assertEquals(
            listOf("cn-t9-pinyin", "cn-t9-pinyin-numrow", "qwerty"),
            t9.single().types.map { it.layoutId },
        )
        // 而且**只**落在 t9_pinyin 底下。
        val luna = KeyboardTypes.build(listOf(schema("luna_pinyin", "朙月拼音")), layouts)
        assertEquals(listOf("qwerty"), luna.single().types.map { it.layoutId })
    }

    /** 自動命中的那一份排在同方案其他佈局之前：選單第一項＝使用者現在看到的鍵盤。 */
    @Test
    fun theAutomaticPickLeadsTheOtherLayoutsOfTheSameSchema() {
        val groups = KeyboardTypes.build(
            listOf(schema("t9_pinyin", "九宮格拼音")),
            listOf(
                brief(
                    "t9-pinyin", kind = LayoutKind.GRID,
                    forSchema = arrayOf("t9_pinyin"), autoForSchema = emptyList(),
                ),
                brief("cn-t9-pinyin", kind = LayoutKind.GRID, forSchema = arrayOf("t9_pinyin")),
            ),
        )
        assertEquals("cn-t9-pinyin", groups.single().types.first().layoutId)
    }

    @Test
    fun theUsersPrimaryKeyboardLeadsTheGenericOnes() {
        val groups = KeyboardTypes.build(
            listOf(schema("luna_pinyin", "朙月拼音")),
            listOf(
                brief("intl-gboard", forSchema = arrayOf("*")),
                brief("intl-ios", forSchema = arrayOf("*")),
                brief("qwerty", forSchema = arrayOf("*"), primary = true),
            ),
        )
        assertEquals("qwerty", groups.single().types.first().layoutId)
    }

    @Test
    fun layoutsThatNameTheSchemaComeBeforeTheGenericOnes() {
        // 「為這個方案做的」要排前面：使用者第一眼看到的應該是能用的那幾個，
        // 不是恰好 for_schema 寫了 "*" 的通用鍵盤。
        val groups = KeyboardTypes.build(
            listOf(schema("bopomofo_tw", "注音·臺灣正體")),
            listOf(
                brief("qwerty", forSchema = arrayOf("*")),
                brief("intl-ios", forSchema = arrayOf("*")),
                brief("bopomofo-dachen", kind = LayoutKind.PHONETIC, forSchema = arrayOf("bopomofo_tw")),
            ),
        )
        val types = groups.single().types
        assertEquals("bopomofo-dachen", types.first().layoutId)
        assertTrue(types.first().declared)
        assertFalse(types.drop(1).any { it.declared })
    }

    @Test
    fun symbolAndNumericPanelsAreNotKeyboardTypes() {
        // 符號／數字面板是從 ?123 進去、按 ABC 回來的附屬面板。
        // 把它列進「我要用哪種鍵盤」等於請使用者搬進一個打不出中文的地方。
        val groups = KeyboardTypes.build(
            listOf(schema("luna_pinyin", "朙月拼音")),
            listOf(
                brief("qwerty", forSchema = arrayOf("*")),
                brief("numeric-symbol", kind = LayoutKind.SYMBOL, forSchema = arrayOf("*")),
                brief("cn-symbols", kind = LayoutKind.SYMBOL, forSchema = arrayOf("*")),
            ),
        )
        assertEquals(listOf("qwerty"), groups.single().types.map { it.layoutId })
    }

    @Test
    fun aSchemaNeverDisappearsFromTheMenu() {
        // 沒有任何佈局適用時仍要列出來，否則使用者選不到這個方案 ——
        // 那比「選了得到一個不好看的鍵盤」嚴重得多。
        val groups = KeyboardTypes.build(
            listOf(schema("wubi86", "五筆86")),
            listOf(brief("bopomofo-dachen", forSchema = arrayOf("bopomofo_tw"))),
        )
        val only = groups.single().types.single()
        assertEquals("wubi86", only.schemaId)
        assertNull("沒有佈局可挑時交給 §9.1.1 的自動規則", only.layoutId)
    }

    @Test
    fun whenEveryMatchIsAnAccessoryTheSchemaFallsBackToTheAutomaticRule() {
        // 只剩符號面板可挑時，寧可交給 §9.1.1 也不要請使用者搬進一個
        // 打不出中文的面板 —— 但方案本身仍然選得到。
        val groups = KeyboardTypes.build(
            listOf(schema("s", "方案")),
            listOf(brief("cn-symbols", kind = LayoutKind.SYMBOL, forSchema = arrayOf("*"))),
        )
        assertNull(groups.single().types.single().layoutId)
    }

    /* ── 分組 ─────────────────────────────────────────────────────────── */

    @Test
    fun schemasAreGroupedByLanguageAndGroupsKeepSchemaListOrder() {
        val groups = KeyboardTypes.build(
            listOf(
                schema("luna_pinyin_tw", "朙月拼音·臺灣正體"),
                schema("bopomofo_tw", "注音·臺灣正體"),
                schema("luna_pinyin", "朙月拼音"),
                schema("t9_pinyin", "九宮格拼音"),
            ),
            listOf(brief("qwerty", forSchema = arrayOf("*"))),
        )
        assertEquals(listOf(KeyboardTypes.ZH_TW, KeyboardTypes.ZH), groups.map { it.title })
        assertEquals(
            listOf("luna_pinyin_tw", "bopomofo_tw"),
            groups[0].types.map { it.schemaId },
        )
        assertEquals(listOf("luna_pinyin", "t9_pinyin"), groups[1].types.map { it.schemaId })
    }

    @Test
    fun unrecognisedSchemasGoToOtherRatherThanBeingGuessedAt() {
        assertEquals(KeyboardTypes.OTHER, KeyboardTypes.groupTitleOf(schema("ipa_xsampa", "X-SAMPA")))
        assertEquals(KeyboardTypes.ZH_HK, KeyboardTypes.groupTitleOf(schema("cangjie5_hk", "倉頡")))
        assertEquals(KeyboardTypes.YUE, KeyboardTypes.groupTitleOf(schema("jyutping", "粵拼")))
        assertEquals(KeyboardTypes.ZH, KeyboardTypes.groupTitleOf(schema("stroke", "五筆畫")))
    }

    /**
     * `t9-pinyin` 與 `cn-t9-pinyin` 的 zh-Hant 名字都是「九宮格拼音」。
     * 兩張並排的卡片長得一模一樣時，使用者只能亂猜。
     */
    @Test
    fun layoutsSharingAnameAreDisambiguatedByTheirId() {
        val groups = KeyboardTypes.build(
            listOf(schema("t9_pinyin", "九宮格拼音")),
            listOf(
                brief("t9-pinyin", "九宮格拼音", LayoutKind.GRID, "t9_pinyin"),
                brief("cn-t9-pinyin", "九宮格拼音", LayoutKind.GRID, "t9_pinyin"),
                brief("qwerty", "QWERTY 英數", forSchema = arrayOf("*")),
            ),
        )
        val titles = groups.single().types.map { it.title }
        // ⚠ 這兩份在本夾具裡**同時**自動命中 `t9_pinyin`（`brief()` 的預設是
        //   `auto_for_schema = for_schema − "*"`）。從前第一項是
        //   「t9-pinyin」——那只是因為它在傳進來的 `listOf` 裡排前面,
        //   也就是這一輪要修的那條「清單順序決勝」。改成 [LayoutPriority]
        //   之後,同級用 id 字典序:`cn-t9-pinyin` < `t9-pinyin`。
        //   這一條測的是**撞名要看得出差別**,順序由上面那兩條專門的測試守。
        assertEquals(
            listOf("九宮格拼音（cn-t9-pinyin）", "九宮格拼音（t9-pinyin）", "QWERTY 英數"),
            titles,
        )
    }

    @Test
    fun schemasWithoutAnIdAreDropped() {
        assertTrue(
            KeyboardTypes.build(listOf(schema("", "")), listOf(brief("qwerty", forSchema = arrayOf("*"))))
                .isEmpty()
        )
    }

    /* ── 真檔：每一份佈局都到得了 ─────────────────────────────────────── */

    /**
     * **這一條是本輪改動的驗收條件本身。**
     *
     * repo 的 `core/layouts` 裡有十幾份佈局，改動前使用者只碰得到自動綁上來
     * 的那一份。這裡直接讀真檔，要求每一份非配件佈局都至少在某個方案底下
     * 出現一次 —— 加了新佈局卻忘記讓它可達，這條就會紅。
     */
    @Test
    fun everyNonAccessoryLayoutInTheRepoIsReachableFromTheMenu() {
        val briefs = repoBriefs()
        // 方案清單 = 隨附的四個 + 每一份佈局自己點名的那些。這樣新增一份
        // 「給某個尚未隨附的方案」的佈局時，這條測試不會無端變紅，
        // 但「加了佈局卻沒有任何方案帶得出它」仍然會被抓到。
        val ids = LinkedHashSet(
            listOf("luna_pinyin_tw", "bopomofo_tw", "luna_pinyin", "t9_pinyin")
        )
        briefs.forEach { ids += it.forSchema.filter { s -> s != "*" } }
        val schemas = ids.map { schema(it, it) }
        val offered = KeyboardTypes.build(schemas, briefs)
            .flatMap { g -> g.types.mapNotNull { it.layoutId } }
            .toSet()
        val expected = briefs.filterNot { it.isAccessory }.map { it.id }.toSet()
        assertEquals(
            "這幾份佈局在選單裡碰不到，等於使用者用不到：" + (expected - offered),
            expected,
            offered,
        )
    }

    /* ── 兩份佈局同時自動命中：決勝必須是**確定的** ────────────────────── */

    /**
     * ⛔ 這條規則從前不存在。`declared.firstOrNull { it.autoForSchema… }`
     * 拿的是清單順序，而那份清單來自 `File.listFiles()` —— **檔案系統不保證
     * 順序**。症狀是 `scripts/verify_selection_digit.sh` 一次紅一次綠，
     * 紅的那一次裝置上載進去的根本不是它要驗的那一份佈局。
     *
     * 這一條把「同一組輸入、不同的排列」釘成同一個答案。
     */
    @Test
    fun theAutoHitWinnerDoesNotDependOnListOrder() {
        val a = brief("cn-t9-pinyin", "九宮格拼音", LayoutKind.GRID, "t9_pinyin")
        val b = brief("cn-t9-pinyin-numrow", "九宮格・數字列", LayoutKind.GRID, "t9_pinyin")
        val qwerty = brief("qwerty", "QWERTY", forSchema = arrayOf("*"))

        fun headOf(layouts: List<LayoutBrief>): String? =
            KeyboardTypes.build(listOf(schema("t9_pinyin", "九宮格拼音")), layouts)[0]
                .types[0].layoutId

        assertEquals("id 字典序：cn-t9-pinyin 在前", "cn-t9-pinyin", headOf(listOf(a, b, qwerty)))
        assertEquals(
            "同一組佈局換個排列，第一項必須一模一樣",
            headOf(listOf(a, b, qwerty)),
            headOf(listOf(qwerty, b, a)),
        )
    }

    /**
     * 使用者自己放進 `files/rime/user/layouts/` 的那一份勝過隨附的同級對手。
     * 那是他的明示意圖 —— 被隨附的蓋過去，等於他改了檔案卻什麼都沒發生。
     */
    @Test
    fun aUserProvidedLayoutWinsTheAutoHit() {
        val shipped = brief("cn-t9-pinyin", "九宮格拼音", LayoutKind.GRID, "t9_pinyin")
        val mine = brief("cn-t9-pinyin-numrow", "九宮格・數字列", LayoutKind.GRID, "t9_pinyin")
            .copy(userProvided = true)
        val groups = KeyboardTypes.build(
            listOf(schema("t9_pinyin", "九宮格拼音")),
            listOf(shipped, mine),
        )
        assertEquals("cn-t9-pinyin-numrow", groups[0].types[0].layoutId)
    }

    /** 決勝規則本身：使用者目錄 > 隨附，同級 id 字典序。 */
    @Test
    fun layoutPriorityRanksUserDirAboveShipped() {
        assertTrue(LayoutPriority.rank(true, "zzz") < LayoutPriority.rank(false, "aaa"))
        assertTrue(LayoutPriority.rank(false, "aaa") < LayoutPriority.rank(false, "aab"))
        assertTrue(LayoutPriority.rank(true, "aaa") < LayoutPriority.rank(true, "aab"))
    }

    /** 真檔讀出來的摘要。用的是 [RepoFixtures] 的目錄，不是複製品。 */
    private fun repoBriefs(): List<LayoutBrief> =
        RepoFixtures.coreDir.resolve("layouts").listFiles()
            .orEmpty()
            .filter { it.isFile && it.name.endsWith(".yaml") }
            .map { it.name.removeSuffix(".yaml") }
            .sorted()
            .mapNotNull { id ->
                LayoutLoader.load(id, RepoFixtures.layouts, Platform.ANDROID, locale = LOCALE)
                    .value
                    ?.let {
                        LayoutBrief(
                            it.id, it.name.get(LOCALE), it.kind, it.forSchema,
                            it.autoForSchema, it.primary,
                        )
                    }
            }

    private companion object {
        const val LOCALE = "zh-Hant-TW"
    }
}
