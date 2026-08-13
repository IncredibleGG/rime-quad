package org.luminakey.ime.keyboard

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.luminakey.ime.theme.LayoutLoader
import org.luminakey.ime.theme.Platform
import org.luminakey.ime.theme.RepoFixtures

/**
 * 「切個層，整個鍵盤的比例就變了」（§9.1.2.1）。
 *
 * ⚠ 這一段從前寫著「§9.1.2.1 / §10 第 45 條」。§10 第 45 條講的是**候選列
 *   右端的控制鍵**，與層高比例毫無關係 —— 那個引用是懸空的條號被後來的
 *   改動填上別的內容之後，從「查不到」升級成「指著錯的東西」。
 *   §9.1.2.1 的本文現在真的存在（`docs/theme-format.md`）。
 *
 * ── 使用者的原話 ────────────────────────────────────────────────────────
 * 「我們的輸入法**切到英文後,頂部的數字不見了**,所以**感覺整個被拉伸**。」
 *
 * 而且他同一段話裡的第二句是對我們的：「**這個之前都注意麼?那產品經理在調研
 * 什麼**」。那句成立 —— 上一輪的基礎功能盤點**完全沒有看過英文層**。
 * 這一支就是那個盤點應該做而沒做的事：**逐份佈局**問「切過去之後列高一樣嗎」。
 *
 * ⚠ 逐份，不是列幾個 id。這個專案已經因為清單寫死漏檢過兩次。
 *   實測抓到的兩份裡，`bopomofo-dachen` **從來沒有人回報過** ——
 *   使用者只看得到他在用的那一份。
 */
class LayerGeometryTest {

    private fun layout(id: String) =
        LayoutLoader.load(id, RepoFixtures.layouts, Platform.ANDROID).value
            ?: error("佈局 $id 載不起來")

    @Test
    fun `每一份佈局切到字母層之後列高都不變`() {
        val problems = RepoFixtures.layoutIds.flatMap { LayerGeometry.check(layout(it)) }
        assertTrue(problems.joinToString("\n"), problems.isEmpty())
    }

    /**
     * 這一輪修好的兩份，形狀真的補齊了。
     *
     * 上面那條是通則，這一條是**病歷**：通則哪天被放寬（例如有人把
     * `alpha_layer` 從走訪起點裡拿掉），它會單獨紅出來，而通則不會。
     */
    @Test
    fun `九宮格數字列版與大千注音的字母層都補上了那一列`() {
        for ((id, want) in listOf(
            "cn-t9-pinyin-numrow" to LayerGeometry.Shape(5, 4.83f),
            "bopomofo-dachen" to LayerGeometry.Shape(5, 4.88f),
        )) {
            val l = layout(id)
            for (layerId in LayerGeometry.sameKeyboardLayers(l)) {
                val layer = l.layers.first { it.id == layerId }
                val got = LayerGeometry.shapeOf(layer)
                assertEquals(
                    "$id 的層「$layerId」是 $got，不是 $want",
                    want.rows, got.rows,
                )
                assertEquals("$id 的層「$layerId」Σweight 是 ${got.sumWeight}", want.sumWeight, got.sumWeight, 1e-4f)
            }
        }
    }

    /**
     * 「同一個鍵盤」包含 shift 層。
     *
     * 只顧 `alpha_layer` 而放過它的 shift 層，症狀一模一樣：按一下 shift，
     * 每一列高度改變 28% —— 而規則本身是綠的。
     */
    @Test
    fun `走訪把 shift 層也算進同一個鍵盤`() {
        assertEquals(
            listOf("t9", "en", "en_upper"),
            LayerGeometry.sameKeyboardLayers(layout("cn-t9-pinyin-numrow")),
        )
        assertEquals(
            listOf("bopomofo", "alpha", "alpha_upper"),
            LayerGeometry.sameKeyboardLayers(layout("bopomofo-dachen")),
        )
    }

    /**
     * ⚠ **`layer:` 抵達的符號頁不在本條範圍內，而且那不是「沒問題」。**
     *
     * 實測仍然不一致的三處（全部沒有人回報過，已寫進 coordination.md §5）：
     *   · `cn-t9-pinyin-numrow` 的 `num`（4 列 Σ4.0）vs `t9`（5 列 Σ4.83）
     *   · `bopomofo-dachen` 的 `punct`（4 列）vs `bopomofo`（5 列）
     *   · `intl-samsung` 的 `sym1`/`sym2`（4 列 Σ4.0，`units` 7.25）vs `lower`（5 列 Σ4.83）
     *
     * 把這件事寫成一條**會紅的測試**而不是一段註解：哪天有人把 `LAYER`
     * 加進 [LayerGeometry.SAME_KEYBOARD_VERBS]，這裡會提醒他那三份佈局
     * 也要一起改，而不是讓通則那一條莫名其妙變紅。
     */
    @Test
    fun `符號頁的不一致是已知的、刻意留著的`() {
        val stillDifferent = listOf("cn-t9-pinyin-numrow", "bopomofo-dachen", "intl-samsung")
        for (id in stillDifferent) {
            val l = layout(id)
            val same = LayerGeometry.sameKeyboardLayers(l).toSet()
            val others = l.layers.filter { it.id !in same }
            val base = LayerGeometry.shapeOf(l.layers.first { it.id == LayerGeometry.sameKeyboardLayers(l).first() })
            assertTrue(
                "$id 的符號／標點頁現在與主層形狀相同了 —— 那就把 LAYER 加進 " +
                    "SAME_KEYBOARD_VERBS，讓通則守住它，別讓這個事實只活在註解裡",
                others.any { LayerGeometry.shapeOf(it) != base },
            )
        }
    }

    /**
     * 反向測試：**故意把一層的列拿掉，那條通則必須紅。**
     *
     * 本專案的舊帳是「反向測試自己也會靜靜地不做事」—— 植入沒成功的話樹是
     * 沒改過的，守門當然綠。所以這裡先斷言**植入真的改變了形狀**，
     * 再斷言檢查會紅，兩種失敗分開講。
     */
    @Test
    fun `把字母層砍掉一列之後這條檢查會紅`() {
        val original = layout("cn-t9-pinyin-numrow")
        val before = LayerGeometry.check(original)
        assertTrue("植入之前就已經紅了，這條反向測試證明不了任何事：$before", before.isEmpty())

        val planted = original.copy(
            layers = original.layers.map { layer ->
                if (layer.id == "en") layer.copy(rows = layer.rows.drop(1)) else layer
            }
        )
        val plantedShape = LayerGeometry.shapeOf(planted.layers.first { it.id == "en" })
        assertEquals("植入沒有生效 —— 樹根本沒改過", 4, plantedShape.rows)

        val problems = LayerGeometry.check(planted)
        assertTrue("砍掉一整列，檢查竟然是綠的", problems.isNotEmpty())
        assertTrue(
            "訊息沒有指名是哪兩層 —— 讀的人還要自己去比對，而漏掉的正是沒人回報的那一份：$problems",
            problems.first().contains("t9") && problems.first().contains("en"),
        )
    }
}
