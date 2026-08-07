package org.rimequad.ime.keyboard

import org.rimequad.ime.core.RimeSchema
import org.rimequad.ime.theme.LayoutKind

/**
 * 一份佈局在「鍵盤類型選單」裡需要知道的全部資訊。
 *
 * 刻意不是 [org.rimequad.ime.theme.KeyboardLayout] 本身：那個型別背著整棵
 * layer/row/key 樹，而選單只需要四個欄位。抽出這個小結構讓 [KeyboardTypes]
 * 成為純函式，可以在 JVM 單元測試裡直接餵資料，不必載入 yaml、不碰 Android。
 */
data class LayoutBrief(
    val id: String,
    /** 已依語系解析過的佈局名（§4.9）。 */
    val name: String,
    val kind: LayoutKind,
    /** §9.1.1 的 `for_schema`：資格。`"*"` = 適用於全部方案。 */
    val forSchema: List<String>,
    /** §9.1.1 的 `auto_for_schema`：自動命中。不含 `"*"`。 */
    val autoForSchema: List<String> = emptyList(),
    /** §9.1.1 的 `primary`：使用者的主要英數佈局。 */
    val primary: Boolean = false,
) {
    /** §9.1.1 的 `"*"`：適用於全部方案。 */
    val wildcard: Boolean get() = forSchema.contains("*")

    /** 本佈局宣告自己適用於這個方案（含 `"*"`）。與 `KeyboardLayout.matchesSchema` 同義。 */
    fun matches(schemaId: String): Boolean = wildcard || forSchema.contains(schemaId)

    /** 本佈局**明確**點名這個方案 —— 「為它而做的」，排在選單前面。 */
    fun declares(schemaId: String): Boolean = !wildcard && forSchema.contains(schemaId)

    /**
     * 配件佈局：符號面板、數字面板。
     *
     * 這類佈局不是「一種鍵盤」，而是從某一份鍵盤的 `?123` 鍵進去、按 `ABC`
     * 回來的附屬面板（`switch_layout:@previous` 就是它們的回程）。把它們列進
     * 「我要用哪種鍵盤」等於請使用者選一個他打不出中文的地方住下來。
     */
    val isAccessory: Boolean get() = kind == LayoutKind.SYMBOL || kind == LayoutKind.NUMERIC
}

/**
 * 選單裡的一項 =（方案 + 佈局）。
 *
 * 這就是把內部結構攤平的地方：使用者只選「我要用哪種鍵盤」，
 * 不必先理解「方案」與「佈局」是兩件事。
 */
data class KeyboardType(
    val schemaId: String,
    val schemaName: String,
    /** `null` = 這個方案沒有任何可用佈局，切過去之後交給 §9.1.1 的自動規則。 */
    val layoutId: String?,
    val layoutName: String,
    /** 該佈局的 `for_schema` 明確點名了本方案（而非靠 `"*"` 命中）。 */
    val declared: Boolean,
) {
    /** 選單列表的識別鍵，也是 LazyColumn 的 key。 */
    val key: String get() = "$schemaId/${layoutId ?: AUTO}"

    /** 主標題：使用者眼裡「這是哪一種鍵盤」。 */
    val title: String
        get() = layoutName.ifEmpty { layoutId ?: AUTO_LABEL }

    /** 副標題：這種鍵盤背後是哪個方案。 */
    val subtitle: String get() = schemaName.ifEmpty { schemaId }

    companion object {
        const val AUTO = "auto"
        const val AUTO_LABEL = "自動選擇鍵盤"
    }
}

/** 依語言／地區分組，對應三星選單裡的「简体中文（中国大陆）」那一列標題。 */
data class KeyboardTypeGroup(val title: String, val types: List<KeyboardType>)

