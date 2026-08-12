package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.theme.LayoutLoader
import org.luminakey.ime.theme.Platform
import org.luminakey.ime.theme.RepoFixtures
import org.luminakey.ime.theme.ThemeLoader
import kotlin.math.abs

/**
 * 鍵縫折進命中格之後，**畫出來的每一顆鍵一個像素都不能動**。
 *
 * ── 這條測試在測什麼 ────────────────────────────────────────────────────
 * [KeyCells] 的作法是把間距折進 weight，再由鍵自己往內縮回來。折算寫錯的話
 * 症狀是「鍵盤看起來歪了一點點」—— 那種東西截圖對照得出來，但要人去看；
 * 而它每一份佈局、每一層、每一列都可能不一樣（十二份佈局、四十一層）。
 *
 * 所以這裡對**每一份隨附主題 × 每一份隨附佈局 × 每一層 × 每一列**，
 * 各算兩遍：
 *   · 舊模型：`Arrangement.spacedBy(spacing)` + `weight(width)`；
 *   · 新模型：`spacedBy(0)` + `weight(KeyCells.weights(...))`，再扣掉內縮。
 * 兩邊的每一顆鍵的左緣與寬度必須相等（容差 0.01 dp）。
 *
 * ── 它抓不到什麼 ────────────────────────────────────────────────────────
 * 它算的是**版面公式**，不是 Compose 真的畫出來的像素。KeyView 把 padding
 * 插在鏈上錯誤的位置（例如插在 `pointerInput` 前面）時，這裡照樣是綠的 ——
 * 那一半由 [InputModePairTest] 那種原始碼掃描與實機截圖負責。
 */
class KeyCellsTest {

    private val eps = 0.01f

    /** 模擬器與 S24U 兩種螢幕；寬度不同，四捨五入的位置也不同。 */
    private val screens = listOf(
        Triple("emulator 1080x2400 @420", 1080f, 420f),
        Triple("S24U 1440x3120 @505", 1440f, 505f),
    )

    @Test
    fun visibleRectsAreUnchangedForEveryShippedLayout() {
        var rows = 0
        for (themeId in RepoFixtures.themeIds) {
            val theme = ThemeLoader.load(themeId, RepoFixtures.themes, Platform.ANDROID).value
                ?: error("主題 $themeId 載不起來")
            val pad = theme.keyboard.padding
            for (layoutId in RepoFixtures.layoutIds) {
                val layout = LayoutLoader.load(layoutId, RepoFixtures.layouts, Platform.ANDROID)
                    .value ?: error("佈局 $layoutId 載不起來")
                val keySpacing = layout.metrics.keySpacing ?: theme.keyboard.keySpacing
                for ((name, widthPx, density) in screens) {
                    val widthDp = widthPx / (density / 160f)
                    val innerW = widthDp - pad.left - pad.right
                    val outerL = KeyCells.outerPad(pad.left, keySpacing)
                    val outerR = KeyCells.outerPad(pad.right, keySpacing)
                    for (layer in layout.layers) {
                        for ((rowIndex, row) in layer.rows.withIndex()) {
                            rows++
                            val slack = layer.units - row.widthSum
                            val widths = row.keys.map { it.width } +
                                (if (slack > 0.01f) listOf(slack) else emptyList())

                            // ── 舊模型 ────────────────────────────────────
                            val total = maxOf(layer.units, widths.sum())
                            val gaps = (widths.size - 1).coerceAtLeast(0)
                            val unit = (innerW - keySpacing * gaps) / total
                            val oldRects = mutableListOf<Pair<Float, Float>>()
                            var x = pad.left
                            for (w in widths) {
                                oldRects += x to (w * unit)
                                x += w * unit + keySpacing
                            }

                            // ── 新模型 ────────────────────────────────────
                            val sizes =
                                KeyCells.visibleSizes(widths, layer.units, innerW, keySpacing)
                            val weights = KeyCells.weights(sizes, keySpacing, outerL, outerR)
                            // Compose 會把整條可用寬度按 weight 分掉。
                            val span = widthDp - (pad.left - outerL) - (pad.right - outerR)
                            val sum = weights.sum()
                            val newRects = mutableListOf<Pair<Float, Float>>()
                            var cx = pad.left - outerL
                            for ((i, wgt) in weights.withIndex()) {
                                val cell = span * wgt / sum
                                val start = KeyCells.padStart(i, keySpacing, outerL)
                                val end = KeyCells.padEnd(i, weights.size, keySpacing, outerR)
                                newRects += (cx + start) to (cell - start - end)
                                cx += cell
                            }

                            assertEquals(
                                "$themeId/$layoutId:${layer.id} 第 $rowIndex 列的格數不一致",
                                oldRects.size, newRects.size,
                            )
                            for (i in oldRects.indices) {
                                val where = "$name $themeId/$layoutId:${layer.id}" +
                                    " 第 $rowIndex 列第 $i 顆"
                                assertTrue(
                                    "$where 的左緣移動了：${oldRects[i].first} → ${newRects[i].first}",
                                    abs(oldRects[i].first - newRects[i].first) < eps,
                                )
                                assertTrue(
                                    "$where 的寬度變了：${oldRects[i].second} → ${newRects[i].second}",
                                    abs(oldRects[i].second - newRects[i].second) < eps,
                                )
                            }
                        }
                    }
                }
            }
        }
        // G2：一列都沒掃到就是掃描壞了，不是「全部相等」。
        assertTrue("一列都沒掃到 —— 夾具或迴圈壞了", rows > 200)
    }

