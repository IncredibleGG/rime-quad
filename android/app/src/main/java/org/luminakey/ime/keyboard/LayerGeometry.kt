package org.luminakey.ime.keyboard

import org.luminakey.ime.theme.ActionVerb
import org.luminakey.ime.theme.KeyAction
import org.luminakey.ime.theme.KeyboardLayout
import org.luminakey.ime.theme.LayoutLayer

/**
 * 「切個層，整個鍵盤的比例就變了」的防線 —— 邏輯本體。
 *
 * ── 使用者的原話 ────────────────────────────────────────────────────────
 * 「我們的輸入法**切到英文後,頂部的數字不見了**,所以**感覺整個被拉伸**」。
 *
 * 查下去是兩件不同的事被混在一起，只有一件是缺陷：
 *
 * | 佈局 | 中文層 | 英文層 | 列高 |
 * |---|---|---|---|
 * | `cn-t9-pinyin` | 4 列 Σ4.0 | 4 列 Σ4.0 | **一格沒變**（116/116/116/117 px 兩層相同） |
 * | `cn-t9-pinyin-numrow` | `t9` **5 列 Σ4.83** | `en` **4 列 Σ4.0** | **+28.3%** |
 * | `bopomofo-dachen` | `bopomofo` **5 列 Σ4.88** | `alpha` **4 列 Σ4.0** | **+29.6%**（從沒有人回報過） |
 *
 * 第一組的「拉伸」是**鍵的長寬比** 1.92 → 0.82，成因是欄數 5 → 10 讓鍵寬減半；
 * 規範 §8.8.0 已明文把這一項列為刻意接受的代價（「總高固定與長寬比隨欄數
 * 自適應不可兼得」）。**那一組不是缺陷。**
 *
 * 第二、三組才是那句話的字面重現：**列數與 Σweight 一變，列高就變**。
 *
 * ── ⚠ 方向：§8.8.0 是對的，錯的是佈局檔漏了一列 ────────────────────────
 * 直覺會想去改高度模型。實測三星 S24U 的方向**與我們相反**：它的拉丁層是
 * **5 列（含真的數字列）**，比它的九宮格（4 列）**多**一列，總高同為 387.9 dp。
 * §8.8.0 與三星一致；不一致的是「數字列只給中文層」這個**佈局決定**。
 *
 * 為了一個佈局檔漏了一列，去動一條已經被兩版錯誤病歷驗證過、四端共用的
 * 高度模型，方向是反的。
 *
 * ── 規則（規範草稿 §9.1.2.1）────────────────────────────────────────────
 * **`alpha_layer` 與它的 shift 層，列數與 Σ`row.weight` 必須等於
 * `default_layer`。**
 *
 * 這是 §8.8.0 第 5 步的直接推論：
 *
 *     row_h = (budget − padding.top − padding.bottom − row_spacing × (rows − 1)) / Σweight
 *
 * `budget` 只看裝置寬度與主題的參考格，是固定的。列數或 Σweight 一變，
 * `row_h` 就變。
 *
 * ── 為什麼範圍是「按一顆鍵切過去的同一個鍵盤」──────────────────────────
 * [SAME_KEYBOARD_VERBS] 只含 `layer_once` / `layer_lock`（shift）與
 * `alpha_layer`（中／En）。使用者的心智模型是「同一個鍵盤」：他按一顆鍵，
 * 期待鍵盤換內容而不是換形狀。
 *
 * ⚠ **`layer:` 抵達的符號頁／標點頁不在本條範圍內**，`switch_layout:` 更不在。
 * 那不是「這條規則對它們不成立」，而是**這一輪刻意沒有處理**：實測
 * `cn-t9-pinyin-numrow` 的 `num`（4 列 Σ4.0）、`bopomofo-dachen` 的 `punct`
 * （4 列）、`intl-samsung` 的 `sym1`/`sym2`（4 列 Σ4.0，且 `units` 是 7.25
 * 不是 10）都與各自的主層不同 —— 三件同型、都沒有人回報過，已寫進
 * `docs/coordination.md` §5。把它們一起改要動到那幾份符號頁的內容，
 * 那是另一個決定，不該混在這一輪裡偷渡。
 */
object LayerGeometry {

    /**
     * 算「同一個鍵盤」時採計的導覽動作。
     *
     * `LAYER`（`layer:<id>`）**刻意不在**這裡：它是換頁不是換形狀，見檔頭。
     */
    val SAME_KEYBOARD_VERBS = setOf(ActionVerb.LAYER_ONCE, ActionVerb.LAYER_LOCK)