/**
 * 把「已啟用的方案」與「可用的佈局」攤平成一份使用者選得動的清單。
 *
 * ── 為什麼要攤平 ────────────────────────────────────────────────────────
 * 本專案內部把「方案」（librime schema，決定怎麼把按鍵碼變成字）與「佈局」
 * （yaml，決定鍵盤長什麼樣）分成兩件事，這在工程上是對的。但使用者心裡只有
 * 一件事：「我要用拼音九宮格」。三星的鍵盤類型選單把兩者攤平成一份清單，
 * 「拼音全键盘 / 拼音九键」是同一個方案配兩份佈局，「双拼 / 笔画 / 五笔」
 * 是不同方案，使用者完全不必分辨。這裡照抄那個模型。
 *
 * ── 一項怎麼來（規則本身）──────────────────────────────────────────────
 * 佈局 L 會出現在方案 S 底下，當且僅當 L 的 `for_schema` **點名**了 S，
 * 或 L 是 `for_schema: ["*"]` 的泛用佈局；而且 L 不是配件（符號／數字面板，
 * 見 [LayoutBrief.isAccessory]）。就這樣，沒有別的條件。
 *
 * ── 這裡曾經有兩層變通，現在都沒了 ────────────────────────────────────
 * `for_schema` 以前同時承擔兩個問題的答案：「這份佈局可以給哪些方案用」
 * （選單要的）與「切到某方案時該自動挑哪一份」（§9.1.1 要的）。只有一個欄位，
 * 於是想給同一個方案加第二份佈局的作者為了**不搶自動命中**，只好把
 * `for_schema` 寫成 `"*"`。那是個謊：照字面它會把九宮格佈局提供給
 * `luna_pinyin`，而九宮格送的 `ADGJMPTW` 是 `t9_pinyin` 的 speller 契約
 * （`core/data/shared/t9_pinyin.schema.yaml` 的 `alphabet`），
 * 配上 luna_pinyin 鍵盤畫得出來、**一個中文字也打不出來**。
 *
 * 為了讓那個謊不傷到使用者，這裡曾經有兩層補救：
 *
 *   1. `"*"` 的佈局若 `inherits` 自一份有點名方案的佈局，就沿用父代的宣告
 *      （`declarationsOf`，靠繼承關係猜作者的真意）；
 *   2. 泛用佈局只收 `kind: alphabetic`，把打不出字的 `"*"` 擋在選單外。
 *
 * 規範 §9.1.1 把欄位拆成 `for_schema`（資格）與 `auto_for_schema`（自動命中）
 * 之後，兩層補救**整段刪掉**：作者可以老實寫 `for_schema: ["t9_pinyin"]` +
 * `auto_for_schema: []`，資格與命中各自表述，選單直接讀 `for_schema` 就對了。
 * 剩下的 `"*"` 佈局（`qwerty`、`intl-*`）是真正的泛用拉丁字母佈局，
 * 配任何方案都成立，不再需要用 `kind` 去猜。
 */
object KeyboardTypes {

    /**
     * @param languages 方案 → BCP 47 語言標記的對照表（見 [SchemaLanguages]）。
     *   預設取行程層那一份；單元測試直接把表傳進來，不必碰 Android。
     *   表裡查不到的方案回落到 [groupTitleOf] 的字面啟發式。
     */
    fun build(
        schemas: List<RimeSchema>,
        layouts: List<LayoutBrief>,
        languages: LanguageTable = SchemaLanguages.table,
    ): List<KeyboardTypeGroup> {
        val groups = LinkedHashMap<String, MutableList<KeyboardType>>()
        val rank = HashMap<String, Int>()
        for (schema in schemas) {
            if (schema.id.isEmpty()) continue
            // 語言標記是資料（索引 / 隨 APK 出貨的對照表），查不到才回落到啟發式。
            val tag = languages.tagOf(schema.id)
            val title = if (tag != null) languages.titleOf(tag) else groupTitleOf(schema)
            // 有標記的依語言表的 order 排；沒標記的排在後面，
            // 「其他」永遠墊底 —— 那是收容所，不該擋在使用者要找的東西前面。
            val order = when {
                tag != null -> languages.orderOf(tag)
                title == OTHER -> LanguageTable.UNRANKED + 9
                else -> LanguageTable.UNRANKED
            }
            rank[title] = minOf(rank[title] ?: order, order)
            groups.getOrPut(title) { ArrayList() } += typesFor(schema, layouts)
        }
        return groups.entries
            .filter { it.value.isNotEmpty() }
            .sortedBy { rank[it.key] ?: LanguageTable.UNRANKED }
            .map { KeyboardTypeGroup(it.key, it.value.toList()) }
    }

