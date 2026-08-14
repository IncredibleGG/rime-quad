package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.theme.LayoutLoader
import org.luminakey.ime.theme.Platform
import org.luminakey.ime.theme.RepoFixtures
import org.luminakey.ime.theme.ThemeLoader

/**
 * 交給 `Modifier.weight()` 的每一個數字都必須是**正的**。
 *
 * ── 這條測試是從一次真的崩潰倒推出來的 ─────────────────────────────────
 * CI(commit `d32c72b`,乾淨執行)的 `t9-pinyin` 情境紅在「引擎沒有準備好」,
 * 而 crash buffer 裡是:
 *
 *     E AndroidRuntime: at ...KeyboardViewKt.KeyGrid$lambda$17(KeyboardView.kt:1740)
 *
 * `KeyboardView.kt:1740` 就是 `modifier = Modifier.weight(cellWeight)`。
 * Compose 的 `RowScope.weight()` 第一行是
 *
 *     require(weight > 0.0) { "invalid weight $weight; must be greater than zero" }
 *
 * 也就是說鍵盤**在組字途中整個崩掉**。那幾張 44 KB 的空白截圖不是模擬器沒畫,
 * 是畫的人死了;READY 逾時也是同一件事。
 *
 * ── 為什麼守在這一層 ────────────────────────────────────────────────────
 * `Modifier.weight()` 的四個呼叫點(`KeyboardView.kt` 1638/1698/1740/1746)
 * 的值全部來自 [KeyCells.weights]。所以「不可能是 ≤ 0」這件事釘在
 * [KeyCells.weights] 的出口,而不是在四個呼叫點各寫一次 —— 各寫一次的那一版
 * 遲早會漏掉第五個呼叫點,而漏掉的樣子就是這次的空白截圖。
 *
 * ⚠ 這裡**不能**真的呼叫 Compose 的 `weight()`:JVM 單元測試沒有 Compose 的
 *   量測環境(專案也沒有 Robolectric),`RowScope` 拿不到。[composeWeight] 是
 *   那一行 `require` 的複製品:**條件照抄**(含 NaN),但訊息刻意多帶了值
 *   以利除錯 —— 這一版 Compose 的原文是 `invalid weight; must be greater
 *   than zero`,**沒有值**。所以是同一個例外、同一個條件,不是同一句話。
 */
class KeyWeightFloorTest {

    /**
     * Compose `RowScope.weight()` / `ColumnScope.weight()` 的前置條件,原樣複製。
     *
     * 見 `androidx.compose.foundation.layout.RowColumnImpl`。
     */
    private fun composeWeight(weight: Float): Float {
        require(weight > 0.0) { "invalid weight $weight; must be greater than zero" }
        return weight
    }

    /** 一次 KeyGrid 那一列的算法,原樣照抄(見 `KeyboardView.kt` 1620-1746)。 */
    private fun rowWeightsOf(
        widths: List<Float>,
        units: Float,
        innerW: Float,
        keySpacing: Float,
        outerL: Float,
        outerR: Float,
    ): List<Float> {
        val sizes = KeyCells.visibleSizes(widths, units, innerW, keySpacing)
        return KeyCells.weights(sizes, keySpacing, outerL, outerR)
    }

    /**
     * **退化的量測**：`BoxWithConstraints` 這一幀給 0 dp 寬。
     *
     * `innerW = maxWidth - pad.left - pad.right`,`maxWidth = 0` 就是**負數**。
     * 修之前每一份隨附佈局都會在這裡丟例外。
     */
    @Test
    fun zeroWidthMeasurePassDoesNotThrow() {
        var rows = 0
        forEveryShippedRow { where, widths, units, keySpacing, pad ->
            val outerL = KeyCells.outerPad(pad.first, keySpacing)
            val outerR = KeyCells.outerPad(pad.second, keySpacing)
            for (maxWidth in listOf(0f, 1f, 8f, 24f, 40f)) {
                val innerW = maxWidth - pad.first - pad.second
                val ws = rowWeightsOf(widths, units, innerW, keySpacing, outerL, outerR)
                rows++
                for ((i, w) in ws.withIndex()) {
                    try {
                        composeWeight(w)
                    } catch (e: IllegalArgumentException) {
                        throw AssertionError(
                            "$where 在 maxWidth=$maxWidth dp 的量測下第 $i 格崩潰:${e.message}",
                            e,
                        )
                    }
                }
            }
        }
        assertTrue("一列都沒掃到 —— 夾具或迴圈壞了", rows > 200)
    }