    /**
     * 命中格真的變大了 —— 而且是**恰好一個間距**。
     *
     * 少了這一條，上面那條「外觀不變」可以靠「什麼都不做」通過。
     */
    @Test
    fun hitAreaGrowsByExactlyOneSpacing() {
        val sizes = listOf(34.74f, 34.74f, 34.74f)
        val spacing = 6f
        // 內側的格：兩邊各半個縫。
        val weights = KeyCells.weights(sizes, spacing, outerStartDp = 3f, outerEndDp = 3f)
        for ((i, w) in weights.withIndex()) {
            assertEquals("第 $i 格", (sizes[i] + spacing).toDouble(), w.toDouble(), 1e-4)
        }
    }

    /**
     * padding 比半個縫還小時，最外側那半個討不回來 —— 但**內側的每一道都還在**。
     *
     * `default-light` 的 `padding.top = 4` 對 `row_spacing = 12` 就是這個情形。
     * 這一條把那個取捨釘死：討不回來是物理限制（螢幕邊緣外沒有空間），
     * 不是可以順手放棄整個修法的理由。
     */
    @Test
    fun outerHalfIsCappedByPaddingButInnerGapsSurvive() {
        assertEquals(4f, KeyCells.outerPad(paddingDp = 4f, spacingDp = 12f))
        assertEquals(3f, KeyCells.outerPad(paddingDp = 5f, spacingDp = 6f))
        assertEquals(0f, KeyCells.outerPad(paddingDp = 0f, spacingDp = 6f))
        // 內側永遠是半個縫，與 padding 無關。
        assertEquals(6f, KeyCells.padStart(index = 1, spacingDp = 12f, outerStartDp = 4f))
        assertEquals(6f, KeyCells.padEnd(index = 1, count = 4, spacingDp = 12f, outerEndDp = 4f))
    }

    /** 只有一格的列（例如某些符號層）不會把兩端的量算成三份。 */
    @Test
    fun singleCellRowTakesBothOuterHalvesOnly() {
        val w = KeyCells.weights(listOf(100f), spacingDp = 8f, outerStartDp = 4f, outerEndDp = 4f)
        assertEquals(1, w.size)
        assertEquals(108.0, w[0].toDouble(), 1e-4)
    }
}
