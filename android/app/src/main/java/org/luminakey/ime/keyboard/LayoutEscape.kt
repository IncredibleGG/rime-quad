package org.luminakey.ime.keyboard

import org.luminakey.ime.theme.ActionVerb
import org.luminakey.ime.theme.KeyAction
import org.luminakey.ime.theme.KeyboardLayout
import org.luminakey.ime.theme.LayoutKey

/**
 * 「進得去、出不來」的通用防線 —— 邏輯本體。
 *
 * ── 這段程式碼原本住在 LayoutEscapeTest 裡 ──────────────────────────────
 * 搬到 main 是因為使用者自訂鍵位（[applyKeyRemap]）必須通過**同一條**檢查。
 * 若在驗證那一側另寫一份「差不多」的走訪，兩份會各自演化：測試那份守著隨附
 * 佈局、驗證那份守著使用者佈局，而使用者最容易把自己鎖死的正是後者。
 * 一份邏輯、兩個呼叫端，這是唯一不會漂移的作法。
 * 測試現在只負責提供夾具與斷言（見 LayoutEscapeTest）。
 *
 * ── 規則 ────────────────────────────────────────────────────────────────
 * 從每一份佈局的預設層出發，用**只有 tap 觸發**的導覽鍵（`layer` /
 * `layer_once` / `layer_lock` / `switch_layout`）能走到的每一個狀態，
 * 都必須走得回出發點。
 *
 * 刻意**不**採計 `long_press`、`swipe`、`double_tap` 與 popup 裡的動作：
 * 藏在手勢裡的回程等於沒有回程 —— 使用者卡住的當下不會知道要長按哪裡。
 */
object LayoutEscape {

    /** 走幾步。隨附佈局裡最長的一條合法路徑是 3 步，留一點餘裕。 */
    const val DEPTH = 4

    // `input_mode:toggle` 也算導覽鍵：宣告了 alpha_layer 的佈局按下去會換層，
    // 而「換得過去、換不回來」正是這支要擋的事。不把它列進來，九宮格的
    // 中／En 鍵就會是這張圖上的一個盲點 —— 而那顆鍵正是上一次死路的現場。
    private val NAV_VERBS = setOf(
        ActionVerb.LAYER, ActionVerb.LAYER_ONCE, ActionVerb.LAYER_LOCK, ActionVerb.SWITCH_LAYOUT,
        ActionVerb.INPUT_MODE_TOGGLE,
    )

    /**
     * 從 [startLayout] 出發做圖走訪，回傳所有「回不去」的狀態說明。空清單 = 通過。
     *
     * [newHost] 每次都要交出一個**全新**的 [LayoutHost]：`@previous` 帶歷史，
     * 只記 (佈局, 層) 會騙自己，所以每一條路徑都從頭重放。傳工廠而不是傳
     * repository，是為了讓呼叫端能決定要不要包一層快取（見
     * [CachingLayoutRepository]）—— 重放的次數是指數級的，每次重新解析 YAML
     * 會讓一次驗證從毫秒變成好幾秒。
     */
    fun check(startLayout: String, newHost: () -> LayoutHost): List<String> {
        val origin = replay(newHost, startLayout, emptyList()).state()
        val problems = ArrayList<String>()
        for ((state, path) in reachableFrom(newHost, startLayout, origin)) {
            if (state == origin) continue
            if (!canReturnTo(newHost, startLayout, path, origin)) {
                problems += "佈局 $startLayout：走 ${describe(newHost, startLayout, path)} " +
                    "之後停在 $state，接下來 $DEPTH 步之內沒有任何看得見的鍵能回到 " +
                    "$origin —— 這是死路"
            }
        }
        return problems
    }