    /**
     * 列高那一條路(`KeyboardView.kt:1638`)也走同一支 [KeyCells.weights]。
     *
     * 鍵盤高度被壓到 0(拖曳、或同樣一次退化的量測)時不得丟例外。
     */
    @Test
    fun zeroHeightMeasurePassDoesNotThrow() {
        for (rowCount in 1..6) {
            for (height in listOf(0f, 1f, 10f, 40f)) {
                val rowSpacing = 12f
                val padV = 4f
                val sizes = KeyCells.visibleSizes(
                    weightsIn = List(rowCount) { 1f },
                    total = rowCount.toFloat(),
                    availableDp = height - padV * 2,
                    spacingDp = rowSpacing,
                )
                val outer = KeyCells.outerPad(padV, rowSpacing)
                val ws = KeyCells.weights(sizes, rowSpacing, outer, outer)
                for (w in ws) composeWeight(w)
            }
        }
    }

    /**
     * 降級之後**還是一個鍵盤**:總寬仍然填滿,格與格的先後與比例沒有翻轉。
     *
     * 少了這一條,「全部回傳同一個常數」也會綠 —— 而那等於把鍵盤畫成一排等寬
     * 的細條,連空白鍵都不見了。
     */
    @Test
    fun degradedRowStillFillsAndKeepsOrdering() {
        val widths = listOf(1f, 1f, 1f, 4f, 1f)   // 中間那顆是空白鍵
        val units = 8f
        val keySpacing = 6f
        // 排得下:比例就是宣告的比例。
        val wide = rowWeightsOf(widths, units, 400f, keySpacing, 3f, 3f)
        assertTrue("空白鍵應該最寬", wide[3] > wide[0])
        // 排不下(400 → 20 dp):不丟例外、每一格仍然 > 0、空白鍵仍然不比別人窄。
        val tight = rowWeightsOf(widths, units, 20f, keySpacing, 3f, 3f)
        for (w in tight) composeWeight(w)
        assertTrue("降級後空白鍵不該比一般鍵窄", tight[3] >= tight[0])
    }

    /**
     * 這是純粹的**量測**函式：同樣的輸入永遠得到同樣的輸出,沒有記住任何東西。
     *
     * 這就是「降級之後會自己回復」的全部理由 —— `innerW` 每一次量測都從
     * `BoxWithConstraints` 的 `maxWidth` 重算,轉向 / 視窗重建的暫態過去之後,
     * 下一次量測拿到的就是正常寬度,而這裡不會把上一次的退化值留下來。
     */
    @Test
    fun degradationIsNotSticky() {
        val widths = listOf(1f, 1f, 1f, 4f, 1f)
        val units = 8f
        val before = rowWeightsOf(widths, units, 401f, 6f, 3f, 3f)
        rowWeightsOf(widths, units, -10f, 6f, 3f, 3f)   // 退化的那一幀
        val after = rowWeightsOf(widths, units, 401f, 6f, 3f, 3f)
        assertEquals(before, after)
    }

    /** 邊角:空列、單格、NaN、負 padding —— 出口一律是正數。 */
    @Test
    fun edgeInputsNeverProduceNonPositiveWeight() {
        assertEquals(emptyList<Float>(), KeyCells.visibleSizes(emptyList(), 1f, 100f, 6f))
        for (w in KeyCells.weights(listOf(0f), 0f, 0f, 0f)) composeWeight(w)
        for (w in KeyCells.weights(listOf(Float.NaN, 1f), 6f, 3f, 3f)) composeWeight(w)
        for (w in KeyCells.weights(KeyCells.visibleSizes(listOf(1f), 1f, Float.NaN, 6f), 0f, 0f, 0f)) {
            composeWeight(w)
        }
        for (w in KeyCells.weights(listOf(-500f, -500f), 0f, 0f, 0f)) composeWeight(w)
    }

    /** 每一份隨附主題 × 每一份隨附佈局 × 每一層 × 每一列。 */
    private fun forEveryShippedRow(
        body: (
            where: String,
            widths: List<Float>,
            units: Float,
            keySpacing: Float,
            pad: Pair<Float, Float>,
        ) -> Unit,
    ) {
        for (themeId in RepoFixtures.themeIds) {
            val theme = ThemeLoader.load(themeId, RepoFixtures.themes, Platform.ANDROID).value
                ?: error("主題 $themeId 載不起來")
            val pad = theme.keyboard.padding
            for (layoutId in RepoFixtures.layoutIds) {
                val layout = LayoutLoader.load(layoutId, RepoFixtures.layouts, Platform.ANDROID)
                    .value ?: error("佈局 $layoutId 載不起來")
                val keySpacing = layout.metrics.keySpacing ?: theme.keyboard.keySpacing
                for (layer in layout.layers) {
                    for ((rowIndex, row) in layer.rows.withIndex()) {
                        val slack = layer.units - row.widthSum
                        val widths = row.keys.map { it.width } +
                            (if (slack > 0.01f) listOf(slack) else emptyList())
                        body(
                            "$themeId/$layoutId:${layer.id} 第 $rowIndex 列",
                            widths,
                            layer.units,
                            keySpacing,
                            pad.left to pad.right,
                        )
                    }
                }
            }
        }
    }
}
