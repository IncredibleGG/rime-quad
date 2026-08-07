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
    val forSchema: List<String>,
    /** §9.1.1 的 `primary`：使用者的主要英數佈局。 */
    val primary: Boolean = false,
    /** §7.1 的繼承鏈，最遠祖先在前、自己在最後。見 [KeyboardTypes] 的推導。 */
    val ancestry: List<String> = emptyList(),
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
 * 佈局 L 會出現在方案 S 底下，當且僅當：
 *
 *   1. L **點名**了 S（`for_schema` 含 S），或
 *   2. L 是 `for_schema: ["*"]` 的 **`kind: alphabetic`** 佈局。
 *
 * 而且 L 不是配件（符號／數字面板，見 [LayoutBrief.isAccessory]）。
 *
 * ── 第 2 條為什麼要限定 alphabetic ────────────────────────────────────
 * **因為不限定的話，選單會列出打不出字的鍵盤。** 九宮格佈局送的是
 * `A/D/G/J/M/P/T/W` 八個代表鍵，那套字母表是 `t9_pinyin` 方案的 speller
 * 契約（見 `core/data/shared/t9_pinyin.schema.yaml` 的 `alphabet: 'ADGJMPTW'`）。
 * 同一份九宮格配上 `luna_pinyin`，按「abc」送出的就只是字母 a ——
 * 鍵盤畫得出來，一個中文字也打不出來。
 *
 * 反過來，`kind: alphabetic` 的 `"*"` 佈局（`qwerty`、`intl-*`）送的是
 * 普通拉丁字母，而本 repo 每一個方案的輸入碼都收拉丁字母，所以它們配誰都成立。
 *
 * ── ⚠ 資料面的缺陷（見回報）───────────────────────────────────────────
 * `for_schema` 同時承擔兩個問題的答案：「這份佈局可以給哪些方案用」（選單要
 * 的）與「切到某方案時該自動挑哪一份」（§9.1.1 要的）。只有一個欄位，於是
 * 佈局作者為了**不搶自動命中**，只好把 `for_schema` 寫成 `"*"` ——
 * `cn-t9-pinyin-numrow.yaml` 的註解白紙黑字這麼寫：
 *
 * > 不繼承 cn-t9-pinyin 的 ["t9_pinyin"]：避免兩份佈局搶同一個方案的自動命中。
 *
 * 那份佈局其實只給 `t9_pinyin` 用，`"*"` 是規範逼出來的謊。所以這裡多一條
 * 補救：**`"*"` 的佈局若 `inherits` 自一份有點名方案的佈局，就沿用父代的
 * 宣告**（[declarationsOf]）。這讓 `cn-t9-pinyin-numrow` 正確地落在
 * `t9_pinyin` 底下，而不是散在每一個方案下面變成一顆地雷。
 *
 * 正解是規範把兩件事拆成兩個欄位（例如 `for_schema` 管適用、
 * `auto_for_schema` 管自動命中）。拆開之後這段補救可以整段刪掉。
 */
object KeyboardTypes {

    fun build(schemas: List<RimeSchema>, layouts: List<LayoutBrief>): List<KeyboardTypeGroup> {
        val byId = layouts.associateBy { it.id }
        val resolved = layouts.map { it to declarationsOf(it, byId) }
        val groups = LinkedHashMap<String, MutableList<KeyboardType>>()
        for (schema in schemas) {
            if (schema.id.isEmpty()) continue
            val bucket = groups.getOrPut(groupTitleOf(schema)) { ArrayList() }
            bucket += typesFor(schema, resolved)
        }
        return groups.entries
            .filter { it.value.isNotEmpty() }
            .map { KeyboardTypeGroup(it.key, it.value.toList()) }
    }

    /**
     * 佈局實際宣告了哪些方案。`["*"]` 代表「泛用」。
     *
     * `"*"` 的佈局會往 `inherits` 鏈上找（由近而遠）第一個有點名方案的祖先，
     * 沿用它的宣告 —— 見本物件檔頭談 `cn-t9-pinyin-numrow` 的那一段。
     */
    internal fun declarationsOf(
        layout: LayoutBrief,
        byId: Map<String, LayoutBrief>,
    ): List<String> {
        if (!layout.wildcard) return layout.forSchema
        for (ancestorId in layout.ancestry.asReversed()) {
            if (ancestorId == layout.id) continue
            val ancestor = byId[ancestorId] ?: continue
            if (!ancestor.wildcard) return ancestor.forSchema
        }
        return WILDCARD
    }

    private fun typesFor(
        schema: RimeSchema,
        resolved: List<Pair<LayoutBrief, List<String>>>,
    ): List<KeyboardType> {
        val declared = resolved.filter { (l, decl) -> !l.isAccessory && decl.contains(schema.id) }
        // 泛用佈局只收 alphabetic：其餘的 `"*"` 是規範逼出來的，配上別的方案
        // 打不出字。理由見檔頭。
        val generic = resolved.filter { (l, decl) ->
            !l.isAccessory && decl.contains("*") && l.kind == LayoutKind.ALPHABETIC
        }
        // primary 排在泛用的最前面 —— 它是使用者的基準全鍵盤，埋在幾份
        // 長得差不多的 QWERTY 中間會讓「我只想要普通鍵盤」變成一件要找的事。
        val usable = declared.map { it.first } +
            generic.map { it.first }.sortedByDescending { it.primary }
        if (usable.isEmpty()) {
            // 一份都配不上時仍要列出這個方案，否則使用者選不到它 ——
            // 那比「選了得到一個不夠好的鍵盤」嚴重得多。佈局交給 §9.1.1。
            return listOf(
                KeyboardType(schema.id, schema.name, null, KeyboardType.AUTO_LABEL, false)
            )
        }
        val declaredIds = declared.map { it.first.id }.toSet()
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

    private val WILDCARD = listOf("*")

    /* ────────────────────────── 分組 ────────────────────────── */

    const val ZH_TW = "中文（臺灣正體）"
    const val ZH_HK = "中文（香港）"
    const val YUE = "粵語"
    const val ZH = "中文"
    const val OTHER = "其他"

    /**
     * 方案 → 分組標題。
     *
     * ⚠ **這是啟發式，不是資料。** `rs_schema_list()` 只給得出 `(id, name)`
     * 兩個欄位（見 [RimeSchema]），librime 的 `schema.yaml` 也沒有語言／地區
     * 欄位；方案市集的索引有 `category`，但那是「拼音類／形碼類」的分類，
     * 不是語言，而且只涵蓋市集裝進來的方案，隨附的四個方案不在其中。
     *
     * 所以這裡只能從 id 的地區後綴與方案名的字面推。判準由寬到嚴：
     * 地區後綴（`_tw` / `_hk`）→ 方案名裡的地區字樣 → 中文字碼族的啟發式。
     * 認不出來的一律歸「其他」，**不猜**。
     *
     * 想要真正正確的分組，需要在方案索引（`docs/schema-store.md`）或主題格式
     * 之外的某處補一個 `language: zh-Hant-TW` 欄位。已記在回報中。
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