    /**
     * `layer:<id>` 指向本佈局不存在的層 = 那顆鍵是啞的，也是卡死的來源之一。
     * [knownLayoutIds] 是搜尋路徑上看得見的全部佈局，用來檢查 `switch_layout`。
     */
    fun checkLayerReferences(
        layout: KeyboardLayout,
        knownLayoutIds: Collection<String>,
    ): List<String> {
        val problems = ArrayList<String>()
        val known = layout.layers.map { it.id }.toSet()
        for (layer in layout.layers) {
            for (action in navActionsIn(layer.rows.flatMap { it.keys })) {
                if (action.verb == ActionVerb.INPUT_MODE_TOGGLE) {
                    // 目標不是寫在鍵上，而是佈局的 alpha_layer；
                    // 那個欄位在解析時就驗過了（F9），這裡沒有第二種寫錯的方式。
                    continue
                }
                val arg = action.arg ?: continue
                if (action.verb == ActionVerb.SWITCH_LAYOUT) {
                    val ok = arg == "@primary" || arg == "@previous" || knownLayoutIds.contains(arg)
                    if (!ok) {
                        problems += "${layout.id}/${layer.id}: switch_layout 指向不存在的佈局「$arg」"
                    }
                } else if (!known.contains(arg)) {
                    problems += "${layout.id}/${layer.id}: ${action.raw} 指向本佈局沒有的層「$arg」"
                }
            }
        }
        return problems
    }

    /* ── 圖的走訪 ─────────────────────────────────────────────────────── */

    private fun LayoutHost.state(): String = "${layout?.id}/$layerId"

    /** 只認 tap；藏在手勢裡的回程不算回程。 */
    fun navActionsIn(keys: List<LayoutKey>): List<KeyAction> =
        keys.mapNotNull { it.tap }.filter { NAV_VERBS.contains(it.verb) }

    private fun navActions(h: LayoutHost): List<KeyAction> =
        navActionsIn(h.layout?.layer(h.layerId)?.rows.orEmpty().flatMap { it.keys })

    /** 每次都從頭重放：`@previous` 有歷史，只記 (佈局, 層) 會騙自己。 */
    private fun replay(
        newHost: () -> LayoutHost,
        startLayout: String,
        path: List<Int>,
    ): LayoutHost {
        val h = newHost()
        h.ensureLoaded()                 // 使用者的起點永遠是 primary
        h.switchLayout(startLayout)
        for (i in path) {
            val actions = navActions(h)
            apply(h, actions[i])
        }
        return h
    }

    private fun apply(h: LayoutHost, a: KeyAction) {
        // input_mode:toggle 沒有參數，所以不能像其他導覽動詞那樣先取 arg。
        if (a.verb == ActionVerb.INPUT_MODE_TOGGLE) {
            h.toggleInputMode()
            return
        }
        val arg = a.arg ?: return
        when (a.verb) {
            ActionVerb.LAYER -> h.setLayer(arg)
            ActionVerb.LAYER_ONCE -> h.setLayerOnce(arg)
            ActionVerb.LAYER_LOCK -> h.lockLayer(arg)
            ActionVerb.SWITCH_LAYOUT -> h.switchLayout(arg)
            else -> Unit
        }
    }

    private fun reachableFrom(
        newHost: () -> LayoutHost,
        start: String,
        origin: String,
    ): Map<String, List<Int>> {
        val seen = LinkedHashMap<String, List<Int>>()
        seen[origin] = emptyList()
        var frontier = listOf<List<Int>>(emptyList())
        repeat(DEPTH) {
            val next = ArrayList<List<Int>>()
            for (p in frontier) {
                val h = replay(newHost, start, p)
                for (i in navActions(h).indices) {
                    val np = p + i
                    val s = replay(newHost, start, np).state()
                    if (!seen.containsKey(s)) {
                        seen[s] = np
                        next += np
                    }
                }
            }
            frontier = next
        }
        return seen
    }

    private fun canReturnTo(
        newHost: () -> LayoutHost,
        start: String,
        prefix: List<Int>,
        origin: String,
    ): Boolean {
        var frontier = listOf(prefix)
        val visited = HashSet<String>()
        repeat(DEPTH) {
            val next = ArrayList<List<Int>>()
            for (p in frontier) {
                val h = replay(newHost, start, p)
                for (i in navActions(h).indices) {
                    val np = p + i
                    val s = replay(newHost, start, np).state()
                    if (s == origin) return true
                    if (visited.add(s + "@" + np.size)) next += np
                }
            }
            frontier = next
        }
        return false
    }

    private fun describe(newHost: () -> LayoutHost, start: String, path: List<Int>): String {
        val steps = ArrayList<String>()
        for (n in path.indices) {
            val h = replay(newHost, start, path.take(n))
            steps += navActions(h)[path[n]].raw
        }
        return steps.joinToString(" → ")
    }
}
