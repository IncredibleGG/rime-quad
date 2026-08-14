package org.luminakey.ime.keyboard

/**
 * 方案名裡那句「臺灣正體」與簡繁開關,是**兩個不同的東西**。
 *
 * ── 走查抓到的畫面(工單 #107,#81 的 Android 雙胞胎)────────────────────
 * 設定頁把簡繁切成「簡體」之後,同一個畫面上同時看得到:
 *
 *   候選列   立好  利好  你好          ← 引擎真的在吐簡體
 *   空白鍵   朙月拼音·臺灣正體         ← 而這裡寫著臺灣正體
 *
 * 兩句話都是「真的」,只是講的不是同一件事:
 *   · `朙月拼音·臺灣正體` 是**方案的名字**,`luna_pinyin_tw.schema.yaml` 裡
 *     的一個字串,它宣告的是這個方案**預設**的字集;
 *   · 簡繁開關是**使用者的覆寫**,走的是 librime 的 `zh_hans` / `zh_hant_tw`
 *     那組互斥選項(見 [org.luminakey.ime.core.VariantPlan])。
 *
 * 使用者沒有義務知道這兩件事是分開的。他看到的是一個自己打自己的畫面,
 * 而且**沒有任何線索告訴他哪一句才是現在生效的**。
 *
 * ── 這支函式的判斷:只在真的矛盾時介入 ────────────────────────────────
 * 三種情形,三種答案:
 *   1. 方案名沒有宣告字集(`朙月拼音`、`倉頡五代`)→ **原樣不動**。
 *      沒有矛盾就不要多嘴;更要緊的是,對非中文方案胡亂接上「繁體」是錯的。
 *   2. 宣告的與現在生效的**一致** → **原樣不動**,保留 `臺灣正體`。
 *      它比 `繁體` 精確(台灣的字形取捨),沒有理由把它換成更粗的說法。
 *   3. 宣告的與現在生效的**相反** → 換成現在生效的那一個。
 *      這是唯一會改字的那條路,而它正是矛盾發生的那一刻。
 *
 * 這條規則的形狀與 `home/SettingsPages.kt` 的 [familyNameOf] 相同:那裡把
 * 「預設淺色」的「淺色」剝掉,因為深淺**已經是另一個控制項了**。同一個道理。
 *
 * ── 誰來呼叫,以及 `simplified` 為什麼是可空的 ──────────────────────────
 * 兩個呼叫端問的是同一個問題,但手上的事實不同:
 *   · 鍵盤(空白鍵鍵面)拿的是引擎的當下狀態 `RimeStatus.isSimplified`,
 *     那是一個 Boolean —— 引擎一定處在某一邊;
 *   · App 設定頁拿的是 `UserPrefs.simplification`,那是 `Boolean?`,
 *     而 **null 的語義是「使用者沒有覆寫,讓方案自己說」** ——
 *     那正是情形 1,原樣不動。
 *
 * 所以參數是 `Boolean?`,null 直接回傳原名。兩個呼叫端因此共用同一段判斷,
 * 不會各寫一次然後漂移。
 */
object SchemaVariantLabel {

    /**
     * ⚠ 這幾個漢字**刻意不進 strings.xml**,與 `KeyboardView.faceOf()` 裡的
     * 「中／En」「繁／简」同一個理由:它們是**鍵面**,不是介面文案。
     * 一顆中文輸入法的空白鍵在英文系統上仍然印中文方案名,而方案名本來就是
     * 中文的 —— 把接在它後面的那一段翻成 `Simplified` 會產生
     * `朙月拼音·Simplified` 這種半中半英的東西。
     */
    const val TRADITIONAL = "繁體"
    const val SIMPLIFIED = "简体"

    /** 方案名裡用來接字集的分隔符。`·` 是隨附方案用的那一個。 */
    private const val SEPARATOR = '·'

    private val SEPARATORS = charArrayOf('·', '・', '-', ' ')

    /**
     * 尾段宣告的是簡體嗎。null = 這一段不是字集宣告(或根本沒有尾段)。
     *
     * ⚠ 這張表刻意**只認隨附方案與常見第三方方案真的用過的寫法**,不做
     * 模糊比對。認得太寬的下場是把方案名裡合法的一段(例如「五筆·簡入繁出」
     * 的「簡入繁出」)誤判成字集宣告然後改掉它 —— 那是把一個顯示問題
     * 換成一個更難查的顯示問題。認不出來就原樣不動,那是安全的那一邊。
     */
    private val CLAIMS: Map<String, Boolean> = mapOf(
        "臺灣正體" to false,
        "台灣正體" to false,
        "臺灣字形" to false,
        "台湾字形" to false,
        "香港字形" to false,
        "傳統漢字" to false,
        "繁體" to false,
        "繁体" to false,
        "正體" to false,
        "正体" to false,
        "简化字" to true,
        "簡化字" to true,
        "简体" to true,
        "簡體" to true,
        "简体字" to true,
        "簡體字" to true,
    )

    /** 這個方案名的尾段宣告了哪一種字集;null = 沒有宣告。 */
    fun claimOf(schemaName: String): Boolean? {
        val cut = schemaName.lastIndexOfAny(SEPARATORS)
        if (cut < 0) return null
        val tail = schemaName.substring(cut + 1).trim()
        return CLAIMS[tail]
    }

    /**
     * 要印在空白鍵／設定摘要上的名字。
     *
     * @param schemaName `rs_status` 或帳本給的方案名。
     * @param simplified 現在**真的**在用的字集;null = 沒有人覆寫過。
     */
    fun display(schemaName: String, simplified: Boolean?): String {
        if (simplified == null) return schemaName
        val claim = claimOf(schemaName) ?: return schemaName
        if (claim == simplified) return schemaName

        val cut = schemaName.lastIndexOfAny(SEPARATORS)
        val base = schemaName.substring(0, cut).trimEnd()
        if (base.isEmpty()) return schemaName
        return base + SEPARATOR + if (simplified) SIMPLIFIED else TRADITIONAL
    }
}