    private fun typesFor(
        schema: RimeSchema,
        layouts: List<LayoutBrief>,
    ): List<KeyboardType> {
        val declared = layouts.filter { !it.isAccessory && it.declares(schema.id) }
        val generic = layouts.filter { !it.isAccessory && it.wildcard }
        // primary 排在泛用的最前面 —— 它是使用者的基準全鍵盤，埋在幾份
        // 長得差不多的 QWERTY 中間會讓「我只想要普通鍵盤」變成一件要找的事。
        // 點名的佈局裡，自動命中的那一份排最前面：它就是使用者不做任何事時
        // 會看到的鍵盤，選單的第一項應該與現況一致。
        val usable = declared.sortedByDescending { it.autoForSchema.contains(schema.id) } +
            generic.sortedByDescending { it.primary }
        if (usable.isEmpty()) {
            // 一份都配不上時仍要列出這個方案，否則使用者選不到它 ——
            // 那比「選了得到一個不夠好的鍵盤」嚴重得多。佈局交給 §9.1.1。
            return listOf(
                KeyboardType(schema.id, schema.name, null, KeyboardType.AUTO_LABEL, false)
            )
        }
        val declaredIds = declared.map { it.id }.toSet()
        // 同名佈局要看得出差別。`t9-pinyin` 與 `cn-t9-pinyin` 的 zh-Hant 名字
        // 都是「九宮格拼音」，並排時使用者只能亂猜 —— 猜錯就是換到一個他沒要的
        // 鍵盤。撞名時把 id 補在後面，這是唯一保證唯一的東西。
        val duplicated = usable.groupingBy { it.name }.eachCount().filterValues { it > 1 }.keys
        return usable.map {
            val title =
                if (it.name.isEmpty() || duplicated.contains(it.name)) {
                    listOf(it.name, "（${it.id}）").joinToString("").trim()
                } else {
                    it.name
                }
            KeyboardType(schema.id, schema.name, it.id, title, declaredIds.contains(it.id))
        }
    }

    /* ────────────────────────── 分組 ────────────────────────── */

    const val ZH_TW = "中文（臺灣正體）"
    const val ZH_HK = "中文（香港）"
    const val YUE = "粵語"
    const val ZH = "中文"
    const val OTHER = "其他"

    /**
     * 方案 → 分組標題的**回落路徑**。
     *
     * ⚠ **這是啟發式，不是資料** —— 現在它只在 [SchemaLanguages] 的對照表
     * 查不到這個方案時才跑。
     *
     * 原本它是唯一的路徑，因為 `rs_schema_list()` 只給得出 `(id, name)`
     * 兩個欄位（見 [RimeSchema]），librime 的 `schema.yaml` 也沒有語言／地區
     * 欄位；方案市集的索引有 `category`，但那是「拼音類／形碼類」的分類，
     * 不是語言，而且只涵蓋市集裝進來的方案，隨附的四個方案不在其中。
     *
     * 語言標記現在補在兩處：市集索引每個方案的 `language` 欄位，以及隨 APK
     * 出貨的 `core/schema-languages.json`。兩者都由
     * `scripts/schema_store/languages.py` 產生，判定依據可追溯 ——
     * rppi 的分類路徑、方案名裡的地區字樣、詞庫的繁簡字集探針；
     * 判不出來的標 `und`，**不猜**。
     *
     * 這段**刻意留著**：索引比 app 新、或使用者自己把方案丟進 user_data_dir
     * 時，對照表就是查不到。那時使用者要的是「分組沒那麼準」，
     * 而不是「方案從選單裡消失」。
     *
     * 判準由寬到嚴：地區後綴（`_tw` / `_hk`）→ 方案名裡的地區字樣
     * → 中文字碼族的啟發式。認不出來的一律歸「其他」，**不猜**。
     */
    fun groupTitleOf(schema: RimeSchema): String {
        val id = schema.id.lowercase()
        val name = schema.name
        return when {
            id.endsWith("_hk") || name.contains("香港") -> ZH_HK

            id.contains("jyutping") || id.contains("cantonese") ||
                name.contains("粵") || name.contains("粤") -> YUE

            id.endsWith("_tw") || name.contains("臺灣") || name.contains("台灣") ||
                name.contains("正體") || name.contains("正体") -> ZH_TW

            looksChinese(id, name) -> ZH

            else -> OTHER
        }
    }

    /** 中文輸入方案的字面啟發式：方案名有漢字，或 id 落在已知的方案族裡。 */
    private fun looksChinese(id: String, name: String): Boolean =
        name.any { it.code in 0x4E00..0x9FFF } || CHINESE_FAMILIES.any { id.contains(it) }

    private val CHINESE_FAMILIES = listOf(
        "pinyin", "bopomofo", "zhuyin", "cangjie", "quick", "wubi", "stroke",
        "array", "dayi", "scj", "terra", "luna", "wugniu", "sampheng", "combo",
    )
}