    /** 一層的高度形狀：列數與 Σweight。兩者相等 ⇔ 列高相等。 */
    data class Shape(val rows: Int, val sumWeight: Float) {
        override fun toString(): String = "%d 列 Σ%.2f".format(rows, sumWeight)
    }

    fun shapeOf(layer: LayoutLayer): Shape =
        Shape(layer.rows.size, layer.rows.fold(0f) { acc, r -> acc + r.weight })

    /**
     * 「同一個鍵盤」包含哪些層：`default_layer`、`alpha_layer`，以及從這兩層
     * 出發、只走 [SAME_KEYBOARD_VERBS] 到得了的層（也就是各自的 shift 層）。
     *
     * 回傳順序固定（先 default、再 alpha、再各自的 shift 層），
     * 診斷訊息才不會在兩次執行之間換位置。
     */
    fun sameKeyboardLayers(layout: KeyboardLayout): List<String> {
        val byId = layout.layers.associateBy { it.id }
        val out = LinkedHashSet<String>()
        val queue = ArrayDeque<String>()
        for (start in listOfNotNull(layout.defaultLayer, layout.alphaLayer)) {
            if (byId.containsKey(start)) queue.addLast(start)
        }
        while (queue.isNotEmpty()) {
            val id = queue.removeFirst()
            if (!out.add(id)) continue
            val layer = byId[id] ?: continue
            for (row in layer.rows) {
                for (key in row.keys) {
                    if (key.spacer) continue
                    for (action in actionsOf(key.tap, key.doubleTap, key.longPress)) {
                        if (action.verb !in SAME_KEYBOARD_VERBS) continue
                        val target = action.args.firstOrNull() ?: continue
                        if (byId.containsKey(target)) queue.addLast(target)
                    }
                }
            }
        }
        return out.toList()
    }

    private fun actionsOf(vararg actions: KeyAction?): List<KeyAction> =
        actions.filterNotNull()

    /**
     * 回傳所有「切過去列高會變」的說明。空清單 = 通過。
     *
     * 訊息**指名兩個 layer id 與各自的形狀** —— 「某份佈局不一致」這種話
     * 讀的人還要自己去比對，而漏掉的那一份正是沒有人回報過的那一份。
     */
    fun check(layout: KeyboardLayout): List<String> {
        val ids = sameKeyboardLayers(layout)
        if (ids.size < 2) return emptyList()
        val byId = layout.layers.associateBy { it.id }
        val base = byId[ids.first()] ?: return emptyList()
        val baseShape = shapeOf(base)
        val problems = ArrayList<String>()
        for (id in ids.drop(1)) {
            val layer = byId[id] ?: continue
            val shape = shapeOf(layer)
            if (shape.rows == baseShape.rows &&
                kotlin.math.abs(shape.sumWeight - baseShape.sumWeight) < 1e-4f
            ) continue
            problems += "佈局 ${layout.id}：層「${base.id}」是 $baseShape，" +
                "但按一顆鍵就切得過去的層「$id」是 $shape —— " +
                "§8.8.0 的列高 = (預算 − padding − row_spacing×(列數−1)) / Σweight，" +
                "兩者不等就代表**切一次層每一列的高度都會變**（實測差 ${
                    "%.1f".format(rowHeightDeltaPercent(baseShape, shape))
                }%），使用者的原話是「感覺整個被拉伸」"
        }
        return problems
    }

    /**
     * 兩個形狀在同一個預算下的列高差（%），只用來把診斷訊息說清楚。
     *
     * 用 emulator-5558 實測的那組數（411.43 dp、`intl-ios-light`：預算
     * 219.6 dp、padding 上下各 4 dp、`row_spacing` 實效 10.29 dp）當基準 ——
     * 換一台機器百分比會差一點點，但「有沒有差」不會變，而訊息要的是後者。
     */
    fun rowHeightDeltaPercent(
        base: Shape,
        other: Shape,
        budget: Float = 219.6f,
        paddingV: Float = 4f,
        rowSpacing: Float = 10.29f,
    ): Float {
        fun h(s: Shape): Float {
            val chrome = 2f * paddingV + rowSpacing * (s.rows - 1).coerceAtLeast(0)
            val w = if (s.sumWeight > 0f) s.sumWeight else 1f
            return (budget - chrome) / w
        }
        val a = h(base)
        val b = h(other)
        return if (a == 0f) 0f else (b - a) / a * 100f
    }
}
